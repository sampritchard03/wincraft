#include "density_shape.h"

#include "sculpt_terrain.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/error_macros.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/plane.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace godot;

namespace {

constexpr int ZSTD = 2; // FileAccess.COMPRESSION_ZSTD

float falloff(float t) {
	return t >= 1.0f ? 0.0f : 1.0f - t * t * (3.0f - 2.0f * t);
}

// A repeatable pseudo-random number in [0, 1) per point and call (as the
// terrain's brushes use): added before rounding, small changes add up on
// average instead of always rounding away.
float dither(int x, int y, int z, uint32_t p_serial) {
	uint32_t h = (uint32_t)x * 0x8DA6B343u ^ (uint32_t)y * 0xD8163841u ^ (uint32_t)z * 0xCB1AB31Fu ^ p_serial * 0x9E3779B9u;
	h ^= h >> 15;
	h *= 0x2C1B3C6Du;
	h ^= h >> 12;
	return (float)(h & 0xFFFFFF) / (float)0x1000000;
}

// The closest point of triangle abc to p, as barycentric weights.
Vector3 closest_on_triangle(const Vector3 &p, const Vector3 &a, const Vector3 &b, const Vector3 &c) {
	const Vector3 ab = b - a, ac = c - a, ap = p - a;
	const float d1 = ab.dot(ap), d2 = ac.dot(ap);
	if (d1 <= 0.0f && d2 <= 0.0f) {
		return Vector3(1, 0, 0);
	}
	const Vector3 bp = p - b;
	const float d3 = ab.dot(bp), d4 = ac.dot(bp);
	if (d3 >= 0.0f && d4 <= d3) {
		return Vector3(0, 1, 0);
	}
	const float vc = d1 * d4 - d3 * d2;
	if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
		const float v = d1 / (d1 - d3);
		return Vector3(1 - v, v, 0);
	}
	const Vector3 cp = p - c;
	const float d5 = ab.dot(cp), d6 = ac.dot(cp);
	if (d6 >= 0.0f && d5 <= d6) {
		return Vector3(0, 0, 1);
	}
	const float vb = d5 * d2 - d1 * d6;
	if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
		const float w = d2 / (d2 - d6);
		return Vector3(1 - w, 0, w);
	}
	const float va = d3 * d6 - d5 * d4;
	if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
		const float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
		return Vector3(0, 1 - w, w);
	}
	const float denom = 1.0f / (va + vb + vc);
	const float v = vb * denom, w = vc * denom;
	return Vector3(1 - v - w, v, w);
}

// The solid angle triangle abc covers seen from p (signed by its winding).
float solid_angle(const Vector3 &p, const Vector3 &a, const Vector3 &b, const Vector3 &c) {
	const Vector3 x = a - p, y = b - p, z = c - p;
	const float lx = x.length(), ly = y.length(), lz = z.length();
	const float num = x.dot(y.cross(z));
	const float den = lx * ly * lz + x.dot(y) * lz + x.dot(z) * ly + y.dot(z) * lx;
	return 2.0f * std::atan2(num, den);
}

// How each material behaves in a body: density (relative to a colour,
// 1), bounce and friction.
struct MaterialPhysics {
	float density, bounce, friction;
};
MaterialPhysics material_physics(MaterialId p_material) {
	if (is_color_material(p_material)) {
		return { 1.0f, 0.6f, 0.5f };
	}
	switch (p_material) {
		case MAT_GRASS:
			return { 0.7f, 0.3f, 0.8f };
		case MAT_DIRT:
			return { 0.9f, 0.2f, 0.8f };
		case MAT_STONE:
			return { 1.6f, 0.35f, 0.6f };
		case MAT_SAND:
			return { 1.1f, 0.05f, 0.9f };
		case MAT_WOOD:
			return { 0.6f, 0.45f, 0.6f };
		case MAT_COBBLESTONE:
			return { 1.5f, 0.3f, 0.7f };
		case MAT_CLOTH:
			return { 0.4f, 0.15f, 0.9f };
		case MAT_SPONGE:
			return { 0.25f, 0.95f, 0.8f };
		default:
			return { 1.0f, 0.4f, 0.6f };
	}
}

// Mass units per cubic voxel of density 1.
constexpr double MASS_PER_VOXEL = 0.01;

MaterialId material_of_color(const Color &p_color) {
	auto channel = [](float v) { return std::clamp((int)std::lround(v * 31.0f), 0, 31); };
	return color_material(channel(p_color.r), channel(p_color.g), channel(p_color.b));
}

} // namespace

void DensityShape::changed() {
	version++;
	emit_changed();
}

void DensityShape::set_size(const Vector3i &p_size) {
	size = Vector3i(std::clamp(p_size.x, 1, MAX_SIZE), std::clamp(p_size.y, 1, MAX_SIZE), std::clamp(p_size.z, 1, MAX_SIZE));
	for (std::vector<float> &k : keys) {
		k.assign(count(), -DENSITY_CLAMP);
	}
	materials.assign(count(), MAT_STONE);
	changed();
}

void DensityShape::set_keys(const Array &p_keys) {
	keys.clear();
	for (int64_t k = 0; k < p_keys.size(); k++) {
		const PackedByteArray bytes = p_keys[k];
		std::vector<float> values(count(), -DENSITY_CLAMP);
		const int64_t n = std::min<int64_t>(bytes.size(), (int64_t)values.size());
		for (int64_t i = 0; i < n; i++) {
			values[i] = (int8_t)bytes[i] * DENSITY_STEP;
		}
		keys.push_back(std::move(values));
	}
	changed();
}

Array DensityShape::get_keys() const {
	Array result;
	for (const std::vector<float> &k : keys) {
		PackedByteArray bytes;
		bytes.resize((int64_t)k.size());
		uint8_t *w = bytes.ptrw();
		for (size_t i = 0; i < k.size(); i++) {
			w[i] = (uint8_t)quantize_density(k[i]);
		}
		result.push_back(bytes);
	}
	return result;
}

void DensityShape::set_materials(const PackedByteArray &p_bytes) {
	materials.assign(count(), MAT_STONE);
	const int64_t n = std::min<int64_t>(p_bytes.size() / 2, (int64_t)materials.size());
	for (int64_t i = 0; i < n; i++) {
		materials[i] = (MaterialId)(p_bytes[2 * i] | p_bytes[2 * i + 1] << 8);
	}
	changed();
}

PackedByteArray DensityShape::get_materials() const {
	PackedByteArray bytes;
	bytes.resize((int64_t)materials.size() * 2);
	uint8_t *w = bytes.ptrw();
	for (size_t i = 0; i < materials.size(); i++) {
		w[2 * i] = (uint8_t)(materials[i] & 0xFF);
		w[2 * i + 1] = (uint8_t)(materials[i] >> 8);
	}
	return bytes;
}

Ref<DensityShape> DensityShape::make_sphere(double p_radius, int p_material, int p_stripe_material, double p_voxel_size) {
	Ref<DensityShape> shape;
	shape.instantiate();
	const double vs = std::max(p_voxel_size, 0.01);
	const int n = std::min((int)std::ceil(2.0 * p_radius / vs) + 5, MAX_SIZE);
	shape->voxel_size = vs;
	shape->size = Vector3i(n, n, n);
	shape->origin = Vector3(1, 1, 1) * (float)(-(n - 1) * 0.5 * vs);
	shape->keys.assign(1, std::vector<float>(shape->count()));
	shape->materials.assign(shape->count(), (MaterialId)std::max(p_material, 0));
	for (int z = 0; z < n; z++) {
		for (int y = 0; y < n; y++) {
			for (int x = 0; x < n; x++) {
				const Vector3 p = shape->origin + Vector3((float)x, (float)y, (float)z) * (float)vs;
				const size_t i = shape->index(x, y, z);
				shape->keys[0][i] = std::clamp((float)((p_radius - p.length()) / vs), -DENSITY_CLAMP, DENSITY_CLAMP);
				if (p_stripe_material >= 0 && std::abs(p.y) < 0.22 * p_radius) {
					shape->materials[i] = (MaterialId)p_stripe_material;
				}
			}
		}
	}
	shape->changed();
	return shape;
}

