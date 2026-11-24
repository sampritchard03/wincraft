#include "sculpt_terrain.h"

#include "density_body.h"
#include "density_shape.h"
#include "parallel.h"

#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/error_macros.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/variant/aabb.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace godot;

namespace {

// 1 at the centre, easing to 0 at distance 1.
float falloff(float t) {
	return t >= 1.0f ? 0.0f : 1.0f - t * t * (3.0f - 2.0f * t);
}

// A repeatable pseudo-random number in [0, 1) per point and stroke. Adding it
// before rounding to 1/16-voxel steps makes small brush changes add up on
// average instead of always rounding away.
float dither(int x, int y, int z, uint32_t p_stroke) {
	uint32_t h = (uint32_t)x * 0x8DA6B343u ^ (uint32_t)y * 0xD8163841u ^ (uint32_t)z * 0xCB1AB31Fu ^ p_stroke * 0x9E3779B9u;
	h ^= h >> 15;
	h *= 0x2C1B3C6Du;
	h ^= h >> 12;
	return (float)(h & 0xFFFFFF) / (float)0x1000000;
}

// Replaces a block of densities (x fastest, then y, then z) with the signed
// distance from each point to the surface (where the density crosses 0),
// clamped to DENSITY_CLAMP, keeping each point's side.
//
// Points next to the surface get it from the density itself: along each
// grid edge that crosses it, the crossing's linear position, and the value
// divided by the local slope, whichever is nearer. The rest take the nearest
// of those plus the path to it, over a two-pass 26-neighbour distance
// transform (within ~8% of straight-line distance). Surface outside the
// block is unknown, so only points at least CLAMP inside its edge come out
// right.
//
// Work is skipped in 8^3 tiles with no surface in or next to them: their
// points are at least 8 (> CLAMP) from it, so they just get +-CLAMP.
void redistance(std::vector<float> &r_values, const Vector3i &p_size) {
	const int nx = p_size.x, ny = p_size.y, nz = p_size.z;
	const int T = 8;
	const int tx = (nx + T - 1) / T, ty = (ny + T - 1) / T, tz = (nz + T - 1) / T;
	// Padded copy: a 1-point border of "infinitely far" so the transform's
	// inner loop needs no bounds checks.
	const int px = nx + 2, py = ny + 2, pz = nz + 2;
	const ptrdiff_t psy = px, psz = (ptrdiff_t)px * py;
	const float INF = 1e30f;
	std::vector<float> dist((size_t)px * py * pz, INF);
	auto at = [&](int x, int y, int z) { return (size_t)z * nx * ny + (size_t)y * nx + x; };
	auto pat = [&](int x, int y, int z) { return (z + 1) * psz + (y + 1) * psy + (x + 1); };
	std::vector<uint8_t> crossing((size_t)tx * ty * tz, 0);
	auto tile = [&](int x, int y, int z) { return ((size_t)(z / T) * ty + y / T) * tx + x / T; };

	// Seeds: points next to a crossing.
	const size_t step[3] = { 1, (size_t)nx, (size_t)nx * ny };
	const ptrdiff_t pstep[3] = { 1, psy, psz };
	for (int z = 0; z < nz; z++) {
		for (int y = 0; y < ny; y++) {
			for (int x = 0; x < nx; x++) {
				const size_t i = at(x, y, z);
				const ptrdiff_t pi = pat(x, y, z);
				const float v = r_values[i];
				const int pos[3] = { x, y, z };
				const int lim[3] = { nx, ny, nz };
				for (int a = 0; a < 3; a++) {
					if (pos[a] + 1 >= lim[a]) {
						continue;
					}
					const float w = r_values[i + step[a]];
					if ((v > 0.0f) == (w > 0.0f)) {
						continue;
					}
					const float t = v / (v - w); // crossing, from this point
					dist[pi] = std::min(dist[pi], t);
					dist[pi + pstep[a]] = std::min(dist[pi + pstep[a]], 1.0f - t);
					crossing[tile(x, y, z)] = 1;
					int q[3] = { x, y, z };
					q[a]++;
					crossing[tile(q[0], q[1], q[2])] = 1;
				}
			}
		}
	}
	// A tile is worked on if it or a neighbour has surface.
	std::vector<uint8_t> active(crossing.size(), 0);
	for (int z = 0; z < tz; z++) {
		for (int y = 0; y < ty; y++) {
			for (int x = 0; x < tx; x++) {
				bool any = false;
				for (int dz = -1; dz <= 1 && !any; dz++) {
					for (int dy = -1; dy <= 1 && !any; dy++) {
						for (int dx = -1; dx <= 1 && !any; dx++) {
							const int ax = x + dx, ay = y + dy, az = z + dz;
							any = ax >= 0 && ay >= 0 && az >= 0 && ax < tx && ay < ty && az < tz && crossing[((size_t)az * ty + ay) * tx + ax];
						}
					}
				}
				active[((size_t)z * ty + y) * tx + x] = any;
			}
		}
	}
	// Refine the seeds with value / slope, which is closer for surfaces at an
	// angle to the grid.
	for (int z = 1; z + 1 < nz; z++) {
		for (int y = 1; y + 1 < ny; y++) {
			for (int x = 1; x + 1 < nx; x++) {
				const ptrdiff_t pi = pat(x, y, z);
				if (dist[pi] >= INF) {
					continue;
				}
				const size_t i = at(x, y, z);
				const float gx = (r_values[i + step[0]] - r_values[i - step[0]]) * 0.5f;
				const float gy = (r_values[i + step[1]] - r_values[i - step[1]]) * 0.5f;
				const float gz = (r_values[i + step[2]] - r_values[i - step[2]]) * 0.5f;
				const float g = std::sqrt(gx * gx + gy * gy + gz * gz);
				if (g > 1e-3f) {
					dist[pi] = std::min(dist[pi], std::abs(r_values[i]) / g);
				}
			}
		}
	}
	// Two passes over the 13 neighbours before / after each point, in
	// active tiles only (inactive ones stay "infinitely far", which is fine:
	// any path through them is longer than CLAMP).
	ptrdiff_t offset[13];
	float length[13];
	int count = 0;
	for (int dz = -1; dz <= 0; dz++) {
		for (int dy = -1; dy <= 1; dy++) {
			for (int dx = -1; dx <= 1; dx++) {
				if (dz == 0 && (dy > 0 || (dy == 0 && dx >= 0))) {
					continue;
				}
				offset[count] = dz * psz + dy * psy + dx;
				length[count] = std::sqrt((float)(dx * dx + dy * dy + dz * dz));
				count++;
			}
		}
	}
	float *d = dist.data();
	for (int z = 0; z < nz; z++) {
		for (int y = 0; y < ny; y++) {
			for (int t = 0; t < tx; t++) {
				if (!active[((size_t)(z / T) * ty + y / T) * tx + t]) {
					continue;
				}
				for (int x = t * T; x < std::min((t + 1) * T, nx); x++) {
					float *p = d + pat(x, y, z);
					float best = *p;
					for (int k = 0; k < 13; k++) {
						best = std::min(best, p[offset[k]] + length[k]);
					}
					*p = best;
				}
			}
		}
	}
	for (int z = nz - 1; z >= 0; z--) {
		for (int y = ny - 1; y >= 0; y--) {
			for (int t = tx - 1; t >= 0; t--) {
				if (!active[((size_t)(z / T) * ty + y / T) * tx + t]) {
					continue;
				}
				for (int x = std::min((t + 1) * T, nx) - 1; x >= t * T; x--) {
					float *p = d + pat(x, y, z);
					float best = *p;
					for (int k = 0; k < 13; k++) {
						best = std::min(best, p[-offset[k]] + length[k]);
					}
					*p = best;
				}
			}
		}
	}
	for (int z = 0; z < nz; z++) {
		for (int y = 0; y < ny; y++) {
			for (int x = 0; x < nx; x++) {
				float &v = r_values[at(x, y, z)];
				const float e = std::min(d[pat(x, y, z)], DENSITY_CLAMP);
				v = v > 0.0f ? e : -e;
			}
		}
	}
}

} // namespace

// For DensityShape (density_shape.h), which edits small grids the same way.
void godot::density_redistance(std::vector<float> &r_values, const Vector3i &p_size) {
	redistance(r_values, p_size);
}

// --- Settings ------------------------------------------------------------------

void SculptTerrain::set_material(const Ref<ShaderMaterial> &p_material) {
	material = p_material;
	bind_material();
}

void SculptTerrain::set_prepass_material(const Ref<ShaderMaterial> &p_material) {
	prepass_material = p_material;
	bind_material();
}

void SculptTerrain::set_view_distance(double p_value) {
	view_distance = std::max(p_value, 16.0);
	bind_material();
}

Variant SculptTerrain::find_island(const Vector3 &p_near) const {
	Vector3 top;
	return world.find_island(p_near, top) ? Variant(top) : Variant();
}

void SculptTerrain::set_clip_size(int p_size) {
	ERR_FAIL_COND_MSG(p_size < 2 * Clipmap::BRICK || p_size % Clipmap::BRICK != 0, vformat("SculptTerrain.clip_size must be a multiple of %d.", Clipmap::BRICK));
	clip_size = p_size;
}

void SculptTerrain::set_clip_levels(int p_levels) {
	ERR_FAIL_COND_MSG(p_levels < 1 || p_levels > MAX_CLIP_LEVELS, vformat("SculptTerrain.clip_levels must be 1 to %d.", MAX_CLIP_LEVELS));
	clip_levels = p_levels;
}

int SculptTerrain::worker_count() const {
	return resolve_thread_count(thread_count);
}

void SculptTerrain::report_world_error(const String &p_what) {
	UtilityFunctions::push_error(vformat("SculptTerrain: %s: %s", p_what, world.get_last_error()));
}

// --- World -----------------------------------------------------------------------

bool SculptTerrain::open_world() {
	if (opened) {
		save_world();
	}
	clipmap.cancel_jobs(); // they read the generator settings
	cache_generation++;
	world.configure(island_settings);
	// Generates the whole world (it all stays in memory), then the edits.
	const auto started = std::chrono::steady_clock::now();
	const bool ok = world.open(save_path, worker_count());
	if (!ok) {
		report_world_error("could not open " + save_path + " (the world won't be saved)");
	}
	island_settings = world.get_settings(); // a saved world keeps its own
	const int64_t opened_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
	clipmap.configure(clip_size, clip_levels);
	for (Ref<Texture2DArray> &texture : clip_textures) {
		texture.unref();
	}
	// Create every texture now (empty), while the game is loading: creating
	// one later means uploading all of it in one frame.
	for (int l = 0; l < clip_levels; l++) {
		TypedArray<Image> layers;
		layers.resize(clip_size);
		const Ref<Image> blank = Image::create_empty(clip_size, clip_size, false, Image::FORMAT_RGB8);
		for (int z = 0; z < clip_size; z++) {
			layers[z] = blank;
		}
		clip_textures[l].instantiate();
		clip_textures[l]->create_from_images(layers);
	}
	// Brick distances of all levels: layer l * bricks + z holds slice z of level l.
	const int nb = clipmap.get_brick_count();
	TypedArray<Image> brick_layers;
	brick_layers.resize(nb * clip_levels);
	const Ref<Image> blank_bricks = Image::create_empty(nb, nb, false, Image::FORMAT_R8);
	for (int i = 0; i < nb * clip_levels; i++) {
		brick_layers[i] = blank_bricks;
	}
	brick_texture.instantiate();
	brick_texture->create_from_images(brick_layers);
	UtilityFunctions::print(vformat("SculptTerrain: %d x %d x %d world opened in %d ms, %d edited chunks",
			island_settings.world_size, island_settings.world_height, island_settings.world_size, opened_ms,
			world.get_edited_chunk_count()));
	opened = true;
	last_save = std::chrono::steady_clock::now();
	return ok;
}

