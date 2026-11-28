#include "density_shape.h"

#include "parallel.h"
#include "sculpt_terrain.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/error_macros.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/plane.hpp>

#include <algorithm>
#include <condition_variable>
#include <functional>
#include <thread>
#include <cmath>
#include <cstring>

using namespace godot;

namespace {

constexpr int ZSTD = 2; // FileAccess.COMPRESSION_ZSTD

// Saved motion and residuals: 16-bit, in 1/64 voxel.
constexpr float KEY_STEP = 1.0f / 64.0f;
// Most frames between two keys (else as many as keep any point within about
// a voxel of where it was the frame before).
constexpr int MAX_GAP_FRAMES = 16;

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

int16_t quantize_key(float p_value) {
	return (int16_t)std::clamp((int)std::lround(p_value / KEY_STEP), -32767, 32767);
}

bool all_zero(const std::vector<float> &p_values) {
	for (float v : p_values) {
		if (std::abs(v) > 1e-5f) {
			return false;
		}
	}
	return true;
}

bool all_zero(const std::vector<Vector3> &p_values) {
	for (const Vector3 &v : p_values) {
		if (v.length_squared() > 1e-10f) {
			return false;
		}
	}
	return true;
}

} // namespace

void DensityShape::changed() {
	version++;
	emit_changed();
}

// --- Properties ----------------------------------------------------------------------

void DensityShape::set_size(const Vector3i &p_size) {
	size = Vector3i(std::clamp(p_size.x, 1, MAX_SIZE), std::clamp(p_size.y, 1, MAX_SIZE), std::clamp(p_size.z, 1, MAX_SIZE));
	if (!keys.empty()) {
		rest.assign(count(), -DENSITY_CLAMP);
		for (Key &k : keys) {
			k = Key();
		}
	}
	materials.assign(count(), MAT_STONE);
	changed();
}

void DensityShape::set_keys(const Array &p_keys) {
	if (p_keys.is_empty()) {
		return; // (newer files: the shape is in `rest` and `key_data`)
	}
	auto decode = [&](const PackedByteArray &p_bytes) {
		std::vector<float> values(count(), -DENSITY_CLAMP);
		const int64_t n = std::min<int64_t>(p_bytes.size(), (int64_t)values.size());
		for (int64_t i = 0; i < n; i++) {
			values[i] = (int8_t)p_bytes[i] * DENSITY_STEP;
		}
		return values;
	};
	rest = decode(p_keys[0]);
	keys.assign(p_keys.size(), Key());
	for (int64_t k = 1; k < p_keys.size(); k++) {
		std::vector<float> residual = decode(p_keys[k]);
		for (size_t i = 0; i < residual.size(); i++) {
			residual[i] -= rest[i];
		}
		if (!all_zero(residual)) {
			keys[k].residual = std::move(residual);
		}
	}
	changed();
}

Array DensityShape::get_keys() const {
	Array result;
	for (int k = 0; k < (int)keys.size(); k++) {
		const std::vector<float> &d = frame(key_frame(k)).density;
		PackedByteArray bytes;
		bytes.resize((int64_t)d.size());
		uint8_t *w = bytes.ptrw();
		for (size_t i = 0; i < d.size(); i++) {
			w[i] = (uint8_t)quantize_density(d[i]);
		}
		result.push_back(bytes);
	}
	return result;
}

void DensityShape::set_rest(const PackedByteArray &p_bytes) {
	rest.assign(count(), -DENSITY_CLAMP);
	const int64_t n = std::min<int64_t>(p_bytes.size(), (int64_t)rest.size());
	for (int64_t i = 0; i < n; i++) {
		rest[i] = (int8_t)p_bytes[i] * DENSITY_STEP;
	}
	if (keys.empty()) {
		keys.assign(1, Key());
	}
	changed();
}

PackedByteArray DensityShape::get_rest() const {
	PackedByteArray bytes;
	bytes.resize((int64_t)rest.size());
	uint8_t *w = bytes.ptrw();
	for (size_t i = 0; i < rest.size(); i++) {
		w[i] = (uint8_t)quantize_density(rest[i]);
	}
	return bytes;
}

// Each key: a flags byte (1 motion, 2 residual), then the motion (x y z per
// point) and the residual, as 16-bit little-endian numbers of KEY_STEP.
void DensityShape::set_key_data(const Array &p_data) {
	if (p_data.is_empty()) {
		return;
	}
	const size_t n = count();
	keys.assign(p_data.size(), Key());
	for (int64_t k = 0; k < p_data.size(); k++) {
		const PackedByteArray bytes = p_data[k];
		if (bytes.is_empty()) {
			continue;
		}
		const uint8_t *r = bytes.ptr();
		const uint8_t flags = r[0];
		int64_t at = 1;
		auto next = [&]() {
			if (at + 2 > bytes.size()) {
				return 0.0f;
			}
			const int16_t v = (int16_t)(r[at] | r[at + 1] << 8);
			at += 2;
			return v * KEY_STEP;
		};
		if (flags & 1) {
			keys[k].motion.resize(n);
			for (size_t i = 0; i < n; i++) {
				const float x = next(), y = next(), z = next();
				keys[k].motion[i] = Vector3(x, y, z);
			}
		}
		if (flags & 2) {
			keys[k].residual.resize(n);
			for (size_t i = 0; i < n; i++) {
				keys[k].residual[i] = next();
			}
		}
	}
	if (rest.size() != n) {
		rest.assign(n, -DENSITY_CLAMP);
	}
	changed();
}