void DensityShape::bake_meshes(const Array &p_poses, const Array &p_colors, double p_voxel_size, double p_margin) {
	ERR_FAIL_COND_MSG(p_poses.is_empty(), "DensityShape.bake_meshes(): no poses.");
	Vector3 lo(1e30f, 1e30f, 1e30f), hi(-1e30f, -1e30f, -1e30f);
	for (int64_t k = 0; k < p_poses.size(); k++) {
		const PackedVector3Array tris = p_poses[k];
		for (int64_t i = 0; i < tris.size(); i++) {
			lo = lo.min(tris[i]);
			hi = hi.max(tris[i]);
		}
	}
	ERR_FAIL_COND_MSG(lo.x > hi.x, "DensityShape.bake_meshes(): the poses have no triangles.");
	const Vector3 margin = Vector3(1, 1, 1) * (float)p_margin;
	lo -= margin;
	hi += margin;
	const Vector3 extent = hi - lo;
	double vs = std::max(p_voxel_size, 0.01);
	vs = std::max(vs, (double)std::max({ extent.x, extent.y, extent.z }) / (MAX_SIZE - 1));
	voxel_size = vs;
	origin = lo;
	size = Vector3i((int)std::ceil(extent.x / vs) + 1, (int)std::ceil(extent.y / vs) + 1, (int)std::ceil(extent.z / vs) + 1);
	size = size.min(Vector3i(MAX_SIZE, MAX_SIZE, MAX_SIZE));
	keys.assign(p_poses.size(), std::vector<float>(count()));
	materials.assign(count(), MAT_STONE);
	for (int64_t k = 0; k < p_poses.size(); k++) {
		const PackedVector3Array tris = p_poses[k];
		const PackedColorArray colors = k < p_colors.size() ? (PackedColorArray)p_colors[k] : PackedColorArray();
		const bool has_colors = colors.size() == tris.size();
		const Vector3 *t = tris.ptr();
		const int64_t tri_count = tris.size() / 3;
		for (int z = 0; z < size.z; z++) {
			for (int y = 0; y < size.y; y++) {
				for (int x = 0; x < size.x; x++) {
					const Vector3 p = origin + Vector3((float)x, (float)y, (float)z) * (float)vs;
					float best = 1e30f, winding = 0.0f;
					int64_t best_tri = -1;
					Vector3 best_bary;
					for (int64_t j = 0; j < tri_count; j++) {
						const Vector3 &a = t[3 * j], &b = t[3 * j + 1], &c = t[3 * j + 2];
						winding += solid_angle(p, a, b, c);
						const Vector3 bary = closest_on_triangle(p, a, b, c);
						const float d2 = (a * bary.x + b * bary.y + c * bary.z - p).length_squared();
						if (d2 < best) {
							best = d2;
							best_tri = j;
							best_bary = bary;
						}
					}
					// Inside: the surface winds round the point (robust to
					// small holes, either triangle winding).
					const bool inside = std::abs(winding) / (4.0f * (float)Math_PI) > 0.5f;
					const float d = std::sqrt(best) / (float)vs;
					const size_t i = index(x, y, z);
					keys[k][i] = std::clamp(inside ? d : -d, -DENSITY_CLAMP, DENSITY_CLAMP);
					if (k == 0 && has_colors && best_tri >= 0) {
						const Color c = colors[3 * best_tri] * best_bary.x + colors[3 * best_tri + 1] * best_bary.y + colors[3 * best_tri + 2] * best_bary.z;
						materials[i] = material_of_color(c);
					}
				}
			}
		}
	}
	edited = true;
	changed();
}

Ref<DensityShape> DensityShape::duplicate_shape() const {
	Ref<DensityShape> copy;
	copy.instantiate();
	copy->size = size;
	copy->origin = origin;
	copy->voxel_size = voxel_size;
	copy->keys = keys;
	copy->materials = materials;
	copy->edited = edited;
	return copy;
}

Dictionary DensityShape::to_dict() const {
	Dictionary d;
	d["size"] = size;
	d["origin"] = origin;
	d["voxel_size"] = voxel_size;
	Array packed;
	for (const Variant &k : get_keys()) {
		packed.push_back(((PackedByteArray)k).compress(ZSTD));
	}
	d["keys"] = packed;
	d["materials"] = get_materials().compress(ZSTD);
	return d;
}

Ref<DensityShape> DensityShape::from_dict(const Dictionary &p_dict) {
	Ref<DensityShape> shape;
	shape.instantiate();
	shape->set_size(p_dict.get("size", Vector3i(1, 1, 1)));
	shape->origin = p_dict.get("origin", Vector3());
	shape->voxel_size = p_dict.get("voxel_size", 1.0);
	const int64_t n = (int64_t)shape->count();
	Array keys;
	const Array packed = p_dict.get("keys", Array());
	for (const Variant &k : packed) {
		keys.push_back(((PackedByteArray)k).decompress(n, ZSTD));
	}
	shape->set_keys(keys);
	shape->set_materials(((PackedByteArray)p_dict.get("materials", PackedByteArray())).decompress(n * 2, ZSTD));
	shape->edited = true;
	return shape;
}

// --- Sampling ------------------------------------------------------------------------

float DensityShape::sample_key(int p_key, const Vector3 &p_grid) const {
	const std::vector<float> &v = keys[p_key];
	const Vector3 hi((float)(size.x - 1), (float)(size.y - 1), (float)(size.z - 1));
	const Vector3 c = p_grid.clamp(Vector3(), hi);
	const int x0 = std::min((int)c.x, size.x - 1), y0 = std::min((int)c.y, size.y - 1), z0 = std::min((int)c.z, size.z - 1);
	const int x1 = std::min(x0 + 1, size.x - 1), y1 = std::min(y0 + 1, size.y - 1), z1 = std::min(z0 + 1, size.z - 1);
	const float fx = c.x - x0, fy = c.y - y0, fz = c.z - z0;
	auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
	const float d = lerp(lerp(lerp(v[index(x0, y0, z0)], v[index(x1, y0, z0)], fx), lerp(v[index(x0, y1, z0)], v[index(x1, y1, z0)], fx), fy),
			lerp(lerp(v[index(x0, y0, z1)], v[index(x1, y0, z1)], fx), lerp(v[index(x0, y1, z1)], v[index(x1, y1, z1)], fx), fy), fz);
	return d - (p_grid - c).length();
}

void DensityShape::pose_keys(double p_pose, int &r_a, int &r_b, float &r_t) const {
	const int last = std::max((int)keys.size() - 1, 0);
	const double p = std::clamp(p_pose, 0.0, (double)last);
	r_a = std::min((int)p, last);
	r_b = std::min(r_a + 1, last);
	r_t = r_b == r_a ? 0.0f : (float)(p - r_a);
}

float DensityShape::sample_pose(const Vector3 &p_grid, double p_pose) const {
	if (keys.empty()) {
		return -DENSITY_CLAMP;
	}
	int a, b;
	float t;
	pose_keys(p_pose, a, b, t);
	const float da = sample_key(a, p_grid);
	return t > 0.0f ? da + (sample_key(b, p_grid) - da) * t : da;
}

double DensityShape::get_density(const Vector3 &p_local, double p_pose) const {
	return sample_pose((p_local - origin) / (float)voxel_size, p_pose) * voxel_size;
}

double DensityShape::get_material_density_at(const Vector3 &p_local) const {
	const int m = get_material_at(p_local);
	return m >= 0 ? material_physics((MaterialId)m).density : 1.0;
}