bool SculptTerrain::save_world() {
	if (!opened || save_path.is_empty()) {
		return true;
	}
	const bool ok = world.save();
	if (!ok) {
		report_world_error("saving failed");
	}
	last_save = std::chrono::steady_clock::now();
	return ok;
}

void SculptTerrain::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE:
			singleton = this;
			break;
		case NOTIFICATION_READY:
			set_physics_process(!Engine::get_singleton()->is_editor_hint());
			break;
		case NOTIFICATION_PHYSICS_PROCESS:
			if (opened && physics_enabled) {
				DensityBody3D::step_all(this, get_physics_process_delta_time(), Vector3(0, -(float)gravity, 0));
			}
			break;
		case NOTIFICATION_EXIT_TREE:
			if (singleton == this) {
				singleton = nullptr;
			}
			save_world();
			break;
	}
}

// --- Clipmap -------------------------------------------------------------------

void SculptTerrain::update_view(const Vector3 &p_camera_position, bool p_wait) {
	if (!opened) {
		return;
	}
	// Level 0 is built at once the first time, so there is something to see.
	const int moved = clipmap.follow(world, p_camera_position, worker_count(), true, p_wait);
	// Upload changed layers, nearest level first, at most LAYERS_PER_FRAME
	// (each is clip_size^2 * 2 bytes); a level that moved switches to its new
	// origin once all of its changed layers are up.
	bool changed = moved != 0;
	int budget = p_wait ? 1 << 30 : LAYERS_PER_FRAME;
	for (int l = 0; l < clipmap.get_level_count(); l++) {
		changed |= upload_level(l, budget);
	}
	if (changed) {
		bind_material();
	}
	for (const Ref<ShaderMaterial> &m : { material, prepass_material }) {
		if (m.is_valid()) {
			m->set_shader_parameter("view_position", p_camera_position);
		}
	}
	// Keep the area around the camera in memory (a few milliseconds' worth
	// of loading a frame), and now and then let go of what's far away.
	world.load_near(p_camera_position, (float)simulation_radius, LOAD_BUDGET_MS);
	if (++frames_since_unload >= 60) {
		frames_since_unload = 0;
		world.unload_far(p_camera_position, (float)simulation_radius + 64.0f);
	}
	bind_bodies(p_camera_position);
	update_selection_mask();
	// Autosave.
	if (autosave_interval > 0.0 && std::chrono::duration<double>(std::chrono::steady_clock::now() - last_save).count() >= autosave_interval) {
		save_world();
	}
}

bool SculptTerrain::upload_level(int p_level, int &r_budget) {
	Clipmap::Level &level = clipmap.level(p_level);
	if (!level.valid) {
		return false;
	}
	const int n = clipmap.get_size();
	const int64_t layer_bytes = (int64_t)n * n * Clipmap::TEXEL_BYTES;
	auto layer_image = [&](int p_layer) {
		PackedByteArray bytes;
		bytes.resize(layer_bytes);
		std::memcpy(bytes.ptrw(), &level.texels[(size_t)p_layer * layer_bytes], layer_bytes);
		upload_bytes += layer_bytes;
		return Image::create_from_data(n, n, false, Image::FORMAT_RGB8, bytes);
	};
	Ref<Texture2DArray> &texture = clip_textures[p_level];
	bool changed = false;
	// Brick distances are tiny: all of the level's go up whenever they change.
	if (level.bricks_dirty) {
		const int nb = clipmap.get_brick_count();
		const int64_t brick_layer_bytes = (int64_t)nb * nb;
		PackedByteArray bytes;
		bytes.resize(brick_layer_bytes);
		for (int z = 0; z < nb; z++) {
			std::memcpy(bytes.ptrw(), &level.bricks[(size_t)z * brick_layer_bytes], brick_layer_bytes);
			brick_texture->update_layer(Image::create_from_data(nb, nb, false, Image::FORMAT_R8, bytes), p_level * nb + z);
		}
		upload_bytes += brick_layer_bytes * nb;
		level.bricks_dirty = false;
	}
	// Level 0's first contents go up at once so there is something to see.
	const bool unlimited = p_level == 0 && !level.published;
	for (int z = 0; z < n && (r_budget > 0 || unlimited); z++) {
		if (level.layer_dirty[z]) {
			texture->update_layer(layer_image(z), z);
			level.layer_dirty[z] = 0;
			r_budget--;
		}
	}
	const bool all_up = std::find(level.layer_dirty.begin(), level.layer_dirty.end(), 1) == level.layer_dirty.end();
	if (all_up && (!level.published || level.published_origin != level.origin)) {
		level.published_origin = level.origin;
		level.published = true;
		changed = true;
	}
	return changed;
}

void SculptTerrain::bind_material() {
	PackedVector3Array origins;
	for (int l = 0; l < MAX_CLIP_LEVELS; l++) {
		const bool used = l < clipmap.get_level_count() && clipmap.level(l).published;
		origins.push_back(used ? Vector3(clipmap.level(l).published_origin * clipmap.level(l).scale) : Vector3());
	}
	int count = 0;
	while (count < clipmap.get_level_count() && clipmap.level(count).published && clip_textures[count].is_valid()) {
		count++;
	}
	for (const Ref<ShaderMaterial> &m : { material, prepass_material }) {
		if (m.is_null()) {
			continue;
		}
		for (int l = 0; l < MAX_CLIP_LEVELS; l++) {
			m->set_shader_parameter(vformat("clip_texture%d", l), clip_textures[l]);
		}
		m->set_shader_parameter("clip_bricks", brick_texture);
		m->set_shader_parameter("clip_brick_count", (float)clipmap.get_brick_count());
		m->set_shader_parameter("clip_count", count);
		m->set_shader_parameter("clip_origin", origins);
		m->set_shader_parameter("clip_size", (float)clipmap.get_size());
		m->set_shader_parameter("clip_reach", (float)clipmap.reach());
		m->set_shader_parameter("view_distance_limit", (float)view_distance);
	}
	if (material.is_valid()) {
		// Terrain worlds have a sea (islands don't).
		material->set_shader_parameter("has_sea", world.get_settings().world_type == WORLD_TERRAIN);
		material->set_shader_parameter("sea_level", world.get_settings().sea_level);
	}
}

int64_t SculptTerrain::take_upload_bytes() {
	const int64_t bytes = upload_bytes;
	upload_bytes = 0;
	return bytes;
}

Vector3i SculptTerrain::get_clip_origin(int p_level) const {
	ERR_FAIL_INDEX_V(p_level, clipmap.get_level_count(), Vector3i());
	return clipmap.level(p_level).world_origin();
}

static bool texel_in_level(const Vector3i &p_texel, int p_size) {
	return p_texel.x >= 0 && p_texel.y >= 0 && p_texel.z >= 0 && p_texel.x < p_size && p_texel.y < p_size && p_texel.z < p_size;
}

double SculptTerrain::get_clip_texel(int p_level, const Vector3i &p_texel) const {
	ERR_FAIL_INDEX_V(p_level, clipmap.get_level_count(), 0.0);
	const Clipmap::Level &level = clipmap.level(p_level);
	ERR_FAIL_COND_V(!level.valid || !texel_in_level(p_texel, clipmap.get_size()), 0.0);
	return clipmap.texel_value(level, level.origin + p_texel);
}

int SculptTerrain::get_clip_brick(int p_level, const Vector3i &p_texel) const {
	ERR_FAIL_INDEX_V(p_level, clipmap.get_level_count(), 0);
	const Clipmap::Level &level = clipmap.level(p_level);
	ERR_FAIL_COND_V(!level.valid || !texel_in_level(p_texel, clipmap.get_size()), 0);
	return clipmap.brick_value(level, level.origin + p_texel);
}

double SculptTerrain::get_lod_density(const Vector3i &p_point, int p_level) {
	world.prepare(p_point, p_point, p_level);
	MaterialId material;
	return world.lod_texel(p_point.x, p_point.y, p_point.z, p_level, material) * DENSITY_STEP * (1 << p_level);
}

// --- Physics -------------------------------------------------------------------

SculptTerrain *SculptTerrain::singleton = nullptr;

void SculptTerrain::shift_origin(const Vector3i &p_offset) {
	const int size = world.world_size();
	ERR_FAIL_COND_MSG(!opened, "SculptTerrain: call open_world() first.");
	ERR_FAIL_COND_MSG(p_offset.y != 0 || p_offset.x % size != 0 || p_offset.z % size != 0,
			"SculptTerrain.shift_origin(): the offset must be whole world sizes in x and z.");
	clipmap.shift(world, p_offset);
	DensityBody3D::shift_all(Vector3(p_offset));
	shift_selection(p_offset);
	bind_material();
}

void SculptTerrain::mark_edited(const Vector3i &p_min, const Vector3i &p_max) {
	cache_generation++;
	DensityBody3D::wake_near(Vector3(p_min), Vector3(p_max + Vector3i(1, 1, 1)), world.world_size());
}

int SculptTerrain::grid_slot(DensityBody3D *p_body, const Ref<DensityShape> &p_shape, int p_key) {
	const uint64_t id = p_body->get_instance_id();
	const int layers = std::clamp(p_shape->get_size().z, 1, GRID_SLOT);
	int found = -1;
	for (int i = 0; i < (int)grid_slots.size(); i++) {
		const GridSlot &s = grid_slots[i];
		if (s.body == id && s.key == p_key && s.shape == p_shape.ptr()) {
			found = i;
			break;
		}
	}
	if (found >= 0 && grid_slots[found].count != layers) {
		grid_slots.erase(grid_slots.begin() + found); // grown: somewhere new
		found = -1;
	}
	if (found < 0) {
		// A run of free layers (first fit), making room by dropping what
		// was used longest ago (not this frame).
		const int total = GRID_SLOTS * GRID_SLOT;
		int base = -1;
		while (true) {
			std::sort(grid_slots.begin(), grid_slots.end(), [](const GridSlot &a, const GridSlot &b) { return a.base < b.base; });
			int at = 0;
			bool fits = false;
			for (const GridSlot &s : grid_slots) {
				if (s.base - at >= layers) {
					fits = true; // the gap before this one
					break;
				}
				at = std::max(at, s.base + s.count);
			}
			if (fits || total - at >= layers) {
				base = at;
				break;
			}
			int oldest = -1;
			for (int i = 0; i < (int)grid_slots.size(); i++) {
				if (grid_slots[i].used <= frame_clock && (oldest < 0 || grid_slots[i].used < grid_slots[oldest].used)) {
					oldest = i;
				}
			}
			if (oldest < 0) {
				return -1; // all in use this frame
			}
			grid_slots.erase(grid_slots.begin() + oldest);
		}
		GridSlot s;
		s.base = base;
		s.count = layers;
		s.body = id;
		s.key = p_key;
		s.shape = p_shape.ptr();
		grid_slots.push_back(s);
		found = (int)grid_slots.size() - 1;
	}
	GridSlot &s = grid_slots[found];
	if (s.version != p_shape->get_version()) {
		PackedByteArray bytes;
		bytes.resize(GRID_SLOT * GRID_SLOT * 3);
		for (int z = 0; z < layers; z++) {
			p_shape->fill_slice(p_key, z, bytes.ptrw());
			body_atlas->update_layer(Image::create_from_data(GRID_SLOT, GRID_SLOT, false, Image::FORMAT_RGB8, bytes), s.base + z);
		}
		s.version = p_shape->get_version();
	}
	s.used = ++slot_clock;
	return s.base;
}

