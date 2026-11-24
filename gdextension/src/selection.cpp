// SculptTerrain's selection (see sculpt_terrain.h): a set of lattice points,
// a bit per point in 32^3 chunks (by unwrapped chunk coordinate, like the
// player's), drawn by the terrain shader as a tint, masking the brushes,
// and turned into DensityShapes (a body, or a file).

#include "sculpt_terrain.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <queue>

#ifdef _MSC_VER
#include <intrin.h>
#endif

using namespace godot;

namespace {

constexpr int CH = VoxelWorld::CHUNK;
constexpr int WORDS = CH * CH * CH / 64;

inline int lowest_bit(uint64_t v) {
#ifdef _MSC_VER
	unsigned long i;
	_BitScanForward64(&i, v);
	return (int)i;
#else
	return __builtin_ctzll(v);
#endif
}

inline int bit_of(int x, int y, int z, const Vector3i &c) {
	return ((z - c.z * CH) * CH + (y - c.y * CH)) * CH + (x - c.x * CH);
}

} // namespace

bool SculptTerrain::is_selected(int x, int y, int z) const {
	if (selection.empty()) {
		return false;
	}
	const Vector3i c = VoxelWorld::chunk_of(x, y, z);
	auto it = selection.find(c);
	if (it == selection.end()) {
		return false;
	}
	const int i = bit_of(x, y, z, c);
	return (it->second[i >> 6] >> (i & 63)) & 1u;
}

void SculptTerrain::set_selected(int x, int y, int z, bool p_on) {
	const Vector3i c = VoxelWorld::chunk_of(x, y, z);
	const int i = bit_of(x, y, z, c);
	const uint64_t bit = (uint64_t)1 << (i & 63);
	if (p_on) {
		std::vector<uint64_t> &bits = selection[c];
		if (bits.empty()) {
			bits.assign(WORDS, 0);
		}
		if (!(bits[i >> 6] & bit)) {
			bits[i >> 6] |= bit;
			selected_count++;
			selection_changed = true;
			selection_serial++;
		}
	} else {
		auto it = selection.find(c);
		if (it != selection.end() && (it->second[i >> 6] & bit)) {
			it->second[i >> 6] &= ~bit;
			selected_count--;
			selection_changed = true;
			selection_serial++;
		}
	}
}

template <typename F>
void SculptTerrain::for_selected(F &&p_f) const {
	for (const auto &[c, bits] : selection) {
		for (int w = 0; w < WORDS; w++) {
			uint64_t word = bits[w];
			while (word) {
				const int b = w * 64 + lowest_bit(word);
				word &= word - 1;
				p_f(c.x * CH + b % CH, c.y * CH + (b / CH) % CH, c.z * CH + b / (CH * CH));
			}
		}
	}
}

void SculptTerrain::clear_selection() {
	if (!selection.empty()) {
		selection.clear();
		selected_count = 0;
		selection_changed = true;
		selection_serial++;
	}
}

Array SculptTerrain::get_selection_state() const {
	PackedInt32Array keys;
	PackedByteArray bits;
	keys.resize((int64_t)selection.size() * 3);
	bits.resize((int64_t)selection.size() * WORDS * 8);
	int64_t k = 0;
	for (const auto &[c, words] : selection) {
		keys.set(k * 3, c.x);
		keys.set(k * 3 + 1, c.y);
		keys.set(k * 3 + 2, c.z);
		memcpy(bits.ptrw() + k * WORDS * 8, words.data(), WORDS * 8);
		k++;
	}
	Array state;
	state.append(keys);
	state.append(bits);
	state.append(selection_shift);
	state.append(selected_count);
	return state;
}