Vector3 DensityShape::get_normal(const Vector3 &p_local, double p_pose) const {
	const Vector3 g = (p_local - origin) / (float)voxel_size;
	const float h = 0.5f;
	const Vector3 grad(sample_pose(g + Vector3(h, 0, 0), p_pose) - sample_pose(g - Vector3(h, 0, 0), p_pose),
			sample_pose(g + Vector3(0, h, 0), p_pose) - sample_pose(g - Vector3(0, h, 0), p_pose),
			sample_pose(g + Vector3(0, 0, h), p_pose) - sample_pose(g - Vector3(0, 0, h), p_pose));
	return grad.length_squared() > 1e-12f ? -grad.normalized() : Vector3(0, 1, 0);
}

int DensityShape::get_material_at(const Vector3 &p_local) const {
	if (materials.empty()) {
		return -1;
	}
	const Vector3 g = (p_local - origin) / (float)voxel_size;
	const int x = std::clamp((int)std::lround(g.x), 0, size.x - 1), y = std::clamp((int)std::lround(g.y), 0, size.y - 1),
			  z = std::clamp((int)std::lround(g.z), 0, size.z - 1);
	return materials[index(x, y, z)];
}

// --- Sculpting -----------------------------------------------------------------------

void DensityShape::grow(Vector3i p_lo, Vector3i p_hi) {
	Vector3i nlo, nhi = size - Vector3i(1, 1, 1);
	for (int a = 0; a < 3; a++) {
		// Never drop what's there: grow only as far as MAX_SIZE allows.
		const int room = MAX_SIZE - size[a];
		const int down = std::min(std::max(-p_lo[a], 0), room);
		const int up = std::min(std::max(p_hi[a] - (size[a] - 1), 0), room - down);
		nlo[a] = -down;
		nhi[a] = size[a] - 1 + up;
	}
	if (nlo == Vector3i() && nhi == size - Vector3i(1, 1, 1)) {
		return;
	}
	const Vector3i nsize = nhi - nlo + Vector3i(1, 1, 1);
	auto new_index = [&](int x, int y, int z) { return ((size_t)z * nsize.y + y) * nsize.x + x; };
	std::vector<std::vector<float>> new_keys(keys.size(), std::vector<float>((size_t)nsize.x * nsize.y * nsize.z, -DENSITY_CLAMP));
	std::vector<MaterialId> new_materials((size_t)nsize.x * nsize.y * nsize.z);
	for (int z = 0; z < nsize.z; z++) {
		for (int y = 0; y < nsize.y; y++) {
			for (int x = 0; x < nsize.x; x++) {
				const Vector3i old = Vector3i(x, y, z) + nlo;
				const Vector3i c = old.clamp(Vector3i(), size - Vector3i(1, 1, 1));
				// New air keeps the material of the edge it faces.
				new_materials[new_index(x, y, z)] = materials[index(c.x, c.y, c.z)];
				if (old == c) {
					for (size_t k = 0; k < keys.size(); k++) {
						new_keys[k][new_index(x, y, z)] = keys[k][index(c.x, c.y, c.z)];
					}
				}
			}
		}
	}
	keys = std::move(new_keys);
	materials = std::move(new_materials);
	origin += Vector3(nlo) * (float)voxel_size;
	size = nsize;
}

void DensityShape::coarsen() {
	// Points 0, 2, 4... of the old grid, one more if needed so the last old
	// point is still covered (sampled past the edge as sample_key() does).
	const Vector3i nsize = (size - Vector3i(1, 1, 1)) / 2 + Vector3i(1, 1, 1) + Vector3i(size.x % 2 == 0, size.y % 2 == 0, size.z % 2 == 0);
	const Vector3i n = nsize.clamp(Vector3i(1, 1, 1), Vector3i(MAX_SIZE, MAX_SIZE, MAX_SIZE));
	const size_t ncount = (size_t)n.x * n.y * n.z;
	auto coarse_keys = [&](const std::vector<std::vector<float>> &p_keys) {
		std::vector<std::vector<float>> result(p_keys.size(), std::vector<float>(ncount));
		for (size_t k = 0; k < p_keys.size(); k++) {
			// sample_key() reads `keys`: sample from the given set.
			const std::vector<float> &v = p_keys[k];
			for (int z = 0; z < n.z; z++) {
				for (int y = 0; y < n.y; y++) {
					for (int x = 0; x < n.x; x++) {
						const Vector3i o(x * 2, y * 2, z * 2);
						const Vector3i c = o.clamp(Vector3i(), size - Vector3i(1, 1, 1));
						// Distances in the new (twice as big) voxels.
						const float d = v[index(c.x, c.y, c.z)] - Vector3(o - c).length();
						result[k][((size_t)z * n.y + y) * n.x + x] = std::clamp(d * 0.5f, -DENSITY_CLAMP, DENSITY_CLAMP);
					}
				}
			}
			density_redistance(result[k], n);
		}
		return result;
	};
	auto coarse_materials = [&](const std::vector<MaterialId> &p_materials) {
		std::vector<MaterialId> result(ncount);
		for (int z = 0; z < n.z; z++) {
			for (int y = 0; y < n.y; y++) {
				for (int x = 0; x < n.x; x++) {
					const Vector3i c = Vector3i(x * 2, y * 2, z * 2).clamp(Vector3i(), size - Vector3i(1, 1, 1));
					result[((size_t)z * n.y + y) * n.x + x] = p_materials[index(c.x, c.y, c.z)];
				}
			}
		}
		return result;
	};
	if (!grab_keys.empty() && grab_keys.size() == keys.size() && grab_materials.size() == materials.size()) {
		grab_keys = coarse_keys(grab_keys);
		grab_materials = coarse_materials(grab_materials);
	}
	keys = coarse_keys(keys);
	materials = coarse_materials(materials);
	size = n;
	voxel_size *= 2.0;
}

void DensityShape::fit(const Vector3 &p_lo, const Vector3 &p_hi, bool p_coarsen) {
	bool coarsened = false;
	while (p_coarsen && voxel_size * 2.0 <= MAX_VOXEL_SIZE) {
		// The box with the grid, in points at the current voxel size.
		const Vector3 grid_hi = origin + Vector3(size - Vector3i(1, 1, 1)) * (float)voxel_size;
		const Vector3 extent = p_hi.max(grid_hi) - p_lo.min(origin);
		const float points = std::max({ extent.x, extent.y, extent.z }) / (float)voxel_size + 2.0f;
		if (points <= (float)MAX_SIZE) {
			break;
		}
		coarsen();
		coarsened = true;
	}
	const float vs = (float)voxel_size;
	Vector3i lo, hi;
	for (int a = 0; a < 3; a++) {
		lo[a] = (int)std::floor((p_lo[a] - origin[a]) / vs);
		hi[a] = (int)std::ceil((p_hi[a] - origin[a]) / vs);
	}
	grow(lo, hi);
	if (coarsened) {
		changed(); // (even if the edit that needed the room then changes nothing)
	}
}