void SculptTerrain::bind_bodies(const Vector3 &p_camera) {
	if (material.is_null()) {
		return;
	}
	// Drawn bodies, nearest the camera first: sphere clusters (drawn as
	// exact spheres) and grids (marched through the atlas).
	struct Drawn {
		float distance;
		DensityBody3D *body;
	};
	std::vector<Drawn> sphere_bodies, grid_bodies;
	for (DensityBody3D *b : DensityBody3D::all()) {
		if (!b->is_inside_tree() || !b->is_drawn() || !b->is_visible_in_tree()) {
			continue;
		}
		const float distance = std::max(b->get_global_center_of_mass().distance_to(p_camera) - (float)b->get_bound_radius(), 0.0f);
		if (b->get_shape().is_valid() && b->get_shape()->get_key_count() > 0) {
			grid_bodies.push_back({ distance, b });
		} else {
			sphere_bodies.push_back({ distance, b });
		}
	}
	auto nearest = [](const Drawn &a, const Drawn &b) { return a.distance < b.distance; };
	std::sort(sphere_bodies.begin(), sphere_bodies.end(), nearest);
	std::sort(grid_bodies.begin(), grid_bodies.end(), nearest);

	// Spheres. The uniforms' full length, so a shrinking list leaves nothing stale.
	PackedVector4Array spheres, colors;
	spheres.resize(MAX_BODY_SPHERES);
	colors.resize(MAX_BODY_SPHERES);
	int sphere_count = 0;
	for (const Drawn &d : sphere_bodies) {
		const Transform3D t = d.body->get_global_transform();
		const float s = t.basis.get_column(0).length();
		const Color c = d.body->get_color();
		const PackedVector4Array list = d.body->get_spheres();
		for (int64_t i = 0; i < list.size() && sphere_count < MAX_BODY_SPHERES; i++) {
			const Vector3 centre = t.xform(Vector3(list[i].x, list[i].y, list[i].z));
			spheres.set(sphere_count, Vector4(centre.x, centre.y, centre.z, list[i].w * s));
			colors.set(sphere_count, Vector4(c.r, c.g, c.b, (float)d.body->get_material()));
			sphere_count++;
		}
	}
	material->set_shader_parameter("body_spheres", spheres);
	material->set_shader_parameter("body_colors", colors);
	material->set_shader_parameter("body_sphere_count", sphere_count);

	// Grids: each pose key in a slot of the atlas (uploaded when new or
	// sculpted), the two keys around the body's pose blended by the shader.
	if (body_atlas.is_null()) {
		TypedArray<Image> layers;
		layers.resize(GRID_SLOTS * GRID_SLOT);
		PackedByteArray air;
		air.resize(GRID_SLOT * GRID_SLOT * 3);
		for (int64_t i = 0; i < air.size(); i += 3) {
			air.set(i, (uint8_t)(128 - DENSITY_LIMIT));
			air.set(i + 1, (uint8_t)MAT_STONE);
			air.set(i + 2, 0);
		}
		const Ref<Image> blank = Image::create_from_data(GRID_SLOT, GRID_SLOT, false, Image::FORMAT_RGB8, air);
		for (int i = 0; i < GRID_SLOTS * GRID_SLOT; i++) {
			layers[i] = blank;
		}
		body_atlas.instantiate();
		body_atlas->create_from_images(layers);
		grid_slots.clear();
		material->set_shader_parameter("body_atlas", body_atlas);
	}
	PackedVector4Array rows0, rows1, rows2, sizes, poses, bounds;
	for (PackedVector4Array *a : { &rows0, &rows1, &rows2, &sizes, &poses, &bounds }) {
		a->resize(MAX_GRID_BODIES);
	}
	frame_clock = slot_clock;
	int grid_count = 0;
	for (const Drawn &d : grid_bodies) {
		if (grid_count >= MAX_GRID_BODIES) {
			break;
		}
		const Ref<DensityShape> shape = d.body->get_shape();
		if (shape.is_null() || shape->get_key_count() == 0) {
			continue;
		}
		int ka, kb;
		float blend;
		shape->pose_keys(d.body->get_pose(), ka, kb, blend);
		const int slot_a = grid_slot(d.body, shape, ka);
		const int slot_b = kb == ka ? slot_a : grid_slot(d.body, shape, kb);
		if (slot_a < 0 || slot_b < 0) {
			break;
		}
		// World point -> grid point: (B^-1 (p - T) - origin) / voxel_size, for
		// any basis B (a transformed selection may be scaled unevenly).
		const Transform3D t = d.body->get_global_transform();
		const Basis inverse = t.basis.inverse();
		const float vs = (float)shape->get_voxel_size();
		const Vector3 o = shape->get_origin();
		const float s_min = std::max(std::min({ t.basis.get_column(0).length(), t.basis.get_column(1).length(), t.basis.get_column(2).length() }), 1e-6f);
		const float s_max = std::max({ t.basis.get_column(0).length(), t.basis.get_column(1).length(), t.basis.get_column(2).length() });
		const float s_first = std::max(t.basis.get_column(0).length(), 1e-6f);
		PackedVector4Array *rows[3] = { &rows0, &rows1, &rows2 };
		for (int a = 0; a < 3; a++) {
			const Vector3 row = inverse.rows[a] / vs;
			rows[a]->set(grid_count, Vector4(row.x, row.y, row.z, -row.dot(t.origin) - o[a] / vs));
		}
		const Vector3i n = shape->get_size();
		// Densities times the smallest scale are safe world distances to step.
		sizes.set(grid_count, Vector4((float)n.x, (float)n.y, (float)n.z, vs * s_min));
		poses.set(grid_count, Vector4((float)slot_a, (float)slot_b, blend, 0.0f));
		const Vector3 c = d.body->get_global_center_of_mass();
		bounds.set(grid_count, Vector4(c.x, c.y, c.z, (float)d.body->get_bound_radius() * s_max / s_first + vs * s_max));
		grid_count++;
	}
	material->set_shader_parameter("grid_rows0", rows0);
	material->set_shader_parameter("grid_rows1", rows1);
	material->set_shader_parameter("grid_rows2", rows2);
	material->set_shader_parameter("grid_size", sizes);
	material->set_shader_parameter("grid_pose", poses);
	material->set_shader_parameter("grid_bound", bounds);
	material->set_shader_parameter("grid_count", grid_count);
}

Dictionary SculptTerrain::get_stats() const {
	Dictionary stats;
	stats["edited_chunks"] = world.get_edited_chunk_count();
	stats["memory_mb"] = world.get_memory_bytes() >> 20;
	stats["full_chunks"] = world.get_full_chunk_count();
	stats["loaded_chunks"] = world.get_loaded_chunk_count();
	stats["repaired_chunks"] = world.get_repaired_chunk_count();
	stats["unsaved_chunks"] = world.get_dirty_chunk_count();
	stats["region_files"] = world.get_region_file_count();
	stats["last_error"] = world.get_last_error();
	return stats;
}

// --- Sculpting -----------------------------------------------------------------

int8_t SculptTerrain::cached_point(int x, int y, int z) {
	// Unedited ground is computed from noise wherever it's read, which is
	// slow for physics reading the same points over and over: keep the
	// latest ones in a table (one slot per hash; a newer point takes it
	// over), forgotten all at once by an edit (a new generation).
	if (point_cache.empty()) {
		point_cache.resize(POINT_CACHE_SIZE);
	}
	const uint32_t h = ((uint32_t)x * 73856093u) ^ ((uint32_t)y * 19349663u) ^ ((uint32_t)z * 83492791u);
	CachedPoint &e = point_cache[(h ^ (h >> 15)) & (POINT_CACHE_SIZE - 1)];
	if (e.generation != cache_generation || e.x != x || e.y != y || e.z != z) {
		e.x = x;
		e.y = y;
		e.z = z;
		e.generation = cache_generation;
		e.value = world.point(x, y, z);
	}
	return e.value;
}

float SculptTerrain::cached_sample(const Vector3 &p) {
	const float fx = std::floor(p.x), fy = std::floor(p.y), fz = std::floor(p.z);
	const int x = (int)fx, y = (int)fy, z = (int)fz;
	const float tx = p.x - fx, ty = p.y - fy, tz = p.z - fz;
	float c[8];
	for (int k = 0; k < 8; k++) {
		c[k] = cached_point(x + (k & 1), y + ((k >> 1) & 1), z + ((k >> 2) & 1)) * DENSITY_STEP;
	}
	auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
	return lerp(lerp(lerp(c[0], c[1], tx), lerp(c[2], c[3], tx), ty), lerp(lerp(c[4], c[5], tx), lerp(c[6], c[7], tx), ty), tz);
}

double SculptTerrain::get_density(const Vector3 &p_pos) {
	return cached_sample(p_pos);
}

Vector3 SculptTerrain::get_normal(const Vector3 &p_pos, double p_spread) {
	const float h = (float)std::max(p_spread, 0.25);
	const Vector3 g(cached_sample(p_pos + Vector3(h, 0, 0)) - cached_sample(p_pos - Vector3(h, 0, 0)),
			cached_sample(p_pos + Vector3(0, h, 0)) - cached_sample(p_pos - Vector3(0, h, 0)),
			cached_sample(p_pos + Vector3(0, 0, h)) - cached_sample(p_pos - Vector3(0, 0, h)));
	return g.length_squared() > 1e-12f ? -g.normalized() : Vector3(0, 1, 0);
}

int SculptTerrain::get_block_material(const Vector3 &p_pos) {
	if (!opened) {
		return -1;
	}
	// The most solid corner of the lattice cell holding the position.
	const int x = (int)std::floor(p_pos.x), y = (int)std::floor(p_pos.y), z = (int)std::floor(p_pos.z);
	int best = INT32_MIN;
	Vector3i at;
	for (int k = 0; k < 8; k++) {
		const Vector3i c(x + (k & 1), y + ((k >> 1) & 1), z + ((k >> 2) & 1));
		const int v = world.point(c.x, c.y, c.z);
		if (v > best) {
			best = v;
			at = c;
		}
	}
	if (best * DENSITY_STEP < -1.0f) {
		return -1;
	}
	return world.point_material(at.x, at.y, at.z);
}