void SculptTerrain::set_selection_state(const Array &p_state) {
	ERR_FAIL_COND_MSG(p_state.size() != 4, "SculptTerrain.set_selection_state(): not a selection state.");
	const PackedInt32Array keys = p_state[0];
	const PackedByteArray bits = p_state[1];
	const Vector3i shift = Vector3i(selection_shift) - Vector3i(p_state[2]);
	const int64_t n = keys.size() / 3;
	ERR_FAIL_COND(bits.size() != n * WORDS * 8);
	selection.clear();
	for (int64_t k = 0; k < n; k++) {
		const Vector3i c = Vector3i(keys[k * 3], keys[k * 3 + 1], keys[k * 3 + 2]) + shift / CH;
		std::vector<uint64_t> &words = selection[c];
		words.resize(WORDS);
		memcpy(words.data(), bits.ptr() + k * WORDS * 8, WORDS * 8);
	}
	selected_count = (int64_t)p_state[3];
	selection_changed = true;
	selection_serial++;
}

bool SculptTerrain::selection_bounds(Vector3i &r_lo, Vector3i &r_hi) const {
	if (selected_count <= 0) {
		return false;
	}
	r_lo = Vector3i(INT32_MAX, INT32_MAX, INT32_MAX);
	r_hi = Vector3i(INT32_MIN, INT32_MIN, INT32_MIN);
	for_selected([&](int x, int y, int z) {
		r_lo = r_lo.min(Vector3i(x, y, z));
		r_hi = r_hi.max(Vector3i(x, y, z));
	});
	return true;
}

AABB SculptTerrain::get_selection_bounds() const {
	Vector3i lo, hi;
	if (!selection_bounds(lo, hi)) {
		return AABB();
	}
	return AABB(Vector3(lo), Vector3(hi - lo));
}

void SculptTerrain::select_sphere(const Vector3 &p_center, double p_radius, bool p_add) {
	ERR_FAIL_COND(!opened || p_radius <= 0.0);
	const float r = (float)p_radius;
	Vector3i lo, hi;
	for (int a = 0; a < 3; a++) {
		lo[a] = (int)std::floor(p_center[a] - r);
		hi[a] = (int)std::ceil(p_center[a] + r);
	}
	std::vector<float> density;
	if (p_add) {
		const Vector3i n = hi - lo + Vector3i(1, 1, 1);
		density.resize((size_t)n.x * n.y * n.z);
		world.read_points(lo, hi, density.data(), worker_count());
	}
	size_t i = 0;
	for (int z = lo.z; z <= hi.z; z++) {
		for (int y = lo.y; y <= hi.y; y++) {
			for (int x = lo.x; x <= hi.x; x++, i++) {
				if ((Vector3((float)x, (float)y, (float)z) - p_center).length_squared() > r * r) {
					continue;
				}
				// Adding takes the ground (and the skin of air just outside,
				// so the surface shows as selected); removing takes anything.
				if (!p_add || density[i] > -0.5f) {
					set_selected(x, y, z, p_add);
				}
			}
		}
	}
}