void DensityShape::repair(std::vector<float> &r_after, const std::vector<float> &p_before, const Vector3i &p_lo, const Vector3i &p_hi) const {
	// A changed point moves distances up to the clamp away (the edit box
	// grown by M); those are worked out from the surface up to the clamp
	// beyond them (grown by M again). The rest of the grid can't change, so
	// repairing all of it every brush call (as it used to) was wasted work.
	const int M = (int)std::ceil(DENSITY_CLAMP) + 1;
	const Vector3i last = size - Vector3i(1, 1, 1), m(M, M, M);
	const Vector3i elo = (p_lo - m).clamp(Vector3i(), last), ehi = (p_hi + m).clamp(Vector3i(), last);
	const Vector3i rlo = (elo - m).clamp(Vector3i(), last), rhi = (ehi + m).clamp(Vector3i(), last);
	if (elo.x > ehi.x || elo.y > ehi.y || elo.z > ehi.z) {
		return;
	}
	const Vector3i n = rhi - rlo + Vector3i(1, 1, 1);
	auto sub_index = [&](int x, int y, int z) { return ((size_t)(z - rlo.z) * n.y + (y - rlo.y)) * n.x + (x - rlo.x); };
	std::vector<float> repaired((size_t)n.x * n.y * n.z);
	for (int z = rlo.z; z <= rhi.z; z++) {
		for (int y = rlo.y; y <= rhi.y; y++) {
			std::memcpy(&repaired[sub_index(rlo.x, y, z)], &r_after[index(rlo.x, y, z)], sizeof(float) * n.x);
		}
	}
	density_redistance(repaired, n);
	// Points the edit changed take the repaired value; the rest keep theirs
	// unless the repair finds the surface nearer, or on their other side.
	// Re-rounding them every call would make the whole shape drift (the
	// repair's distances are only within ~8%).
	const float tolerance = 2.0f * DENSITY_STEP;
	for (int z = elo.z; z <= ehi.z; z++) {
		for (int y = elo.y; y <= ehi.y; y++) {
			for (int x = elo.x; x <= ehi.x; x++) {
				const size_t i = index(x, y, z);
				const bool changed = std::abs(r_after[i] - p_before[i]) > 1e-5f;
				const float r = repaired[sub_index(x, y, z)], b = p_before[i];
				r_after[i] = changed || (r > 0.0f) != (b > 0.0f) || std::abs(r) < std::abs(b) - tolerance ? r : b;
			}
		}
	}
}

bool DensityShape::apply_brush(const Vector3 &p_center, double p_radius, double p_strength, int p_mode, int p_material, int p_key) {
	if (keys.empty() || p_radius <= 0.0 || p_mode < 0 || p_mode > 6 || (p_mode == 4 && p_material < 0)) {
		return false;
	}
	enum { ADD, SUBTRACT, SMOOTH, FLATTEN, PAINT, INFLATE, PINCH };
	if (p_mode == ADD) {
		// Room for what it builds, plus a little air: past MAX_SIZE points a
		// side, coarser voxels rather than a wall it can't build past.
		const Vector3 room = Vector3(1, 1, 1) * (float)(p_radius + 2.0 * voxel_size);
		fit(p_center - room, p_center + room, true);
	}
	const float vs = (float)voxel_size;
	const float r = (float)p_radius / vs, s = (float)p_strength / vs, C = DENSITY_CLAMP;
	const Vector3 c = (p_center - origin) / vs;
	const bool sphere_mode = p_mode == ADD || p_mode == SUBTRACT;
	const float reach = sphere_mode ? r + C : r + 1.0f;
	Vector3i lo, hi;
	for (int a = 0; a < 3; a++) {
		lo[a] = std::max((int)std::floor(c[a] - reach), 0);
		hi[a] = std::min((int)std::ceil(c[a] + reach), size[a] - 1);
		if (lo[a] > hi[a]) {
			return false;
		}
	}
	const float blend = std::clamp(s / r, 0.0f, 1.0f);
	const std::vector<MaterialId> materials_before = materials;
	bool any = false;
	for (int k = 0; k < (int)keys.size(); k++) {
		if (p_key >= 0 && k != p_key) {
			continue;
		}
		const std::vector<float> before = keys[k];
		std::vector<float> &after = keys[k];
		auto at = [&](int x, int y, int z) {
			return before[index(std::clamp(x, 0, size.x - 1), std::clamp(y, 0, size.y - 1), std::clamp(z, 0, size.z - 1))];
		};
		Vector3 plane_normal;
		if (p_mode == FLATTEN || p_mode == PINCH) {
			plane_normal = get_normal(p_center, k);
		}
		for (int z = lo.z; z <= hi.z; z++) {
			for (int y = lo.y; y <= hi.y; y++) {
				for (int x = lo.x; x <= hi.x; x++) {
					const Vector3 p((float)x, (float)y, (float)z);
					const size_t i = index(x, y, z);
					const float v = before[i];
					const float dist = (p - c).length();
					switch (p_mode) {
						case ADD:
							after[i] = v + (std::max(v, std::min(r - dist, C)) - v) * blend;
							if (p_material >= 0 && after[i] > v + 1e-4f && after[i] > -2.0f) {
								materials[i] = (MaterialId)p_material;
							}
							break;
						case SUBTRACT:
							after[i] = v + (std::min(v, std::max(dist - r, -C)) - v) * blend;
							break;
						case SMOOTH: {
							const float avg = (v + at(x - 1, y, z) + at(x + 1, y, z) + at(x, y - 1, z) + at(x, y + 1, z) + at(x, y, z - 1) + at(x, y, z + 1)) / 7.0f;
							after[i] = v + (avg - v) * std::min(s * falloff(dist / r), 1.0f);
						} break;
						case FLATTEN: {
							const float target = std::clamp(-(p - c).dot(plane_normal), -C, C);
							after[i] = v + (target - v) * std::min(s * falloff(dist / r), 1.0f);
						} break;
						case INFLATE:
							if (std::abs(v) < C) {
								after[i] = std::clamp(v + s * falloff(dist / r), -C, C);
							}
							break;
						case PINCH: {
							const float f = falloff(dist / r);
							if (f > 0.0f) {
								const Vector3 d = p - c;
								const Vector3 along = plane_normal * d.dot(plane_normal);
								const Vector3 q = c + along + (d - along) * (1.0f + std::clamp(s / r, -0.3f, 0.3f) * f);
								after[i] = [&]() {
									// From the key as it was before this brush.
									const Vector3 hi((float)(size.x - 1), (float)(size.y - 1), (float)(size.z - 1));
									const Vector3 cq = q.clamp(Vector3(), hi);
									const int x0 = (int)cq.x, y0 = (int)cq.y, z0 = (int)cq.z;
									const int x1 = std::min(x0 + 1, size.x - 1), y1 = std::min(y0 + 1, size.y - 1), z1 = std::min(z0 + 1, size.z - 1);
									const float fx = cq.x - x0, fy = cq.y - y0, fz = cq.z - z0;
									auto b = [&](int xx, int yy, int zz) { return before[index(xx, yy, zz)]; };
									auto lerp = [](float a0, float a1, float t) { return a0 + (a1 - a0) * t; };
									return lerp(lerp(lerp(b(x0, y0, z0), b(x1, y0, z0), fx), lerp(b(x0, y1, z0), b(x1, y1, z0), fx), fy),
											lerp(lerp(b(x0, y0, z1), b(x1, y0, z1), fx), lerp(b(x0, y1, z1), b(x1, y1, z1), fx), fy), fz);
								}();
							}
						} break;
						case PAINT:
							if (dist <= r && std::abs(v) < 2.5f) {
								materials[i] = (MaterialId)p_material;
							}
							break;
					}
					if ((p_mode == SMOOTH || p_mode == FLATTEN || p_mode == INFLATE) && v <= 0.0f && after[i] > 0.0f) {
						// Raised from air: the most solid neighbour's material.
						const Vector3i ns[6] = { { x - 1, y, z }, { x + 1, y, z }, { x, y - 1, z }, { x, y + 1, z }, { x, y, z - 1 }, { x, y, z + 1 } };
						float most = v;
						for (const Vector3i &q : ns) {
							const Vector3i cq = q.clamp(Vector3i(), size - Vector3i(1, 1, 1));
							if (before[index(cq.x, cq.y, cq.z)] > most) {
								most = before[index(cq.x, cq.y, cq.z)];
								materials[i] = materials_before[index(cq.x, cq.y, cq.z)];
							}
						}
					}
					any |= after[i] != v;
				}
			}
		}
		if (p_mode != PAINT) {
			// Repair the distances as the terrain does (commit_edit()), around
			// the brush only.
			repair(after, before, lo, hi);
		}
	}
	any |= materials != materials_before;
	if (any) {
		edited = true;
		changed();
	}
	return any;
}