// --- Color painting ----------------------------------------------------------------

void SculptTerrain::set_material_colors(const PackedColorArray &p_colors) {
	material_colors.assign(p_colors.ptr(), p_colors.ptr() + p_colors.size());
}

Color SculptTerrain::get_material_color(int p_material) const {
	const MaterialId m = (MaterialId)p_material;
	if (is_color_material(m)) {
		return Color(((m >> 10) & 31) / 31.0f, ((m >> 5) & 31) / 31.0f, (m & 31) / 31.0f);
	}
	const uint8_t layer = render_layer(m);
	return layer < material_colors.size() ? material_colors[layer] : Color(0.5f, 0.5f, 0.5f);
}

int SculptTerrain::color_to_material(const Color &p_color) {
	auto channel = [](float v) { return std::clamp((int)std::lround(v * 31.0f), 0, 31); };
	return color_material(channel(p_color.r), channel(p_color.g), channel(p_color.b));
}

MaterialId SculptTerrain::stroke_start_material(int x, int y, int z, MaterialId p_now) const {
	const int C = VoxelWorld::CHUNK;
	const Vector3i c = VoxelWorld::chunk_of(x, y, z);
	auto it = recording.index.find(world.wrap_chunk(c));
	if (it == recording.index.end()) {
		return p_now;
	}
	return recording.chunks[it->second].materials[((size_t)(z - c.z * C) * C + (y - c.y * C)) * C + (x - c.x * C)];
}

void SculptTerrain::paint_color(const Vector3 &p_center, double p_radius, const Color &p_color, double p_opacity, double p_hardness) {
	ERR_FAIL_COND_MSG(!opened, "SculptTerrain: call open_world() first.");
	if (p_radius <= 0.0) {
		return;
	}
	const float r = (float)p_radius;
	const float opacity = std::clamp((float)p_opacity, 0.0f, 1.0f);
	const float hard = std::clamp((float)p_hardness, 0.0f, 0.999f);
	Vector3i lo, hi;
	for (int a = 0; a < 3; a++) {
		lo[a] = (int)std::floor(p_center[a] - r - 1.0f);
		hi[a] = (int)std::ceil(p_center[a] + r + 1.0f);
	}
	const Vector3i n = hi - lo + Vector3i(1, 1, 1);
	std::vector<float> before((size_t)n.x * n.y * n.z);
	std::vector<MaterialId> materials_before(before.size());
	world.read_points(lo, hi, before.data(), worker_count(), materials_before.data());
	std::vector<MaterialId> materials_after = materials_before;
	const int C = VoxelWorld::CHUNK;
	size_t i = 0;
	bool any = false;
	for (int z = lo.z; z <= hi.z; z++) {
		for (int y = lo.y; y <= hi.y; y++) {
			for (int x = lo.x; x <= hi.x; x++, i++) {
				// The surface skin (both sides) within the radius, as Paint.
				const float dist = (Vector3((float)x, (float)y, (float)z) - p_center).length() / r;
				if (dist > 1.0f || std::abs(before[i]) >= 2.5f) {
					continue;
				}
				if (selected_count > 0 && !is_selected(x, y, z) &&
						!(before[i] <= 0.0f && (is_selected(x + 1, y, z) || is_selected(x - 1, y, z) || is_selected(x, y + 1, z) ||
												  is_selected(x, y - 1, z) || is_selected(x, y, z + 1) || is_selected(x, y, z - 1)))) {
					continue; // the selection masks it
				}
				float t = dist <= hard ? 0.0f : (dist - hard) / (1.0f - hard);
				const float coverage = 1.0f - t * t * (3.0f - 2.0f * t); // smoothstep fade
				const uint8_t want = (uint8_t)std::lround(coverage * 255.0f);
				const Vector3i c = VoxelWorld::chunk_of(x, y, z);
				std::vector<uint8_t> &chunk_coverage = stroke_coverage[world.wrap_chunk(c)];
				if (chunk_coverage.empty()) {
					chunk_coverage.assign(VoxelWorld::CHUNK_POINTS, 0);
				}
				uint8_t &had = chunk_coverage[((size_t)(z - c.z * C) * C + (y - c.y * C)) * C + (x - c.x * C)];
				if (want <= had) {
					continue;
				}
				had = want;
				const Color base = get_material_color(stroke_start_material(x, y, z, materials_before[i]));
				materials_after[i] = (MaterialId)color_to_material(base.lerp(p_color, opacity * want / 255.0f));
				any = true;
			}
		}
	}
	if (!any) {
		return;
	}
	const std::vector<uint8_t> brushed(before.size(), 0);
	commit_edit(lo, hi, lo, n, before, before, brushed, true, materials_before, materials_after);
}

PackedStringArray SculptTerrain::get_material_names() {
	PackedStringArray names;
	for (int i = 0; i < MATERIAL_COUNT; i++) {
		names.push_back(MATERIAL_NAMES[i]);
	}
	return names;
}

void SculptTerrain::apply_brush(const Vector3 &p_center, double p_radius, double p_strength, int p_mode, int p_material) {
	ERR_FAIL_COND_MSG(!opened, "SculptTerrain: call open_world() first.");
	ERR_FAIL_COND(p_mode < BRUSH_ADD || p_mode > BRUSH_PINCH);
	ERR_FAIL_COND(p_material > 0xFFFF);
	if (p_radius <= 0.0 || (p_mode == BRUSH_PAINT && p_material < 0)) {
		return;
	}
	const float r = (float)p_radius, s = (float)p_strength, C = DENSITY_CLAMP;
	// Points the brush rewrites ("edit box"): adding or carving a sphere also
	// changes the distances out to CLAMP around it.
	const bool sphere_mode = p_mode == BRUSH_ADD || p_mode == BRUSH_SUBTRACT;
	const float reach = sphere_mode ? r + C : r + 1.0f;
	// Pinch reads from up to half a radius farther out.
	const float pinch_k = p_mode == BRUSH_PINCH ? std::clamp(s / r, -0.3f, 0.3f) : 0.0f;
	Vector3i lo, hi;
	for (int a = 0; a < 3; a++) {
		lo[a] = (int)std::floor(p_center[a] - reach);
		hi[a] = (int)std::ceil(p_center[a] + reach);
	}
	// Distances are recomputed from the surface, which can be up to CLAMP
	// outside the edit box: read that much more around it.
	const int margin = (int)std::ceil(C) + 1;
	const Vector3i rlo = lo - Vector3i(margin, margin, margin), rhi = hi + Vector3i(margin, margin, margin);
	const Vector3i n = rhi - rlo + Vector3i(1, 1, 1);
	auto index = [&](int x, int y, int z) { return ((size_t)(z - rlo.z) * n.y + (y - rlo.y)) * n.x + (x - rlo.x); };
	std::vector<float> before((size_t)n.x * n.y * n.z);
	std::vector<MaterialId> materials_before(before.size());
	world.read_points(rlo, rhi, before.data(), worker_count(), materials_before.data());
	std::vector<float> after = before;
	std::vector<MaterialId> materials_after = materials_before;
	const MaterialId paint = (MaterialId)std::max(p_material, 0);

	// The brushes, in a form that keeps the density a distance field: a
	// blend with a fixed weight between two distance fields is one too.
	//   ADD / SUBTRACT: blend towards the terrain with a sphere (radius r at
	//   the centre) added / carved. The weight sets the speed: the surface at
	//   the centre moves about `s` voxels.
	//   SMOOTH: blend towards the average of each point's neighbours.
	//   FLATTEN: blend towards a plane through the centre, facing the surface
	//   there. The last two fade out towards the brush edge; the distance
	//   repair below puts back what that bends.
	const float blend = std::clamp(s / r, 0.0f, 1.0f);
	// Flatten: towards the plane set for the stroke (set_flatten_plane()),
	// or else one through the centre facing the surface there.
	const Vector3 plane_point = flatten_plane_set ? flatten_point : p_center;
	const Vector3 plane_normal = p_mode != BRUSH_FLATTEN ? Vector3() : (flatten_plane_set ? flatten_normal : get_normal(p_center, p_radius * 0.5));
	// Pinch squeezes across the surface, not into it: along the plane
	// facing the surface's general direction at the centre.
	const Vector3 pinch_normal = p_mode == BRUSH_PINCH ? get_normal(p_center, p_radius * 0.5) : Vector3();
	auto sample_before = [&](const Vector3 &q) {
		const Vector3 c = q.clamp(Vector3(rlo), Vector3(rhi) - Vector3(1, 1, 1) * 0.001f);
		const int x0 = (int)std::floor(c.x), y0 = (int)std::floor(c.y), z0 = (int)std::floor(c.z);
		const int x1 = std::min(x0 + 1, rhi.x), y1 = std::min(y0 + 1, rhi.y), z1 = std::min(z0 + 1, rhi.z);
		const float fx = c.x - x0, fy = c.y - y0, fz = c.z - z0;
		auto v = [&](int x, int y, int z) { return before[index(x, y, z)]; };
		auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
		return lerp(lerp(lerp(v(x0, y0, z0), v(x1, y0, z0), fx), lerp(v(x0, y1, z0), v(x1, y1, z0), fx), fy),
				lerp(lerp(v(x0, y0, z1), v(x1, y0, z1), fx), lerp(v(x0, y1, z1), v(x1, y1, z1), fx), fy), fz);
	};
	for (int z = lo.z; z <= hi.z; z++) {
		for (int y = lo.y; y <= hi.y; y++) {
			for (int x = lo.x; x <= hi.x; x++) {
				const Vector3 p((float)x, (float)y, (float)z);
				const size_t i = index(x, y, z);
				const float v = before[i];
				const float dist = (p - p_center).length();
				switch (p_mode) {
					case BRUSH_ADD:
						after[i] = v + (std::max(v, std::min(r - dist, C)) - v) * blend;
						// What it builds (and the air just outside, which
						// carries the surface's material) is the brush's.
						if (p_material >= 0 && after[i] > v + 1e-4f && after[i] > -2.0f) {
							materials_after[i] = paint;
						}
						break;
					case BRUSH_SUBTRACT:
						after[i] = v + (std::min(v, std::max(dist - r, -C)) - v) * blend;
						break;
					case BRUSH_SMOOTH: {
						const float f = falloff(dist / r);
						const float avg = (v + before[index(x - 1, y, z)] + before[index(x + 1, y, z)] + before[index(x, y - 1, z)] +
												  before[index(x, y + 1, z)] + before[index(x, y, z - 1)] + before[index(x, y, z + 1)]) /
								7.0f;
						after[i] = v + (avg - v) * std::min(s * f, 1.0f);
					} break;
					case BRUSH_FLATTEN: {
						const float f = falloff(dist / r);
						const float target = std::clamp(-(p - plane_point).dot(plane_normal), -C, C); // positive below
						after[i] = v + (target - v) * std::min(s * f, 1.0f);
					} break;
					case BRUSH_INFLATE:
					// Every part of the surface moves along its own normal:
					// the distance field just shifts (s voxels at the centre).
					if (std::abs(v) < C) {
						after[i] = std::clamp(v + s * falloff(dist / r), -C, C);
					}
					break;
				case BRUSH_PINCH: {
					// Each point takes the value from a little farther out across
					// the surface: material slides in towards the middle line.
					const float f = falloff(dist / r);
					if (f > 0.0f) {
						const Vector3 d = p - p_center;
						const Vector3 along = pinch_normal * d.dot(pinch_normal);
						const Vector3 q = p_center + along + (d - along) * (1.0f + pinch_k * f);
						after[i] = sample_before(q);
						const Vector3i qi((int)std::lround(q.x), (int)std::lround(q.y), (int)std::lround(q.z));
						const Vector3i qc = qi.clamp(rlo, rhi);
						materials_after[i] = materials_before[index(qc.x, qc.y, qc.z)];
					}
				} break;
				case BRUSH_PAINT:
						// The surface skin (both sides) within the radius.
						if (dist <= r && std::abs(v) < 2.5f) {
							materials_after[i] = paint;
						}
						break;
				}
				// Ground raised where there was air takes the material of
				// the most solid ground next to it.
				if ((p_mode == BRUSH_SMOOTH || p_mode == BRUSH_FLATTEN || p_mode == BRUSH_INFLATE) && v <= 0.0f && after[i] > 0.0f) {
					const size_t neighbours[6] = { index(x - 1, y, z), index(x + 1, y, z), index(x, y - 1, z), index(x, y + 1, z), index(x, y, z - 1), index(x, y, z + 1) };
					size_t from = i;
					for (size_t j : neighbours) {
						if (before[j] > before[from]) {
							from = j;
						}
					}
					materials_after[i] = materials_before[from];
				}
			}
		}
	}

	// Repair: make every point of the edit box hold its true distance to the
	// surface again (signed, clamped to CLAMP), whatever the brush (or an
	// older edit) did to the values around it. Lighting and ray stepping rely
	// on the density reading as a distance.
	std::vector<Vector3i> grown;
	mask_to_selection(lo, hi, rlo, n, before, after, materials_before, materials_after, false, grown);
	if (blocky_size >= 2 && p_mode != BRUSH_PAINT) {
		snap_to_blocks(lo, hi, rlo, n, before, after, materials_after);
	}
	// Which points the brush itself changed (before the repair).
	std::vector<uint8_t> brushed(after.size(), 0);
	for (size_t i = 0; i < after.size(); i++) {
		brushed[i] = std::abs(after[i] - before[i]) > 1e-5f;
	}
	if (p_mode != BRUSH_PAINT) {
		redistance(after, n);
	}

	commit_edit(lo, hi, rlo, n, before, after, brushed, true, materials_before, materials_after);
	select_grown(grown);
}