int SculptTerrain::flood_select(const Vector3 &p_from, double p_radius, double p_threshold, bool p_by_color, bool p_add) {
	ERR_FAIL_COND_V(!opened, 0);
	const int R = std::clamp((int)std::ceil(p_radius), 1, 256);
	// Points are read a chunk at a time, as the fill reaches them.
	struct Block {
		std::vector<float> density;
		std::vector<MaterialId> materials;
		std::vector<uint8_t> seen;
	};
	std::unordered_map<Vector3i, Block, Vector3iHash> blocks;
	auto block_of = [&](const Vector3i &p) -> std::pair<Block *, size_t> {
		const Vector3i c = VoxelWorld::chunk_of(p.x, p.y, p.z);
		Block &b = blocks[c];
		if (b.density.empty()) {
			b.density.resize((size_t)CH * CH * CH);
			b.materials.resize(b.density.size());
			b.seen.assign(b.density.size(), 0);
			world.read_points(c * CH, c * CH + Vector3i(CH - 1, CH - 1, CH - 1), b.density.data(), 1, b.materials.data());
		}
		return { &b, (size_t)bit_of(p.x, p.y, p.z, c) };
	};
	const Vector3i centre((int)std::lround(p_from.x), (int)std::lround(p_from.y), (int)std::lround(p_from.z));
	// Start from the most solid point next to where the ray hit.
	Vector3i start = centre;
	float best = -1e9f;
	for (int dz = -1; dz <= 1; dz++) {
		for (int dy = -1; dy <= 1; dy++) {
			for (int dx = -1; dx <= 1; dx++) {
				const Vector3i q = centre + Vector3i(dx, dy, dz);
				auto [b, i] = block_of(q);
				if (b->density[i] > best) {
					best = b->density[i];
					start = q;
				}
			}
		}
	}
	if (best <= 0.0f) {
		return 0;
	}
	Color want;
	{
		auto [b, i] = block_of(start);
		want = get_material_color(b->materials[i]);
	}
	auto accept = [&](Block *b, size_t i) {
		if (b->density[i] <= 0.0f) {
			return false;
		}
		if (!p_by_color) {
			return true;
		}
		const Color c = get_material_color(b->materials[i]);
		const float dr = c.r - want.r, dg = c.g - want.g, db = c.b - want.b;
		return std::sqrt(dr * dr + dg * dg + db * db) <= (float)p_threshold;
	};
	// Breadth first through connected solid points, within R of the start.
	std::vector<Vector3i> found;
	std::queue<Vector3i> todo;
	todo.push(start);
	{
		auto [b, i] = block_of(start);
		b->seen[i] = 1;
	}
	const Vector3i steps[6] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
	const size_t LIMIT = 8u << 20; // points, so a fill into endless ground stops
	while (!todo.empty() && found.size() < LIMIT) {
		const Vector3i p = todo.front();
		todo.pop();
		found.push_back(p);
		for (const Vector3i &s : steps) {
			const Vector3i q = p + s;
			if ((q - start).length_squared() > R * R) {
				continue;
			}
			auto [b, i] = block_of(q);
			if (!b->seen[i] && accept(b, i)) {
				b->seen[i] = 1;
				todo.push(q);
			}
		}
	}
	// The skin of air round what was found, so its surface shows selected.
	for (size_t k = 0, count = found.size(); k < count; k++) {
		for (const Vector3i &s : steps) {
			const Vector3i q = found[k] + s;
			auto [b, i] = block_of(q);
			if (!b->seen[i] && b->density[i] <= 0.0f && b->density[i] > -1.0f) {
				b->seen[i] = 1;
				found.push_back(q);
			}
		}
	}
	for (const Vector3i &p : found) {
		set_selected(p.x, p.y, p.z, p_add);
	}
	return (int)found.size();
}

int SculptTerrain::select_connected(const Vector3 &p_from, double p_radius, bool p_add) {
	return flood_select(p_from, p_radius, 0.0, false, p_add);
}

int SculptTerrain::select_similar(const Vector3 &p_from, double p_radius, double p_threshold, bool p_add) {
	return flood_select(p_from, p_radius, p_threshold, true, p_add);
}