bool DensityShape::paint_color(const Vector3 &p_center, double p_radius, const Color &p_color, double p_opacity, double p_hardness) {
	if (keys.empty() || p_radius <= 0.0) {
		return false;
	}
	const float vs = (float)voxel_size, r = (float)p_radius / vs;
	const Vector3 c = (p_center - origin) / vs;
	const float hard = std::clamp((float)p_hardness, 0.0f, 0.999f);
	const float opacity = std::clamp((float)p_opacity, 0.0f, 1.0f);
	// Only the points within the brush (not the whole grid, every frame).
	Vector3i lo, hi;
	for (int a = 0; a < 3; a++) {
		lo[a] = std::max((int)std::floor(c[a] - r), 0);
		hi[a] = std::min((int)std::ceil(c[a] + r), size[a] - 1);
		if (lo[a] > hi[a]) {
			return false;
		}
	}
	// What the brush lays down, in the 5-bit steps a color material has.
	const float target[3] = { (float)std::lround(std::clamp(p_color.r, 0.0f, 1.0f) * 31.0f), (float)std::lround(std::clamp(p_color.g, 0.0f, 1.0f) * 31.0f),
		(float)std::lround(std::clamp(p_color.b, 0.0f, 1.0f) * 31.0f) };
	const SculptTerrain *terrain = SculptTerrain::get_singleton();
	paint_serial++;
	bool any = false;
	for (int z = lo.z; z <= hi.z; z++) {
		for (int y = lo.y; y <= hi.y; y++) {
			for (int x = lo.x; x <= hi.x; x++) {
				const float dist = (Vector3((float)x, (float)y, (float)z) - c).length();
				if (dist > r) {
					continue;
				}
				const size_t i = index(x, y, z);
				// The skin of any pose (the surface drawn is the pose's keys,
				// not just the first one's: else other poses show the old colour).
				float nearest = DENSITY_CLAMP;
				for (const std::vector<float> &k : keys) {
					nearest = std::min(nearest, std::abs(k[i]));
				}
				if (nearest > 2.5f) {
					continue;
				}
				const float cover = dist <= hard * r ? 1.0f : 1.0f - (dist - hard * r) / (r - hard * r);
				const float t = opacity * cover;
				if (t <= 0.0f) {
					continue;
				}
				const MaterialId m = materials[i];
				// A block starts from its texture's average colour, as on the terrain.
				const Color base = terrain != nullptr ? terrain->get_material_color(m)
													  : (is_color_material(m) ? Color(((m >> 10) & 31) / 31.0f, ((m >> 5) & 31) / 31.0f, (m & 31) / 31.0f) : Color(0.5f, 0.5f, 0.5f));
				const float from[3] = { base.r * 31.0f, base.g * 31.0f, base.b * 31.0f };
				// Brushing blends a little every frame. Rounded plainly to 5
				// bits, a small step rounds away every time and the colour stops
				// short of the paint, the old one showing through; a dither
				// makes the steps add up on average.
				const float d = dither(x, y, z, paint_serial);
				int channel[3];
				for (int a = 0; a < 3; a++) {
					channel[a] = std::clamp((int)std::floor(from[a] + (target[a] - from[a]) * t + d), 0, 31);
				}
				const MaterialId next = color_material(channel[0], channel[1], channel[2]);
				if (next != m) {
					materials[i] = next;
					any = true;
				}
			}
		}
	}
	if (any) {
		edited = true;
		changed();
	}
	return any;
}

// --- For bodies and drawing ----------------------------------------------------------

const std::vector<Vector4> &DensityShape::probes(int p_key) const {
	if (probe_version != version || probe_cache.size() != keys.size()) {
		probe_cache.assign(keys.size(), {});
		probe_version = version;
	}
	const int key = std::clamp(p_key, 0, std::max((int)keys.size() - 1, 0));
	std::vector<Vector4> &list = probe_cache[key];
	if (!list.empty() || keys.empty()) {
		return list;
	}
	const std::vector<Vector4> skin = skin_points(key);
	// At most ~256: every n-th (in grid order, which spreads them evenly enough).
	const size_t step = std::max<size_t>(1, (skin.size() + 255) / 256);
	for (size_t i = 0; i < skin.size(); i += step) {
		list.push_back(skin[i]);
	}
	return list;
}

std::vector<Vector4> DensityShape::skin_points(int p_key) const {
	std::vector<Vector4> skin;
	if (keys.empty()) {
		return skin;
	}
	const int key = std::clamp(p_key, 0, (int)keys.size() - 1);
	// Solid points next to air (the skin), each a sphere as big as its
	// distance to the surface, so together they stand in for the surface.
	const std::vector<float> &v = keys[key];
	auto solid = [&](int x, int y, int z) {
		return x >= 0 && y >= 0 && z >= 0 && x < size.x && y < size.y && z < size.z && v[index(x, y, z)] > 0.0f;
	};
	for (int z = 0; z < size.z; z++) {
		for (int y = 0; y < size.y; y++) {
			for (int x = 0; x < size.x; x++) {
				const float d = v[index(x, y, z)];
				if (d <= 0.0f) {
					continue;
				}
				if (solid(x - 1, y, z) && solid(x + 1, y, z) && solid(x, y - 1, z) && solid(x, y + 1, z) && solid(x, y, z - 1) && solid(x, y, z + 1)) {
					continue;
				}
				const Vector3 p = origin + Vector3((float)x, (float)y, (float)z) * (float)voxel_size;
				skin.push_back(Vector4(p.x, p.y, p.z, std::max(d, 0.5f) * (float)voxel_size));
			}
		}
	}
	return skin;
}

void DensityShape::mass_properties_now(Vector3 &r_centroid, Basis &r_inertia) const {
	const float vs = (float)voxel_size;
	std::vector<Vector3> points;
	std::vector<float> weights; // each point's material density
	if (!keys.empty()) {
		for (int z = 0; z < size.z; z++) {
			for (int y = 0; y < size.y; y++) {
				for (int x = 0; x < size.x; x++) {
					if (keys[0][index(x, y, z)] > 0.0f) {
						points.push_back(origin + Vector3((float)x, (float)y, (float)z) * vs);
						weights.push_back(material_physics(materials[index(x, y, z)]).density);
					}
				}
			}
		}
	}
	if (points.empty()) {
		r_centroid = origin + Vector3(size - Vector3i(1, 1, 1)) * (vs * 0.5f);
		r_inertia = Basis().scaled(Vector3(1, 1, 1) * vs * vs);
		return;
	}
	// Heavier materials pull the centre of mass their way.
	Vector3 sum;
	float total = 0.0f;
	for (size_t i = 0; i < points.size(); i++) {
		sum += points[i] * weights[i];
		total += weights[i];
	}
	r_centroid = sum / total;
	Basis inertia = Basis(Vector3(), Vector3(), Vector3());
	for (size_t i = 0; i < points.size(); i++) {
		const Vector3 &p = points[i];
		const float w = weights[i] / total;
		const Vector3 d = p - r_centroid;
		for (int a = 0; a < 3; a++) {
			for (int b = 0; b < 3; b++) {
				inertia.rows[a][b] += w * ((a == b ? d.length_squared() + vs * vs / 6.0f : 0.0f) - d[a] * d[b]);
			}
		}
	}
	r_inertia = inertia;
}

void DensityShape::fill_slice(int p_key, int p_z, uint8_t *r_texels) const {
	for (int y = 0; y < MAX_SIZE; y++) {
		for (int x = 0; x < MAX_SIZE; x++) {
			uint8_t *t = r_texels + ((size_t)y * MAX_SIZE + x) * 3;
			if (p_key < (int)keys.size() && x < size.x && y < size.y && p_z < size.z) {
				const size_t i = index(x, y, p_z);
				t[0] = (uint8_t)(quantize_density(keys[p_key][i]) + 128);
				render_bytes(materials[i], t + 1);
			} else {
				t[0] = (uint8_t)(128 - DENSITY_LIMIT);
				t[1] = (uint8_t)MAT_STONE;
				t[2] = 0;
			}
		}
	}
}