void SculptTerrain::select_grown(const std::vector<Vector3i> &p_points) {
	for (const Vector3i &p : p_points) {
		if (world.sample(Vector3(p)) > 0.0f) {
			set_selected(p.x, p.y, p.z, true);
		}
	}
}

void SculptTerrain::snap_to_blocks(const Vector3i &lo, const Vector3i &hi, const Vector3i &rlo, const Vector3i &n,
		const std::vector<float> &before, std::vector<float> &after, std::vector<MaterialId> &materials_after) const {
	auto index = [&](int x, int y, int z) { return ((size_t)(z - rlo.z) * n.y + (y - rlo.y)) * n.x + (x - rlo.x); };
	const int B = blocky_size;
	// Every block (on a grid fixed to the world) the tool changed any point
	// of becomes one value: the average of its points (so growth on any of
	// its sides counts the same), with the material of its most solid point.
	// Blocks reaching past the edit box are averaged whole but only written
	// inside it; tools change points well inside it, so that's rare.
	const Vector3i b0(floor_div(lo.x, B), floor_div(lo.y, B), floor_div(lo.z, B));
	const Vector3i b1(floor_div(hi.x, B), floor_div(hi.y, B), floor_div(hi.z, B));
	const Vector3i rhi = rlo + n - Vector3i(1, 1, 1);
	for (int bz = b0.z; bz <= b1.z; bz++) {
		for (int by = b0.y; by <= b1.y; by++) {
			for (int bx = b0.x; bx <= b1.x; bx++) {
				const Vector3i p0(bx * B, by * B, bz * B);
				const Vector3i p1 = p0 + Vector3i(B - 1, B - 1, B - 1);
				if (p0.x < rlo.x || p0.y < rlo.y || p0.z < rlo.z || p1.x > rhi.x || p1.y > rhi.y || p1.z > rhi.z) {
					continue; // not wholly read (only at the very edge of the read box)
				}
				bool changed = false;
				float sum = 0.0f, most_solid = -INFINITY;
				MaterialId material = MAT_STONE;
				for (int z = p0.z; z <= p1.z; z++) {
					for (int y = p0.y; y <= p1.y; y++) {
						for (int x = p0.x; x <= p1.x; x++) {
							const size_t i = index(x, y, z);
							changed |= std::abs(after[i] - before[i]) > 1e-5f;
							sum += after[i];
							if (after[i] > most_solid) {
								most_solid = after[i];
								material = materials_after[i];
							}
						}
					}
				}
				if (!changed) {
					continue;
				}
				const float value = sum / (float)(B * B * B);
				for (int z = std::max(p0.z, lo.z); z <= std::min(p1.z, hi.z); z++) {
					for (int y = std::max(p0.y, lo.y); y <= std::min(p1.y, hi.y); y++) {
						for (int x = std::max(p0.x, lo.x); x <= std::min(p1.x, hi.x); x++) {
							const size_t i = index(x, y, z);
							after[i] = value;
							materials_after[i] = material;
						}
					}
				}
			}
		}
	}
}

void SculptTerrain::commit_edit(const Vector3i &lo, const Vector3i &hi, const Vector3i &rlo, const Vector3i &n,
		const std::vector<float> &before, const std::vector<float> &after, const std::vector<uint8_t> &brushed, bool p_dither,
		const std::vector<MaterialId> &materials_before, const std::vector<MaterialId> &materials_after) {
	auto index = [&](int x, int y, int z) { return ((size_t)(z - rlo.z) * n.y + (y - rlo.y)) * n.x + (x - rlo.x); };
	// Quantize to 1/16-voxel steps and find what changed: only chunks with
	// changes are written (and so become edited chunks), and the clipmap is
	// refreshed only around them.
	//   Points the brush changed: dithered rounding (if p_dither), so small
	//   changes add up on average instead of always rounding away.
	//   Points it didn't: left alone unless the repair found the surface
	//   nearer than they say (by more than REPAIR_TOLERANCE: the edit
	//   brought it closer, or an older edit left them wrong) or on their
	//   other side, and then rounded plainly. Re-rounding them with a dither
	//   every stroke would make them drift at random, roughening the
	//   surface around the brush. A value lower than the true distance is
	//   left as it is: it's safe (rays just step more carefully), and the
	//   terrain generator gives those everywhere on purpose; rewriting them
	//   across the whole edit box would leave a crease along its edges.
	const float REPAIR_TOLERANCE = 2.0f * DENSITY_STEP;
	// Only points within reach of what the brush changed (the distances it
	// can affect: up to DENSITY_CLAMP away) may be repaired at all: farther
	// out the repair can only be "correcting" the generator's own estimates,
	// and doing that within the edit box but not beyond it leaves a crease
	// along the box's edges. (A box-shaped dilation of the brushed points.)
	std::vector<uint8_t> reachable = brushed;
	{
		const int R = (int)std::ceil(DENSITY_CLAMP) + 1;
		const int sizes[3] = { n.x, n.y, n.z };
		const size_t strides[3] = { 1, (size_t)n.x, (size_t)n.x * n.y };
		std::vector<uint8_t> line;
		for (int a = 0; a < 3; a++) {
			std::vector<uint8_t> next(reachable.size(), 0);
			const int len = sizes[a];
			line.assign(len, 0);
			const int b = (a + 1) % 3, c = (a + 2) % 3;
			for (int j = 0; j < sizes[c]; j++) {
				for (int i = 0; i < sizes[b]; i++) {
					const size_t base = (size_t)i * strides[b] + (size_t)j * strides[c];
					// Distance to the nearest marked point along this line, both ways.
					int last = -1000000;
					for (int k = 0; k < len; k++) {
						if (reachable[base + k * strides[a]]) {
							last = k;
						}
						line[k] = k - last <= R;
					}
					last = 1000000;
					for (int k = len - 1; k >= 0; k--) {
						if (reachable[base + k * strides[a]]) {
							last = k;
						}
						next[base + k * strides[a]] = line[k] || last - k <= R;
					}
				}
			}
			reachable.swap(next);
		}
	}
	const Vector3i c0 = VoxelWorld::chunk_of(lo.x, lo.y, lo.z), c1 = VoxelWorld::chunk_of(hi.x, hi.y, hi.z);
	const Vector3i cn = c1 - c0 + Vector3i(1, 1, 1);
	auto chunk_slot = [&](const Vector3i &c) { return ((size_t)(c.z - c0.z) * cn.y + (c.y - c0.y)) * cn.x + (c.x - c0.x); };
	std::vector<uint8_t> chunk_changed((size_t)cn.x * cn.y * cn.z, 0);
	std::vector<int8_t> quantized((size_t)n.x * n.y * n.z);
	Vector3i changed_lo = hi, changed_hi = lo;
	for (int z = lo.z; z <= hi.z; z++) {
		for (int y = lo.y; y <= hi.y; y++) {
			for (int x = lo.x; x <= hi.x; x++) {
				const size_t i = index(x, y, z);
				float steps;
				if (brushed[i]) {
					steps = p_dither ? std::floor(after[i] * 16.0f + dither(x, y, z, brush_strokes)) : std::nearbyint(after[i] * 16.0f);
				} else if (reachable[i] && ((after[i] > 0.0f) != (before[i] > 0.0f) || std::abs(after[i]) < std::abs(before[i]) - REPAIR_TOLERANCE)) {
					steps = std::nearbyint(after[i] * 16.0f);
				} else {
					steps = std::nearbyint(before[i] * 16.0f);
				}
				const int8_t q = (int8_t)std::clamp(steps, (float)-DENSITY_LIMIT, (float)DENSITY_LIMIT);
				quantized[i] = q;
				if (q != (int8_t)std::lround(before[i] * 16.0f) || materials_after[i] != materials_before[i]) {
					chunk_changed[chunk_slot(VoxelWorld::chunk_of(x, y, z))] = 1;
					changed_lo = Vector3i(std::min(changed_lo.x, x), std::min(changed_lo.y, y), std::min(changed_lo.z, z));
					changed_hi = Vector3i(std::max(changed_hi.x, x), std::max(changed_hi.y, y), std::max(changed_hi.z, z));
				}
			}
		}
	}
	brush_strokes++;
	if (changed_lo.x > changed_hi.x) {
		return; // nothing moved by a whole step
	}
	const int CH = VoxelWorld::CHUNK;
	std::vector<Vector3i> created;
	for (int cz = c0.z; cz <= c1.z; cz++) {
		for (int cy = c0.y; cy <= c1.y; cy++) {
			for (int cx = c0.x; cx <= c1.x; cx++) {
				const Vector3i c(cx, cy, cz);
				if (!chunk_changed[chunk_slot(c)]) {
					continue;
				}
				const bool was_edited = world.chunk_edited(c);
				const VoxelWorld::ChunkData data = world.edit_chunk(c);
				if (data.density == nullptr) {
					continue; // beyond the world's top or bottom
				}
				if (!was_edited) {
					created.push_back(c);
				}
				record_chunk(c, data);
				Vector3i a, b;
				for (int k = 0; k < 3; k++) {
					a[k] = std::max(c[k] * CH, lo[k]);
					b[k] = std::min(c[k] * CH + CH - 1, hi[k]);
				}
				for (int z = a.z; z <= b.z; z++) {
					for (int y = a.y; y <= b.y; y++) {
						for (int x = a.x; x <= b.x; x++) {
							const size_t at = ((size_t)(z - c.z * CH) * CH + (y - c.y * CH)) * CH + (x - c.x * CH);
							data.density[at] = quantized[index(x, y, z)];
							data.material[at] = materials_after[index(x, y, z)];
						}
					}
				}
				world.update_mips(c);
			}
		}
	}
	clipmap.refresh(world, changed_lo, changed_hi);
	mark_edited(changed_lo, changed_hi);
	// A newly edited chunk's coarse levels now come from its mips rather
	// than the generator: refresh it whole, once.
	const int CHUNK_LAST = VoxelWorld::CHUNK - 1;
	for (const Vector3i &c : created) {
		clipmap.refresh(world, c * VoxelWorld::CHUNK, c * VoxelWorld::CHUNK + Vector3i(CHUNK_LAST, CHUNK_LAST, CHUNK_LAST));
	}
}