void SculptTerrain::mask_to_selection(const Vector3i &lo, const Vector3i &hi, const Vector3i &rlo, const Vector3i &n,
		const std::vector<float> &before, std::vector<float> &after, const std::vector<MaterialId> &materials_before,
		std::vector<MaterialId> &materials_after, bool p_any_air, std::vector<Vector3i> &r_grown) const {
	if (selected_count <= 0) {
		return;
	}
	auto index = [&](int x, int y, int z) { return ((size_t)(z - rlo.z) * n.y + (y - rlo.y)) * n.x + (x - rlo.x); };
	for (int z = lo.z; z <= hi.z; z++) {
		for (int y = lo.y; y <= hi.y; y++) {
			for (int x = lo.x; x <= hi.x; x++) {
				const size_t i = index(x, y, z);
				if (after[i] == before[i] && materials_after[i] == materials_before[i]) {
					continue;
				}
				const bool selected = is_selected(x, y, z);
				bool allowed = selected;
				// Air may take material: next to the selection (adding grows
				// it), or anywhere for a grab or snake hook (they move it).
				if (!allowed && before[i] <= 0.0f) {
					allowed = p_any_air || is_selected(x + 1, y, z) || is_selected(x - 1, y, z) || is_selected(x, y + 1, z) ||
							is_selected(x, y - 1, z) || is_selected(x, y, z + 1) || is_selected(x, y, z - 1);
				}
				if (!allowed) {
					after[i] = before[i];
					materials_after[i] = materials_before[i];
				} else if (!selected && after[i] > 0.0f) {
					r_grown.push_back(Vector3i(x, y, z)); // new material joins the selection
				}
			}
		}
	}
}

Ref<DensityShape> SculptTerrain::selection_to_shape(bool p_carve) {
	Vector3i lo, hi;
	ERR_FAIL_COND_V_MSG(!opened || !selection_bounds(lo, hi), Ref<DensityShape>(), "SculptTerrain: nothing is selected.");
	// Room for the surface round it, and coarser voxels if it's bigger than
	// a shape can be.
	const int pad = 2;
	const Vector3i first = lo - Vector3i(pad, pad, pad);
	const Vector3i total = hi - lo + Vector3i(1 + 2 * pad, 1 + 2 * pad, 1 + 2 * pad);
	const int largest = std::max({ total.x, total.y, total.z });
	const int step = std::max(1, (largest + DensityShape::MAX_SIZE - 1) / DensityShape::MAX_SIZE);
	const Vector3i size((total.x + step - 1) / step, (total.y + step - 1) / step, (total.z + step - 1) / step);
	const Vector3i last = first + (size - Vector3i(1, 1, 1)) * step;
	const Vector3i n = last - first + Vector3i(1, 1, 1);
	std::vector<float> density((size_t)n.x * n.y * n.z);
	std::vector<MaterialId> materials(density.size());
	world.read_points(first, last, density.data(), worker_count(), materials.data());
	auto index = [&](int x, int y, int z) { return ((size_t)(z - first.z) * n.y + (y - first.y)) * n.x + (x - first.x); };
	std::vector<float> grid((size_t)size.x * size.y * size.z);
	std::vector<MaterialId> grid_materials(grid.size());
	size_t g = 0;
	for (int k = 0; k < size.z; k++) {
		for (int j = 0; j < size.y; j++) {
			for (int i = 0; i < size.x; i++, g++) {
				const Vector3i p = first + Vector3i(i, j, k) * step;
				const size_t w = index(p.x, p.y, p.z);
				const float d = density[w];
				// Unselected ground is air to the shape (the distance repair
				// puts the distances right).
				grid[g] = (is_selected(p.x, p.y, p.z) || d <= 0.0f ? d : -0.5f) / step;
				grid_materials[g] = materials[w];
			}
		}
	}
	const Vector3 centre = (Vector3(lo) + Vector3(hi)) * 0.5f;
	Ref<DensityShape> shape;
	shape.instantiate();
	shape->init_grid(size, Vector3(first) - centre, step, std::move(grid), std::move(grid_materials));
	selection_center = centre;

	if (p_carve) {
		// Take it out of the terrain, as one undo step.
		begin_stroke();
		const float C = DENSITY_CLAMP;
		const int margin = (int)std::ceil(C) + 1;
		const Vector3i rlo = lo - Vector3i(margin, margin, margin), rhi = hi + Vector3i(margin, margin, margin);
		const Vector3i rn = rhi - rlo + Vector3i(1, 1, 1);
		auto rindex = [&](int x, int y, int z) { return ((size_t)(z - rlo.z) * rn.y + (y - rlo.y)) * rn.x + (x - rlo.x); };
		std::vector<float> before((size_t)rn.x * rn.y * rn.z);
		std::vector<MaterialId> materials_before(before.size());
		world.read_points(rlo, rhi, before.data(), worker_count(), materials_before.data());
		std::vector<float> after = before;
		for_selected([&](int x, int y, int z) {
			after[rindex(x, y, z)] = -C;
		});
		std::vector<uint8_t> brushed(after.size(), 0);
		for (size_t i = 0; i < after.size(); i++) {
			brushed[i] = after[i] != before[i];
		}
		density_redistance(after, rn);
		commit_edit(lo, hi, rlo, rn, before, after, brushed, false, materials_before, materials_before);
		end_stroke();
	}
	return shape;
}