Array DensityShape::get_key_data() const {
	Array result;
	for (const Key &k : keys) {
		const bool m = !k.motion.empty(), e = !k.residual.empty();
		PackedByteArray bytes;
		bytes.resize(1 + (m ? (int64_t)k.motion.size() * 6 : 0) + (e ? (int64_t)k.residual.size() * 2 : 0));
		uint8_t *w = bytes.ptrw();
		w[0] = (uint8_t)((m ? 1 : 0) | (e ? 2 : 0));
		size_t at = 1;
		auto put = [&](float p_value) {
			const int16_t v = quantize_key(p_value);
			w[at] = (uint8_t)(v & 0xFF);
			w[at + 1] = (uint8_t)((uint16_t)v >> 8);
			at += 2;
		};
		for (const Vector3 &v : k.motion) {
			put(v.x);
			put(v.y);
			put(v.z);
		}
		for (float v : k.residual) {
			put(v);
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

// --- Keys ----------------------------------------------------------------------------

bool DensityShape::has_motion() const {
	for (const Key &k : keys) {
		if (!k.motion.empty()) {
			return true;
		}
	}
	return false;
}

int DensityShape::add_key(double p_pose, const PackedFloat32Array &p_weights) {
	ERR_FAIL_COND_V_MSG(keys.empty(), -1, "DensityShape.add_key(): the shape is empty.");
	std::vector<float> w;
	pose_weights(p_pose, p_weights, w);
	const size_t n = count();
	Key key;
	for (size_t k = 0; k < keys.size(); k++) {
		if (std::abs(w[k]) < 1e-6f) {
			continue;
		}
		const Key &from = keys[k];
		if (!from.motion.empty()) {
			key.motion.resize(n);
			for (size_t i = 0; i < n; i++) {
				key.motion[i] += from.motion[i] * w[k];
			}
		}
		if (!from.residual.empty()) {
			key.residual.resize(n, 0.0f);
			for (size_t i = 0; i < n; i++) {
				key.residual[i] += from.residual[i] * w[k];
			}
		}
	}
	keys.push_back(std::move(key));
	edited = true;
	changed();
	return (int)keys.size() - 1;
}

void DensityShape::remove_key(int p_key) {
	ERR_FAIL_COND_MSG(keys.size() <= 1, "DensityShape.remove_key(): the last key stays.");
	ERR_FAIL_INDEX(p_key, (int)keys.size());
	keys.erase(keys.begin() + p_key);
	edited = true;
	changed();
}

void DensityShape::move_key(int p_from, int p_to) {
	ERR_FAIL_INDEX(p_from, (int)keys.size());
	ERR_FAIL_INDEX(p_to, (int)keys.size());
	if (p_from == p_to) {
		return;
	}
	Key key = std::move(keys[p_from]);
	keys.erase(keys.begin() + p_from);
	keys.insert(keys.begin() + p_to, std::move(key));
	edited = true;
	changed();
}

void DensityShape::set_key_name(int p_key, const String &p_name) {
	ERR_FAIL_INDEX(p_key, (int)keys.size());
	keys[p_key].name = p_name.strip_edges();
	edited = true;
	changed(); // (a new version: saved and sent to other players)
}

String DensityShape::get_key_name(int p_key) const {
	return p_key >= 0 && p_key < (int)keys.size() ? keys[p_key].name : String();
}

String DensityShape::get_key_label(int p_key) const {
	const String name = get_key_name(p_key);
	return name.is_empty() ? vformat("Key %d", p_key) : name;
}

void DensityShape::set_key_names(const PackedStringArray &p_names) {
	for (int k = 0; k < (int)keys.size() && k < p_names.size(); k++) {
		keys[k].name = p_names[k];
	}
}

PackedStringArray DensityShape::get_key_names() const {
	PackedStringArray names;
	for (const Key &k : keys) {
		names.push_back(k.name);
	}
	return names;
}

void DensityShape::reset_key(int p_key) {
	ERR_FAIL_INDEX(p_key, (int)keys.size());
	const String name = keys[p_key].name;
	keys[p_key] = Key();
	keys[p_key].name = name;
	edited = true;
	changed();
}

// --- Making shapes -------------------------------------------------------------------

Ref<DensityShape> DensityShape::make_sphere(double p_radius, int p_material, int p_stripe_material, double p_voxel_size) {
	Ref<DensityShape> shape;
	shape.instantiate();
	const double vs = std::max(p_voxel_size, 0.01);
	const int n = std::min((int)std::ceil(2.0 * p_radius / vs) + 5, MAX_SIZE);
	shape->voxel_size = vs;
	shape->size = Vector3i(n, n, n);
	shape->origin = Vector3(1, 1, 1) * (float)(-(n - 1) * 0.5 * vs);
	shape->rest.assign(shape->count(), 0.0f);
	shape->keys.assign(1, Key());
	shape->materials.assign(shape->count(), (MaterialId)std::max(p_material, 0));
	for (int z = 0; z < n; z++) {
		for (int y = 0; y < n; y++) {
			for (int x = 0; x < n; x++) {
				const Vector3 p = shape->origin + Vector3((float)x, (float)y, (float)z) * (float)vs;
				const size_t i = shape->index(x, y, z);
				shape->rest[i] = std::clamp((float)((p_radius - p.length()) / vs), -DENSITY_CLAMP, DENSITY_CLAMP);
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
	const size_t n = count();
	std::vector<std::vector<float>> baked(p_poses.size(), std::vector<float>(n));
	// Where each point's nearest surface is on the first pose's triangles:
	// the same triangle, same place on it, in another pose is where it goes.
	std::vector<int64_t> near_tri(n, -1);
	std::vector<Vector3> near_bary(n);
	materials.assign(n, MAT_STONE);
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
					baked[k][i] = std::clamp(inside ? d : -d, -DENSITY_CLAMP, DENSITY_CLAMP);
					if (k == 0) {
						near_tri[i] = best_tri;
						near_bary[i] = best_bary;
						if (has_colors && best_tri >= 0) {
							const Color c = colors[3 * best_tri] * best_bary.x + colors[3 * best_tri + 1] * best_bary.y + colors[3 * best_tri + 2] * best_bary.z;
							materials[i] = material_of_color(c);
						}
					}
				}
			}
		}
	}
	rest = baked[0];
	keys.assign(p_poses.size(), Key());
	version++; // (frames of the new size)
	const PackedVector3Array first = p_poses[0];
	for (int64_t k = 1; k < p_poses.size(); k++) {
		const PackedVector3Array tris = p_poses[k];
		Key &key = keys[k];
		if (tris.size() == first.size()) {
			// Motion: each point goes as its nearest bit of surface does.
			key.motion.assign(n, Vector3());
			for (size_t i = 0; i < n; i++) {
				const int64_t j = near_tri[i];
				if (j < 0) {
					continue;
				}
				const Vector3 &b = near_bary[i];
				const Vector3 from = first[3 * j] * b.x + first[3 * j + 1] * b.y + first[3 * j + 2] * b.z;
				const Vector3 to = tris[3 * j] * b.x + tris[3 * j + 1] * b.y + tris[3 * j + 2] * b.z;
				key.motion[i] = (to - from) / (float)vs;
			}
			// Smoothed away from the surface (deep inside, the nearest surface
			// jumps from one side to another), the skin's kept as it is.
			std::vector<Vector3> smooth = key.motion;
			for (int it = 0; it < 12; it++) {
				for (int z = 0; z < size.z; z++) {
					for (int y = 0; y < size.y; y++) {
						for (int x = 0; x < size.x; x++) {
							const size_t i = index(x, y, z);
							if (std::abs(rest[i]) < 1.5f) {
								continue;
							}
							Vector3 sum;
							int m = 0;
							const Vector3i ns[6] = { { x - 1, y, z }, { x + 1, y, z }, { x, y - 1, z }, { x, y + 1, z }, { x, y, z - 1 }, { x, y, z + 1 } };
							for (const Vector3i &q : ns) {
								if (q.x >= 0 && q.y >= 0 && q.z >= 0 && q.x < size.x && q.y < size.y && q.z < size.z) {
									sum += key.motion[index(q.x, q.y, q.z)];
									m++;
								}
							}
							smooth[i] = sum / (float)m;
						}
					}
				}
				key.motion = smooth;
			}
			// What the motion doesn't get right, as a residual.
			Frame moved;
			resolve((int)k, (int)k, 0.0f, false, moved);
			key.residual.assign(n, 0.0f);
			for (size_t i = 0; i < n; i++) {
				key.residual[i] = baked[k][i] - moved.density[i];
			}
		} else {
			// Not the same triangles: the pose as a plain residual.
			key.residual.assign(n, 0.0f);
			for (size_t i = 0; i < n; i++) {
				key.residual[i] = baked[k][i] - rest[i];
			}
		}
		if (all_zero(key.residual)) {
			key.residual.clear();
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
	copy->rest = rest;
	copy->keys = keys;
	copy->materials = materials;
	copy->edited = edited;
	return copy;
}

Ref<DensityShape> DensityShape::resampled(double p_voxel_size) const {
	if (keys.empty() || rest.empty()) {
		return duplicate_shape();
	}
	const float C = DENSITY_CLAMP;
	// What it holds: points near or inside a surface, in the rest shape or
	// any key's (a key's motion moves them; its residual adds some).
	Vector3 lo(1e30f, 1e30f, 1e30f), hi(-1e30f, -1e30f, -1e30f);
	for (int z = 0; z < size.z; z++) {
		for (int y = 0; y < size.y; y++) {
			for (int x = 0; x < size.x; x++) {
				const size_t i = index(x, y, z);
				float most = rest[i];
				for (const Key &k : keys) {
					if (!k.residual.empty()) {
						most = std::max(most, rest[i] + k.residual[i]);
					}
				}
				if (most < -2.0f) {
					continue;
				}
				const Vector3 p((float)x, (float)y, (float)z);
				lo = lo.min(p);
				hi = hi.max(p);
				for (const Key &k : keys) {
					if (!k.motion.empty()) {
						lo = lo.min(p + k.motion[i]);
						hi = hi.max(p + k.motion[i]);
					}
				}
			}
		}
	}
	if (lo.x > hi.x) {
		return duplicate_shape();
	}
	const float old_vs = (float)voxel_size;
	// A couple of old voxels' room round it, in body space.
	const Vector3 room(2.0f, 2.0f, 2.0f);
	const Vector3 blo = origin + (lo - room) * old_vs, bhi = origin + (hi + room) * old_vs;
	double vs = std::clamp(p_voxel_size, 0.01, MAX_VOXEL_SIZE);
	Vector3i n;
	for (;;) {
		const Vector3 extent = (bhi - blo) / (float)vs;
		n = Vector3i((int)std::ceil(extent.x) + 1, (int)std::ceil(extent.y) + 1, (int)std::ceil(extent.z) + 1);
		if ((n.x <= MAX_SIZE && n.y <= MAX_SIZE && n.z <= MAX_SIZE) || vs * 2.0 > MAX_VOXEL_SIZE) {
			break;
		}
		vs *= 2.0;
	}
	n = n.clamp(Vector3i(2, 2, 2), Vector3i(MAX_SIZE, MAX_SIZE, MAX_SIZE));
	const float k = old_vs / (float)vs; // old voxels -> new
	const size_t ncount = (size_t)n.x * n.y * n.z;
	// Plain trilinear (not a distance: no fall-off past the edge).
	auto sample_plain = [&](const std::vector<float> &v, const Vector3 &q) {
		const Vector3 c = q.clamp(Vector3(), Vector3(size - Vector3i(1, 1, 1)));
		const int x0 = std::min((int)c.x, size.x - 1), y0 = std::min((int)c.y, size.y - 1), z0 = std::min((int)c.z, size.z - 1);
		const int x1 = std::min(x0 + 1, size.x - 1), y1 = std::min(y0 + 1, size.y - 1), z1 = std::min(z0 + 1, size.z - 1);
		const float fx = c.x - x0, fy = c.y - y0, fz = c.z - z0;
		auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
		return lerp(lerp(lerp(v[index(x0, y0, z0)], v[index(x1, y0, z0)], fx), lerp(v[index(x0, y1, z0)], v[index(x1, y1, z0)], fx), fy),
				lerp(lerp(v[index(x0, y0, z1)], v[index(x1, y0, z1)], fx), lerp(v[index(x0, y1, z1)], v[index(x1, y1, z1)], fx), fy), fz);
	};
	Ref<DensityShape> copy;
	copy.instantiate();
	copy->size = n;
	copy->origin = blo;
	copy->voxel_size = vs;
	copy->rest.resize(ncount);
	copy->materials.resize(ncount);
	copy->keys.resize(keys.size());
	for (size_t kk = 0; kk < keys.size(); kk++) {
		copy->keys[kk].name = keys[kk].name;
		if (!keys[kk].motion.empty()) {
			copy->keys[kk].motion.resize(ncount);
		}
		if (!keys[kk].residual.empty()) {
			copy->keys[kk].residual.resize(ncount);
		}
	}
	for (int z = 0; z < n.z; z++) {
		for (int y = 0; y < n.y; y++) {
			for (int x = 0; x < n.x; x++) {
				const size_t j = ((size_t)z * n.y + y) * n.x + x;
				const Vector3 q = (blo + Vector3((float)x, (float)y, (float)z) * (float)vs - origin) / old_vs;
				copy->rest[j] = std::clamp(sample_grid(rest, q) * k, -C, C);
				const Vector3i near = Vector3i((int)std::lround(q.x), (int)std::lround(q.y), (int)std::lround(q.z)).clamp(Vector3i(), size - Vector3i(1, 1, 1));
				copy->materials[j] = materials.empty() ? (MaterialId)MAT_STONE : materials[index(near.x, near.y, near.z)];
				for (size_t kk = 0; kk < keys.size(); kk++) {
					if (!keys[kk].motion.empty()) {
						copy->keys[kk].motion[j] = sample_motion(keys[kk].motion, q) * k;
					}
					if (!keys[kk].residual.empty()) {
						copy->keys[kk].residual[j] = sample_plain(keys[kk].residual, q) * k;
					}
				}
			}
		}
	}
	density_redistance(copy->rest, n);
	copy->edited = true;
	return copy;
}

Ref<DensityShape> DensityShape::mirrored(const Vector3 &p_point, const Vector3 &p_normal) const {
	if (keys.empty() || rest.empty() || p_normal.length_squared() < 1e-12f) {
		return duplicate_shape();
	}
	const Vector3 N = p_normal.normalized();
	const float C = DENSITY_CLAMP;
	const float old_vs = (float)voxel_size;
	auto side = [&](const Vector3 &p_body) { return (p_body - p_point).dot(N); };
	auto reflect = [&](const Vector3 &p_body) { return p_body - N * (2.0f * side(p_body)); };
	// What it holds on the kept side (as resampled() measures it), and that mirrored.
	Vector3 lo(1e30f, 1e30f, 1e30f), hi(-1e30f, -1e30f, -1e30f);
	auto hold = [&](const Vector3 &p_body) {
		if (side(p_body) >= -old_vs) {
			lo = lo.min(p_body).min(reflect(p_body));
			hi = hi.max(p_body).max(reflect(p_body));
		}
	};
	for (int z = 0; z < size.z; z++) {
		for (int y = 0; y < size.y; y++) {
			for (int x = 0; x < size.x; x++) {
				const size_t i = index(x, y, z);
				float most = rest[i];
				for (const Key &k : keys) {
					if (!k.residual.empty()) {
						most = std::max(most, rest[i] + k.residual[i]);
					}
				}
				if (most < -2.0f) {
					continue;
				}
				const Vector3 p((float)x, (float)y, (float)z);
				hold(origin + p * old_vs);
				for (const Key &k : keys) {
					if (!k.motion.empty()) {
						hold(origin + (p + k.motion[i]) * old_vs);
					}
				}
			}
		}
	}
	if (lo.x > hi.x) {
		return duplicate_shape(); // (nothing on the kept side)
	}
	const Vector3 room = Vector3(2.0f, 2.0f, 2.0f) * old_vs;
	Vector3 blo = lo - room, bhi = hi + room;
	double vs = voxel_size;
	Vector3i n;
	for (;;) {
		const Vector3 extent = (bhi - blo) / (float)vs;
		n = Vector3i((int)std::ceil(extent.x) + 1, (int)std::ceil(extent.y) + 1, (int)std::ceil(extent.z) + 1);
		if ((n.x <= MAX_SIZE && n.y <= MAX_SIZE && n.z <= MAX_SIZE) || vs * 2.0 > MAX_VOXEL_SIZE) {
			break;
		}
		vs *= 2.0;
	}
	n = n.clamp(Vector3i(2, 2, 2), Vector3i(MAX_SIZE, MAX_SIZE, MAX_SIZE));
	// (Grid points on the old grid's where nothing was coarsened: the kept
	// side keeps its values exactly.)
	if (vs == voxel_size) {
		const Vector3 shift = ((blo - origin) / old_vs).floor();
		blo = origin + shift * old_vs;
		n = Vector3i((int)std::ceil((bhi.x - blo.x) / old_vs) + 1, (int)std::ceil((bhi.y - blo.y) / old_vs) + 1,
				(int)std::ceil((bhi.z - blo.z) / old_vs) + 1)
					.clamp(Vector3i(2, 2, 2), Vector3i(MAX_SIZE, MAX_SIZE, MAX_SIZE));
	}
	const float k = old_vs / (float)vs;
	const size_t ncount = (size_t)n.x * n.y * n.z;
	auto sample_plain = [&](const std::vector<float> &v, const Vector3 &q) {
		const Vector3 c = q.clamp(Vector3(), Vector3(size - Vector3i(1, 1, 1)));
		const int x0 = std::min((int)c.x, size.x - 1), y0 = std::min((int)c.y, size.y - 1), z0 = std::min((int)c.z, size.z - 1);
		const int x1 = std::min(x0 + 1, size.x - 1), y1 = std::min(y0 + 1, size.y - 1), z1 = std::min(z0 + 1, size.z - 1);
		const float fx = c.x - x0, fy = c.y - y0, fz = c.z - z0;
		auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
		return lerp(lerp(lerp(v[index(x0, y0, z0)], v[index(x1, y0, z0)], fx), lerp(v[index(x0, y1, z0)], v[index(x1, y1, z0)], fx), fy),
				lerp(lerp(v[index(x0, y0, z1)], v[index(x1, y0, z1)], fx), lerp(v[index(x0, y1, z1)], v[index(x1, y1, z1)], fx), fy), fz);
	};
	Ref<DensityShape> copy;
	copy.instantiate();
	copy->size = n;
	copy->origin = blo;
	copy->voxel_size = vs;
	copy->rest.resize(ncount);
	copy->materials.resize(ncount);
	copy->keys.resize(keys.size());
	for (size_t kk = 0; kk < keys.size(); kk++) {
		copy->keys[kk].name = keys[kk].name;
		if (!keys[kk].motion.empty()) {
			copy->keys[kk].motion.resize(ncount);
		}
		if (!keys[kk].residual.empty()) {
			copy->keys[kk].residual.resize(ncount);
		}
	}
	for (int z = 0; z < n.z; z++) {
		for (int y = 0; y < n.y; y++) {
			for (int x = 0; x < n.x; x++) {
				const size_t j = ((size_t)z * n.y + y) * n.x + x;
				const Vector3 b = blo + Vector3((float)x, (float)y, (float)z) * (float)vs;
				const bool mirror = side(b) < 0.0f;
				const Vector3 from = mirror ? reflect(b) : b;
				const Vector3 q = (from - origin) / old_vs;
				copy->rest[j] = std::clamp(sample_grid(rest, q) * k, -C, C);
				const Vector3i near = Vector3i((int)std::lround(q.x), (int)std::lround(q.y), (int)std::lround(q.z)).clamp(Vector3i(), size - Vector3i(1, 1, 1));
				copy->materials[j] = materials.empty() ? (MaterialId)MAT_STONE : materials[index(near.x, near.y, near.z)];
				for (size_t kk = 0; kk < keys.size(); kk++) {
					if (!keys[kk].motion.empty()) {
						Vector3 m = sample_motion(keys[kk].motion, q) * k;
						if (mirror) {
							m -= N * (2.0f * m.dot(N)); // (a move mirrored, as the shape is)
						}
						copy->keys[kk].motion[j] = m;
					}
					if (!keys[kk].residual.empty()) {
						copy->keys[kk].residual[j] = sample_plain(keys[kk].residual, q) * k;
					}
				}
			}
		}
	}
	density_redistance(copy->rest, n);
	copy->edited = true;
	return copy;
}

Dictionary DensityShape::to_dict() const {
	Dictionary d;
	d["size"] = size;
	d["origin"] = origin;
	d["voxel_size"] = voxel_size;
	d["rest"] = get_rest().compress(ZSTD);
	Array packed;
	PackedInt64Array sizes;
	for (const Variant &k : get_key_data()) {
		packed.push_back(((PackedByteArray)k).compress(ZSTD));
		sizes.push_back(((PackedByteArray)k).size());
	}
	d["key_data"] = packed;
	d["key_sizes"] = sizes;
	d["key_names"] = get_key_names();
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
	if (p_dict.has("rest")) {
		shape->set_rest(((PackedByteArray)p_dict["rest"]).decompress(n, ZSTD));
		Array data;
		const Array packed = p_dict.get("key_data", Array());
		const PackedInt64Array sizes = p_dict.get("key_sizes", PackedInt64Array());
		for (int64_t k = 0; k < packed.size(); k++) {
			const int64_t bytes = k < sizes.size() ? sizes[k] : 1;
			data.push_back(((PackedByteArray)packed[k]).decompress(bytes, ZSTD));
		}
		shape->set_key_data(data);
	} else {
		// Older saves: every key's whole density.
		Array keys;
		const Array packed = p_dict.get("keys", Array());
		for (const Variant &k : packed) {
			keys.push_back(((PackedByteArray)k).decompress(n, ZSTD));
		}
		shape->set_keys(keys);
	}
	shape->set_materials(((PackedByteArray)p_dict.get("materials", PackedByteArray())).decompress(n * 2, ZSTD));
	shape->set_key_names(p_dict.get("key_names", PackedStringArray()));
	shape->edited = true;
	return shape;
}

// --- Frames --------------------------------------------------------------------------

void DensityShape::update_layout() const {
	if (layout_version == version) {
		return;
	}
	layout_version = version;
	const int k_count = (int)keys.size();
	gap_frames.assign(std::max(k_count - 1, 0), 1);
	const size_t n = count();
	for (int g = 0; g + 1 < k_count; g++) {
		const Key &a = keys[g], &b = keys[g + 1];
		if (a.motion.empty() && b.motion.empty()) {
			continue;
		}
		// The most any point near solid moves from one key to the next.
		float most = 0.0f;
		for (size_t i = 0; i < n && i < rest.size(); i++) {
			if (rest[i] <= -DENSITY_CLAMP + 0.5f) {
				continue;
			}
			const Vector3 ma = a.motion.empty() ? Vector3() : a.motion[i], mb = b.motion.empty() ? Vector3() : b.motion[i];
			most = std::max(most, (mb - ma).length_squared());
		}
		gap_frames[g] = std::clamp((int)std::ceil(std::sqrt(most)), 1, MAX_GAP_FRAMES);
	}
	int total = k_count > 0 ? 1 : 0;
	for (int f : gap_frames) {
		total += f;
	}
	frames.clear();
	frames.resize(total);
	// Key weights round to steps that move no point much more than a voxel.
	key_steps.assign(k_count, 1);
	for (int k = 0; k < k_count; k++) {
		const Key &key = keys[k];
		if (!key.motion.empty()) {
			float most = 0.0f;
			for (size_t i = 0; i < n && i < rest.size(); i++) {
				if (rest[i] > -DENSITY_CLAMP + 0.5f) {
					most = std::max(most, key.motion[i].length_squared());
				}
			}
			key_steps[k] = std::clamp((int)std::ceil(std::sqrt(most)), 1, MAX_GAP_FRAMES);
		}
		if (!key.residual.empty()) {
			key_steps[k] = std::max(key_steps[k], 4);
		}
	}
	cancel_jobs(-1);
	combos.clear();
	next_combo = COMBO_BASE;
}

bool DensityShape::has_weights(const PackedFloat32Array &p_weights) {
	for (int64_t i = 0; i < p_weights.size(); i++) {
		if (std::abs(p_weights[i]) > 1e-4f) {
			return true;
		}
	}
	return false;
}

bool DensityShape::pose_weights(double p_pose, const PackedFloat32Array &p_weights, std::vector<float> &r_weights) const {
	const int k_count = (int)keys.size();
	r_weights.assign(k_count, 0.0f);
	if (k_count == 0) {
		return false;
	}
	const int last = k_count - 1;
	const double p = std::clamp(p_pose, 0.0, (double)last);
	const int a = std::min((int)p, last), b = std::min(a + 1, last);
	const float t = b == a ? 0.0f : (float)(p - a);
	r_weights[a] += 1.0f - t;
	r_weights[b] += t;
	bool any = false;
	for (int k = 0; k < std::min(k_count, (int)p_weights.size()); k++) {
		const float w = std::clamp(p_weights[k], -1.0f, 2.0f);
		if (std::abs(w) > 1e-4f) {
			r_weights[k] += w;
			any = true;
		}
	}
	return any;
}

int DensityShape::combo_frame(const std::vector<int16_t> &p_steps) const {
	for (Combo &c : combos) {
		if (c.steps == p_steps) {
			c.used = ++combo_clock;
			return c.id;
		}
	}
	// Room: as many as about 256 MB holds, 32 to 1024 (several weights
	// animated at once visit many mixes, over and over: a walk with a head
	// turn on top goes through hundreds, and with fewer kept they were
	// worked out again every time round, the creature showing old frames
	// meanwhile).
	const size_t per = std::max<size_t>(count() * (sizeof(float) + sizeof(MaterialId) + sizeof(Vector3)), 1);
	const size_t most = std::clamp<size_t>(((size_t)256 << 20) / per, 32, 1024);
	while (combos.size() >= most) {
		size_t oldest = 0;
		for (size_t i = 1; i < combos.size(); i++) {
			if (combos[i].used < combos[oldest].used) {
				oldest = i;
			}
		}
		cancel_jobs(combos[oldest].id);
		combos.erase(combos.begin() + oldest);
	}
	Combo c;
	c.steps = p_steps;
	c.weights.resize(p_steps.size());
	for (size_t k = 0; k < p_steps.size(); k++) {
		c.weights[k] = (float)p_steps[k] / (float)key_steps[k];
	}
	c.id = next_combo++;
	c.used = ++combo_clock;
	combos.push_back(std::move(c));
	return combos.back().id;
}

int DensityShape::get_frame_count() const {
	std::lock_guard<std::recursive_mutex> lock(frame_mutex);
	update_layout();
	return (int)frames.size();
}

void DensityShape::frame_keys(int p_frame, int &r_a, int &r_b, float &r_t) const {
	int base = 0;
	for (int g = 0; g < (int)gap_frames.size(); g++) {
		if (p_frame < base + gap_frames[g]) {
			r_a = g;
			r_b = g + 1;
			r_t = (float)(p_frame - base) / (float)gap_frames[g];
			return;
		}
		base += gap_frames[g];
	}
	r_a = r_b = std::max((int)keys.size() - 1, 0);
	r_t = 0.0f;
}

int DensityShape::key_frame(int p_key) const {
	std::lock_guard<std::recursive_mutex> lock(frame_mutex);
	update_layout();
	int f = 0;
	for (int g = 0; g < std::min(p_key, (int)gap_frames.size()); g++) {
		f += gap_frames[g];
	}
	return f;
}

void DensityShape::pose_blend(double p_pose, const PackedFloat32Array &p_weights, Blend &r_blend) const {
	std::lock_guard<std::recursive_mutex> lock(frame_mutex);
	update_layout();
	r_blend = Blend();
	std::vector<float> w;
	if (has_weights(p_weights) && pose_weights(p_pose, p_weights, w)) {
		// Each weight in its steps: the mix is inside the cube of step
		// corners round it, and in that, in the simplex (Freudenthal's)
		// from the corner below it stepping up one key at a time, the keys
		// farthest past their step first. Its corners' weights move
		// smoothly whichever key weight changes (two keys between steps
		// don't jump when one of them crosses a step).
		const int k_count = (int)keys.size();
		std::vector<int16_t> steps(k_count);
		std::vector<std::pair<float, int>> between; // (how far past its step, key)
		for (int k = 0; k < k_count; k++) {
			const float x = w[k] * key_steps[k];
			const float f = std::floor(x + 1e-4f);
			steps[k] = (int16_t)f;
			if (x - f > 1e-3f) {
				between.push_back({ x - f, k });
			}
		}
		std::sort(between.begin(), between.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
		struct Corner {
			std::vector<int16_t> steps;
			float weight;
		};
		std::vector<Corner> corners;
		corners.push_back({ steps, 1.0f - (between.empty() ? 0.0f : between[0].first) });
		for (size_t j = 0; j < between.size(); j++) {
			steps[between[j].second]++;
			const float next = j + 1 < between.size() ? between[j + 1].first : 0.0f;
			corners.push_back({ steps, between[j].first - next });
		}
		// The heaviest (up to MAX_BLEND; more keys between steps at once are rare).
		std::stable_sort(corners.begin(), corners.end(), [](const Corner &a, const Corner &b) { return a.weight > b.weight; });
		float total = 0.0f;
		int n = 0;
		for (size_t j = 0; j < corners.size() && n < MAX_BLEND; j++) {
			if (corners[j].weight <= 1e-4f && n > 0) {
				break;
			}
			r_blend.frames[n] = combo_frame(corners[j].steps);
			r_blend.weights[n] = std::max(corners[j].weight, 0.0f);
			total += r_blend.weights[n];
			n++;
		}
		r_blend.count = n;
		for (int j = 0; j < n; j++) {
			r_blend.weights[j] = total > 1e-6f ? r_blend.weights[j] / total : (j == 0 ? 1.0f : 0.0f);
		}
		return;
	}
	const int last = std::max((int)keys.size() - 1, 0);
	const double p = std::clamp(p_pose, 0.0, (double)last);
	const int a = std::min((int)p, last);
	if (a >= last || gap_frames.empty()) {
		r_blend.frames[0] = key_frame(last);
		return;
	}
	const int n = gap_frames[a];
	const double x = (p - a) * n;
	const int j = std::clamp((int)x, 0, n - 1);
	const float t = (float)std::clamp(x - j, 0.0, 1.0);
	const int below = key_frame(a) + j;
	if (t <= 1e-4f) {
		r_blend.frames[0] = below;
		return;
	}
	const bool up = t > 0.5f;
	r_blend.frames[0] = up ? below + 1 : below;
	r_blend.weights[0] = up ? t : 1.0f - t;
	r_blend.frames[1] = up ? below : below + 1;
	r_blend.weights[1] = up ? 1.0f - t : t;
	r_blend.count = 2;
}

DensityShape::Frame *DensityShape::frame_slot(int p_frame) const {
	if (p_frame >= COMBO_BASE) {
		for (Combo &c : combos) {
			if (c.id == p_frame) {
				return &c.frame;
			}
		}
		return nullptr; // (dropped for room)
	}
	if (frames.empty()) {
		return nullptr;
	}
	return &frames[std::clamp(p_frame, 0, (int)frames.size() - 1)];
}

std::vector<float> DensityShape::frame_weights(int p_frame) const {
	if (p_frame >= COMBO_BASE) {
		for (const Combo &c : combos) {
			if (c.id == p_frame) {
				return c.weights;
			}
		}
		return std::vector<float>(keys.size(), 0.0f);
	}
	int a, b;
	float t;
	frame_keys(std::clamp(p_frame, 0, std::max((int)frames.size() - 1, 0)), a, b, t);
	std::vector<float> w(keys.size(), 0.0f);
	if (!keys.empty()) {
		w[std::clamp(a, 0, (int)keys.size() - 1)] += 1.0f - t;
		w[std::clamp(b, 0, (int)keys.size() - 1)] += t;
	}
	return w;
}

const DensityShape::Frame &DensityShape::frame(int p_frame) const {
	std::lock_guard<std::recursive_mutex> lock(frame_mutex);
	update_layout();
	collect_jobs();
	static const Frame none;
	Frame *f = frame_slot(p_frame);
	if (f == nullptr) {
		return none;
	}
	if (f->version != version) {
		cancel_jobs(p_frame); // (needed now)
		resolve_weights(frame_weights(p_frame), true, *f);
		f->version = version;
	}
	return *f;
}

bool DensityShape::frame_ready(int p_frame) const {
	std::lock_guard<std::recursive_mutex> lock(frame_mutex);
	update_layout();
	collect_jobs();
	const Frame *f = frame_slot(p_frame);
	if (f != nullptr && p_frame >= COMBO_BASE) {
		for (Combo &c : combos) {
			if (c.id == p_frame) {
				c.used = ++combo_clock; // (shown: kept)
			}
		}
	}
	return f != nullptr && f->version == version;
}

// A few threads working out frames in the background, the latest asked for
// first (each frame is its own job: several creatures animating at once
// need many, and one thread fell behind them).
namespace {
class FrameWorker {
public:
	static FrameWorker &get() {
		static FrameWorker *worker = new FrameWorker(); // (never deleted: shutdown() joins)
		return *worker;
	}
	void push(std::function<void()> &&p_job) {
		{
			std::lock_guard<std::mutex> lock(mutex);
			if (stopping) {
				return;
			}
			queue.push_back(std::move(p_job));
			if (threads.empty()) {
				// (Leaving cores for the game's own threads; each frame's work
				// is split over threads in places too.)
				const int count = std::clamp((int)std::thread::hardware_concurrency() / 4, 1, 3);
				for (int i = 0; i < count; i++) {
					threads.emplace_back([this] { loop(); });
				}
			}
		}
		wake.notify_one();
	}
	void shutdown() {
		{
			std::lock_guard<std::mutex> lock(mutex);
			stopping = true;
			queue.clear();
		}
		wake.notify_all();
		for (std::thread &t : threads) {
			if (t.joinable()) {
				t.join();
			}
		}
		threads.clear();
	}

private:
	std::mutex mutex;
	std::condition_variable wake;
	std::vector<std::function<void()>> queue;
	std::vector<std::thread> threads;
	bool stopping = false;
	void loop() {
		while (true) {
			std::function<void()> job;
			{
				std::unique_lock<std::mutex> lock(mutex);
				wake.wait(lock, [&] { return stopping || !queue.empty(); });
				if (stopping) {
					return;
				}
				job = std::move(queue.back());
				queue.pop_back();
			}
			job();
		}
	}
};
} // namespace

void DensityShape::shutdown_worker() {
	FrameWorker::get().shutdown();
}

void DensityShape::request_frame(int p_frame) const {
	std::lock_guard<std::recursive_mutex> lock(frame_mutex);
	update_layout();
	collect_jobs();
	const Frame *f = frame_slot(p_frame);
	if (f == nullptr || f->version == version) {
		return;
	}
	for (const std::shared_ptr<Job> &j : jobs) {
		if (j->frame == p_frame) {
			return; // on its way
		}
	}
	if (source == nullptr || source->version != version) {
		auto copy = std::make_shared<Source>();
		copy->size = size;
		copy->rest = rest;
		copy->keys.reserve(keys.size());
		for (const Key &k : keys) {
			Key c;
			c.motion = k.motion;
			c.residual = k.residual;
			copy->keys.push_back(std::move(c)); // (not the name: Strings stay on this thread)
		}
		copy->materials = materials;
		copy->version = version;
		source = copy;
	}
	auto job = std::make_shared<Job>();
	job->source = source;
	job->weights = frame_weights(p_frame);
	job->frame = p_frame;
	jobs.push_back(job);
	FrameWorker::get().push([job]() {
		if (job->cancelled.load(std::memory_order_acquire)) {
			return;
		}
		const Source &src = *job->source;
		resolve_source(src.size, src.rest, src.keys, src.materials, job->weights, true, job->result);
		job->done.store(true, std::memory_order_release);
	});
}

void DensityShape::collect_jobs() const {
	for (size_t i = 0; i < jobs.size();) {
		Job &j = *jobs[i];
		if (j.source->version != version) {
			j.cancelled.store(true, std::memory_order_release); // (the shape changed)
		} else if (j.done.load(std::memory_order_acquire)) {
			Frame *f = frame_slot(j.frame);
			if (f != nullptr && f->version != version) {
				*f = std::move(j.result);
				f->version = version;
			}
		} else {
			i++;
			continue;
		}
		jobs.erase(jobs.begin() + i);
	}
}

void DensityShape::cancel_jobs(int p_frame) const {
	for (size_t i = 0; i < jobs.size();) {
		if (p_frame < 0 || jobs[i]->frame == p_frame) {
			jobs[i]->cancelled.store(true, std::memory_order_release);
			jobs.erase(jobs.begin() + i);
		} else {
			i++;
		}
	}
}

DensityShape::~DensityShape() {
	std::lock_guard<std::recursive_mutex> lock(frame_mutex);
	cancel_jobs(-1);
}

void DensityShape::resolve(int p_a, int p_b, float p_t, bool p_residual, Frame &r_frame) const {
	std::vector<float> w(keys.size(), 0.0f);
	if (!keys.empty()) {
		w[std::clamp(p_a, 0, (int)keys.size() - 1)] += 1.0f - p_t;
		w[std::clamp(p_b, 0, (int)keys.size() - 1)] += p_t;
	}
	resolve_weights(w, p_residual, r_frame);
}

void DensityShape::resolve_weights(const std::vector<float> &p_weights, bool p_residual, Frame &r_frame) const {
	resolve_source(size, rest, keys, materials, p_weights, p_residual, r_frame);
}

void DensityShape::resolve_source(const Vector3i &p_size, const std::vector<float> &p_rest, const std::vector<Key> &p_keys,
		const std::vector<MaterialId> &p_materials, const std::vector<float> &p_weights, bool p_residual, Frame &r_frame) {
	const Vector3i size = p_size;
	const size_t n = (size_t)size.x * size.y * size.z;
	auto index = [&](int x, int y, int z) { return ((size_t)z * size.y + y) * size.x + x; };
	const float C = DENSITY_CLAMP;
	r_frame.density.assign(n, -C);
	r_frame.materials.clear();
	r_frame.to_rest.clear();
	if (p_keys.empty() || p_rest.size() != n) {
		return;
	}
	// The keys that count, and how much.
	std::vector<std::pair<const std::vector<Vector3> *, float>> motions;
	std::vector<std::pair<const std::vector<float> *, float>> residuals;
	for (size_t k = 0; k < p_keys.size() && k < p_weights.size(); k++) {
		const float w = p_weights[k];
		if (std::abs(w) < 1e-6f) {
			continue;
		}
		if (!p_keys[k].motion.empty()) {
			motions.push_back({ &p_keys[k].motion, w });
		}
		if (p_residual && !p_keys[k].residual.empty()) {
			residuals.push_back({ &p_keys[k].residual, w });
		}
	}
	auto residual_at = [&](size_t i) {
		float e = 0.0f;
		for (const auto &r : residuals) {
			e += (*r.first)[i] * r.second;
		}
		return e;
	};
	std::vector<Vector3> m;
	bool moves = false;
	Vector3 reach; // the most any solid point moves, per axis
	if (!motions.empty() && size.x > 1 && size.y > 1 && size.z > 1) {
		m.assign(n, Vector3());
		for (size_t i = 0; i < n; i++) {
			for (const auto &mo : motions) {
				m[i] += (*mo.first)[i] * mo.second;
			}
			if (m[i].length_squared() > 1e-8f) {
				moves = true;
				if (p_rest[i] > -C + 1e-3f) {
					reach = reach.max(m[i].abs());
				}
			}
		}
	}
	if (!moves) {
		for (size_t i = 0; i < n; i++) {
			r_frame.density[i] = std::clamp(p_rest[i] + residual_at(i), -C, C);
		}
		return;
	}
	// Each cell of the rest grid, moved (its corners by the motion), covers
	// some points of the posed grid: those take the density there (trilinear
	// within the cell), the most solid where cells overlap.
	std::vector<float> best(n, -INFINITY);
	std::vector<Vector3> to_rest(n);
	auto scatter = [&](int cx, int cy, int cz) {
		float v[8];
		Vector3 P[8];
		size_t ci[8];
		float most = -INFINITY;
		for (int c = 0; c < 8; c++) {
			ci[c] = index(cx + (c & 1), cy + ((c >> 1) & 1), cz + (c >> 2));
			v[c] = p_rest[ci[c]];
			most = std::max(most, v[c]);
		}
		if (most <= -C + 1e-3f) {
			return; // air: what uncovered points are anyway
		}
		Vector3 lo(INFINITY, INFINITY, INFINITY), hi(-INFINITY, -INFINITY, -INFINITY);
		bool rigid = true;
		Vector3 mean;
		for (int c = 0; c < 8; c++) {
			P[c] = Vector3((float)(cx + (c & 1)), (float)(cy + ((c >> 1) & 1)), (float)(cz + (c >> 2))) + m[ci[c]];
			lo = lo.min(P[c]);
			hi = hi.max(P[c]);
			rigid &= (m[ci[c]] - m[ci[0]]).length_squared() < 1e-8f;
			mean += m[ci[c]];
		}
		mean /= 8.0f;
		Vector3i glo, ghi;
		for (int a = 0; a < 3; a++) {
			glo[a] = std::max((int)std::ceil(lo[a] - 1e-4f), 0);
			ghi[a] = std::min((int)std::floor(hi[a] + 1e-4f), size[a] - 1);
			if (glo[a] > ghi[a]) {
				return;
			}
		}
		// The cell's trilinear map as a polynomial in (u, v, w).
		const Vector3 A = P[0], B = P[1] - P[0], Cc = P[2] - P[0], D = P[4] - P[0];
		const Vector3 E = P[3] - P[1] - P[2] + P[0], F = P[5] - P[1] - P[4] + P[0], G = P[6] - P[2] - P[4] + P[0];
		const Vector3 H = P[7] - P[3] - P[5] - P[6] + P[1] + P[2] + P[4] - P[0];
		const Vector3 corner((float)cx, (float)cy, (float)cz);
		for (int gz = glo.z; gz <= ghi.z; gz++) {
			for (int gy = glo.y; gy <= ghi.y; gy++) {
				for (int gx = glo.x; gx <= ghi.x; gx++) {
					const Vector3 g((float)gx, (float)gy, (float)gz);
					Vector3 u;
					if (rigid) {
						u = g - P[0];
					} else {
						// Newton's method, from where it'd be if the cell only moved.
						u = (g - corner - mean).clamp(Vector3(-0.5f, -0.5f, -0.5f), Vector3(1.5f, 1.5f, 1.5f));
						bool found = false;
						for (int it = 0; it < 8; it++) {
							const Vector3 X = A + B * u.x + Cc * u.y + D * u.z + E * (u.x * u.y) + F * (u.x * u.z) + G * (u.y * u.z) + H * (u.x * u.y * u.z);
							const Vector3 r = X - g;
							if (r.length_squared() < 1e-6f) {
								found = true;
								break;
							}
							const Vector3 Ju = B + E * u.y + F * u.z + H * (u.y * u.z);
							const Vector3 Jv = Cc + E * u.x + G * u.z + H * (u.x * u.z);
							const Vector3 Jw = D + F * u.x + G * u.y + H * (u.x * u.y);
							const Vector3 vw = Jv.cross(Jw);
							const float det = Ju.dot(vw);
							if (std::abs(det) < 1e-6f) {
								break;
							}
							const float inv = 1.0f / det;
							u -= Vector3(r.dot(vw), Ju.dot(r.cross(Jw)), Ju.dot(Jv.cross(r))) * inv;
							u = u.clamp(Vector3(-0.5f, -0.5f, -0.5f), Vector3(1.5f, 1.5f, 1.5f));
						}
						if (!found) {
							continue;
						}
					}
					const float e = 1e-3f;
					if (u.x < -e || u.y < -e || u.z < -e || u.x > 1 + e || u.y > 1 + e || u.z > 1 + e) {
						continue;
					}
					u = u.clamp(Vector3(), Vector3(1, 1, 1));
					const float x0 = 1.0f - u.x, y0 = 1.0f - u.y, z0 = 1.0f - u.z;
					const float value = ((v[0] * x0 + v[1] * u.x) * y0 + (v[2] * x0 + v[3] * u.x) * u.y) * z0 +
							((v[4] * x0 + v[5] * u.x) * y0 + (v[6] * x0 + v[7] * u.x) * u.y) * u.z;
					const size_t gi = index(gx, gy, gz);
					if (value > best[gi]) {
						best[gi] = value;
						to_rest[gi] = corner + u;
					}
				}
			}
		}
	};
	// In slabs across the axis parts move least along, each thicker than
	// twice that: every other slab writes points no other of them does, so
	// they run at once (the even ones, then the odd ones).
	int axis = 2, slabs = 1, thick = size.z;
	for (int a = 0; a < 3; a++) {
		const int t = 2 * (int)std::ceil(reach[a]) + 2;
		const int count = (size[a] - 1) / t;
		if (count > slabs) {
			axis = a;
			slabs = count;
			thick = t;
		}
	}
	const int cells_along = size[axis] - 1;
	if (slabs < 4) {
		slabs = 1;
		thick = cells_along;
	}
	auto run_slab = [&](int p_slab) {
		Vector3i lo(0, 0, 0), hi = size - Vector3i(1, 1, 1); // cells [lo, hi)
		lo[axis] = p_slab * thick;
		hi[axis] = p_slab + 1 == slabs ? cells_along : (p_slab + 1) * thick;
		for (int cz = lo.z; cz < hi.z; cz++) {
			for (int cy = lo.y; cy < hi.y; cy++) {
				for (int cx = lo.x; cx < hi.x; cx++) {
					scatter(cx, cy, cz);
				}
			}
		}
	};
	if (slabs == 1) {
		run_slab(0);
	} else {
		const int threads = std::clamp((int)std::thread::hardware_concurrency() / 2, 1, 8);
		for (int parity = 0; parity < 2; parity++) {
			parallel_for((slabs - parity + 1) / 2, threads, [&](int i) { run_slab(i * 2 + parity); });
		}
	}
	const Vector3i last = size - Vector3i(1, 1, 1);
	std::vector<MaterialId> mats(n, MAT_STONE);
	for (int z = 0; z < size.z; z++) {
		for (int y = 0; y < size.y; y++) {
			for (int x = 0; x < size.x; x++) {
				const size_t i = index(x, y, z);
				if (best[i] == -INFINITY) {
					// Nothing moved here: air, from about where it'd come from.
					to_rest[i] = Vector3((float)x, (float)y, (float)z) - m[i];
					best[i] = -C;
				}
				// The material of the rest point nearest what's shown.
				const Vector3i q = Vector3i(to_rest[i].round()).clamp(Vector3i(), last);
				mats[i] = p_materials.size() == n ? p_materials[index(q.x, q.y, q.z)] : MAT_STONE;
				r_frame.density[i] = std::clamp(best[i] + residual_at(i), -C, C);
			}
		}
	}
	// Stretched or squashed, the densities are no longer distances: repaired.
	density_redistance(r_frame.density, size);
	r_frame.materials = std::move(mats);
	r_frame.to_rest = std::move(to_rest);
}

// --- Sampling ------------------------------------------------------------------------

float DensityShape::sample_grid(const std::vector<float> &v, const Vector3 &p_grid) const {
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

Vector3 DensityShape::sample_motion(const std::vector<Vector3> &v, const Vector3 &p_grid) const {
	if (v.empty()) {
		return Vector3();
	}
	const Vector3 hi((float)(size.x - 1), (float)(size.y - 1), (float)(size.z - 1));
	const Vector3 c = p_grid.clamp(Vector3(), hi);
	const int x0 = std::min((int)c.x, size.x - 1), y0 = std::min((int)c.y, size.y - 1), z0 = std::min((int)c.z, size.z - 1);
	const int x1 = std::min(x0 + 1, size.x - 1), y1 = std::min(y0 + 1, size.y - 1), z1 = std::min(z0 + 1, size.z - 1);
	const float fx = c.x - x0, fy = c.y - y0, fz = c.z - z0;
	auto lerp = [](const Vector3 &a, const Vector3 &b, float t) { return a + (b - a) * t; };
	return lerp(lerp(lerp(v[index(x0, y0, z0)], v[index(x1, y0, z0)], fx), lerp(v[index(x0, y1, z0)], v[index(x1, y1, z0)], fx), fy),
			lerp(lerp(v[index(x0, y0, z1)], v[index(x1, y0, z1)], fx), lerp(v[index(x0, y1, z1)], v[index(x1, y1, z1)], fx), fy), fz);
}

float DensityShape::sample_pose(const Vector3 &p_grid, double p_pose, const PackedFloat32Array &p_weights) const {
	if (keys.empty()) {
		return -DENSITY_CLAMP;
	}
	std::lock_guard<std::recursive_mutex> lock(frame_mutex);
	Blend blend;
	pose_blend(p_pose, p_weights, blend);
	return sample_blend(p_grid, blend);
}

float DensityShape::sample_blend(const Vector3 &p_grid, const Blend &p_blend) const {
	if (keys.empty()) {
		return -DENSITY_CLAMP;
	}
	std::lock_guard<std::recursive_mutex> lock(frame_mutex);
	float d = 0.0f;
	for (int i = 0; i < p_blend.count; i++) {
		d += sample_grid(frame(p_blend.frames[i]).density, p_grid) * p_blend.weights[i];
	}
	return d;
}

double DensityShape::get_blend_density(const Vector3 &p_local, const Blend &p_blend) const {
	return sample_blend((p_local - origin) / (float)voxel_size, p_blend) * voxel_size;
}

Vector3 DensityShape::get_blend_normal(const Vector3 &p_local, const Blend &p_blend) const {
	const Vector3 g = (p_local - origin) / (float)voxel_size;
	const float h = 0.5f;
	const Vector3 grad(sample_blend(g + Vector3(h, 0, 0), p_blend) - sample_blend(g - Vector3(h, 0, 0), p_blend),
			sample_blend(g + Vector3(0, h, 0), p_blend) - sample_blend(g - Vector3(0, h, 0), p_blend),
			sample_blend(g + Vector3(0, 0, h), p_blend) - sample_blend(g - Vector3(0, 0, h), p_blend));
	return grad.length_squared() > 1e-12f ? -grad.normalized() : Vector3(0, 1, 0);
}

double DensityShape::get_density(const Vector3 &p_local, double p_pose, const PackedFloat32Array &p_weights) const {
	return sample_pose((p_local - origin) / (float)voxel_size, p_pose, p_weights) * voxel_size;
}

double DensityShape::get_material_density_at(const Vector3 &p_local) const {
	const int m = get_material_at(p_local);
	return m >= 0 ? material_physics((MaterialId)m).density : 1.0;
}

Vector3 DensityShape::get_normal(const Vector3 &p_local, double p_pose, const PackedFloat32Array &p_weights) const {
	const Vector3 g = (p_local - origin) / (float)voxel_size;
	const float h = 0.5f;
	const PackedFloat32Array &w = p_weights;
	const Vector3 grad(sample_pose(g + Vector3(h, 0, 0), p_pose, w) - sample_pose(g - Vector3(h, 0, 0), p_pose, w),
			sample_pose(g + Vector3(0, h, 0), p_pose, w) - sample_pose(g - Vector3(0, h, 0), p_pose, w),
			sample_pose(g + Vector3(0, 0, h), p_pose, w) - sample_pose(g - Vector3(0, 0, h), p_pose, w));
	return grad.length_squared() > 1e-12f ? -grad.normalized() : Vector3(0, 1, 0);
}

int DensityShape::get_material_at(const Vector3 &p_local, double p_pose, const PackedFloat32Array &p_weights) const {
	if (materials.empty()) {
		return -1;
	}
	const Vector3 g = (p_local - origin) / (float)voxel_size;
	const int x = std::clamp((int)std::lround(g.x), 0, size.x - 1), y = std::clamp((int)std::lround(g.y), 0, size.y - 1),
			  z = std::clamp((int)std::lround(g.z), 0, size.z - 1);
	if (has_motion()) {
		std::lock_guard<std::recursive_mutex> lock(frame_mutex);
		Blend blend;
		pose_blend(p_pose, p_weights, blend);
		const Frame &f = frame(blend.frames[0]);
		if (!f.materials.empty()) {
			return f.materials[index(x, y, z)];
		}
	}
	return materials[index(x, y, z)];
}

Vector3 DensityShape::rest_grid_point(const Vector3 &p_grid, double p_pose, const PackedFloat32Array &p_weights) const {
	if (!has_motion()) {
		return p_grid;
	}
	std::lock_guard<std::recursive_mutex> lock(frame_mutex);
	Blend blend;
	pose_blend(p_pose, p_weights, blend);
	const Vector3i last = size - Vector3i(1, 1, 1);
	const Vector3i g = Vector3i(p_grid.round()).clamp(Vector3i(), last);
	auto from = [&](int p_frame) {
		const Frame &f = frame(p_frame);
		if (f.to_rest.empty()) {
			return p_grid;
		}
		// Off the shape (the air round it doesn't move with it), from the
		// nearest solid point: what's added there is added onto that, and
		// moves as it does (inherit_motion()).
		Vector3i anchor = g;
		if (f.density[index(g.x, g.y, g.z)] <= 0.0f) {
			const int R = (int)std::ceil(DENSITY_CLAMP);
			float best = INFINITY;
			for (int z = std::max(g.z - R, 0); z <= std::min(g.z + R, last.z); z++) {
				for (int y = std::max(g.y - R, 0); y <= std::min(g.y + R, last.y); y++) {
					for (int x = std::max(g.x - R, 0); x <= std::min(g.x + R, last.x); x++) {
						const float d2 = (Vector3((float)x, (float)y, (float)z) - p_grid).length_squared();
						if (d2 < best && f.density[index(x, y, z)] > 0.0f) {
							best = d2;
							anchor = Vector3i(x, y, z);
						}
					}
				}
			}
		}
		// The anchor's rest point, moved as far as p_grid is from it.
		return f.to_rest[index(anchor.x, anchor.y, anchor.z)] + (p_grid - Vector3(anchor));
	};
	Vector3 r;
	for (int i = 0; i < blend.count; i++) {
		r += from(blend.frames[i]) * blend.weights[i];
	}
	return r;
}

Vector3 DensityShape::rest_point(const Vector3 &p_local, double p_pose, const PackedFloat32Array &p_weights) const {
	const float vs = (float)voxel_size;
	return origin + rest_grid_point((p_local - origin) / vs, p_pose, p_weights) * vs;
}

Vector3 DensityShape::key_point(const Vector3 &p_rest, int p_key) const {
	if (p_key < 0 || p_key >= (int)keys.size()) {
		return p_rest;
	}
	const float vs = (float)voxel_size;
	const Vector3 g = (p_rest - origin) / vs;
	return origin + (g + sample_motion(keys[p_key].motion, g)) * vs;
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
	const size_t old_count = count(), new_count = (size_t)nsize.x * nsize.y * nsize.z;
	auto new_index = [&](int x, int y, int z) { return ((size_t)z * nsize.y + y) * nsize.x + x; };
	// Old point for each new one, and whether it's inside the old grid.
	auto remap = [&](auto &r_values, auto p_outside, bool p_extend) {
		using T = typename std::decay_t<decltype(r_values)>::value_type;
		if (r_values.size() != old_count) {
			return;
		}
		std::vector<T> result(new_count, p_outside);
		for (int z = 0; z < nsize.z; z++) {
			for (int y = 0; y < nsize.y; y++) {
				for (int x = 0; x < nsize.x; x++) {
					const Vector3i old = Vector3i(x, y, z) + nlo;
					const Vector3i c = old.clamp(Vector3i(), size - Vector3i(1, 1, 1));
					if (old == c || p_extend) {
						result[new_index(x, y, z)] = r_values[index(c.x, c.y, c.z)];
					}
				}
			}
		}
		r_values = std::move(result);
	};
	auto remap_all = [&](std::vector<float> &r_rest, std::vector<Key> &r_keys, std::vector<MaterialId> &r_materials) {
		remap(r_rest, -DENSITY_CLAMP, false);
		for (Key &k : r_keys) {
			remap(k.motion, Vector3(), true); // new air moves as the edge it faces
			remap(k.residual, 0.0f, false);
		}
		remap(r_materials, (MaterialId)MAT_STONE, true); // new air keeps the material of the edge it faces
	};
	remap_all(rest, keys, materials);
	remap_all(grab_rest, grab_keys, grab_materials);
	remap(grab_key_density, -DENSITY_CLAMP, false);
	origin += Vector3(nlo) * (float)voxel_size;
	size = nsize;
	version++; // (frames of the new size)
}

void DensityShape::coarsen() {
	// Points 0, 2, 4... of the old grid, one more if needed so the last old
	// point is still covered (sampled past the edge as sample_grid() does).
	const Vector3i nsize = (size - Vector3i(1, 1, 1)) / 2 + Vector3i(1, 1, 1) + Vector3i(size.x % 2 == 0, size.y % 2 == 0, size.z % 2 == 0);
	const Vector3i n = nsize.clamp(Vector3i(1, 1, 1), Vector3i(MAX_SIZE, MAX_SIZE, MAX_SIZE));
	const size_t ncount = (size_t)n.x * n.y * n.z;
	const size_t old_count = count();
	auto coarse_density = [&](std::vector<float> &r_values) {
		if (r_values.size() != old_count) {
			return;
		}
		std::vector<float> result(ncount);
		for (int z = 0; z < n.z; z++) {
			for (int y = 0; y < n.y; y++) {
				for (int x = 0; x < n.x; x++) {
					const Vector3i o(x * 2, y * 2, z * 2);
					const Vector3i c = o.clamp(Vector3i(), size - Vector3i(1, 1, 1));
					// Distances in the new (twice as big) voxels.
					const float d = r_values[index(c.x, c.y, c.z)] - Vector3(o - c).length();
					result[((size_t)z * n.y + y) * n.x + x] = std::clamp(d * 0.5f, -DENSITY_CLAMP, DENSITY_CLAMP);
				}
			}
		}
		density_redistance(result, n);
		r_values = std::move(result);
	};
	// Every other point, halved (in the new voxels) if p_scale.
	auto coarse = [&](auto &r_values, bool p_scale) {
		using T = typename std::decay_t<decltype(r_values)>::value_type;
		if (r_values.size() != old_count) {
			return;
		}
		std::vector<T> result(ncount);
		for (int z = 0; z < n.z; z++) {
			for (int y = 0; y < n.y; y++) {
				for (int x = 0; x < n.x; x++) {
					const Vector3i c = Vector3i(x * 2, y * 2, z * 2).clamp(Vector3i(), size - Vector3i(1, 1, 1));
					T v = r_values[index(c.x, c.y, c.z)];
					if constexpr (!std::is_same_v<T, MaterialId>) {
						if (p_scale) {
							v = v * 0.5f;
						}
					}
					result[((size_t)z * n.y + y) * n.x + x] = v;
				}
			}
		}
		r_values = std::move(result);
	};
	auto coarse_all = [&](std::vector<float> &r_rest, std::vector<Key> &r_keys, std::vector<MaterialId> &r_materials) {
		coarse_density(r_rest);
		for (Key &k : r_keys) {
			coarse(k.motion, true);
			coarse(k.residual, true);
		}
		coarse(r_materials, false);
	};
	coarse_all(rest, keys, materials);
	coarse_all(grab_rest, grab_keys, grab_materials);
	coarse_density(grab_key_density);
	size = n;
	voxel_size *= 2.0;
	version++;
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

bool DensityShape::brush_grid(std::vector<float> &r_values, std::vector<MaterialId> &r_materials, const Vector3 &c, float r, float s, int p_mode, int p_material, Vector3i &lo, Vector3i &hi) {
	enum { ADD, SUBTRACT, SMOOTH, FLATTEN, PAINT, INFLATE, PINCH, LAYER, CLAY, CLONE };
	const float C = DENSITY_CLAMP;
	const bool sphere_mode = p_mode == ADD || p_mode == SUBTRACT;
	const float reach = sphere_mode ? r + C : (p_mode == CLAY ? r * 1.42f + 1.0f : r + 1.0f);
	for (int a = 0; a < 3; a++) {
		lo[a] = std::max((int)std::floor(c[a] - reach), 0);
		hi[a] = std::min((int)std::ceil(c[a] + reach), size[a] - 1);
		if (lo[a] > hi[a]) {
			return false;
		}
	}
	const float blend = std::clamp(s / r, 0.0f, 1.0f);
	const std::vector<MaterialId> materials_before = r_materials;
	const std::vector<float> before = r_values;
	std::vector<float> &after = r_values;
	std::vector<uint8_t> needs_material; // see density_spread_materials()
	auto at = [&](int x, int y, int z) {
		return before[index(std::clamp(x, 0, size.x - 1), std::clamp(y, 0, size.y - 1), std::clamp(z, 0, size.z - 1))];
	};
	Vector3 plane_normal;
	Vector3 plane_point = c;
	if (p_mode == FLATTEN && brush_local.plane_set) {
		plane_point = brush_local.plane_point;
		plane_normal = brush_local.plane_normal;
	} else if (p_mode == FLATTEN || p_mode == PINCH || p_mode == CLAY) {
		const float h = 0.5f;
		const Vector3 grad(sample_grid(before, c + Vector3(h, 0, 0)) - sample_grid(before, c - Vector3(h, 0, 0)),
				sample_grid(before, c + Vector3(0, h, 0)) - sample_grid(before, c - Vector3(0, h, 0)),
				sample_grid(before, c + Vector3(0, 0, h)) - sample_grid(before, c - Vector3(0, 0, h)));
		plane_normal = grad.length_squared() > 1e-12f ? -grad.normalized() : Vector3(0, 1, 0);
	}
	const float clay_depth = r * 0.2f * (s < 0.0f ? -1.0f : 1.0f);
	const float layer = brush_options().layer_height / (float)voxel_size * (s < 0.0f ? -1.0f : 1.0f);
	const Vector3 last_point = Vector3(size - Vector3i(1, 1, 1));
	bool any = false;
	for (int z = lo.z; z <= hi.z; z++) {
		for (int y = lo.y; y <= hi.y; y++) {
			for (int x = lo.x; x <= hi.x; x++) {
				const Vector3 p((float)x, (float)y, (float)z);
				const size_t i = index(x, y, z);
				const float v = before[i];
				const float dist = brush_distance(p - c);
				switch (p_mode) {
					case ADD:
						after[i] = v + (std::max(v, std::min(r - dist, C)) - v) * blend * sphere_brush_weight(dist / r);
						if (p_material >= 0 && after[i] > v + 1e-4f && after[i] > -2.0f) {
							r_materials[i] = (MaterialId)p_material;
						}
						break;
					case SUBTRACT:
						after[i] = v + (std::min(v, std::max(dist - r, -C)) - v) * blend * sphere_brush_weight(dist / r);
						break;
					case SMOOTH: {
						const float avg = (v + at(x - 1, y, z) + at(x + 1, y, z) + at(x, y - 1, z) + at(x, y + 1, z) + at(x, y, z - 1) + at(x, y, z + 1)) / 7.0f;
						after[i] = std::clamp(v + (avg - v) * std::clamp(s * brush_falloff(dist / r), -0.5f, 1.0f), -C, C);
					} break;
					case FLATTEN: {
						const float target = std::clamp(-(p - plane_point).dot(plane_normal), -C, C);
						after[i] = flatten_limit(v, std::clamp(v + (target - v) * std::clamp(s * brush_falloff(dist / r), -0.5f, 1.0f), -C, C));
					} break;
					case INFLATE:
						if (std::abs(v) < C) {
							after[i] = std::clamp(v + s * brush_falloff(dist / r), -C, C);
						}
						break;
					case PINCH: {
						const float f = brush_falloff(dist / r);
						if (f > 0.0f) {
							const Vector3 d = p - c;
							const Vector3 along = plane_normal * d.dot(plane_normal);
							const Vector3 q = c + along + (d - along) * (1.0f + std::clamp(s / r, -0.3f, 0.3f) * f);
							// From the grid as it was before this brush.
							after[i] = sample_grid(before, q.clamp(Vector3(), Vector3(size - Vector3i(1, 1, 1))));
						}
					} break;
					case PAINT:
						if (dist <= r && std::abs(v) < 2.5f) {
							r_materials[i] = (MaterialId)p_material;
						}
						break;
					case LAYER: {
						// Up to `layer` above the surface as the stroke found it.
						const float f = brush_falloff(dist / r);
						if (f > 0.0f) {
							const float base = brush_local.layer_from != nullptr && brush_local.layer_from->size() == before.size() ? (*brush_local.layer_from)[i] : v;
							// (Not from "at least CLAMP away": see SculptTerrain.)
							if (layer > 0.0f ? base <= -C + 0.01f : base >= C - 0.01f) {
								break;
							}
							const float target = std::clamp(base + layer * f, -C, C);
							const float moved = v + (target - v) * std::min(std::abs(s), 1.0f);
							after[i] = s >= 0.0f ? std::max(v, moved) : std::min(v, moved);
						}
					} break;
					case CLAY: {
						const Vector3 l = strip_frame(p - c, plane_normal, brush_local.stroke_direction);
						const float f = brush_falloff(std::max(std::abs(l.x), std::abs(l.y)) / r);
						if (f > 0.0f && std::abs(l.z) < r) {
							const float target = std::clamp(clay_depth - l.z, -C, C);
							const float moved = v + (target - v) * std::min(std::abs(s) * f, 1.0f);
							after[i] = s >= 0.0f ? std::max(v, moved) : std::min(v, moved);
							if (p_material >= 0 && after[i] > v + 1e-4f && after[i] > -2.0f) {
								r_materials[i] = (MaterialId)p_material;
							}
						}
					} break;
					case CLONE: {
						const float f = brush_falloff(dist / r);
						const Vector3 q = p + brush_local.clone_offset;
						if (f > 0.0f && brush_local.clone_offset.length_squared() > 0.25f) {
							const float from = sample_grid(before, q.clamp(Vector3(), last_point));
							after[i] = v + (from - v) * std::min(std::abs(s) * f, 1.0f);
							if (f > 0.3f && from > -2.5f) {
								const Vector3i qi = Vector3i(q.round()).clamp(Vector3i(), size - Vector3i(1, 1, 1));
								r_materials[i] = materials_before[index(qi.x, qi.y, qi.z)];
							}
						}
					} break;
				}
				if ((p_mode == SMOOTH || p_mode == FLATTEN || p_mode == INFLATE || p_mode == LAYER || (p_mode == CLAY && p_material < 0)) && v <= 0.0f && after[i] > 0.0f) {
					// Raised from air: the material of the solid neighbour
					// nearest the surface (the top coat, not what's under
					// it), or with none, of the nearest ground it grew from.
					const Vector3i ns[6] = { { x - 1, y, z }, { x + 1, y, z }, { x, y - 1, z }, { x, y + 1, z }, { x, y, z - 1 }, { x, y, z + 1 } };
					float least = INFINITY;
					for (const Vector3i &q : ns) {
						const Vector3i cq = q.clamp(Vector3i(), size - Vector3i(1, 1, 1));
						const float b = before[index(cq.x, cq.y, cq.z)];
						if (b > 0.0f && b < least) {
							least = b;
							r_materials[i] = materials_before[index(cq.x, cq.y, cq.z)];
						}
					}
					if (least == INFINITY) {
						if (needs_material.empty()) {
							needs_material.assign(after.size(), 0);
						}
						needs_material[i] = 1;
					}
				}
				any |= after[i] != v;
			}
		}
	}
	if (!needs_material.empty()) {
		density_spread_materials(size, after, needs_material, r_materials);
	}
	if (p_mode != PAINT && p_mode != SMOOTH && p_mode != LAYER && p_mode != CLAY) {
		// Repair the distances as the terrain does (commit_edit()), around
		// the brush only. (Not after smoothing: an average of distances is
		// still a safe one, and the repair's own error, every frame, would
		// outweigh a gentle smoothing: SculptTerrain::apply_brush().)
		repair(after, before, lo, hi);
	}
	any |= r_materials != materials_before;
	return any;
}

void DensityShape::inherit_motion(const std::vector<float> &p_before, const Vector3i &p_lo, const Vector3i &p_hi) {
	if (!has_motion()) {
		return;
	}
	// The box the edit (and its distance repair) can have changed, and round
	// it, room to find what it was added onto.
	const int M = 2 * ((int)std::ceil(DENSITY_CLAMP) + 1);
	const Vector3i last = size - Vector3i(1, 1, 1), m(M, M, M);
	const Vector3i lo = (p_lo - m).clamp(Vector3i(), last), hi = (p_hi + m).clamp(Vector3i(), last);
	const Vector3i n = hi - lo + Vector3i(1, 1, 1);
	auto sub_index = [&](int x, int y, int z) { return ((size_t)(z - lo.z) * n.y + (y - lo.y)) * n.x + (x - lo.x); };
	// Breadth first from what was solid: each point's nearest solid point.
	std::vector<int64_t> source((size_t)n.x * n.y * n.z, -1);
	std::vector<Vector3i> queue;
	for (int z = lo.z; z <= hi.z; z++) {
		for (int y = lo.y; y <= hi.y; y++) {
			for (int x = lo.x; x <= hi.x; x++) {
				const size_t i = index(x, y, z);
				if (p_before[i] > 0.0f) {
					source[sub_index(x, y, z)] = (int64_t)i;
					queue.push_back(Vector3i(x, y, z));
				}
			}
		}
	}
	if (queue.empty()) {
		return; // nothing to follow: it moves as the air there does
	}
	for (size_t q = 0; q < queue.size(); q++) {
		const Vector3i p = queue[q];
		const int64_t from = source[sub_index(p.x, p.y, p.z)];
		const Vector3i ns[6] = { { p.x - 1, p.y, p.z }, { p.x + 1, p.y, p.z }, { p.x, p.y - 1, p.z }, { p.x, p.y + 1, p.z }, { p.x, p.y, p.z - 1 }, { p.x, p.y, p.z + 1 } };
		for (const Vector3i &o : ns) {
			if (o.x < lo.x || o.y < lo.y || o.z < lo.z || o.x > hi.x || o.y > hi.y || o.z > hi.z) {
				continue;
			}
			int64_t &s = source[sub_index(o.x, o.y, o.z)];
			if (s < 0) {
				s = from;
				queue.push_back(o);
			}
		}
	}
	for (int z = lo.z; z <= hi.z; z++) {
		for (int y = lo.y; y <= hi.y; y++) {
			for (int x = lo.x; x <= hi.x; x++) {
				const size_t i = index(x, y, z);
				const int64_t from = source[sub_index(x, y, z)];
				if (p_before[i] > 0.0f || rest[i] == p_before[i] || from < 0) {
					continue;
				}
				for (Key &k : keys) {
					if (!k.motion.empty()) {
						k.motion[i] = k.motion[from];
					}
				}
			}
		}
	}
}

bool DensityShape::apply_brush(const Vector3 &p_center, double p_radius, double p_strength, int p_mode, int p_material, int p_key, double p_pose, const PackedFloat32Array &p_weights) {
	if (keys.empty() || p_radius <= 0.0 || p_mode < 0 || p_mode > 9 || (p_mode == 4 && p_material < 0)) {
		return false;
	}
	enum { ADD = 0, LAYER = 7 };
	const bool one_key = p_key >= 0 && p_key < (int)keys.size();
	// Where it edits: the rest shape under the brush, or that in the key's pose.
	Vector3 target = p_center;
	if (has_motion()) {
		const Vector3 rp = rest_point(p_center, p_pose, p_weights);
		target = one_key ? key_point(rp, p_key) : rp;
	}
	// (Inflate, layer and clay strips build out too, unless carving.)
	if (p_mode == ADD || ((p_mode == 5 || p_mode == LAYER || p_mode == 8) && p_strength > 0.0)) {
		// Room for what it builds, plus a little air: past MAX_SIZE points a
		// side, coarser voxels rather than a wall it can't build past.
		const Vector3 room = Vector3(1, 1, 1) * (float)(p_radius + 2.0 * voxel_size);
		fit(target - room, target + room, true);
	}
	const float vs = (float)voxel_size;
	const float r = (float)p_radius / vs, s = (float)p_strength / vs;
	const Vector3 c = (target - origin) / vs;
	Vector3i lo, hi;
	bool any = false;
	// Layer measures from the grid as the stroke found it (taken at its
	// first dab: a new stroke, another key, or a resized grid takes it anew).
	brush_local.layer_from = nullptr;
	if (p_mode == LAYER) {
		const uint32_t serial = brush_options().stroke_serial;
		if (layer_serial != serial || layer_key != p_key || layer_base.size() != count()) {
			layer_serial = serial;
			layer_key = p_key;
			layer_base = one_key ? frame(key_frame(p_key)).density : rest;
		}
		brush_local.layer_from = &layer_base;
	}
	if (!one_key) {
		const std::vector<float> before = rest;
		any = brush_grid(rest, materials, c, r, s, p_mode, p_material, lo, hi);
		if (any) {
			inherit_motion(before, lo, hi);
		}
	} else {
		// The key as it looks, edited; what changed becomes its residual.
		const Frame &f = frame(key_frame(p_key));
		std::vector<float> density = f.density;
		std::vector<MaterialId> posed = f.materials.empty() ? materials : f.materials;
		const std::vector<Vector3> to_rest = f.to_rest;
		const std::vector<float> density_before = density;
		const std::vector<MaterialId> posed_before = posed;
		any = brush_grid(density, posed, c, r, s, p_mode, p_material, lo, hi);
		if (any) {
			Key &k = keys[p_key];
			const size_t n = count();
			if (k.motion.empty()) {
				// Not moving: the key is exactly rest + residual.
				k.residual.assign(n, 0.0f);
				for (size_t i = 0; i < n; i++) {
					k.residual[i] = density[i] - rest[i];
				}
			} else {
				if (k.residual.empty()) {
					k.residual.assign(n, 0.0f);
				}
				for (size_t i = 0; i < n; i++) {
					k.residual[i] += density[i] - density_before[i];
				}
			}
			if (all_zero(k.residual)) {
				k.residual.clear();
			}
			// Paint lands on the rest shape's points (materials are shared).
			const Vector3i last = size - Vector3i(1, 1, 1);
			for (size_t i = 0; i < n; i++) {
				if (posed[i] == posed_before[i]) {
					continue;
				}
				if (to_rest.empty()) {
					materials[i] = posed[i];
				} else {
					const Vector3i q = Vector3i(to_rest[i].round()).clamp(Vector3i(), last);
					materials[index(q.x, q.y, q.z)] = posed[i];
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

bool DensityShape::paint_color(const Vector3 &p_center, double p_radius, const Color &p_color, double p_opacity, double p_hardness, double p_pose, const PackedFloat32Array &p_weights, double p_scatter, int p_seed) {
	if (keys.empty() || p_radius <= 0.0) {
		return false;
	}
	// Colours are the rest shape's: painted where the brush is on it.
	const Vector3 target = has_motion() ? rest_point(p_center, p_pose, p_weights) : p_center;
	const float vs = (float)voxel_size, r = (float)p_radius / vs;
	const Vector3 c = (target - origin) / vs;
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
	auto target_of = [](const Color &p_c, float r_target[3]) {
		r_target[0] = (float)std::lround(std::clamp(p_c.r, 0.0f, 1.0f) * 31.0f);
		r_target[1] = (float)std::lround(std::clamp(p_c.g, 0.0f, 1.0f) * 31.0f);
		r_target[2] = (float)std::lround(std::clamp(p_c.b, 0.0f, 1.0f) * 31.0f);
	};
	float target_color[3];
	target_of(p_color, target_color);
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
				// The skin of the rest shape, or of a key that doesn't move
				// (its surface is at these points too: else it shows the old colour).
				float nearest = std::abs(rest[i]);
				for (const Key &k : keys) {
					if (k.motion.empty() && !k.residual.empty()) {
						nearest = std::min(nearest, std::abs(rest[i] + k.residual[i]));
					}
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
				if (p_scatter > 0.0) {
					target_of(SculptTerrain::scatter_color(p_color, p_scatter, x, y, z, p_seed), target_color);
				}
				int channel[3];
				for (int a = 0; a < 3; a++) {
					channel[a] = std::clamp((int)std::floor(from[a] + (target_color[a] - from[a]) * t + d), 0, 31);
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

int DensityShape::paint_fill(const Vector3 &p_center, double p_threshold, int p_material, const Color &p_color, double p_opacity, double p_scatter, int p_seed, double p_pose, const PackedFloat32Array &p_weights) {
	if (keys.empty() || rest.empty()) {
		return 0;
	}
	const Vector3 target = has_motion() ? rest_point(p_center, p_pose, p_weights) : p_center;
	const Vector3 c = (target - origin) / (float)voxel_size;
	const SculptTerrain *terrain = SculptTerrain::get_singleton();
	auto color_of = [&](MaterialId m) {
		return terrain != nullptr ? terrain->get_material_color(m)
								  : (is_color_material(m) ? Color(((m >> 10) & 31) / 31.0f, ((m >> 5) & 31) / 31.0f, (m & 31) / 31.0f) : Color(0.5f, 0.5f, 0.5f));
	};
	// How near a surface a point is: the rest shape's, or a key's that
	// doesn't move (as paint_color()).
	auto nearest = [&](size_t i) {
		float v = std::abs(rest[i]);
		for (const Key &k : keys) {
			if (k.motion.empty() && !k.residual.empty()) {
				v = std::min(v, std::abs(rest[i] + k.residual[i]));
			}
		}
		return v;
	};
	const float SKIN = 2.5f;
	auto inside = [&](const Vector3i &q) { return q.x >= 0 && q.y >= 0 && q.z >= 0 && q.x < size.x && q.y < size.y && q.z < size.z; };
	// From the most solid point next to where the ray hit.
	const Vector3i centre((int)std::lround(c.x), (int)std::lround(c.y), (int)std::lround(c.z));
	Vector3i start = centre;
	float best = -1e9f;
	for (int dz = -1; dz <= 1; dz++) {
		for (int dy = -1; dy <= 1; dy++) {
			for (int dx = -1; dx <= 1; dx++) {
				const Vector3i q = centre + Vector3i(dx, dy, dz);
				if (inside(q) && rest[index(q.x, q.y, q.z)] > best) {
					best = rest[index(q.x, q.y, q.z)];
					start = q;
				}
			}
		}
	}
	if (best <= 0.0f) {
		return 0;
	}
	const Color want = color_of(materials[index(start.x, start.y, start.z)]);
	std::vector<uint8_t> seen(count(), 0);
	auto accept = [&](size_t i) {
		if (rest[i] <= 0.0f || nearest(i) >= SKIN) {
			return false;
		}
		const Color m = color_of(materials[i]);
		const float dr = m.r - want.r, dg = m.g - want.g, db = m.b - want.b;
		return std::sqrt(dr * dr + dg * dg + db * db) <= (float)p_threshold + 1e-4f;
	};
	const Vector3i steps[6] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
	std::vector<Vector3i> found;
	std::vector<int> depth; // air: steps out from the solid found
	seen[index(start.x, start.y, start.z)] = 1;
	found.push_back(start);
	depth.push_back(0);
	for (size_t k = 0; k < found.size(); k++) {
		const Vector3i p = found[k];
		const bool solid = depth[k] == 0;
		for (const Vector3i &s : steps) {
			const Vector3i q = p + s;
			if (!inside(q)) {
				continue;
			}
			const size_t i = index(q.x, q.y, q.z);
			if (seen[i]) {
				continue;
			}
			if (solid && accept(i)) {
				seen[i] = 1;
				found.push_back(q);
				depth.push_back(0);
			} else if (rest[i] <= 0.0f && nearest(i) < SKIN && depth[k] < 2) {
				// The skin of air just outside it, two steps deep.
				seen[i] = 1;
				found.push_back(q);
				depth.push_back(depth[k] + 1);
			}
		}
	}
	const float opacity = std::clamp((float)p_opacity, 0.0f, 1.0f);
	int painted = 0;
	for (const Vector3i &p : found) {
		const size_t i = index(p.x, p.y, p.z);
		const MaterialId next = p_material >= 0 ? (MaterialId)p_material
												: (MaterialId)SculptTerrain::color_to_material(color_of(materials[i]).lerp(SculptTerrain::scatter_color(p_color, p_scatter, p.x, p.y, p.z, p_seed), opacity));
		if (next != materials[i]) {
			materials[i] = next;
			painted++;
		}
	}
	if (painted > 0) {
		edited = true;
		changed();
	}
	return painted;
}

// --- For bodies and drawing ----------------------------------------------------------

const std::vector<Vector4> &DensityShape::probes(int p_frame) const {
	std::lock_guard<std::recursive_mutex> lock(frame_mutex);
	if (probe_version != version || probe_cache.size() > 256) {
		probe_cache.clear();
		probe_version = version;
	}
	static const std::vector<Vector4> none;
	if (keys.empty()) {
		return none;
	}
	std::vector<Vector4> &list = probe_cache[p_frame];
	if (!list.empty()) {
		return list;
	}
	const std::vector<Vector4> skin = skin_points(p_frame);
	// At most ~256: every n-th (in grid order, which spreads them evenly enough).
	const size_t step = std::max<size_t>(1, (skin.size() + 255) / 256);
	for (size_t i = 0; i < skin.size(); i += step) {
		list.push_back(skin[i]);
	}
	return list;
}

std::vector<Vector4> DensityShape::skin_points(int p_frame) const {
	std::vector<Vector4> skin;
	if (keys.empty()) {
		return skin;
	}
	// Solid points next to air (the skin), each a sphere as big as its
	// distance to the surface, so together they stand in for the surface.
	std::lock_guard<std::recursive_mutex> lock(frame_mutex);
	const std::vector<float> &v = frame(p_frame).density;
	if (v.size() != count()) {
		return skin;
	}
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
	if (rest.size() == count()) {
		for (int z = 0; z < size.z; z++) {
			for (int y = 0; y < size.y; y++) {
				for (int x = 0; x < size.x; x++) {
					if (rest[index(x, y, z)] > 0.0f) {
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

void DensityShape::fill_slice(int p_frame, int p_z, uint8_t *r_texels) const {
	std::lock_guard<std::recursive_mutex> lock(frame_mutex);
	const Frame *f = keys.empty() || p_frame < 0 ? nullptr : &frame(p_frame);
	if (f != nullptr && f->density.size() != count()) {
		f = nullptr;
	}
	const std::vector<MaterialId> &mats = f != nullptr && !f->materials.empty() ? f->materials : materials;
	for (int y = 0; y < MAX_SIZE; y++) {
		for (int x = 0; x < MAX_SIZE; x++) {
			uint8_t *t = r_texels + ((size_t)y * MAX_SIZE + x) * 3;
			if (f != nullptr && x < size.x && y < size.y && p_z < size.z) {
				const size_t i = index(x, y, p_z);
				t[0] = (uint8_t)(quantize_density(f->density[i]) + 128);
				render_bytes(mats[i], t + 1);
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
	ClassDB::bind_method(D_METHOD("set_rest", "bytes"), &DensityShape::set_rest);
	ClassDB::bind_method(D_METHOD("get_rest"), &DensityShape::get_rest);
	ClassDB::bind_method(D_METHOD("set_key_data", "data"), &DensityShape::set_key_data);
	ClassDB::bind_method(D_METHOD("get_key_data"), &DensityShape::get_key_data);
	ClassDB::bind_method(D_METHOD("set_materials", "bytes"), &DensityShape::set_materials);
	ClassDB::bind_method(D_METHOD("get_materials"), &DensityShape::get_materials);
	ClassDB::bind_method(D_METHOD("set_edited", "edited"), &DensityShape::set_edited);
	ClassDB::bind_method(D_METHOD("is_edited"), &DensityShape::is_edited);
	ClassDB::bind_method(D_METHOD("get_key_count"), &DensityShape::get_key_count);
	ClassDB::bind_method(D_METHOD("has_motion"), &DensityShape::has_motion);
	ClassDB::bind_method(D_METHOD("add_key", "pose", "weights"), &DensityShape::add_key, DEFVAL(PackedFloat32Array()));
	ClassDB::bind_method(D_METHOD("remove_key", "key"), &DensityShape::remove_key);
	ClassDB::bind_method(D_METHOD("move_key", "from", "to"), &DensityShape::move_key);
	ClassDB::bind_method(D_METHOD("reset_key", "key"), &DensityShape::reset_key);
	ClassDB::bind_method(D_METHOD("set_key_name", "key", "name"), &DensityShape::set_key_name);
	ClassDB::bind_method(D_METHOD("get_key_name", "key"), &DensityShape::get_key_name);
	ClassDB::bind_method(D_METHOD("get_key_label", "key"), &DensityShape::get_key_label);
	ClassDB::bind_method(D_METHOD("set_key_names", "names"), &DensityShape::set_key_names);
	ClassDB::bind_method(D_METHOD("get_key_names"), &DensityShape::get_key_names);
	ClassDB::bind_method(D_METHOD("get_frame_count"), &DensityShape::get_frame_count);
	ClassDB::bind_static_method("DensityShape", D_METHOD("make_sphere", "radius", "material", "stripe_material", "voxel_size"), &DensityShape::make_sphere, DEFVAL(-1), DEFVAL(1.0));
	ClassDB::bind_method(D_METHOD("bake_meshes", "poses", "colors", "voxel_size", "margin"), &DensityShape::bake_meshes);
	ClassDB::bind_method(D_METHOD("duplicate_shape"), &DensityShape::duplicate_shape);
	ClassDB::bind_method(D_METHOD("resampled", "voxel_size"), &DensityShape::resampled);
	ClassDB::bind_method(D_METHOD("mirrored", "point", "normal"), &DensityShape::mirrored);
	ClassDB::bind_method(D_METHOD("to_dict"), &DensityShape::to_dict);
	ClassDB::bind_static_method("DensityShape", D_METHOD("from_dict", "dict"), &DensityShape::from_dict);
	ClassDB::bind_method(D_METHOD("get_density", "position", "pose", "weights"), &DensityShape::get_density, DEFVAL(0.0), DEFVAL(PackedFloat32Array()));
	ClassDB::bind_method(D_METHOD("get_normal", "position", "pose", "weights"), &DensityShape::get_normal, DEFVAL(0.0), DEFVAL(PackedFloat32Array()));
	ClassDB::bind_method(D_METHOD("get_material_at", "position", "pose", "weights"), &DensityShape::get_material_at, DEFVAL(0.0), DEFVAL(PackedFloat32Array()));
	ClassDB::bind_method(D_METHOD("rest_point", "position", "pose", "weights"), &DensityShape::rest_point, DEFVAL(PackedFloat32Array()));
	ClassDB::bind_method(D_METHOD("key_point", "rest_position", "key"), &DensityShape::key_point);
	ClassDB::bind_method(D_METHOD("apply_brush", "center", "radius", "strength", "mode", "material", "key", "pose", "weights"), &DensityShape::apply_brush, DEFVAL(-1), DEFVAL(-1), DEFVAL(0.0), DEFVAL(PackedFloat32Array()));
	ClassDB::bind_method(D_METHOD("paint_color", "center", "radius", "color", "opacity", "hardness", "pose", "weights", "scatter", "seed"), &DensityShape::paint_color, DEFVAL(0.0), DEFVAL(PackedFloat32Array()), DEFVAL(0.0), DEFVAL(0));
	ClassDB::bind_method(D_METHOD("paint_fill", "center", "threshold", "material", "color", "opacity", "scatter", "seed", "pose", "weights"), &DensityShape::paint_fill, DEFVAL(0.0), DEFVAL(PackedFloat32Array()));
	ClassDB::bind_method(D_METHOD("get_version"), &DensityShape::get_version);
	ClassDB::bind_method(D_METHOD("get_volume"), &DensityShape::get_volume);
	ClassDB::bind_method(D_METHOD("get_mass"), &DensityShape::get_mass);
	ClassDB::bind_method(D_METHOD("split_parts", "min_voxels"), &DensityShape::split_parts, DEFVAL(4));
	ClassDB::bind_static_method("DensityShape", D_METHOD("make_hull", "points", "material", "voxel_size"), &DensityShape::make_hull, DEFVAL(1.0));
	ClassDB::bind_static_method("DensityShape", D_METHOD("make_solids", "solids", "voxel_size"), &DensityShape::make_solids, DEFVAL(1.0));

	// Saved: size first (it sizes the rest). `keys` is how older files saved
	// the shape: still read, no longer written.
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3I, "size", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE), "set_size", "get_size");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "origin"), "set_origin", "get_origin");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "voxel_size", PROPERTY_HINT_RANGE, "0.01,16,0.01"), "set_voxel_size", "get_voxel_size");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "keys", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NONE), "set_keys", "get_keys");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_BYTE_ARRAY, "rest", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE), "set_rest", "get_rest");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "key_data", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE), "set_key_data", "get_key_data");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_STRING_ARRAY, "key_names", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE), "set_key_names", "get_key_names"); // (after key_data: names the keys it made)
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
	rest = std::move(p_density);
	keys.assign(1, Key());
	materials = std::move(p_materials);
	density_redistance(rest, size);
	edited = true;
	changed();
}

double DensityShape::get_volume() const {
	size_t solid = 0;
	for (float d : rest) {
		solid += d > 0.0f;
	}
	return solid * voxel_size * voxel_size * voxel_size;
}

void DensityShape::begin_grab() {
	grab_rest = rest;
	grab_keys = keys;
	grab_materials = materials;
	grab_key_density.clear();
	grab_center_offsets.clear();
	grab_has_box = false;
}

void DensityShape::end_grab() {
	grab_rest.clear();
	grab_keys.clear();
	grab_materials.clear();
	grab_key_density.clear();
	grab_center_offsets.clear();
	grab_has_box = false;
}

bool DensityShape::warp(const std::vector<Vector3> &p_centers, const std::vector<Vector3> &p_deltas, double p_radius, bool p_grab, int p_key, double p_pose, const PackedFloat32Array &p_weights) {
	const int count = (int)std::min(p_centers.size(), p_deltas.size());
	if (keys.empty() || count == 0 || p_radius <= 0.0 || (p_grab && (grab_rest.size() != rest.size() || grab_keys.size() != keys.size()))) {
		return false;
	}
	const bool one_key = p_key >= 0 && p_key < (int)keys.size();
	// Where each copy's centre edits (the brush is on the pose shown): the
	// rest shape there, or that in the key's pose. A grab works it out once
	// (the shape changes under it as it goes); a snake hook keeps it from
	// step to step while it's the only thing changing the shape.
	std::vector<Vector3> mapped(count);
	{
		auto offset_of = [&](const Vector3 &p_c) {
			if (!has_motion()) {
				return Vector3();
			}
			const Vector3 rp = rest_point(p_c, p_pose, p_weights);
			return (one_key ? key_point(rp, p_key) : rp) - p_c;
		};
		if (p_grab) {
			if ((int)grab_center_offsets.size() != count) {
				grab_center_offsets.resize(count);
				for (int i = 0; i < count; i++) {
					grab_center_offsets[i] = offset_of(p_centers[i]);
				}
			}
			for (int i = 0; i < count; i++) {
				mapped[i] = p_centers[i] + grab_center_offsets[i];
			}
		} else {
			bool follows = snake_version == version && (int)snake_centers.size() == count;
			for (int i = 0; follows && i < count; i++) {
				follows = (p_centers[i] - (snake_centers[i] + p_deltas[i])).length() < 0.01f * (float)p_radius + 1e-3f;
			}
			if (!follows) {
				snake_offsets.resize(count);
				for (int i = 0; i < count; i++) {
					snake_offsets[i] = offset_of(p_centers[i]);
				}
			}
			for (int i = 0; i < count; i++) {
				mapped[i] = p_centers[i] + snake_offsets[i];
			}
			snake_centers = p_centers;
		}
	}
	// Room for where it goes (the grid grows as for adding): past MAX_SIZE
	// points a side, coarser voxels (the grab's saved shape with them) rather
	// than a wall the shape stops at.
	{
		Vector3 lo_l(1e30f, 1e30f, 1e30f), hi_l(-1e30f, -1e30f, -1e30f);
		for (int i = 0; i < count; i++) {
			const Vector3 a = mapped[i], b = p_grab ? mapped[i] + p_deltas[i] : mapped[i] - p_deltas[i];
			lo_l = lo_l.min(a.min(b));
			hi_l = hi_l.max(a.max(b));
		}
		const Vector3 room = Vector3(1, 1, 1) * (float)(p_radius + 2.0 * voxel_size);
		fit(lo_l - room, hi_l + room, true);
	}
	const size_t n = this->count();
	if (p_grab && one_key && grab_key_density.size() != n) {
		// The key as it looked when the grab began: which of the places a
		// point could have come from was most solid.
		grab_key_density = frame(key_frame(p_key)).density;
	}
	const float vs = (float)voxel_size, r = (float)p_radius / vs, C = DENSITY_CLAMP;
	std::vector<Vector3> centers(count), deltas(count);
	for (int i = 0; i < count; i++) {
		centers[i] = (mapped[i] - origin) / vs;
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
	bool outside = false;
	for (int a = 0; a < 3; a++) {
		lo[a] = std::max((int)std::floor(blo[a]), 0);
		hi[a] = std::min((int)std::ceil(bhi[a]), size[a] - 1);
		outside |= lo[a] > hi[a];
	}
	// Trilinear from a grid of this size (air beyond it).
	auto sample_from = [&](const std::vector<float> &p_values, const Vector3 &q) {
		return sample_grid(p_values, q);
	};
	// Grab: where the material at p came from: p - delta * s, s =
	// falloff(|that - c| / r), the most solid (by p_solid) of the roots of
	// every copy (as the terrain's grab).
	auto grab_source = [&](const Vector3 &p, auto &&p_solid) {
		// Each copy that moves p offers the most solid of its roots; the
		// most solid of those wins. One that doesn't move it offers nothing
		// (its "stays put" mustn't win over another copy's move).
		float value = -INFINITY;
		Vector3 best_from = p;
		for (int cc = 0; cc < count; cc++) {
			const Vector3 &c = centers[cc], &delta = deltas[cc];
			// Only material within r of the centre's path can reach p;
			// otherwise s = 0 is the only root.
			const float dd = delta.length_squared();
			const float tt = dd > 0.0f ? std::clamp((p - c).dot(delta) / dd, 0.0f, 1.0f) : 0.0f;
			if ((p - c - delta * tt).length_squared() >= r * r) {
				continue;
			}
			auto h = [&](float s) { return falloff((p - delta * s - c).length() / r) - s; };
			const int SCAN = 12;
			float s0 = 0.0f, h0 = h(0.0f);
			bool copy_moves = false;
			float copy_value = h0 <= 0.0f ? p_solid(p) : -INFINITY;
			Vector3 copy_from = p;
			for (int j = 1; j <= SCAN; j++) {
				const float s1 = (float)j / SCAN, h1 = h(s1);
				if ((h0 > 0.0f) != (h1 > 0.0f)) {
					float a = s0, b = s1;
					for (int it = 0; it < 8; it++) {
						const float m = (a + b) * 0.5f;
						((h(m) > 0.0f) == (h0 > 0.0f) ? a : b) = m;
					}
					const Vector3 q = p - delta * ((a + b) * 0.5f);
					const float v = p_solid(q);
					if (v > copy_value) {
						copy_value = v;
						copy_from = q;
					}
					copy_moves = true;
				}
				s0 = s1;
				h0 = h1;
			}
			if (copy_moves && copy_value > value) {
				value = copy_value;
				best_from = copy_from;
			}
		}
		return best_from;
	};
	// Snake hook: how far a point is pulled this step.
	auto snake_displacement = [&](const Vector3 &p) {
		Vector3 displacement;
		for (int cc = 0; cc < count; cc++) {
			displacement += deltas[cc] * falloff((p - centers[cc]).length() / r);
		}
		return displacement;
	};
	bool any = false;
	if (one_key) {
		// One key: its motion moves, the rest shape stays. Each rest point
		// goes where the key had it, then as the brush moves what's there.
		Key &key = keys[p_key];
		const Key start = p_grab ? grab_keys[p_key] : key;
		if (key.motion.empty()) {
			key.motion.assign(n, Vector3());
		}
		for (int z = 0; z < size.z; z++) {
			for (int y = 0; y < size.y; y++) {
				for (int x = 0; x < size.x; x++) {
					const size_t i = index(x, y, z);
					const Vector3 from = start.motion.empty() ? Vector3() : start.motion[i];
					const Vector3 posed = Vector3((float)x, (float)y, (float)z) + from;
					Vector3 move;
					for (int cc = 0; cc < count; cc++) {
						move += deltas[cc] * falloff((posed - centers[cc]).length() / r);
					}
					const Vector3 to = from + move;
					any |= (to - key.motion[i]).length_squared() > 1e-10f;
					key.motion[i] = to;
				}
			}
		}
		// Its residual is at its posed points: moved with them.
		if (!start.residual.empty() && !outside) {
			const std::vector<float> source_residual = start.residual;
			for (int z = lo.z; z <= hi.z; z++) {
				for (int y = lo.y; y <= hi.y; y++) {
					for (int x = lo.x; x <= hi.x; x++) {
						const Vector3 p((float)x, (float)y, (float)z);
						const Vector3 q = p_grab ? grab_source(p, [&](const Vector3 &s) { return grab_key_density.size() == n ? sample_from(grab_key_density, s) : 0.0f; })
												 : p - snake_displacement(p);
						const Vector3 cq = q.clamp(Vector3(), Vector3(size - Vector3i(1, 1, 1)));
						// (trilinear: residuals aren't distances, no fall-off past the edge)
						key.residual[index(x, y, z)] = sample_from(source_residual, cq);
					}
				}
			}
		}
		if (all_zero(key.motion)) {
			key.motion.clear();
		}
	} else if (!outside) {
		// All keys: the rest shape is moved (and what moves carries its
		// motion in every key along).
		const std::vector<float> before = rest;
		const std::vector<float> &source_values = p_grab ? grab_rest : before;
		std::vector<MaterialId> materials_copy;
		if (!p_grab) {
			materials_copy = materials;
		}
		const std::vector<MaterialId> &materials_from = p_grab ? grab_materials : materials_copy;
		std::vector<std::vector<Vector3>> motion_copies;
		if (!p_grab) {
			for (const Key &k : keys) {
				motion_copies.push_back(k.motion);
			}
		}
		auto source = [&](const Vector3 &q) { return sample_from(source_values, q); };
		auto source_material = [&](const Vector3 &q) {
			const Vector3i i = Vector3i(q.round()).clamp(Vector3i(), size - Vector3i(1, 1, 1));
			return materials_from[index(i.x, i.y, i.z)];
		};
		const float thickness = 0.4f * r;
		for (int z = lo.z; z <= hi.z; z++) {
			for (int y = lo.y; y <= hi.y; y++) {
				for (int x = lo.x; x <= hi.x; x++) {
					const Vector3 p((float)x, (float)y, (float)z);
					const size_t i = index(x, y, z);
					Vector3 from;
					if (p_grab) {
						from = grab_source(p, source);
						rest[i] = source(from);
					} else {
						// Snake hook: pulled along a step, keeping a tendril's
						// thickness (overlapping copies add up).
						const Vector3 displacement = snake_displacement(p);
						float capsule = -C;
						for (int cc = 0; cc < count; cc++) {
							const Vector3 &c = centers[cc], &delta = deltas[cc];
							const Vector3 a = c - delta;
							const float t = delta.length_squared() > 0.0f ? std::clamp((p - a).dot(delta) / delta.length_squared(), 0.0f, 1.0f) : 0.0f;
							capsule = std::max(capsule, thickness - (p - (a + delta * t)).length());
						}
						if (displacement.length_squared() == 0.0f && capsule <= -C) {
							continue;
						}
						from = p - displacement;
						rest[i] = std::max(source(from), std::min(capsule, C));
					}
					materials[i] = source_material(from);
					for (size_t k = 0; k < keys.size(); k++) {
						if (!keys[k].motion.empty()) {
							keys[k].motion[i] = sample_motion(p_grab ? grab_keys[k].motion : motion_copies[k], from);
						}
					}
				}
			}
		}
		// Repair, around what moved (see apply_brush).
		repair(rest, before, lo, hi);
		any = rest != before;
	}
	if (any) {
		edited = true;
		changed();
	}
	if (!p_grab) {
		snake_version = version;
	}
	return any;
}

bool DensityShape::twist(const std::vector<Vector3> &p_centers, const std::vector<Basis> &p_bases, double p_radius, int p_key, double p_pose, const PackedFloat32Array &p_weights) {
	const int count = (int)std::min(p_centers.size(), p_bases.size());
	if (keys.empty() || count == 0 || p_radius <= 0.0 || grab_rest.size() != rest.size() || grab_keys.size() != keys.size()) {
		return false;
	}
	const bool one_key = p_key >= 0 && p_key < (int)keys.size();
	// Where each copy's centre edits (the brush is on the pose shown), worked
	// out once, as a grab does.
	if ((int)grab_center_offsets.size() != count) {
		grab_center_offsets.assign(count, Vector3());
		if (has_motion()) {
			for (int i = 0; i < count; i++) {
				const Vector3 rp = rest_point(p_centers[i], p_pose, p_weights);
				grab_center_offsets[i] = (one_key ? key_point(rp, p_key) : rp) - p_centers[i];
			}
		}
	}
	// Each copy, as SculptTerrain.twist(): material at distance u from the
	// centre ends up turned by slerp(identity, q, s) and scaled by
	// 1 + (k - 1) s, s = falloff(u / r).
	struct Copy {
		Vector3 centre; // body space, then grid space
		Quaternion q;
		float k = 1.0f;
		float reach = 0.0f; // the farthest any of it lands
	};
	const float R = (float)p_radius;
	std::vector<Copy> copies(count);
	Vector3 lo_l(1e30f, 1e30f, 1e30f), hi_l(-1e30f, -1e30f, -1e30f);
	for (int i = 0; i < count; i++) {
		Copy &cp = copies[i];
		cp.centre = p_centers[i] + grab_center_offsets[i];
		const Basis &b = p_bases[i];
		cp.k = std::clamp(std::cbrt(std::abs((float)b.determinant())), 0.05f, 20.0f);
		Basis rot = b * (1.0f / cp.k);
		rot.orthonormalize();
		if (rot.determinant() < 0.0f) {
			rot = rot * Basis().scaled(Vector3(-1, -1, -1));
		}
		cp.q = rot.get_rotation_quaternion().normalized();
		cp.reach = R;
		for (int j = 0; j <= 48; j++) {
			const float u = R * (float)j / 48.0f;
			cp.reach = std::max(cp.reach, u * (1.0f + (cp.k - 1.0f) * falloff(u / R)));
		}
		lo_l = lo_l.min(cp.centre - Vector3(1, 1, 1) * cp.reach);
		hi_l = hi_l.max(cp.centre + Vector3(1, 1, 1) * cp.reach);
	}
	// Room for where it goes (as a grab: coarser voxels past MAX_SIZE).
	const Vector3 room = Vector3(2, 2, 2) * (float)voxel_size;
	fit(lo_l - room, hi_l + room, true);
	const size_t n = this->count();
	const float vs = (float)voxel_size, r = R / vs;
	for (Copy &cp : copies) {
		cp.centre = (cp.centre - origin) / vs;
		cp.reach /= vs;
	}
	// The points rewritten: what this covers and everything it has since it
	// began (body space: the grid may have grown).
	Vector3 box_lo = lo_l - room, box_hi = hi_l + room;
	if (grab_has_box) {
		box_lo = box_lo.min(grab_box_lo);
		box_hi = box_hi.max(grab_box_hi);
	}
	grab_box_lo = box_lo;
	grab_box_hi = box_hi;
	grab_has_box = true;
	Vector3i lo, hi;
	bool outside = false;
	for (int a = 0; a < 3; a++) {
		lo[a] = std::max((int)std::floor((box_lo[a] - origin[a]) / vs), 0);
		hi[a] = std::min((int)std::ceil((box_hi[a] - origin[a]) / vs), size[a] - 1);
		outside |= lo[a] > hi[a];
	}
	// Where the material at p came from (grid space): the most solid of
	// every copy's roots, as SculptTerrain.twist().
	auto source = [&](const Vector3 &q) { return sample_grid(grab_rest, q); };
	auto twist_source = [&](const Vector3 &p) {
		float value = -INFINITY;
		Vector3 from = p;
		auto take = [&](const Vector3 &q) {
			const float v = source(q);
			if (v > value) {
				value = v;
				from = q;
			}
		};
		const int SCAN = 16;
		for (const Copy &cp : copies) {
			const Vector3 d = p - cp.centre;
			const float rho = d.length();
			if (rho >= cp.reach) {
				take(p);
				continue;
			}
			if (rho >= r) {
				take(p);
			}
			if (rho < 1e-5f) {
				take(cp.centre);
				continue;
			}
			auto land = [&](float u) { return u * (1.0f + (cp.k - 1.0f) * falloff(u / r)) - rho; };
			auto source_of = [&](float u) {
				const float sv = falloff(u / r);
				const Quaternion turn = Quaternion().slerp(cp.q, sv);
				return cp.centre + turn.inverse().xform(d) / (1.0f + (cp.k - 1.0f) * sv);
			};
			float u0 = 0.0f, h0 = land(0.0f);
			for (int k = 1; k <= SCAN; k++) {
				const float u1 = r * (float)k / SCAN, h1 = land(u1);
				if ((h0 > 0.0f) != (h1 > 0.0f)) {
					float a = u0, b = u1;
					for (int it = 0; it < 10; it++) {
						const float m = (a + b) * 0.5f;
						((land(m) > 0.0f) == (h0 > 0.0f) ? a : b) = m;
					}
					take(source_of((a + b) * 0.5f));
				}
				u0 = u1;
				h0 = h1;
			}
		}
		return from;
	};
	// Where a point lands (grid space): the forward map, for a key's motion.
	auto twist_point = [&](const Vector3 &p) {
		Vector3 to = p;
		for (const Copy &cp : copies) {
			const Vector3 d = to - cp.centre;
			const float sv = falloff(d.length() / r);
			if (sv <= 0.0f) {
				continue;
			}
			to = cp.centre + Quaternion().slerp(cp.q, sv).xform(d) * (1.0f + (cp.k - 1.0f) * sv);
		}
		return to;
	};
	bool any = false;
	if (one_key) {
		// One key: its motion moves (each point from where the key had it
		// when this began, carried by the twist), the rest shape stays.
		Key &key = keys[p_key];
		const Key &start = grab_keys[p_key];
		if (key.motion.empty()) {
			key.motion.assign(n, Vector3());
		}
		for (int z = 0; z < size.z; z++) {
			for (int y = 0; y < size.y; y++) {
				for (int x = 0; x < size.x; x++) {
					const size_t i = index(x, y, z);
					const Vector3 g((float)x, (float)y, (float)z);
					const Vector3 from = start.motion.empty() ? Vector3() : start.motion[i];
					const Vector3 to = twist_point(g + from) - g;
					any |= (to - key.motion[i]).length_squared() > 1e-10f;
					key.motion[i] = to;
				}
			}
		}
		if (all_zero(key.motion)) {
			key.motion.clear();
		}
	} else if (!outside) {
		// The rest shape moves, carrying every key's motion along.
		const std::vector<float> before = rest;
		for (int z = lo.z; z <= hi.z; z++) {
			for (int y = lo.y; y <= hi.y; y++) {
				for (int x = lo.x; x <= hi.x; x++) {
					const Vector3 p((float)x, (float)y, (float)z);
					const size_t i = index(x, y, z);
					const Vector3 from = twist_source(p);
					rest[i] = source(from);
					const Vector3i m = Vector3i(from.round()).clamp(Vector3i(), size - Vector3i(1, 1, 1));
					materials[i] = grab_materials[index(m.x, m.y, m.z)];
					for (size_t k = 0; k < keys.size(); k++) {
						if (!grab_keys[k].motion.empty() && keys[k].motion.size() == n) {
							keys[k].motion[i] = sample_motion(grab_keys[k].motion, from);
						}
					}
				}
			}
		}
		repair(rest, before, lo, hi);
		any = rest != before;
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
	if (rest.size() != count()) {
		return;
	}
	const double cube = voxel_size * voxel_size * voxel_size;
	double bounce = 0.0, friction = 0.0, skin = 0.0;
	const std::vector<float> &v = rest;
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
	if (keys.empty() || rest.size() != count()) {
		return parts;
	}
	// Label the solid points of the rest shape by connected part (6 neighbours).
	const size_t n = count();
	std::vector<int> label(n, -1);
	std::vector<int> part_size;
	std::vector<size_t> queue;
	const std::vector<float> &v = rest;
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
	// are repaired). Keys that don't move keep their whole density masked
	// the same way; residuals of moving ones just lose what they'd add.
	auto only = [&](int p_id, std::vector<float> &r_rest, std::vector<Key> &r_keys) {
		r_rest = rest;
		for (size_t i = 0; i < n; i++) {
			if (label[i] != p_id) {
				r_rest[i] = std::min(r_rest[i], -0.5f);
			}
		}
		density_redistance(r_rest, size);
		r_keys = keys;
		for (Key &k : r_keys) {
			if (k.residual.empty()) {
				continue;
			}
			if (k.motion.empty()) {
				std::vector<float> whole(n);
				for (size_t i = 0; i < n; i++) {
					whole[i] = std::clamp(rest[i] + k.residual[i], -DENSITY_CLAMP, DENSITY_CLAMP);
					if (label[i] != p_id) {
						whole[i] = std::min(whole[i], -0.5f);
					}
				}
				density_redistance(whole, size);
				for (size_t i = 0; i < n; i++) {
					k.residual[i] = whole[i] - r_rest[i];
				}
			} else {
				for (size_t i = 0; i < n; i++) {
					if (label[i] != p_id) {
						k.residual[i] = std::min(k.residual[i], 0.0f);
					}
				}
			}
		}
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
		only(id, part->rest, part->keys);
		part->materials = materials;
		part->edited = true;
		part->changed();
		parts.push_back(part);
	}
	std::vector<float> kept_rest;
	std::vector<Key> kept_keys;
	only(biggest, kept_rest, kept_keys);
	rest = std::move(kept_rest);
	keys = std::move(kept_keys);
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
	// Room for the whole distance band around it: stamped into the terrain,
	// its distances replace the old ones out to the clamp (a thinner pad left
	// the ground's old, farther distances just above a box's top, and the
	// player, trusting them, dropped into it and was pushed back out every
	// frame).
	const float pad_by = std::ceil(DENSITY_CLAMP) + 1.0f;
	const Vector3 pad = Vector3(pad_by, pad_by, pad_by);
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
	shape->rest.assign(shape->count(), 0.0f);
	shape->keys.assign(1, Key());
	shape->materials.assign(shape->count(), (MaterialId)std::max(p_material, 0));
	for (int z = 0; z < shape->size.z; z++) {
		for (int y = 0; y < shape->size.y; y++) {
			for (int x = 0; x < shape->size.x; x++) {
				const Vector3 p = lo + Vector3((float)x, (float)y, (float)z) * (float)vs;
				float outside = -1e30f; // how far beyond the farthest face
				for (const Plane &pl : planes) {
					outside = std::max(outside, pl.normal.dot(p) - pl.d);
				}
				shape->rest[shape->index(x, y, z)] = std::clamp(-outside / (float)vs, -DENSITY_CLAMP, DENSITY_CLAMP);
			}
		}
	}
	shape->edited = true;
	shape->changed();
	return shape;
}

// --- Solids (scripts' shapes) ------------------------------------------------------

Ref<DensityShape> DensityShape::make_solids(const Array &p_solids, double p_voxel_size) {
	struct Solid {
		int type = 0;
		Vector3 a, b, r;
		float ra = 0.0f, rb = 0.0f;
		MaterialId material = MAT_STONE;
		Vector3 lo, hi;
	};
	std::vector<Solid> solids;
	Vector3 lo(1e30f, 1e30f, 1e30f), hi(-1e30f, -1e30f, -1e30f);
	for (int64_t i = 0; i < p_solids.size(); i++) {
		const Dictionary d = p_solids[i];
		Solid solid;
		solid.type = d.get("type", 0);
		solid.a = d.get("a", Vector3());
		solid.material = (MaterialId)(int)d.get("material", (int)MAT_STONE);
		if (solid.type == 0) {
			solid.r = ((Vector3)d.get("r", Vector3(1, 1, 1))).max(Vector3(0.01f, 0.01f, 0.01f));
			solid.lo = solid.a - solid.r;
			solid.hi = solid.a + solid.r;
		} else {
			solid.b = d.get("b", solid.a);
			solid.ra = std::max((float)(double)d.get("ra", 1.0), 0.01f);
			solid.rb = std::max((float)(double)d.get("rb", solid.ra), 0.01f);
			const float r = std::max(solid.ra, solid.rb);
			solid.lo = solid.a.min(solid.b) - Vector3(r, r, r);
			solid.hi = solid.a.max(solid.b) + Vector3(r, r, r);
		}
		lo = lo.min(solid.lo);
		hi = hi.max(solid.hi);
		solids.push_back(solid);
	}
	ERR_FAIL_COND_V_MSG(solids.empty(), Ref<DensityShape>(), "DensityShape.make_solids(): no solids.");
	// A few voxels of air round them (the surface's outside), coarser
	// voxels while it doesn't fit.
	double vs = std::max(p_voxel_size, 0.01);
	Vector3i n;
	Vector3 origin;
	while (true) {
		const float margin = 3.0f * (float)vs;
		origin = ((lo - Vector3(margin, margin, margin)) / (float)vs).floor() * (float)vs;
		const Vector3 far = hi + Vector3(margin, margin, margin);
		n = Vector3i(((far - origin) / (float)vs).ceil()) + Vector3i(1, 1, 1);
		if ((n.x <= MAX_SIZE && n.y <= MAX_SIZE && n.z <= MAX_SIZE) || vs >= MAX_VOXEL_SIZE) {
			n = n.min(Vector3i(MAX_SIZE, MAX_SIZE, MAX_SIZE));
			break;
		}
		vs *= 2.0;
	}
	const size_t count = (size_t)n.x * n.y * n.z;
	std::vector<float> density(count, -1e30f);
	std::vector<MaterialId> materials(count, solids[0].material);
	auto distance = [](const Solid &s, const Vector3 &p) -> float {
		if (s.type == 0) {
			// Ellipsoid: a close bound of the true distance.
			const Vector3 q = (p - s.a) / s.r;
			const float k0 = q.length();
			if (k0 < 1e-5f) {
				return std::min({ s.r.x, s.r.y, s.r.z });
			}
			const float k1 = (q / s.r).length();
			return -k0 * (k0 - 1.0f) / k1;
		}
		const Vector3 ab = s.b - s.a;
		const float t = std::clamp((p - s.a).dot(ab) / std::max(ab.length_squared(), 1e-6f), 0.0f, 1.0f);
		return s.ra + (s.rb - s.ra) * t - p.distance_to(s.a + ab * t);
	};
	// Each solid at the points within the stored distances of it (each
	// point's material: the most solid one's, so air near a surface has the
	// surface's; farther points are clamped air anyway).
	const float reach = (DENSITY_CLAMP + 1.0f) * (float)vs;
	for (const Solid &s : solids) {
		const Vector3i a = Vector3i(((s.lo - Vector3(reach, reach, reach) - origin) / (float)vs).floor()).max(Vector3i()).min(n - Vector3i(1, 1, 1));
		const Vector3i b = Vector3i(((s.hi + Vector3(reach, reach, reach) - origin) / (float)vs).ceil()).max(Vector3i()).min(n - Vector3i(1, 1, 1));
		WorkerPool::get().run(b.z - a.z + 1, [&](int dz) {
			const int z = a.z + dz;
			for (int y = a.y; y <= b.y; y++) {
				for (int x = a.x; x <= b.x; x++) {
					const size_t i = ((size_t)z * n.y + y) * n.x + x;
					const float d = distance(s, origin + Vector3((float)x, (float)y, (float)z) * (float)vs) / (float)vs;
					if (d > density[i]) {
						density[i] = d;
						materials[i] = s.material;
					}
				}
			}
		});
	}
	Ref<DensityShape> shape;
	shape.instantiate();
	shape->init_grid(n, origin, vs, std::move(density), std::move(materials));
	return shape;
}