// --- Grab and snake hook ----------------------------------------------------------

void SculptTerrain::record_chunk(const Vector3i &p_unwrapped_chunk, const VoxelWorld::ChunkData &p_data) {
	// By the chunk itself, not where in which repeat of the world it was
	// touched (symmetry copies can reach the same one twice).
	const Vector3i p_chunk = world.wrap_chunk(p_unwrapped_chunk);
	if (recording.index.count(p_chunk)) {
		return; // already holds how it was before this stroke
	}
	recording.index[p_chunk] = recording.chunks.size();
	const int N = VoxelWorld::CHUNK_POINTS;
	recording.chunks.push_back({ p_chunk, std::vector<int8_t>(p_data.density, p_data.density + N), std::vector<MaterialId>(p_data.material, p_data.material + N) });
}

void SculptTerrain::begin_stroke() {
	end_stroke();
}

void SculptTerrain::end_stroke() {
	if (stroke_held) {
		return;
	}
	stroke_coverage.clear();
	if (recording.chunks.empty()) {
		return;
	}
	undo_serial++;
	recording.index.clear();
	undo_bytes += recording.bytes();
	undo_stack.push_back(std::move(recording));
	recording = UndoStep();
	for (const UndoStep &step : redo_stack) {
		undo_bytes -= step.bytes();
	}
	redo_stack.clear();
	// Forget the oldest strokes past the limits (always keeping the newest).
	size_t drop = 0;
	while (undo_stack.size() - drop > 1 && (undo_stack.size() - drop > (size_t)MAX_UNDO_STEPS || undo_bytes > MAX_UNDO_BYTES)) {
		undo_bytes -= undo_stack[drop].bytes();
		drop++;
	}
	undo_stack.erase(undo_stack.begin(), undo_stack.begin() + drop);
}

void SculptTerrain::swap_step(UndoStep &r_step) {
	const int CH = VoxelWorld::CHUNK;
	for (UndoChunk &saved : r_step.chunks) {
		const VoxelWorld::ChunkData current = world.edit_chunk(saved.chunk);
		if (current.density == nullptr) {
			continue;
		}
		std::swap_ranges(saved.density.begin(), saved.density.end(), current.density);
		std::swap_ranges(saved.materials.begin(), saved.materials.end(), current.material);
		world.update_mips(saved.chunk);
		clipmap.refresh(world, saved.chunk * CH, saved.chunk * CH + Vector3i(CH - 1, CH - 1, CH - 1));
		mark_edited(saved.chunk * CH, saved.chunk * CH + Vector3i(CH - 1, CH - 1, CH - 1));
	}
}

bool SculptTerrain::undo() {
	end_stroke();
	if (undo_stack.empty()) {
		return false;
	}
	UndoStep step = std::move(undo_stack.back());
	undo_stack.pop_back();
	swap_step(step);
	redo_stack.push_back(std::move(step));
	return true;
}

bool SculptTerrain::redo() {
	end_stroke();
	if (redo_stack.empty()) {
		return false;
	}
	UndoStep step = std::move(redo_stack.back());
	redo_stack.pop_back();
	swap_step(step);
	undo_stack.push_back(std::move(step));
	return true;
}

void SculptTerrain::begin_grab() {
	grab_snapshot.clear();
	grab_has_box = false;
}

void SculptTerrain::end_grab() {
	grab_snapshot.clear();
	grab_has_box = false;
}

void SculptTerrain::capture_snapshot(const Vector3i &p_min, const Vector3i &p_max) {
	const int C = VoxelWorld::CHUNK;
	const Vector3i c0 = VoxelWorld::chunk_of(p_min.x, p_min.y, p_min.z), c1 = VoxelWorld::chunk_of(p_max.x, p_max.y, p_max.z);
	for (int z = c0.z; z <= c1.z; z++) {
		for (int y = c0.y; y <= c1.y; y++) {
			for (int x = c0.x; x <= c1.x; x++) {
				const Vector3i c(x, y, z);
				if (grab_snapshot.count(c)) {
					continue;
				}
				SnapshotChunk &saved = grab_snapshot[c];
				saved.density.resize((size_t)C * C * C);
				saved.materials.resize((size_t)C * C * C);
				world.read_points(c * C, c * C + Vector3i(C - 1, C - 1, C - 1), saved.density.data(), worker_count(), saved.materials.data());
			}
		}
	}
}

float SculptTerrain::snapshot_point(int x, int y, int z) const {
	const int C = VoxelWorld::CHUNK;
	const Vector3i c = VoxelWorld::chunk_of(x, y, z);
	auto it = grab_snapshot.find(c);
	if (it == grab_snapshot.end()) {
		return -DENSITY_CLAMP; // not captured: never reached by this grab
	}
	return it->second.density[((size_t)(z - c.z * C) * C + (y - c.y * C)) * C + (x - c.x * C)];
}

MaterialId SculptTerrain::snapshot_material(int x, int y, int z) const {
	const int C = VoxelWorld::CHUNK;
	const Vector3i c = VoxelWorld::chunk_of(x, y, z);
	auto it = grab_snapshot.find(c);
	if (it == grab_snapshot.end()) {
		return MAT_STONE;
	}
	return it->second.materials[((size_t)(z - c.z * C) * C + (y - c.y * C)) * C + (x - c.x * C)];
}