void DensityShape::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_size", "size"), &DensityShape::set_size);
	ClassDB::bind_method(D_METHOD("get_size"), &DensityShape::get_size);
	ClassDB::bind_method(D_METHOD("set_origin", "origin"), &DensityShape::set_origin);
	ClassDB::bind_method(D_METHOD("get_origin"), &DensityShape::get_origin);
	ClassDB::bind_method(D_METHOD("set_voxel_size", "size"), &DensityShape::set_voxel_size);
	ClassDB::bind_method(D_METHOD("get_voxel_size"), &DensityShape::get_voxel_size);
	ClassDB::bind_method(D_METHOD("set_keys", "keys"), &DensityShape::set_keys);
	ClassDB::bind_method(D_METHOD("get_keys"), &DensityShape::get_keys);
	ClassDB::bind_method(D_METHOD("set_materials", "bytes"), &DensityShape::set_materials);
	ClassDB::bind_method(D_METHOD("get_materials"), &DensityShape::get_materials);
	ClassDB::bind_method(D_METHOD("set_edited", "edited"), &DensityShape::set_edited);
	ClassDB::bind_method(D_METHOD("is_edited"), &DensityShape::is_edited);
	ClassDB::bind_method(D_METHOD("get_key_count"), &DensityShape::get_key_count);
	ClassDB::bind_static_method("DensityShape", D_METHOD("make_sphere", "radius", "material", "stripe_material", "voxel_size"), &DensityShape::make_sphere, DEFVAL(-1), DEFVAL(1.0));
	ClassDB::bind_method(D_METHOD("bake_meshes", "poses", "colors", "voxel_size", "margin"), &DensityShape::bake_meshes);
	ClassDB::bind_method(D_METHOD("duplicate_shape"), &DensityShape::duplicate_shape);
	ClassDB::bind_method(D_METHOD("to_dict"), &DensityShape::to_dict);
	ClassDB::bind_static_method("DensityShape", D_METHOD("from_dict", "dict"), &DensityShape::from_dict);
	ClassDB::bind_method(D_METHOD("get_density", "position", "pose"), &DensityShape::get_density, DEFVAL(0.0));
	ClassDB::bind_method(D_METHOD("get_normal", "position", "pose"), &DensityShape::get_normal, DEFVAL(0.0));
	ClassDB::bind_method(D_METHOD("get_material_at", "position"), &DensityShape::get_material_at);
	ClassDB::bind_method(D_METHOD("apply_brush", "center", "radius", "strength", "mode", "material", "key"), &DensityShape::apply_brush, DEFVAL(-1), DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("paint_color", "center", "radius", "color", "opacity", "hardness"), &DensityShape::paint_color);
	ClassDB::bind_method(D_METHOD("get_version"), &DensityShape::get_version);
	ClassDB::bind_method(D_METHOD("get_volume"), &DensityShape::get_volume);
	ClassDB::bind_method(D_METHOD("get_mass"), &DensityShape::get_mass);
	ClassDB::bind_method(D_METHOD("split_parts", "min_voxels"), &DensityShape::split_parts, DEFVAL(4));
	ClassDB::bind_static_method("DensityShape", D_METHOD("make_hull", "points", "material", "voxel_size"), &DensityShape::make_hull, DEFVAL(1.0));

	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3I, "size", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE), "set_size", "get_size");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "origin"), "set_origin", "get_origin");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "voxel_size", PROPERTY_HINT_RANGE, "0.01,16,0.01"), "set_voxel_size", "get_voxel_size");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "keys", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE), "set_keys", "get_keys");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_BYTE_ARRAY, "materials", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE), "set_materials", "get_materials");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "edited", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE), "set_edited", "is_edited");
}

// --- Selections, grab and snake hook ---------------------------------------------

void DensityShape::init_grid(const Vector3i &p_size, const Vector3 &p_origin, double p_voxel_size, std::vector<float> &&p_density, std::vector<MaterialId> &&p_materials) {
	size = p_size;
	origin = p_origin;
	voxel_size = p_voxel_size;
	for (float &d : p_density) {
		d = std::clamp(d, -DENSITY_CLAMP, DENSITY_CLAMP);
	}
	keys.assign(1, std::move(p_density));
	materials = std::move(p_materials);
	density_redistance(keys[0], size);
	edited = true;
	changed();
}

double DensityShape::get_volume() const {
	if (keys.empty()) {
		return 0.0;
	}
	size_t solid = 0;
	for (float d : keys[0]) {
		solid += d > 0.0f;
	}
	return solid * voxel_size * voxel_size * voxel_size;
}

void DensityShape::begin_grab() {
	grab_keys = keys;
	grab_materials = materials;
	grab_has_box = false;
}

void DensityShape::end_grab() {
	grab_keys.clear();
	grab_materials.clear();
	grab_has_box = false;
}