void SculptTerrain::update_selection_mask() {
	if (!selection_changed || material.is_null()) {
		return;
	}
	selection_changed = false;
	Vector3i lo, hi;
	if (!selection_bounds(lo, hi)) {
		material->set_shader_parameter("selection_on", false);
		return;
	}
	// One texel per point, or per block of points for big selections (the
	// mask is at most MASK_SIZE a side).
	const Vector3i extent = hi - lo + Vector3i(1, 1, 1);
	const int largest = std::max({ extent.x, extent.y, extent.z });
	const int cell = std::max(1, (largest + MASK_SIZE - 1) / MASK_SIZE);
	const Vector3i dims((extent.x + cell - 1) / cell, (extent.y + cell - 1) / cell, (extent.z + cell - 1) / cell);
	std::vector<PackedByteArray> layers(dims.z);
	for (PackedByteArray &l : layers) {
		l.resize((int64_t)dims.x * dims.y);
		l.fill(0);
	}
	for_selected([&](int x, int y, int z) {
		const Vector3i c = (Vector3i(x, y, z) - lo) / cell;
		layers[c.z].set((int64_t)c.y * dims.x + c.x, 255);
	});
	TypedArray<Image> images;
	for (const PackedByteArray &l : layers) {
		images.push_back(Image::create_from_data(dims.x, dims.y, false, Image::FORMAT_R8, l));
	}
	selection_texture.instantiate();
	selection_texture->create_from_images(images);
	material->set_shader_parameter("selection_mask", selection_texture);
	material->set_shader_parameter("selection_origin", Vector3(lo));
	material->set_shader_parameter("selection_cell", (float)cell);
	material->set_shader_parameter("selection_dims", Vector3(dims));
	material->set_shader_parameter("selection_on", true);
}

void SculptTerrain::shift_selection(const Vector3i &p_offset) {
	selection_shift += p_offset;
	if (selection.empty()) {
		return;
	}
	std::unordered_map<Vector3i, std::vector<uint64_t>, Vector3iHash> moved;
	for (auto &[c, bits] : selection) {
		moved.emplace(c + p_offset / CH, std::move(bits));
	}
	selection = std::move(moved);
	selection_changed = true;
}

// --- Stamping, extruding --------------------------------------------------------------

namespace {

// The box of lattice points a shape covers where p_transform puts it.
void shape_box(const Ref<DensityShape> &p_shape, const Transform3D &p_transform, float p_margin, Vector3i &r_lo, Vector3i &r_hi) {
	const Vector3 o = p_shape->get_origin();
	const Vector3 e = Vector3(p_shape->get_size() - Vector3i(1, 1, 1)) * (float)p_shape->get_voxel_size();
	Vector3 lo(1e30f, 1e30f, 1e30f), hi(-1e30f, -1e30f, -1e30f);
	for (int k = 0; k < 8; k++) {
		const Vector3 corner = p_transform.xform(o + Vector3(k & 1 ? e.x : 0.0f, k & 2 ? e.y : 0.0f, k & 4 ? e.z : 0.0f));
		lo = lo.min(corner);
		hi = hi.max(corner);
	}
	for (int a = 0; a < 3; a++) {
		r_lo[a] = (int)std::floor(lo[a] - p_margin);
		r_hi[a] = (int)std::ceil(hi[a] + p_margin);
	}
}

// The smallest scale p_transform applies: shape distances times this are
// (at most) world distances.
float smallest_scale(const Transform3D &p_transform) {
	return std::min({ p_transform.basis.get_column(0).length(), p_transform.basis.get_column(1).length(), p_transform.basis.get_column(2).length() });
}

} // namespace