void SculptTerrain::warp(const PackedVector3Array &p_centers, const PackedVector3Array &p_deltas, double p_radius, bool p_grab) {
	ERR_FAIL_COND_MSG(!opened, "SculptTerrain: call open_world() first.");
	const int count = (int)std::min(p_centers.size(), p_deltas.size());
	if (count == 0 || p_radius <= 0.0) {
		return;
	}
	const float r = (float)p_radius, C = DENSITY_CLAMP;
	const int margin = (int)std::ceil(C) + 1;
	std::vector<Vector3> centers(count), deltas(count);
	for (int i = 0; i < count; i++) {
		centers[i] = p_centers[i];
		deltas[i] = p_deltas[i];
		// A snake hook reads the current values around it, so one step can
		// only reach as far as the margin read around the box.
		if (!p_grab && deltas[i].length() > margin - 1.0f) {
			deltas[i] = deltas[i].normalized() * (margin - 1.0f);
		}
	}
	// The box rewritten: around each centre (for a grab, both where the
	// grabbed material started and where it is now). A grab also rewrites
	// last frame's box, so what it no longer covers goes back to how it was.
	Vector3i lo(INT32_MAX, INT32_MAX, INT32_MAX), hi(INT32_MIN, INT32_MIN, INT32_MIN);
	for (int i = 0; i < count; i++) {
		for (int a = 0; a < 3; a++) {
			const float a0 = centers[i][a], a1 = p_grab ? centers[i][a] + deltas[i][a] : a0;
			lo[a] = std::min(lo[a], (int)std::floor(std::min(a0, a1) - r - 1.0f));
			hi[a] = std::max(hi[a], (int)std::ceil(std::max(a0, a1) + r + 1.0f));
		}
	}
	if (p_grab && grab_has_box) {
		for (int a = 0; a < 3; a++) {
			lo[a] = std::min(lo[a], grab_lo[a]);
			hi[a] = std::max(hi[a], grab_hi[a]);
		}
	}
	const Vector3i rlo = lo - Vector3i(margin, margin, margin), rhi = hi + Vector3i(margin, margin, margin);
	const Vector3i n = rhi - rlo + Vector3i(1, 1, 1);
	auto index = [&](int x, int y, int z) { return ((size_t)(z - rlo.z) * n.y + (y - rlo.y)) * n.x + (x - rlo.x); };
	std::vector<float> before((size_t)n.x * n.y * n.z);
	std::vector<MaterialId> materials_before(before.size());
	world.read_points(rlo, rhi, before.data(), worker_count(), materials_before.data());

	if (p_grab) {
		// Everything a grab reads comes from how the terrain was when it
		// started: capture it before this frame's writes. Anything earlier
		// frames wrote lies in boxes already captured.
		// Overlapping copies add up, so allow for the sum of their moves.
		Vector3 reach;
		for (int i = 0; i < count; i++) {
			reach += deltas[i].abs();
		}
		Vector3i slo, shi;
		for (int a = 0; a < 3; a++) {
			slo[a] = (int)std::floor(lo[a] - reach[a]) - 1;
			shi[a] = (int)std::ceil(hi[a] + reach[a]) + 1;
		}
		capture_snapshot(slo, shi);
	}
	auto source = [&](const Vector3 &q) -> float {
		// Trilinear, from the snapshot (grab) or the values just read (snake).
		const float fx = std::floor(q.x), fy = std::floor(q.y), fz = std::floor(q.z);
		const int x = (int)fx, y = (int)fy, z = (int)fz;
		const float tx = q.x - fx, ty = q.y - fy, tz = q.z - fz;
		float c[8];
		for (int k = 0; k < 8; k++) {
			const int px = x + (k & 1), py = y + ((k >> 1) & 1), pz = z + ((k >> 2) & 1);
			if (p_grab) {
				c[k] = snapshot_point(px, py, pz);
			} else {
				const int cx = std::clamp(px, rlo.x, rhi.x), cy = std::clamp(py, rlo.y, rhi.y), cz = std::clamp(pz, rlo.z, rhi.z);
				c[k] = before[index(cx, cy, cz)];
			}
		}
		auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
		return lerp(lerp(lerp(c[0], c[1], tx), lerp(c[2], c[3], tx), ty), lerp(lerp(c[4], c[5], tx), lerp(c[6], c[7], tx), ty), tz);
	};

	// Material moves with the terrain: the nearest point's to where the value came from.
	auto source_material = [&](const Vector3 &q) -> MaterialId {
		const int x = (int)std::lround(q.x), y = (int)std::lround(q.y), z = (int)std::lround(q.z);
		if (p_grab) {
			return snapshot_material(x, y, z);
		}
		return materials_before[index(std::clamp(x, rlo.x, rhi.x), std::clamp(y, rlo.y, rhi.y), std::clamp(z, rlo.z, rhi.z))];
	};

	std::vector<float> after = before;
	std::vector<MaterialId> materials_after = materials_before;
	if (p_grab) {
		// Grab, as Blender's: material at x moves to x + delta * falloff(|x -
		// centre| / r), centre being where the grab began. Each point p looks
		// for the x that lands on it: x = p - delta * s with
		//   s = falloff(|p - delta * s - centre| / r),
		// which has a root in [0, 1] (the left side minus the right goes from
		// <= 0 to >= 0). Where the move folds material over itself there are
		// several; the most solid wins, so stretched necks stay filled.
		// Each symmetry copy is solved on its own; a point moved by none of
		// them keeps its original value.
		const int SCAN = 12;
		for (int z = lo.z; z <= hi.z; z++) {
			for (int y = lo.y; y <= hi.y; y++) {
				for (int x = lo.x; x <= hi.x; x++) {
					const Vector3 p((float)x, (float)y, (float)z);
					bool moved = false;
					float value = -INFINITY;
					Vector3 from = p; // where the winning value came from
					for (int c = 0; c < count; c++) {
						const Vector3 &centre = centers[c], &delta = deltas[c];
						// Only material within r of the moved centre's path
						// can reach p; otherwise s = 0 is the only root.
						const float dd = delta.length_squared();
						const float t = dd > 0.0f ? std::clamp((p - centre).dot(delta) / dd, 0.0f, 1.0f) : 0.0f;
						if ((p - centre - delta * t).length_squared() >= r * r) {
							const float v = source(p);
							if (v > value) {
								value = v;
								from = p;
							}
							moved = true;
							continue;
						}
						auto h = [&](float s) { return falloff((p - delta * s - centre).length() / r) - s; };
						float s0 = 0.0f, h0 = h(0.0f);
						if (h0 <= 0.0f) {
							// s = 0 is a root: the point's own material (when
							// beyond this copy's reach) stays, unless more
							// solid material lands on it.
							const float v = source(p);
							if (v > value) {
								value = v;
								from = p;
							}
							moved = true;
						}
						for (int k = 1; k <= SCAN; k++) {
							const float s1 = (float)k / SCAN, h1 = h(s1);
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
									from = q;
								}
								moved = true;
							}
							s0 = s1;
							h0 = h1;
						}
					}
					after[index(x, y, z)] = moved ? value : snapshot_point(x, y, z);
					materials_after[index(x, y, z)] = source_material(from);
				}
			}
		}
	} else {
		// Snake hook: each point takes the value from a step behind it,
		// weighted by its closeness to the brush's new position, so material
		// near the brush follows it. Stretching thins a volume, so a capsule
		// of 40% of the radius along the step keeps the tendril's thickness.
		const float thickness = 0.4f * r;
		for (int z = lo.z; z <= hi.z; z++) {
			for (int y = lo.y; y <= hi.y; y++) {
				for (int x = lo.x; x <= hi.x; x++) {
					const Vector3 p((float)x, (float)y, (float)z);
					Vector3 displacement;
					float capsule = -C;
					for (int i = 0; i < count; i++) {
						displacement += deltas[i] * falloff((p - centers[i]).length() / r);
						const Vector3 a = centers[i] - deltas[i], ab = deltas[i];
						const float t = ab.length_squared() > 0.0f ? std::clamp((p - a).dot(ab) / ab.length_squared(), 0.0f, 1.0f) : 0.0f;
						capsule = std::max(capsule, thickness - (p - (a + ab * t)).length());
					}
					if (displacement.length_squared() == 0.0f && capsule <= -C) {
						continue;
					}
					after[index(x, y, z)] = std::max(source(p - displacement), std::min(capsule, C));
					materials_after[index(x, y, z)] = source_material(p - displacement);
				}
			}
		}
	}
	std::vector<Vector3i> grown;
	mask_to_selection(lo, hi, rlo, n, before, after, materials_before, materials_after, true, grown);
	if (blocky_size >= 2) {
		snap_to_blocks(lo, hi, rlo, n, before, after, materials_after);
	}
	std::vector<uint8_t> brushed(after.size(), 0);
	for (size_t i = 0; i < after.size(); i++) {
		brushed[i] = std::abs(after[i] - before[i]) > 1e-5f;
	}
	// A warp bends the distances; put them right (see apply_brush).
	redistance(after, n);
	// No dither: a grab recomputes the same values every frame from the
	// snapshot, and random rounding would make them shimmer.
	commit_edit(lo, hi, rlo, n, before, after, brushed, false, materials_before, materials_after);
	select_grown(grown);
	if (p_grab) {
		grab_lo = lo;
		grab_hi = hi;
		grab_has_box = true;
	}
}

Variant SculptTerrain::raycast(const Vector3 &p_from, const Vector3 &p_dir, double p_max_distance) {
	if (!opened || p_dir.length_squared() < 1e-12f) {
		return Variant();
	}
	const Vector3 dir = p_dir.normalized();
	// March: the density changes by about 2 per voxel at most, so half of it
	// is a safe step in air.
	double prev = 0.0;
	for (double t = 0.0; t <= p_max_distance;) {
		const float v = world.sample(p_from + dir * (float)t);
		if (v > 0.0f) {
			double a = prev, b = t; // air at a (or a == b), solid at b
			for (int i = 0; i < 12 && b - a > 1e-4; i++) {
				const double m = (a + b) * 0.5;
				(world.sample(p_from + dir * (float)m) > 0.0f ? b : a) = m;
			}
			return p_from + dir * (float)b;
		}
		prev = t;
		t += std::max(-v * 0.5, 0.25);
	}
	return Variant();
}

// --- Bindings ----------------------------------------------------------------