bool DensityShape::warp(const std::vector<Vector3> &p_centers, const std::vector<Vector3> &p_deltas, double p_radius, bool p_grab, int p_key) {
	const int count = (int)std::min(p_centers.size(), p_deltas.size());
	if (keys.empty() || count == 0 || p_radius <= 0.0 || (p_grab && grab_keys.size() != keys.size())) {
		return false;
	}
	// Room for where it goes (the grid grows as for adding). A snake hook
	// may make the voxels coarser to fit; a grab doesn't (it works from the
	// shape as it began, which must keep its points).
	{
		Vector3 lo_l(1e30f, 1e30f, 1e30f), hi_l(-1e30f, -1e30f, -1e30f);
		for (int i = 0; i < count; i++) {
			const Vector3 a = p_centers[i], b = p_grab ? p_centers[i] + p_deltas[i] : p_centers[i] - p_deltas[i];
			lo_l = lo_l.min(a.min(b));
			hi_l = hi_l.max(a.max(b));
		}
		const Vector3 room = Vector3(1, 1, 1) * (float)(p_radius + 2.0 * voxel_size);
		const Vector3i old_size = size;
		const Vector3 old_origin = origin;
		fit(lo_l - room, hi_l + room, !p_grab);
		if (p_grab && size != old_size) {
			// The grab's saved shape must cover the same grid.
			const float vs = (float)voxel_size;
			const Vector3i shift = Vector3i(((old_origin - origin) / vs).round());
			std::vector<std::vector<float>> keys_now = std::move(keys);
			std::vector<MaterialId> materials_now = std::move(materials);
			Vector3i size_now = size;
			Vector3 origin_now = origin;
			keys = std::move(grab_keys);
			materials = std::move(grab_materials);
			size = old_size;
			origin = old_origin;
			grow(-shift, -shift + size_now - Vector3i(1, 1, 1));
			grab_keys = std::move(keys);
			grab_materials = std::move(materials);
			keys = std::move(keys_now);
			materials = std::move(materials_now);
			size = size_now;
			origin = origin_now;
		}
	}
	const float vs = (float)voxel_size, r = (float)p_radius / vs, C = DENSITY_CLAMP;
	std::vector<Vector3> centers(count), deltas(count);
	for (int i = 0; i < count; i++) {
		centers[i] = (p_centers[i] - origin) / vs;
		deltas[i] = p_deltas[i] / vs;
	}
	// The points rewritten (they used to be every point of the grid, each
	// frame): around each centre (a grab: where it began and where it is
	// now; a snake hook: this step), and for a grab, every box since it
	// began, so what it no longer covers goes back to how it was.
	Vector3 blo(1e30f, 1e30f, 1e30f), bhi(-1e30f, -1e30f, -1e30f);
	for (int i = 0; i < count; i++) {
		const Vector3 a = centers[i], b = p_grab ? centers[i] + deltas[i] : centers[i] - deltas[i];
		blo = blo.min(a.min(b) - Vector3(1, 1, 1) * (r + 1.0f));
		bhi = bhi.max(a.max(b) + Vector3(1, 1, 1) * (r + 1.0f));
	}
	if (p_grab) {
		// Kept in body space: the grid may grow (and move its origin).
		Vector3 lo_l = origin + blo * vs, hi_l = origin + bhi * vs;
		if (grab_has_box) {
			lo_l = lo_l.min(grab_box_lo);
			hi_l = hi_l.max(grab_box_hi);
		}
		grab_box_lo = lo_l;
		grab_box_hi = hi_l;
		grab_has_box = true;
		blo = (lo_l - origin) / vs;
		bhi = (hi_l - origin) / vs;
	}
	Vector3i lo, hi;
	for (int a = 0; a < 3; a++) {
		lo[a] = std::max((int)std::floor(blo[a]), 0);
		hi[a] = std::min((int)std::ceil(bhi[a]), size[a] - 1);
		if (lo[a] > hi[a]) {
			return false;
		}
	}
	bool any = false;
	std::vector<MaterialId> materials_copy;
	if (!p_grab) {
		materials_copy = materials;
	}
	const std::vector<MaterialId> &materials_from = p_grab ? grab_materials : materials_copy;
	for (int k = 0; k < (int)keys.size(); k++) {
		if (p_key >= 0 && k != p_key) {
			continue;
		}
		const std::vector<float> before = keys[k];
		const std::vector<float> &source_values = p_grab ? grab_keys[k] : before;
		auto source = [&](const Vector3 &q) {
			// Trilinear from the source grid (air beyond it).
			const Vector3 top((float)(size.x - 1), (float)(size.y - 1), (float)(size.z - 1));
			const Vector3 cq = q.clamp(Vector3(), top);
			const int x0 = (int)cq.x, y0 = (int)cq.y, z0 = (int)cq.z;
			const int x1 = std::min(x0 + 1, size.x - 1), y1 = std::min(y0 + 1, size.y - 1), z1 = std::min(z0 + 1, size.z - 1);
			const float fx = cq.x - x0, fy = cq.y - y0, fz = cq.z - z0;
			auto v = [&](int x, int y, int z) { return source_values[index(x, y, z)]; };
			auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
			const float d = lerp(lerp(lerp(v(x0, y0, z0), v(x1, y0, z0), fx), lerp(v(x0, y1, z0), v(x1, y1, z0), fx), fy),
					lerp(lerp(v(x0, y0, z1), v(x1, y0, z1), fx), lerp(v(x0, y1, z1), v(x1, y1, z1), fx), fy), fz);
			return d - (q - cq).length();
		};
		auto source_material = [&](const Vector3 &q) {
			const Vector3i i = Vector3i(q.round()).clamp(Vector3i(), size - Vector3i(1, 1, 1));
			return materials_from[index(i.x, i.y, i.z)];
		};
		std::vector<float> &after = keys[k];
		const bool write_materials = k == 0 || p_key >= 0;
		const float thickness = 0.4f * r;
		for (int z = lo.z; z <= hi.z; z++) {
			for (int y = lo.y; y <= hi.y; y++) {
				for (int x = lo.x; x <= hi.x; x++) {
					const Vector3 p((float)x, (float)y, (float)z);
					const size_t i = index(x, y, z);
					if (p_grab) {
						// As the terrain's grab: the material that lands here
						// came from p - delta * s, s = falloff(|that - c| / r);
						// the most solid of the roots (of every copy) wins.
						float value = -INFINITY;
						Vector3 best_from = p;
						for (int cc = 0; cc < count; cc++) {
							const Vector3 &c = centers[cc], &delta = deltas[cc];
							// Only material within r of the centre's path can reach
							// p; otherwise s = 0 is the only root.
							const float dd = delta.length_squared();
							const float tt = dd > 0.0f ? std::clamp((p - c).dot(delta) / dd, 0.0f, 1.0f) : 0.0f;
							if ((p - c - delta * tt).length_squared() >= r * r) {
								const float v = source(p);
								if (v > value) {
									value = v;
									best_from = p;
								}
								continue;
							}
							auto h = [&](float s) { return falloff((p - delta * s - c).length() / r) - s; };
							const int SCAN = 12;
							float s0 = 0.0f, h0 = h(0.0f);
							if (h0 <= 0.0f) {
								const float v = source(p);
								if (v > value) {
									value = v;
									best_from = p;
								}
							}
							for (int j = 1; j <= SCAN; j++) {
								const float s1 = (float)j / SCAN, h1 = h(s1);
								if ((h0 > 0.0f) != (h1 > 0.0f)) {
									float a = s0, b = s1;
									for (int it = 0; it < 8; it++) {
										const float m = (a + b) * 0.5f;
										((h(m) > 0.0f) == (h0 > 0.0f) ? a : b) = m;
									}
									const Vector3 q = p - delta * ((a + b) * 0.5f);
									const float v = source(q);
									if (v > value) {
										value = v;
										best_from = q;
									}
								}
								s0 = s1;
								h0 = h1;
							}
						}
						after[i] = value == -INFINITY ? source(p) : value;
						if (write_materials) {
							materials[i] = source_material(best_from);
						}
					} else {
						// Snake hook: pulled along a step, keeping a tendril's
						// thickness (overlapping copies add up).
						Vector3 displacement;
						float capsule = -C;
						for (int cc = 0; cc < count; cc++) {
							const Vector3 &c = centers[cc], &delta = deltas[cc];
							displacement += delta * falloff((p - c).length() / r);
							const Vector3 a = c - delta;
							const float t = delta.length_squared() > 0.0f ? std::clamp((p - a).dot(delta) / delta.length_squared(), 0.0f, 1.0f) : 0.0f;
							capsule = std::max(capsule, thickness - (p - (a + delta * t)).length());
						}
						if (displacement.length_squared() == 0.0f && capsule <= -C) {
							continue;
						}
						after[i] = std::max(source(p - displacement), std::min(capsule, C));
						if (write_materials) {
							materials[i] = source_material(p - displacement);
						}
					}
				}
			}
		}
		// Repair, around what moved (see apply_brush).
		repair(after, before, lo, hi);
		any |= after != before;
	}
	if (any) {
		edited = true;
		changed();
	}
	return any;
}

// --- Materials, splitting, hulls ------------------------------------------------------

void DensityShape::physical_properties_now(double &r_mass, double &r_bounce, double &r_friction) const {
	r_mass = 0.0;
	r_bounce = 0.5;
	r_friction = 0.5;
	if (keys.empty()) {
		return;
	}
	const double cube = voxel_size * voxel_size * voxel_size;
	double bounce = 0.0, friction = 0.0, skin = 0.0;
	const std::vector<float> &v = keys[0];
	for (int z = 0; z < size.z; z++) {
		for (int y = 0; y < size.y; y++) {
			for (int x = 0; x < size.x; x++) {
				const size_t i = index(x, y, z);
				if (v[i] <= 0.0f) {
					continue;
				}
				const MaterialPhysics m = material_physics(materials[i]);
				r_mass += m.density * cube * MASS_PER_VOXEL;
				if (v[i] < 1.5f) { // the skin: what touches things
					bounce += m.bounce;
					friction += m.friction;
					skin += 1.0;
				}
			}
		}
	}
	if (skin > 0.0) {
		r_bounce = bounce / skin;
		r_friction = friction / skin;
	}
	r_mass = std::max(r_mass, 0.01);
}

void DensityShape::update_mass_cache() const {
	if (mass_version == version) {
		return;
	}
	mass_version = version;
	physical_properties_now(cached_mass, cached_bounce, cached_friction);
	mass_properties_now(cached_centroid, cached_inertia);
}

void DensityShape::physical_properties(double &r_mass, double &r_bounce, double &r_friction) const {
	update_mass_cache();
	r_mass = cached_mass;
	r_bounce = cached_bounce;
	r_friction = cached_friction;
}

void DensityShape::mass_properties(Vector3 &r_centroid, Basis &r_inertia) const {
	update_mass_cache();
	r_centroid = cached_centroid;
	r_inertia = cached_inertia;
}

double DensityShape::get_mass() const {
	double mass, bounce, friction;
	physical_properties(mass, bounce, friction);
	return mass;
}