void SculptTerrain::stamp_shape(const Ref<DensityShape> &p_shape, const Transform3D &p_transform, int p_material, bool p_select, double p_pose) {
	ERR_FAIL_COND(!opened || p_shape.is_null() || p_shape->get_key_count() == 0);
	const float C = DENSITY_CLAMP;
	Vector3i lo, hi;
	shape_box(p_shape, p_transform, 1.0f, lo, hi);
	const int margin = (int)std::ceil(C) + 1;
	const Vector3i rlo = lo - Vector3i(margin, margin, margin), rhi = hi + Vector3i(margin, margin, margin);
	const Vector3i n = rhi - rlo + Vector3i(1, 1, 1);
	auto index = [&](int x, int y, int z) { return ((size_t)(z - rlo.z) * n.y + (y - rlo.y)) * n.x + (x - rlo.x); };
	std::vector<float> before((size_t)n.x * n.y * n.z);
	std::vector<MaterialId> materials_before(before.size());
	world.read_points(rlo, rhi, before.data(), worker_count(), materials_before.data());
	std::vector<float> after = before;
	std::vector<MaterialId> materials_after = materials_before;
	const Transform3D inverse = p_transform.affine_inverse();
	const float scale = smallest_scale(p_transform);
	std::vector<Vector3i> added;
	for (int z = lo.z; z <= hi.z; z++) {
		for (int y = lo.y; y <= hi.y; y++) {
			for (int x = lo.x; x <= hi.x; x++) {
				const Vector3 local = inverse.xform(Vector3((float)x, (float)y, (float)z));
				const float d = std::clamp((float)p_shape->get_density(local, p_pose) * scale, -C, C);
				const size_t i = index(x, y, z);
				// The union: the more solid of the two, the shape's material
				// where it's the surface.
				if (d > before[i]) {
					after[i] = d;
					if (d > -2.0f) {
						materials_after[i] = (MaterialId)(p_material >= 0 ? p_material : p_shape->get_material_at(local));
					}
				}
				if (d > -0.5f) {
					added.push_back(Vector3i(x, y, z));
				}
			}
		}
	}
	std::vector<uint8_t> brushed(after.size(), 0);
	for (size_t i = 0; i < after.size(); i++) {
		brushed[i] = after[i] != before[i];
	}
	density_redistance(after, n);
	commit_edit(lo, hi, rlo, n, before, after, brushed, false, materials_before, materials_after);
	if (p_select) {
		for (const Vector3i &p : added) {
			set_selected(p.x, p.y, p.z, true);
		}
	}
}

int SculptTerrain::select_inside_shape(const Ref<DensityShape> &p_shape, const Transform3D &p_transform, bool p_add) {
	ERR_FAIL_COND_V(!opened || p_shape.is_null() || p_shape->get_key_count() == 0, 0);
	Vector3i lo, hi;
	shape_box(p_shape, p_transform, 0.0f, lo, hi);
	const Vector3i n = hi - lo + Vector3i(1, 1, 1);
	std::vector<float> density((size_t)n.x * n.y * n.z);
	world.read_points(lo, hi, density.data(), worker_count());
	const Transform3D inverse = p_transform.affine_inverse();
	int count = 0;
	size_t i = 0;
	for (int z = lo.z; z <= hi.z; z++) {
		for (int y = lo.y; y <= hi.y; y++) {
			for (int x = lo.x; x <= hi.x; x++, i++) {
				// Ground (and its skin) inside the shape.
				if (density[i] > -0.5f && p_shape->get_density(inverse.xform(Vector3((float)x, (float)y, (float)z)), 0.0) > 0.0) {
					set_selected(x, y, z, p_add);
					count++;
				}
			}
		}
	}
	return count;
}