void SculptTerrain::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_world_size", "voxels"), &SculptTerrain::set_world_size);
	ClassDB::bind_method(D_METHOD("get_world_size"), &SculptTerrain::get_world_size);
	ClassDB::bind_method(D_METHOD("set_world_height", "voxels"), &SculptTerrain::set_world_height);
	ClassDB::bind_method(D_METHOD("get_world_height"), &SculptTerrain::get_world_height);
	ClassDB::bind_method(D_METHOD("set_world_type", "type"), &SculptTerrain::set_world_type);
	ClassDB::bind_method(D_METHOD("get_world_type"), &SculptTerrain::get_world_type);
	ClassDB::bind_method(D_METHOD("set_terrain_height", "voxels"), &SculptTerrain::set_terrain_height);
	ClassDB::bind_method(D_METHOD("get_terrain_height"), &SculptTerrain::get_terrain_height);
	ClassDB::bind_method(D_METHOD("set_terrain_scale", "voxels"), &SculptTerrain::set_terrain_scale);
	ClassDB::bind_method(D_METHOD("get_terrain_scale"), &SculptTerrain::get_terrain_scale);
	ClassDB::bind_method(D_METHOD("set_caves", "enabled"), &SculptTerrain::set_caves);
	ClassDB::bind_method(D_METHOD("get_caves"), &SculptTerrain::get_caves);
	ClassDB::bind_method(D_METHOD("set_seed", "seed"), &SculptTerrain::set_seed);
	ClassDB::bind_method(D_METHOD("get_seed"), &SculptTerrain::get_seed);
	ClassDB::bind_method(D_METHOD("set_cell_size", "voxels"), &SculptTerrain::set_cell_size);
	ClassDB::bind_method(D_METHOD("get_cell_size"), &SculptTerrain::get_cell_size);
	ClassDB::bind_method(D_METHOD("set_island_chance", "value"), &SculptTerrain::set_island_chance);
	ClassDB::bind_method(D_METHOD("get_island_chance"), &SculptTerrain::get_island_chance);
	ClassDB::bind_method(D_METHOD("set_min_radius", "voxels"), &SculptTerrain::set_min_radius);
	ClassDB::bind_method(D_METHOD("get_min_radius"), &SculptTerrain::get_min_radius);
	ClassDB::bind_method(D_METHOD("set_max_radius", "voxels"), &SculptTerrain::set_max_radius);
	ClassDB::bind_method(D_METHOD("get_max_radius"), &SculptTerrain::get_max_radius);
	ClassDB::bind_method(D_METHOD("set_detail", "voxels"), &SculptTerrain::set_detail);
	ClassDB::bind_method(D_METHOD("get_detail"), &SculptTerrain::get_detail);
	ClassDB::bind_method(D_METHOD("set_detail_frequency", "value"), &SculptTerrain::set_detail_frequency);
	ClassDB::bind_method(D_METHOD("get_detail_frequency"), &SculptTerrain::get_detail_frequency);
	ClassDB::bind_method(D_METHOD("set_view_distance", "voxels"), &SculptTerrain::set_view_distance);
	ClassDB::bind_method(D_METHOD("get_view_distance"), &SculptTerrain::get_view_distance);
	ClassDB::bind_method(D_METHOD("find_island", "near"), &SculptTerrain::find_island);
	ClassDB::bind_method(D_METHOD("set_octaves", "octaves"), &SculptTerrain::set_octaves);
	ClassDB::bind_method(D_METHOD("get_octaves"), &SculptTerrain::get_octaves);
	ClassDB::bind_method(D_METHOD("set_save_path", "path"), &SculptTerrain::set_save_path);
	ClassDB::bind_method(D_METHOD("get_save_path"), &SculptTerrain::get_save_path);
	ClassDB::bind_method(D_METHOD("set_autosave_interval", "seconds"), &SculptTerrain::set_autosave_interval);
	ClassDB::bind_method(D_METHOD("get_autosave_interval"), &SculptTerrain::get_autosave_interval);
	ClassDB::bind_method(D_METHOD("set_material", "material"), &SculptTerrain::set_material);
	ClassDB::bind_method(D_METHOD("get_material"), &SculptTerrain::get_material);
	ClassDB::bind_method(D_METHOD("set_prepass_material", "material"), &SculptTerrain::set_prepass_material);
	ClassDB::bind_method(D_METHOD("get_prepass_material"), &SculptTerrain::get_prepass_material);
	ClassDB::bind_method(D_METHOD("set_clip_size", "size"), &SculptTerrain::set_clip_size);
	ClassDB::bind_method(D_METHOD("get_clip_size"), &SculptTerrain::get_clip_size);
	ClassDB::bind_method(D_METHOD("set_clip_levels", "levels"), &SculptTerrain::set_clip_levels);
	ClassDB::bind_method(D_METHOD("get_clip_levels"), &SculptTerrain::get_clip_levels);
	ClassDB::bind_method(D_METHOD("set_simulation_radius", "voxels"), &SculptTerrain::set_simulation_radius);
	ClassDB::bind_method(D_METHOD("get_simulation_radius"), &SculptTerrain::get_simulation_radius);
	ClassDB::bind_method(D_METHOD("set_blocky_size", "voxels"), &SculptTerrain::set_blocky_size);
	ClassDB::bind_method(D_METHOD("get_blocky_size"), &SculptTerrain::get_blocky_size);
	ClassDB::bind_method(D_METHOD("set_thread_count", "value"), &SculptTerrain::set_thread_count);
	ClassDB::bind_method(D_METHOD("get_thread_count"), &SculptTerrain::get_thread_count);

	ClassDB::bind_method(D_METHOD("open_world"), &SculptTerrain::open_world);
	ClassDB::bind_method(D_METHOD("save_world"), &SculptTerrain::save_world);
	ClassDB::bind_method(D_METHOD("update_view", "camera_position", "wait"), &SculptTerrain::update_view, DEFVAL(false));
	ClassDB::bind_method(D_METHOD("apply_brush", "center", "radius", "strength", "mode", "material"), &SculptTerrain::apply_brush, DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("get_block_material", "position"), &SculptTerrain::get_block_material);
	ClassDB::bind_method(D_METHOD("paint_color", "center", "radius", "color", "opacity", "hardness"), &SculptTerrain::paint_color);
	ClassDB::bind_method(D_METHOD("set_material_colors", "colors"), &SculptTerrain::set_material_colors);
	ClassDB::bind_method(D_METHOD("get_material_color", "material"), &SculptTerrain::get_material_color);
	ClassDB::bind_static_method("SculptTerrain", D_METHOD("color_to_material", "color"), &SculptTerrain::color_to_material);
	ClassDB::bind_static_method("SculptTerrain", D_METHOD("get_material_names"), &SculptTerrain::get_material_names);
	ClassDB::bind_method(D_METHOD("raycast", "from", "direction", "max_distance"), &SculptTerrain::raycast);
	ClassDB::bind_method(D_METHOD("begin_grab"), &SculptTerrain::begin_grab);
	ClassDB::bind_method(D_METHOD("end_grab"), &SculptTerrain::end_grab);
	ClassDB::bind_method(D_METHOD("warp", "centers", "deltas", "radius", "grab"), &SculptTerrain::warp);
	ClassDB::bind_method(D_METHOD("get_density", "position"), &SculptTerrain::get_density);
	ClassDB::bind_method(D_METHOD("get_normal", "position", "spread"), &SculptTerrain::get_normal, DEFVAL(0.5));
	ClassDB::bind_method(D_METHOD("set_flatten_plane", "point", "normal"), &SculptTerrain::set_flatten_plane);
	ClassDB::bind_method(D_METHOD("clear_flatten_plane"), &SculptTerrain::clear_flatten_plane);
	ClassDB::bind_method(D_METHOD("take_upload_bytes"), &SculptTerrain::take_upload_bytes);
	ClassDB::bind_method(D_METHOD("get_clip_origin", "level"), &SculptTerrain::get_clip_origin);
	ClassDB::bind_method(D_METHOD("get_clip_texel", "level", "texel"), &SculptTerrain::get_clip_texel);
	ClassDB::bind_method(D_METHOD("get_clip_brick", "level", "texel"), &SculptTerrain::get_clip_brick);
	ClassDB::bind_method(D_METHOD("get_lod_density", "point", "level"), &SculptTerrain::get_lod_density);
	ClassDB::bind_method(D_METHOD("get_stats"), &SculptTerrain::get_stats);
	ClassDB::bind_method(D_METHOD("shift_origin", "offset"), &SculptTerrain::shift_origin);
	ClassDB::bind_method(D_METHOD("set_gravity", "voxels_per_second_squared"), &SculptTerrain::set_gravity);
	ClassDB::bind_method(D_METHOD("get_gravity"), &SculptTerrain::get_gravity);
	ClassDB::bind_method(D_METHOD("set_physics_enabled", "enabled"), &SculptTerrain::set_physics_enabled);
	ClassDB::bind_method(D_METHOD("is_physics_enabled"), &SculptTerrain::is_physics_enabled);
	ClassDB::bind_method(D_METHOD("is_world_open"), &SculptTerrain::is_world_open);
	ClassDB::bind_method(D_METHOD("select_sphere", "center", "radius", "add"), &SculptTerrain::select_sphere);
	ClassDB::bind_method(D_METHOD("select_connected", "from", "radius", "add"), &SculptTerrain::select_connected);
	ClassDB::bind_method(D_METHOD("select_similar", "from", "radius", "threshold", "add"), &SculptTerrain::select_similar);
	ClassDB::bind_method(D_METHOD("clear_selection"), &SculptTerrain::clear_selection);
	ClassDB::bind_method(D_METHOD("get_selected_count"), &SculptTerrain::get_selected_count);
	ClassDB::bind_method(D_METHOD("has_selection"), &SculptTerrain::has_selection);
	ClassDB::bind_method(D_METHOD("get_selection_bounds"), &SculptTerrain::get_selection_bounds);
	ClassDB::bind_method(D_METHOD("selection_to_shape", "carve"), &SculptTerrain::selection_to_shape);
	ClassDB::bind_method(D_METHOD("get_selection_center"), &SculptTerrain::get_selection_center);
	ClassDB::bind_method(D_METHOD("get_selection_state"), &SculptTerrain::get_selection_state);
	ClassDB::bind_method(D_METHOD("set_selection_state", "state"), &SculptTerrain::set_selection_state);
	ClassDB::bind_method(D_METHOD("get_selection_serial"), &SculptTerrain::get_selection_serial);
	ClassDB::bind_method(D_METHOD("stamp_shape", "shape", "transform", "material", "select", "pose"), &SculptTerrain::stamp_shape, DEFVAL(-1), DEFVAL(true), DEFVAL(0.0));
	ClassDB::bind_method(D_METHOD("select_inside_shape", "shape", "transform", "add"), &SculptTerrain::select_inside_shape);
	ClassDB::bind_method(D_METHOD("carve_shape", "shape", "transform", "pose"), &SculptTerrain::carve_shape, DEFVAL(0.0));
	ClassDB::bind_method(D_METHOD("begin_extrude"), &SculptTerrain::begin_extrude);
	ClassDB::bind_method(D_METHOD("extrude", "direction", "distance", "center", "radius"), &SculptTerrain::extrude, DEFVAL(Vector3()), DEFVAL(0.0));
	ClassDB::bind_method(D_METHOD("end_extrude"), &SculptTerrain::end_extrude);
	ClassDB::bind_method(D_METHOD("set_stroke_held", "held"), &SculptTerrain::set_stroke_held);
	ClassDB::bind_method(D_METHOD("is_stroke_held"), &SculptTerrain::is_stroke_held);
	ClassDB::bind_method(D_METHOD("get_undo_serial"), &SculptTerrain::get_undo_serial);
	ClassDB::bind_method(D_METHOD("begin_stroke"), &SculptTerrain::begin_stroke);
	ClassDB::bind_method(D_METHOD("end_stroke"), &SculptTerrain::end_stroke);
	ClassDB::bind_method(D_METHOD("undo"), &SculptTerrain::undo);
	ClassDB::bind_method(D_METHOD("redo"), &SculptTerrain::redo);
	ClassDB::bind_method(D_METHOD("get_undo_count"), &SculptTerrain::get_undo_count);
	ClassDB::bind_method(D_METHOD("get_redo_count"), &SculptTerrain::get_redo_count);

	ADD_GROUP("World", "");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "world_type", PROPERTY_HINT_ENUM, "Islands,Terrain"), "set_world_type", "get_world_type");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "world_size", PROPERTY_HINT_RANGE, "256,16384,256,suffix:voxels"), "set_world_size", "get_world_size");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "world_height", PROPERTY_HINT_RANGE, "64,4096,32,suffix:voxels"), "set_world_height", "get_world_height");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "seed"), "set_seed", "get_seed");
	ADD_GROUP("Terrain", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "terrain_height", PROPERTY_HINT_RANGE, "1,400,1,suffix:voxels"), "set_terrain_height", "get_terrain_height");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "terrain_scale", PROPERTY_HINT_RANGE, "16,4096,1,suffix:voxels"), "set_terrain_scale", "get_terrain_scale");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "caves"), "set_caves", "get_caves");
	ADD_GROUP("Islands", "");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "cell_size", PROPERTY_HINT_RANGE, "32,1024,1,suffix:voxels"), "set_cell_size", "get_cell_size");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "island_chance", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_island_chance", "get_island_chance");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "min_radius", PROPERTY_HINT_RANGE, "1,400,1,suffix:voxels"), "set_min_radius", "get_min_radius");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "max_radius", PROPERTY_HINT_RANGE, "1,400,1,suffix:voxels"), "set_max_radius", "get_max_radius");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "detail", PROPERTY_HINT_RANGE, "0,16,0.5,suffix:voxels"), "set_detail", "get_detail");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "detail_frequency", PROPERTY_HINT_RANGE, "0.005,0.2,0.005"), "set_detail_frequency", "get_detail_frequency");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "octaves", PROPERTY_HINT_RANGE, "0,8,1"), "set_octaves", "get_octaves");
	ADD_GROUP("Saving", "");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "save_path", PROPERTY_HINT_DIR), "set_save_path", "get_save_path");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "autosave_interval", PROPERTY_HINT_RANGE, "0,600,1,suffix:s"), "set_autosave_interval", "get_autosave_interval");
	ADD_GROUP("Rendering", "");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "material", PROPERTY_HINT_RESOURCE_TYPE, "ShaderMaterial"), "set_material", "get_material");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "prepass_material", PROPERTY_HINT_RESOURCE_TYPE, "ShaderMaterial"), "set_prepass_material", "get_prepass_material");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "clip_size", PROPERTY_HINT_RANGE, "16,256,8"), "set_clip_size", "get_clip_size");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "clip_levels", PROPERTY_HINT_RANGE, "1,8,1"), "set_clip_levels", "get_clip_levels");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "view_distance", PROPERTY_HINT_RANGE, "16,4000,1,suffix:voxels"), "set_view_distance", "get_view_distance");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "thread_count"), "set_thread_count", "get_thread_count");
	ADD_GROUP("Sculpting", "");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "blocky_size", PROPERTY_HINT_RANGE, "0,8,1,suffix:voxels"), "set_blocky_size", "get_blocky_size");
	ADD_GROUP("Simulation", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "simulation_radius", PROPERTY_HINT_RANGE, "32,1024,1,suffix:voxels"), "set_simulation_radius", "get_simulation_radius");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "gravity", PROPERTY_HINT_RANGE, "0,2000,1,or_greater,suffix:voxels/s^2"), "set_gravity", "get_gravity");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "physics_enabled"), "set_physics_enabled", "is_physics_enabled");

	BIND_ENUM_CONSTANT(BRUSH_ADD);
	BIND_ENUM_CONSTANT(BRUSH_SUBTRACT);
	BIND_ENUM_CONSTANT(BRUSH_SMOOTH);
	BIND_ENUM_CONSTANT(BRUSH_FLATTEN);
	BIND_ENUM_CONSTANT(BRUSH_PAINT);
	BIND_ENUM_CONSTANT(BRUSH_INFLATE);
	BIND_ENUM_CONSTANT(BRUSH_PINCH);
	BIND_ENUM_CONSTANT(WORLD_ISLANDS);
	BIND_ENUM_CONSTANT(WORLD_TERRAIN);
}