Array DensityShape::split_parts(int p_min_voxels) {
	Array parts;
	if (keys.empty()) {
		return parts;
	}
	// Label the solid points of key 0 by connected part (6 neighbours).
	const size_t n = count();
	std::vector<int> label(n, -1);
	std::vector<int> part_size;
	std::vector<size_t> queue;
	const std::vector<float> &v = keys[0];
	auto neighbours = [&](size_t i, auto &&p_f) {
		const int x = (int)(i % size.x), y = (int)((i / size.x) % size.y), z = (int)(i / ((size_t)size.x * size.y));
		if (x > 0) p_f(i - 1);
		if (x < size.x - 1) p_f(i + 1);
		if (y > 0) p_f(i - size.x);
		if (y < size.y - 1) p_f(i + size.x);
		if (z > 0) p_f(i - (size_t)size.x * size.y);
		if (z < size.z - 1) p_f(i + (size_t)size.x * size.y);
	};
	for (size_t s = 0; s < n; s++) {
		if (v[s] <= 0.0f || label[s] >= 0) {
			continue;
		}
		const int id = (int)part_size.size();
		part_size.push_back(0);
		queue.assign(1, s);
		label[s] = id;
		for (size_t q = 0; q < queue.size(); q++) {
			part_size[id]++;
			neighbours(queue[q], [&](size_t j) {
				if (v[j] > 0.0f && label[j] < 0) {
					label[j] = id;
					queue.push_back(j);
				}
			});
		}
	}
	if (part_size.size() <= 1) {
		return parts;
	}
	// Every other point belongs to the nearest part (so other keys' poses
	// split along with it), by spreading the labels out breadth first.
	queue.clear();
	for (size_t i = 0; i < n; i++) {
		if (label[i] >= 0) {
			queue.push_back(i);
		}
	}
	for (size_t q = 0; q < queue.size(); q++) {
		const int id = label[queue[q]];
		neighbours(queue[q], [&](size_t j) {
			if (label[j] < 0) {
				label[j] = id;
				queue.push_back(j);
			}
		});
	}
	const int biggest = (int)(std::max_element(part_size.begin(), part_size.end()) - part_size.begin());
	// One part's own copy: everything else becomes air (then the distances
	// are repaired).
	auto only = [&](int p_id) {
		std::vector<std::vector<float>> result = keys;
		for (std::vector<float> &k : result) {
			for (size_t i = 0; i < n; i++) {
				if (label[i] != p_id) {
					k[i] = std::min(k[i], -0.5f);
				}
			}
			density_redistance(k, size);
		}
		return result;
	};
	for (int id = 0; id < (int)part_size.size(); id++) {
		if (id == biggest || part_size[id] < p_min_voxels) {
			continue;
		}
		Ref<DensityShape> part;
		part.instantiate();
		part->size = size;
		part->origin = origin;
		part->voxel_size = voxel_size;
		part->keys = only(id);
		part->materials = materials;
		part->edited = true;
		part->changed();
		parts.push_back(part);
	}
	keys = only(biggest);
	edited = true;
	changed();
	return parts;
}

Ref<DensityShape> DensityShape::make_hull(const PackedVector3Array &p_points, int p_material, double p_voxel_size) {
	ERR_FAIL_COND_V_MSG(p_points.is_empty(), Ref<DensityShape>(), "DensityShape.make_hull(): no points.");
	std::vector<Vector3> points(p_points.ptr(), p_points.ptr() + p_points.size());
	// Give flat (or line, or single point) sets some thickness: points
	// across whatever directions they don't span.
	Vector3 centre;
	for (const Vector3 &p : points) {
		centre += p;
	}
	centre /= (float)points.size();
	Vector3 a_dir, normal;
	for (const Vector3 &p : points) {
		if ((p - centre).length() > 1e-3f) {
			a_dir = (p - centre).normalized();
			break;
		}
	}
	if (a_dir != Vector3()) {
		for (const Vector3 &p : points) {
			const Vector3 c = a_dir.cross(p - centre);
			if (c.length() > 1e-3f) {
				normal = c.normalized();
				break;
			}
		}
	}
	bool flat = true;
	if (normal != Vector3()) {
		for (const Vector3 &p : points) {
			if (std::abs((p - centre).dot(normal)) > 0.05f) {
				flat = false;
				break;
			}
		}
	}
	if (flat) {
		std::vector<Vector3> dirs;
		if (a_dir == Vector3()) {
			dirs = { Vector3(1, 0, 0), Vector3(0, 1, 0), Vector3(0, 0, 1) }; // one point
		} else if (normal == Vector3()) {
			const Vector3 u = a_dir.cross(std::abs(a_dir.y) < 0.9f ? Vector3(0, 1, 0) : Vector3(1, 0, 0)).normalized();
			dirs = { u, a_dir.cross(u).normalized() }; // a line
		} else {
			dirs = { normal }; // a plane
		}
		const std::vector<Vector3> base = points;
		points.clear();
		for (const Vector3 &p : base) {
			for (const Vector3 &d : dirs) {
				points.push_back(p + d * 0.75f);
				points.push_back(p - d * 0.75f);
			}
		}
	}
	// The hull's faces: every plane through three points with all the
	// others on one side (fine for the few points the geometry tool places).
	std::vector<Plane> planes;
	const int m = (int)points.size();
	for (int i = 0; i < m; i++) {
		for (int j = i + 1; j < m; j++) {
			for (int k = j + 1; k < m; k++) {
				Vector3 nrm = (points[j] - points[i]).cross(points[k] - points[i]);
				if (nrm.length() < 1e-5f) {
					continue;
				}
				nrm.normalize();
				const float d = nrm.dot(points[i]);
				bool above = false, below = false;
				for (int q = 0; q < m && !(above && below); q++) {
					const float s = nrm.dot(points[q]) - d;
					above |= s > 1e-3f;
					below |= s < -1e-3f;
				}
				if (above && below) {
					continue;
				}
				const Plane plane = above ? Plane(-nrm, -d) : Plane(nrm, d); // facing out
				bool known = false;
				for (const Plane &e : planes) {
					known |= e.normal.dot(plane.normal) > 0.9999f && std::abs(e.d - plane.d) < 1e-3f;
				}
				if (!known) {
					planes.push_back(plane);
				}
			}
		}
	}
	Vector3 lo = points[0], hi = points[0];
	for (const Vector3 &p : points) {
		lo = lo.min(p);
		hi = hi.max(p);
	}
	const Vector3 pad = Vector3(2, 2, 2);
	lo -= pad;
	hi += pad;
	Ref<DensityShape> shape;
	shape.instantiate();
	double vs = std::max(p_voxel_size, 0.01);
	const Vector3 extent = hi - lo;
	vs = std::max(vs, (double)std::max({ extent.x, extent.y, extent.z }) / (MAX_SIZE - 1));
	shape->voxel_size = vs;
	shape->origin = lo;
	shape->size = Vector3i((int)std::ceil(extent.x / vs) + 1, (int)std::ceil(extent.y / vs) + 1, (int)std::ceil(extent.z / vs) + 1).min(Vector3i(MAX_SIZE, MAX_SIZE, MAX_SIZE));
	shape->keys.assign(1, std::vector<float>(shape->count()));
	shape->materials.assign(shape->count(), (MaterialId)std::max(p_material, 0));
	for (int z = 0; z < shape->size.z; z++) {
		for (int y = 0; y < shape->size.y; y++) {
			for (int x = 0; x < shape->size.x; x++) {
				const Vector3 p = lo + Vector3((float)x, (float)y, (float)z) * (float)vs;
				float outside = -1e30f; // how far beyond the farthest face
				for (const Plane &pl : planes) {
					outside = std::max(outside, pl.normal.dot(p) - pl.d);
				}
				shape->keys[0][shape->index(x, y, z)] = std::clamp(-outside / (float)vs, -DENSITY_CLAMP, DENSITY_CLAMP);
			}
		}
	}
	shape->edited = true;
	shape->changed();
	return shape;
}