void SculptTerrain::set_stroke_held(bool p_held) {
	stroke_held = p_held;
}

bool SculptTerrain::begin_extrude() {
	if (!opened || selected_count <= 0) {
		return false;
	}
	extrude_shape = selection_to_shape(false);
	extrude_center = selection_center;
	grab_snapshot.clear(); // the grab's: an extrude rewrites from it the same way
	grab_has_box = false;
	return extrude_shape.is_valid();
}

void SculptTerrain::end_extrude() {
	extrude_shape.unref();
	grab_snapshot.clear();
	grab_has_box = false;
}

void SculptTerrain::extrude(const Vector3 &p_direction, double p_distance, const Vector3 &p_center, double p_radius) {
	ERR_FAIL_COND_MSG(extrude_shape.is_null(), "SculptTerrain.extrude(): call begin_extrude() first.");
	const Vector3 dir = p_direction.normalized();
	const bool out = p_distance >= 0.0;
	const float depth = std::abs((float)p_distance), C = DENSITY_CLAMP;
	const float radius = (float)p_radius;
	// How far the selection moves at a point of it: all the way, or with a
	// radius, fading to nothing at its edge.
	auto reach = [&](const Vector3 &q) {
		if (radius <= 0.0f) {
			return depth;
		}
		const float t = (q - p_center).length() / radius;
		return t >= 1.0f ? -1.0f : depth * (1.0f - t * t * (3.0f - 2.0f * t));
	};
	// The box of the sweep (and last time's, which goes back to how it was
	// where this one no longer reaches).
	Vector3i lo, hi, lo2, hi2;
	shape_box(extrude_shape, Transform3D(Basis(), extrude_center), 1.0f, lo, hi);
	shape_box(extrude_shape, Transform3D(Basis(), extrude_center + dir * (float)p_distance), 1.0f, lo2, hi2);
	lo = lo.min(lo2);
	hi = hi.max(hi2);
	if (grab_has_box) {
		lo = lo.min(grab_lo);
		hi = hi.max(grab_hi);
	}
	const int margin = (int)std::ceil(C) + 1;
	capture_snapshot(lo - Vector3i(margin, margin, margin), hi + Vector3i(margin, margin, margin));
	const Vector3i rlo = lo - Vector3i(margin, margin, margin), rhi = hi + Vector3i(margin, margin, margin);
	const Vector3i n = rhi - rlo + Vector3i(1, 1, 1);
	auto index = [&](int x, int y, int z) { return ((size_t)(z - rlo.z) * n.y + (y - rlo.y)) * n.x + (x - rlo.x); };
	std::vector<float> before((size_t)n.x * n.y * n.z);
	std::vector<MaterialId> materials_before(before.size());
	world.read_points(rlo, rhi, before.data(), worker_count(), materials_before.data());
	std::vector<float> after = before;
	std::vector<MaterialId> materials_after = materials_before;
	std::vector<Vector3i> carved;
	std::vector<Vector3i> added;
	// The selection swept along the direction, half a voxel at a time, as far
	// as reach() lets each part of it go: extruding adds that to the terrain;
	// intruding sweeps it inwards and takes it away.
	const int samples = (int)std::ceil(depth * 2.0f) + 1;
	for (int z = lo.z; z <= hi.z; z++) {
		for (int y = lo.y; y <= hi.y; y++) {
			for (int x = lo.x; x <= hi.x; x++) {
				const Vector3 p((float)x, (float)y, (float)z);
				const size_t i = index(x, y, z);
				const float original = snapshot_point(x, y, z);
				float swept = -C;
				Vector3 from;
				for (int k = 0; k < samples; k++) {
					const float t = samples > 1 ? depth * k / (samples - 1) : 0.0f;
					const Vector3 q = out ? p - dir * t : p + dir * t; // where in the selection it came from
					const float allowed = reach(q + extrude_center - extrude_center);
					if (allowed < 0.0f || t > allowed + 1e-4f || (!out && allowed <= 0.0f)) {
						continue;
					}
					const float d = (float)extrude_shape->get_density(q - extrude_center, 0.0);
					if (d > swept) {
						swept = d;
						from = q - extrude_center;
					}
				}
				swept = std::clamp(swept, -C, C);
				if (out) {
					after[i] = std::max(original, swept);
					materials_after[i] = swept > original && swept > -2.0f ? (MaterialId)extrude_shape->get_material_at(from) : snapshot_material(x, y, z);
					if (swept > -0.5f) {
						added.push_back(Vector3i(x, y, z));
					}
				} else {
					after[i] = std::min(original, -swept);
					materials_after[i] = snapshot_material(x, y, z);
					if (swept > -1.0f) {
						carved.push_back(Vector3i(x, y, z));
					}
				}
			}
		}
	}
	std::vector<uint8_t> brushed(after.size(), 0);
	for (size_t i = 0; i < after.size(); i++) {
		brushed[i] = std::abs(after[i] - before[i]) > 1e-5f;
	}
	density_redistance(after, n);
	commit_edit(lo, hi, rlo, n, before, after, brushed, false, materials_before, materials_after);
	grab_lo = lo;
	grab_hi = hi;
	grab_has_box = true;
	for (const Vector3i &q : added) {
		set_selected(q.x, q.y, q.z, true);
	}
	// Intruding: the new surface at the bottom of the hole joins the selection.
	for (const Vector3i &q : carved) {
		const float v = world.sample(Vector3(q));
		if (v > -0.5f && v < 1.5f) {
			set_selected(q.x, q.y, q.z, true);
		}
	}
}

void SculptTerrain::carve_shape(const Ref<DensityShape> &p_shape, const Transform3D &p_transform, double p_pose) {
	ERR_FAIL_COND(!opened || p_shape.is_null() || p_shape->get_key_count() == 0);
	const float C = DENSITY_CLAMP;
	Vector3i lo, hi;
	shape_box(p_shape, p_transform, 1.0f, lo, hi);
	const int margin = (int)std::ceil(C) + 1;
	const Vector3i rlo = lo - Vector3i(margin, margin, margin), rhi = hi + Vector3i(margin, margin, margin);
	const Vector3i n = rhi - rlo + Vector3i(1, 1, 1);
	auto index = [&](int x, int y, int z) { return ((size_t)(z - rlo.z) * n.y + (y - rlo.y)) * n.x + (x - rlo.x); };
	std::vector<float> before((size_t)n.x * n.y * n.z);
	std::vector<MaterialId> materials_before(before.size());
	world.read_points(rlo, rhi, before.data(), worker_count(), materials_before.data());
	std::vector<float> after = before;
	const Transform3D inverse = p_transform.affine_inverse();
	const float scale = smallest_scale(p_transform);
	for (int z = lo.z; z <= hi.z; z++) {
		for (int y = lo.y; y <= hi.y; y++) {
			for (int x = lo.x; x <= hi.x; x++) {
				const float d = std::clamp((float)p_shape->get_density(inverse.xform(Vector3((float)x, (float)y, (float)z)), p_pose) * scale, -C, C);
				const size_t i = index(x, y, z);
				// The difference: solid only where the terrain is and the shape isn't.
				after[i] = std::min(before[i], -d);
			}
		}
	}
	std::vector<uint8_t> brushed(after.size(), 0);
	for (size_t i = 0; i < after.size(); i++) {
		brushed[i] = after[i] != before[i];
	}
	density_redistance(after, n);
	commit_edit(lo, hi, rlo, n, before, after, brushed, false, materials_before, materials_before);
}
