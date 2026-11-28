#include "voxel_world.h"

#include "parallel.h"

#include <godot_cpp/classes/config_file.hpp>
#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>

#include <algorithm>
#include <cstring>

using namespace godot;

const char *const godot::MATERIAL_NAMES[MATERIAL_COUNT] = {
	"grass", "dirt", "stone", "sand", "wood", "cobblestone", "cloth", "sponge",
};

namespace {

constexpr uint32_t REGION_MAGIC_1 = 0x31524357; // "WCR1": densities only
constexpr uint32_t REGION_MAGIC_2 = 0x32524357; // "WCR2": densities, then 4-bit materials packed two per byte
constexpr uint32_t REGION_MAGIC_3 = 0x33524357; // "WCR3": densities, then 16-bit materials (little-endian), with water
constexpr uint32_t REGION_MAGIC = 0x34524357; // "WCR4": densities, then 16-bit materials (little-endian)
constexpr int SETTINGS_FORMAT = 4; // wrapping worlds; 2 and 3 were endless
constexpr FileAccess::CompressionMode COMPRESSION = FileAccess::COMPRESSION_ZSTD;
// Terrain version 2: how deep the sea basins sink, in terrain heights.
constexpr float SEA_BASIN = 0.5f;

void put_u32(PackedByteArray &r_out, uint32_t p_value) {
	for (int i = 0; i < 4; i++) {
		r_out.push_back((uint8_t)(p_value >> (8 * i)));
	}
}

// Version 2 materials: 4 bits each, two per byte, low nibble first.
void unpack_nibbles(const uint8_t *p_in, size_t p_count, MaterialId *r_out) {
	for (size_t i = 0; i < p_count; i++) {
		r_out[i] = (p_in[i / 2] >> ((i & 1) * 4)) & 0x0F;
	}
}

void unpack_materials(const uint8_t *p_in, size_t p_count, MaterialId *r_out) {
	for (size_t i = 0; i < p_count; i++) {
		r_out[i] = (MaterialId)(p_in[2 * i] | p_in[2 * i + 1] << 8);
	}
}

// A chunk's (or mip set's) densities followed by its materials, 2 bytes each.
PackedByteArray chunk_bytes(const int8_t *p_density, const MaterialId *p_materials, size_t p_count) {
	PackedByteArray bytes;
	bytes.resize((int64_t)(p_count * 3));
	uint8_t *out = bytes.ptrw();
	std::memcpy(out, p_density, p_count);
	for (size_t i = 0; i < p_count; i++) {
		out[p_count + 2 * i] = (uint8_t)(p_materials[i] & 0xFF);
		out[p_count + 2 * i + 1] = (uint8_t)(p_materials[i] >> 8);
	}
	return bytes;
}

// Bytes of p_count points as region version p_version stores them.
size_t chunk_bytes_size(size_t p_count, int p_version) {
	return p_version >= 3 ? p_count * 3 : (p_version == 2 ? p_count + (p_count + 1) / 2 : p_count);
}

// Versions 2 and 3 had a water material (id 4, the blocks after it one
// higher) and, in 3, water in open space (ids 0xFFF0 and up): stone, the
// same blocks renumbered, and dry ground (sand, as it was the sea floor).
MaterialId upgrade_material(MaterialId p_old) {
	if (p_old >= 0xFFF0) {
		return MAT_SAND;
	}
	if (p_old == 4) {
		return MAT_STONE;
	}
	return p_old > 4 && p_old < 9 ? p_old - 1 : p_old;
}

// Materials of p_count points stored after their densities in p_data.
void read_stored_materials(const uint8_t *p_data, size_t p_count, int p_version, MaterialId *r_out) {
	if (p_version >= 3) {
		unpack_materials(p_data + p_count, p_count, r_out);
	} else {
		unpack_nibbles(p_data + p_count, p_count, r_out);
	}
	if (p_version < 4) {
		for (size_t i = 0; i < p_count; i++) {
			r_out[i] = upgrade_material(r_out[i]);
		}
	}
}

uint32_t get_u32(const uint8_t *p_data) {
	return (uint32_t)p_data[0] | (uint32_t)p_data[1] << 8 | (uint32_t)p_data[2] << 16 | (uint32_t)p_data[3] << 24;
}

// Chebyshev distance from a point to a box.
float box_distance(const Vector3 &p_point, const Vector3 &p_lo, const Vector3 &p_hi) {
	float d = 0.0f;
	for (int a = 0; a < 3; a++) {
		d = std::max({ d, p_lo[a] - p_point[a], p_point[a] - p_hi[a] });
	}
	return d;
}

} // namespace

Vector3i VoxelWorld::chunk_of(int x, int y, int z) {
	return Vector3i(floor_div(x, CHUNK), floor_div(y, CHUNK), floor_div(z, CHUNK));
}

Vector3i VoxelWorld::region_of(const Vector3i &p_chunk) {
	return Vector3i(floor_div(p_chunk.x, REGION), floor_div(p_chunk.y, REGION), floor_div(p_chunk.z, REGION));
}

int VoxelWorld::mip_offset(int p_mip) {
	int offset = 0;
	for (int m = 1; m < p_mip; m++) {
		const int n = CHUNK >> m;
		offset += n * n * n;
	}
	return offset;
}

// --- Generation --------------------------------------------------------------

namespace {

uint64_t mix64(uint64_t &r_state) {
	uint64_t z = (r_state += 0x9E3779B97F4A7C15ull);
	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
	return z ^ (z >> 31);
}

} // namespace

void VoxelWorld::configure(const IslandSettings &p_settings) {
	settings = p_settings;
	IslandSettings &s = settings;
	// Powers of two (wrapping is a mask, and island cells and noise
	// periods have to divide the world), whole chunks tall.
	auto power_of_two = [](int p_value, int p_lo, int p_hi) {
		int v = p_lo;
		while (v * 2 <= std::min(p_value, p_hi)) {
			v *= 2;
		}
		return v;
	};
	s.world_size = power_of_two(s.world_size, 256, 16384);
	s.world_height = std::clamp(s.world_height / CHUNK * CHUNK, 2 * CHUNK, 4096);
	s.octaves = std::clamp(s.octaves, 0, 8);
	s.cell_size = power_of_two(s.cell_size, 32, std::min(s.world_size, s.world_height));
	s.detail = std::max(s.detail, 0.0f);
	s.island_chance = std::clamp(s.island_chance, 0.0f, 1.0f);
	margin = s.detail + 4.0f;
	// An island (with its rocky detail) must fit inside its cell, margin included.
	const float room = s.cell_size * 0.5f - margin - s.detail - 1.0f;
	s.max_radius = std::clamp(s.max_radius, 1.0f, std::max(room, 1.0f));
	s.min_radius = std::clamp(s.min_radius, 1.0f, s.max_radius);
	s.world_type = std::clamp(s.world_type, (int)WORLD_ISLANDS, (int)WORLD_TERRAIN);
	// Terrain must fit between the limits: its top is ~2.9 heights above 6,
	// its deepest tunnels ~1.4 heights + 94 below.
	const float half = s.world_height * 0.5f;
	s.terrain_version = std::clamp(s.terrain_version, 1, 2);
	const float depth_factor = s.terrain_version >= 2 ? 1.4f + SEA_BASIN : 1.4f; // see terrain_bottom()
	s.terrain_height = std::clamp(s.terrain_height, 1.0f, std::max(std::min((half - 16.0f) / 2.9f, (half - 100.0f) / depth_factor), 1.0f));
	s.terrain_scale = std::clamp(s.terrain_scale, 16.0f, 8192.0f);
	noise.set_seed((uint64_t)s.seed);
	perlin.set_seed((uint64_t)s.seed);
	perlin_caves.set_seed((uint64_t)s.seed * 0x2545F4914F6CDD1Dull + 7);
	settings_version++;
	std::unique_lock<std::shared_mutex> lock(feature_lock);
	feature_regions.clear(); // (planned from the old settings)
}

bool VoxelWorld::island_in_cell(const Vector3i &p_cell, Island &r_island) const {
	// Cells wrap with the world (the island is placed relative to the
	// unwrapped cell asked for); none reach past the y limits.
	const int C = settings.cell_size;
	if (p_cell.y * C < y_min() || (p_cell.y + 1) * C > y_max()) {
		return false;
	}
	const int cells = settings.world_size / C;
	const int wx = ((p_cell.x % cells) + cells) % cells, wz = ((p_cell.z % cells) + cells) % cells;
	uint64_t state = (uint64_t)settings.seed * 0xD6E8FEB86659FD93ull ^ (uint64_t)(uint32_t)wx * 0x9E3779B97F4A7C15ull ^
			(uint64_t)(uint32_t)p_cell.y * 0xC2B2AE3D27D4EB4Full ^ (uint64_t)(uint32_t)wz * 0x165667B19E3779F9ull;
	auto random = [&state]() { return (float)(mix64(state) >> 40) / (float)(1 << 24); };
	if (random() >= settings.island_chance) {
		return false;
	}
	const float Cf = (float)C;
	const float R = settings.min_radius + (settings.max_radius - settings.min_radius) * random();
	const float edge = margin + settings.detail; // room kept clear around the island
	const float depth = std::min(R * (1.0f + 0.8f * random()), Cf - 2.0f * edge - 2.0f);
	auto place = [&](float p_lo, float p_hi) { // a random spot in [lo, hi] of the cell
		return p_lo <= p_hi ? p_lo + (p_hi - p_lo) * random() : (p_lo + p_hi) * 0.5f;
	};
	const Vector3 corner = Vector3(p_cell) * Cf;
	r_island.radius = R;
	r_island.depth = depth;
	r_island.centre = corner + Vector3(place(edge + R, Cf - edge - R), place(edge + depth, Cf - edge), place(edge + R, Cf - edge - R));
	return true;
}

float VoxelWorld::generate(int x, int y, int z, int p_octaves, float p_accuracy, int p_spacing, MaterialId *r_material) const {
	x = wrap_xz(x);
	z = wrap_xz(z);
	const float d = generate_ground(x, y, z, p_octaves, p_accuracy, p_spacing, r_material);
	if (feature_defs.empty() || p_spacing > FEATURE_MAX_SPACING) {
		return d;
	}
	return add_features(x, y, z, d, std::min(p_accuracy, DENSITY_CLAMP * (float)p_spacing), r_material);
}

float VoxelWorld::generate_ground(int x, int y, int z, int p_octaves, float p_accuracy, int p_spacing, MaterialId *r_material) const {
	x = wrap_xz(x);
	z = wrap_xz(z);
	if (y < y_min() || y >= y_max()) {
		// Beyond the limits, continue from the edge of the world, so the
		// field stays a distance there: past the top, open air (whatever
		// reaches the top ends half a voxel above it); past the bottom, more
		// of whatever the bottom is. (A plain "this far past the limit"
		// would put a sheet of near-surface values right at it, which rays
		// draw.)
		if (y >= y_max()) {
			const float edge = generate_ground(x, y_max() - 1, z, p_octaves, p_accuracy, p_spacing, r_material);
			return std::min(edge, 0.5f) - (float)(y - (y_max() - 1));
		}
		const float edge = generate_ground(x, y_min(), z, p_octaves, p_accuracy, p_spacing, r_material);
		const float beyond = (float)(y_min() - y);
		return edge > 0.0f ? edge + beyond : edge - beyond;
	}
	if (settings.world_type == WORLD_TERRAIN) {
		return generate_terrain(x, y, z, p_accuracy, p_spacing, r_material);
	}
	return generate_islands(x, y, z, p_octaves, p_accuracy, p_spacing, r_material);
}

float VoxelWorld::generate_islands(int x, int y, int z, int p_octaves, float p_accuracy, int p_spacing, MaterialId *r_material) const {
	const int C = settings.cell_size;
	const Vector3i cell(floor_div(x, C), floor_div(y, C), floor_div(z, C));
	const Vector3 p((float)x, (float)y, (float)z);
	// How far islands in other cells are, at least. Only the cells on the
	// near side of each axis (the 2x2x2 block around p's octant of its cell)
	// can be closer than half a cell plus the margin every island keeps from
	// its cell's faces. For those, a lower bound on the distance to the
	// island itself, skipping cells whose face alone is farther than what we
	// have. (Just the distance to the cell's face, plus the margin, would
	// make open sky along every face look a few voxels from a surface: rays
	// creep along such faces in small steps, and rays running nearly parallel
	// to one ran out of steps and drew it as a sheet of ground.)
	Vector3i side;
	Vector3 face; // distance to the nearer face, per axis
	for (int a = 0; a < 3; a++) {
		const float local = p[a] - (float)(cell[a] * C);
		side[a] = local < 0.5f * C ? -1 : 1;
		face[a] = side[a] < 0 ? local : (float)C - local;
	}
	// Fattening for points p_spacing apart: every point lies within 0.87
	// spacings of one of them, and an island holds a ball of about half its
	// radius, so growing it by the difference keeps a solid sample.
	auto fatten = [&](float p_radius) { return std::max(0.87f * (float)p_spacing - 0.5f * p_radius, 0.0f); };
	const float most_fattening = fatten(settings.min_radius);
	// Distances past p_accuracy are only ever stored clamped to it.
	float far = std::max(std::min(0.5f * C + margin - most_fattening, std::max(p_accuracy, margin)), 0.0f);
	// Consecutive points nearly always share the cell and octant: keep the
	// islands of the cell and those neighbours (per thread, as generate()
	// runs on several).
	struct IslandCache {
		const VoxelWorld *world = nullptr;
		uint32_t version = 0;
		Vector3i cell, side;
		uint8_t has = 0; // bit k: cell k (as below; 0 = p's own) has an island
		Island islands[8];
	};
	thread_local IslandCache cache;
	if (cache.world != this || cache.version != settings_version || cache.cell != cell || cache.side != side) {
		cache.world = this;
		cache.version = settings_version;
		cache.cell = cell;
		cache.side = side;
		cache.has = 0;
		for (int k = 0; k < 8; k++) {
			Vector3i neighbour = cell;
			for (int a = 0; a < 3; a++) {
				if (k & (1 << a)) {
					neighbour[a] += side[a];
				}
			}
			if (island_in_cell(neighbour, cache.islands[k])) {
				cache.has |= 1 << k;
			}
		}
	}
	for (int k = 1; k < 8; k++) {
		if (!(cache.has & (1 << k))) {
			continue;
		}
		Vector3 gap; // from p to the neighbour cell, per axis
		for (int a = 0; a < 3; a++) {
			if (k & (1 << a)) {
				gap[a] = face[a];
			}
		}
		const float reach_limit = far - margin + most_fattening; // islands are at least gap + this far
		if (reach_limit <= 0.0f || gap.length_squared() >= reach_limit * reach_limit) {
			continue;
		}
		// Outside the island's shape before its rocky detail (as below): no
		// farther than the true distance, less the detail.
		const Island &other = cache.islands[k];
		const Vector3 q = p - other.centre;
		const float r = std::sqrt(q.x * q.x + q.z * q.z);
		const float slope = other.radius / other.depth;
		const float cone = ((q.y + other.depth) * slope - r) / std::sqrt(1.0f + slope * slope);
		far = std::min(far, std::max(-std::min(-q.y, cone) - settings.detail - fatten(other.radius), 0.0f));
	}
	float best = -far;

	if (cache.has & 1) {
		const Island &island = cache.islands[0];
		// A flat top over a cone narrowing to a tip `depth` below it.
		const Vector3 q = p - island.centre;
		const float r = std::sqrt(q.x * q.x + q.z * q.z);
		const float k = island.radius / island.depth;
		const float cone = ((q.y + island.depth) * k - r) / std::sqrt(1.0f + k * k);
		float d = std::min(-q.y, cone);
		// Rocky noise, gentler on the top than underneath.
		const float reach = settings.detail + p_accuracy;
		if (d > -reach && d < reach && p_octaves > 0 && settings.detail > 0.0f) {
			const float underneath = std::clamp(0.5f - q.y / 16.0f, 0.0f, 1.0f);
			const float weight = 0.3f + 0.7f * underneath;
			const float f = settings.detail_frequency;
			d += settings.detail * weight * noise.fbm(x * f, y * f, z * f, p_octaves);
		} else {
			// Too far for the noise to matter: the nearest it could bring the surface.
			d += d > 0.0f ? -settings.detail : settings.detail;
		}
		best = std::max(best, d + fatten(island.radius));
		if (r_material != nullptr) {
			// Grass over a few voxels of dirt on the top; stone below and on
			// the cone. Air takes the material of the surface it faces.
			const float spacing = (float)p_spacing;
			const bool top = q.y > -(settings.detail + 3.0f + spacing);
			const float skin = std::max(1.5f, 0.9f * spacing), soil = std::max(4.5f, 1.8f * spacing);
			*r_material = !top ? MAT_STONE : (best < skin ? MAT_GRASS : (best < soil ? MAT_DIRT : MAT_STONE));
		}
		return best;
	}
	if (r_material != nullptr) {
		*r_material = MAT_STONE;
	}
	return best;
}

// --- Terrain ---------------------------------------------------------------------
//
// Ground height from 2D Perlin noise: wide continents and basins, hills
// over them, and steep ridged mountains where a third, wide field is high.
// 3D Perlin noise near the surface adds overhangs, and two more 3D fields
// carve winding tunnels where both are near zero. Grass over a few voxels of
// dirt over stone; sand in the low basins.
//
// The density is the vertical height difference turned into an estimate of
// the distance to the surface (divided by sqrt(1 + slope^2)), scaled down by
// how fast the 3D noise can change, so it stays a safe step for rays.

namespace {

constexpr float OVERHANG = 4.0f; // voxels the 3D noise moves the surface, at most
constexpr float OVERHANG_FREQUENCY = 1.0f / 24.0f;
// The 3D noise changes up to ~2.2 per lattice unit; dividing by this keeps
// the density from changing faster than distance.
constexpr float OVERHANG_LIPSCHITZ = 1.0f + OVERHANG * OVERHANG_FREQUENCY * 2.2f;
constexpr float CAVE_FREQUENCY = 1.0f / 40.0f;
constexpr float CAVE_WIDTH = 0.085f; // tunnels where both cave fields are within this of 0
constexpr float CAVE_DEPTH = 90.0f; // tunnels run no deeper than this below the ground
constexpr float SAND_BELOW = 2.0f; // ground lower than this above sea level is sand (beaches, sea floor)

float smooth01(float a, float b, float x) {
	const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
	return t * t * (3.0f - 2.0f * t);
}

} // namespace

float VoxelWorld::tiled_fbm(float p_x, float p_z, float p_wavelength, int p_octaves, float p_offset) const {
	// A whole number of repeats across the world, and no more octaves than
	// keep the finest one's period within the noise's 256.
	const int repeats = std::clamp((int)std::lround(settings.world_size / p_wavelength), 1, 256);
	int octaves = p_octaves;
	while (octaves > 1 && repeats << (octaves - 1) > 256) {
		octaves--;
	}
	const float k = (float)repeats / (float)settings.world_size;
	return perlin.fbm_periodic(p_x * k + p_offset, p_z * k + p_offset * 0.7f, octaves, repeats);
}

float VoxelWorld::tiled_noise(const PerlinNoise &p_noise, float p_x, float p_y, float p_z, float p_wavelength, float p_y_scale) const {
	const int repeats = std::clamp((int)std::lround(settings.world_size / p_wavelength), 1, 256);
	const float k = (float)repeats / (float)settings.world_size;
	return p_noise.noise_periodic(p_x * k, p_y / p_wavelength * p_y_scale, p_z * k, repeats);
}

float VoxelWorld::terrain_top() const {
	return 6.0f + settings.terrain_height * 2.9f + OVERHANG;
}

float VoxelWorld::terrain_bottom() const {
	return 6.0f - settings.terrain_height * (settings.terrain_version >= 2 ? 1.4f + SEA_BASIN : 1.4f) - OVERHANG;
}

float VoxelWorld::terrain_base_height(float fx, float fz) const {
	const float S = settings.terrain_scale, H = settings.terrain_height;
	const float continent = tiled_fbm(fx, fz, S * 4.0f, 3, 0.0f);
	const float hills = tiled_fbm(fx, fz, S, 5, 101.0f);
	const float mountains = smooth01(-0.1f, 0.35f, tiled_fbm(fx, fz, S * 2.0f, 2, -63.0f));
	const float ridge = 1.0f - std::abs(tiled_fbm(fx, fz, S * 0.75f, 4, 5.0f));
	float h = 6.0f + H * (0.6f * continent + 0.45f * hills + mountains * (1.8f * ridge * ridge - 0.3f));
	if (settings.terrain_version >= 2) {
		// Where the continents dip, the ground sinks into deep basins: the
		// seas (filled to sea_level by the renderer).
		h -= H * SEA_BASIN * smooth01(-0.05f, -0.35f, continent);
	}
	return h;
}

void VoxelWorld::terrain_height(int x, int z, float &r_height, float &r_slope) const {
	// Columns repeat for every y: cache them (per thread), direct-mapped.
	struct Entry {
		const VoxelWorld *world = nullptr;
		uint32_t version = 0;
		int x = 0, z = 0;
		float height = 0.0f, slope = 0.0f;
	};
	constexpr int SLOTS = 4096;
	thread_local Entry cache[SLOTS];
	Entry &e = cache[((uint32_t)x * 73856093u ^ (uint32_t)z * 83492791u) & (SLOTS - 1)];
	if (e.world == this && e.version == settings_version && e.x == x && e.z == z) {
		r_height = e.height;
		r_slope = e.slope;
		return;
	}
	auto height = [&](float fx, float fz) { return terrain_base_height(fx, fz); };
	const float fx = (float)x, fz = (float)z;
	e.height = height(fx, fz);
	const float gx = (height(fx + 1.0f, fz) - height(fx - 1.0f, fz)) * 0.5f;
	const float gz = (height(fx, fz + 1.0f) - height(fx, fz - 1.0f)) * 0.5f;
	e.slope = std::sqrt(gx * gx + gz * gz);
	e.world = this;
	e.version = settings_version;
	e.x = x;
	e.z = z;
	r_height = e.height;
	r_slope = e.slope;
}

float VoxelWorld::generate_terrain(int x, int y, int z, float p_accuracy, int p_spacing, MaterialId *r_material) const {
	const float fy = (float)y;
	// Far above every mountain: sky.
	const float top = terrain_top();
	if (fy > top + p_accuracy) {
		if (r_material != nullptr) {
			*r_material = MAT_GRASS;
		}
		return -(fy - top);
	}
	// Far below the deepest tunnel: rock.
	const float floor_y = terrain_bottom() - CAVE_DEPTH;
	if (fy < floor_y - p_accuracy) {
		if (r_material != nullptr) {
			*r_material = MAT_STONE;
		}
		return floor_y - fy;
	}
	float h, slope;
	terrain_height(x, z, h, slope);
	return generate_terrain_at(x, y, z, h, slope, p_accuracy, p_spacing, r_material);
}

float VoxelWorld::generate_terrain_at(int x, int y, int z, float h, float slope, float p_accuracy, int p_spacing, MaterialId *r_material) const {
	const float fy = (float)y;
	const float vertical = h - fy; // > 0 below the ground
	float ground = vertical / std::sqrt(1.0f + slope * slope);
	if (std::abs(ground) < p_accuracy + OVERHANG) {
		ground += OVERHANG * tiled_noise(perlin, (float)x, (float)y, (float)z, 1.0f / OVERHANG_FREQUENCY, 1.0f);
	} else {
		ground += ground > 0.0f ? -OVERHANG : OVERHANG; // the nearest it could bring the surface
	}
	ground /= OVERHANG_LIPSCHITZ;
	// Tunnels: only under the ground, and only where they can show (a
	// spacing much wider than a tunnel would just make noise of them).
	bool in_cave_rock = false;
	if (settings.caves && p_spacing <= 4 && vertical > 3.0f && vertical < CAVE_DEPTH && ground > -p_accuracy) {
		const float f = CAVE_FREQUENCY;
		// Distance-like: the fields change up to ~2.2 * 1.5 * f per voxel.
		const float per_unit = 1.0f / (2.2f * 1.5f * f);
		// Narrowing to nothing near the surface and the bottom of their band.
		const float narrowing = std::max(0.0f, 8.0f - (vertical - 3.0f)) + std::max(0.0f, 8.0f - (CAVE_DEPTH - vertical));
		const float a = tiled_noise(perlin, (float)x, (float)y, (float)z, 1.0f / f, 1.5f);
		// The tunnel is at least this far (whatever the second field), so
		// when that alone keeps it from mattering (farther than the ground's
		// surface, or than anything stored), skip the second field.
		if ((std::abs(a) - CAVE_WIDTH) * per_unit + narrowing < std::min(ground, p_accuracy)) {
			const float b = tiled_noise(perlin_caves, (float)x, (float)y, (float)z, 1.0f / f, 1.5f);
			const float cave = (std::sqrt(a * a + b * b) - CAVE_WIDTH) * per_unit + narrowing;
			if (cave < ground) {
				ground = cave;
				in_cave_rock = true;
			}
		}
	}
	const float density = ground;
	if (r_material != nullptr) {
		const float spacing = (float)p_spacing;
		const bool low = h < settings.sea_level + SAND_BELOW;
		MaterialId m;
		if (in_cave_rock) {
			m = MAT_STONE; // tunnel walls
		} else if (ground < std::max(1.2f, spacing)) {
			// The top (and the air above it). Measured from the surface
			// itself, which the overhang noise moves away from `h`.
			m = low ? MAT_SAND : MAT_GRASS;
		} else if (ground < std::max(4.0f, 2.0f * spacing)) {
			m = low ? MAT_SAND : MAT_DIRT;
		} else {
			m = MAT_STONE;
		}
		*r_material = m;
	}
	return density;
}

int VoxelWorld::octaves_for_scale(int p_scale) const {
	// All of them, at every scale. Dropping octaves too fine for the spacing
	// makes coarse levels a smoother but different shape, up to `detail`
	// voxels off; shadow rays reaching far-away terrain through those levels
	// then hit (or miss) surface that isn't there in full detail. With every
	// octave, a coarse level's samples are exactly the full-detail values.
	(void)p_scale;
	return settings.octaves;
}

bool VoxelWorld::find_island(const Vector3 &p_near, Vector3 &r_top) const {
	if (settings.world_type == WORLD_TERRAIN) {
		const int x = (int)std::floor(p_near.x), z = (int)std::floor(p_near.z);
		float h, slope;
		terrain_height(wrap_xz(x), wrap_xz(z), h, slope);
		r_top = Vector3((float)x, h, (float)z);
		return true;
	}
	const int C = settings.cell_size;
	const Vector3i home(floor_div((int)std::floor(p_near.x), C), floor_div((int)std::floor(p_near.y), C), floor_div((int)std::floor(p_near.z), C));
	float nearest = 1e30f;
	for (int z = -3; z <= 3; z++) {
		for (int y = -3; y <= 3; y++) {
			for (int x = -3; x <= 3; x++) {
				Island island;
				if (island_in_cell(home + Vector3i(x, y, z), island) && island.centre.distance_to(p_near) < nearest) {
					nearest = island.centre.distance_to(p_near);
					r_top = island.centre;
				}
			}
		}
	}
	return nearest < 1e30f;
}

int8_t VoxelWorld::encode_texel(float p_voxels, int p_level) {
	const float q = std::nearbyint(p_voxels * 16.0f / (float)(1 << p_level));
	return (int8_t)std::clamp(q, (float)-DENSITY_LIMIT, (float)DENSITY_LIMIT);
}

// --- Chunks ----------------------------------------------------------------------

Vector3i VoxelWorld::wrap_chunk(const Vector3i &p_chunk) const {
	const int n = settings.world_size / CHUNK;
	return Vector3i(p_chunk.x & (n - 1), p_chunk.y, p_chunk.z & (n - 1));
}

bool VoxelWorld::chunk_in_world(const Vector3i &p_chunk) const {
	return p_chunk.y >= y_min() / CHUNK && p_chunk.y < y_max() / CHUNK;
}

const VoxelWorld::Chunk *VoxelWorld::chunk_at(const Vector3i &p_chunk) const {
	if (chunks.empty() || !chunk_in_world(p_chunk)) {
		return nullptr;
	}
	Vector3i w = wrap_chunk(p_chunk);
	w.y -= y_min() / CHUNK;
	return &chunks[chunk_index(w)];
}

VoxelWorld::Chunk *VoxelWorld::chunk_at(const Vector3i &p_chunk) {
	return const_cast<Chunk *>(static_cast<const VoxelWorld *>(this)->chunk_at(p_chunk));
}

bool VoxelWorld::uniform_chunk(const Vector3i &p_chunk, int8_t &r_density, MaterialId &r_material) const {
	if (settings.world_type == WORLD_TERRAIN) {
		return false; // see generate_terrain_column()
	}
	// Islands: the generator's distance is a lower bound on the true one
	// (nearly: the rocky noise can reach a little past `detail`), so if the
	// chunk's centre is farther than half its diagonal plus the clamp, with
	// room to spare, from any island, every point is clamped air.
	const Vector3i centre = p_chunk * CHUNK + Vector3i(CHUNK / 2, CHUNK / 2, CHUNK / 2);
	const float d = generate_ground(centre.x, centre.y, centre.z, settings.octaves, 1e6f, 1, nullptr);
	if (d < -(CHUNK * 0.87f + DENSITY_CLAMP + 1.0f + settings.detail) && !features_touch_chunk(p_chunk, DENSITY_CLAMP + 1.0f)) {
		r_density = -DENSITY_LIMIT;
		r_material = MAT_STONE;
		return true;
	}
	return false;
}

void VoxelWorld::store_chunk(const std::vector<int8_t> &p_density, std::vector<MaterialId> &r_materials, Chunk &r_chunk) {
	bool uniform = true;
	for (int i = 0; i < CHUNK_POINTS; i++) {
		// Air this far from any surface never shows its material: make it
		// all alike, so empty chunks fold to one value.
		if (p_density[i] == -DENSITY_LIMIT) {
			r_materials[i] = MAT_STONE;
		}
		uniform &= p_density[i] == p_density[0] && r_materials[i] == r_materials[0];
	}
	r_chunk = Chunk();
	if (uniform) {
		r_chunk.uniform_density = p_density[0];
		r_chunk.uniform_material = r_materials[0];
	} else {
		r_chunk.full = p_density;
		r_chunk.full_materials = r_materials;
	}
}

void VoxelWorld::generate_chunk(const Vector3i &p_chunk, Chunk &r_chunk) const {
	r_chunk = Chunk();
	if (uniform_chunk(p_chunk, r_chunk.uniform_density, r_chunk.uniform_material)) {
		return;
	}
	thread_local std::vector<int8_t> density;
	thread_local std::vector<MaterialId> materials;
	density.resize(CHUNK_POINTS);
	materials.resize(CHUNK_POINTS);
	const Vector3i o = p_chunk * CHUNK;
	size_t i = 0;
	for (int z = 0; z < CHUNK; z++) {
		for (int y = 0; y < CHUNK; y++) {
			for (int x = 0; x < CHUNK; x++, i++) {
				density[i] = quantize_density(generate(o.x + x, o.y + y, o.z + z, settings.octaves, DENSITY_CLAMP, 1, &materials[i]));
			}
		}
	}
	store_chunk(density, materials, r_chunk);
}

void VoxelWorld::column_info(int p_cx, int p_cz, ColumnInfo &r_info) const {
	// The ground heights of the column's 32 x 32 points and one beyond on
	// each side, evaluated exactly as terrain_height() does (same
	// coordinates, same arithmetic), so what's built from them equals
	// generate().
	constexpr int N = CHUNK + 2;
	const int bx = p_cx * CHUNK, bz = p_cz * CHUNK; // wrapped: 0 .. world_size - 1
	float grid[N * N];
	for (int j = 0; j < N; j++) {
		for (int i = 0; i < N; i++) {
			// Corners aren't needed.
			grid[j * N + i] = (i == 0 || i == N - 1) && (j == 0 || j == N - 1) ? 0.0f : terrain_base_height((float)(bx + i - 1), (float)(bz + j - 1));
		}
	}
	r_info.heights.resize((size_t)CHUNK * CHUNK);
	r_info.slopes.resize((size_t)CHUNK * CHUNK);
	r_info.low = 1e30f;
	r_info.high = -1e30f;
	r_info.steepest = 0.0f;
	for (int j = 0; j < CHUNK; j++) {
		for (int i = 0; i < CHUNK; i++) {
			auto g = [&](int di, int dj) { return grid[(j + 1 + dj) * N + (i + 1 + di)]; };
			const float gx = (g(1, 0) - g(-1, 0)) * 0.5f;
			const float gz = (g(0, 1) - g(0, -1)) * 0.5f;
			const float h = g(0, 0), slope = std::sqrt(gx * gx + gz * gz);
			r_info.heights[(size_t)j * CHUNK + i] = h;
			r_info.slopes[(size_t)j * CHUNK + i] = slope;
			// The column's range takes in the neighbours too (edge points
			// look one voxel beyond).
			for (float v : { h, g(1, 0), g(-1, 0), g(0, 1), g(0, -1) }) {
				r_info.low = std::min(r_info.low, v);
				r_info.high = std::max(r_info.high, v);
			}
			r_info.steepest = std::max(r_info.steepest, slope);
		}
	}
}

const VoxelWorld::ColumnInfo &VoxelWorld::cached_column_info(int p_cx, int p_cz) const {
	const int key = p_cz * chunk_counts.x + p_cx;
	auto it = column_cache.find(key);
	if (it != column_cache.end()) {
		return it->second;
	}
	if (column_cache.size() >= 1024) {
		column_cache.clear();
	}
	ColumnInfo &info = column_cache[key];
	column_info(p_cx, p_cz, info);
	return info;
}

int8_t VoxelWorld::terrain_point(int x, int y, int z, float h, float slope, MaterialId *r_material) const {
	return quantize_density(terrain_value(x, y, z, h, slope, r_material));
}

float VoxelWorld::terrain_value(int x, int y, int z, float h, float slope, MaterialId *r_material) const {
	// As generate_ground() for a point of the world (x and z wrapped, y
	// within the limits) whose column's height is known.
	const float fy = (float)y;
	const float top = terrain_top(), floor_y = terrain_bottom() - CAVE_DEPTH;
	float d;
	if (fy > top + DENSITY_CLAMP) {
		*r_material = MAT_GRASS;
		d = -(fy - top);
	} else if (fy < floor_y - DENSITY_CLAMP) {
		*r_material = MAT_STONE;
		d = floor_y - fy;
	} else {
		d = generate_terrain_at(x, y, z, h, slope, DENSITY_CLAMP, 1, r_material);
	}
	return d;
}

void VoxelWorld::generate_terrain_chunk(const Vector3i &p_chunk, const ColumnInfo &p_info, Chunk &r_chunk) const {
	r_chunk = Chunk();
	// generate_terrain_at(): ((h - y) / sqrt(1 + slope^2) +- OVERHANG) /
	// OVERHANG_LIPSCHITZ, stored clamped to DENSITY_CLAMP: every point of a
	// chunk this far above (below) the ground is clamped sky (rock).
	const float clamped = (DENSITY_CLAMP * OVERHANG_LIPSCHITZ + OVERHANG) * std::sqrt(1.0f + p_info.steepest * p_info.steepest) + 1.0f;
	const float deep = std::max(clamped + 4.0f, settings.caves ? CAVE_DEPTH + 1.0f : 0.0f);
	const float y0 = (float)(p_chunk.y * CHUNK), y1 = y0 + CHUNK - 1;
	// Features reaching into it: unioned in at each point (as generate()).
	thread_local std::vector<std::pair<const FeatureInstance *, Vector3>> near;
	near.clear();
	if (!feature_defs.empty()) {
		const Vector3 lo = Vector3(wrap_chunk(p_chunk) * CHUNK) - Vector3(1, 1, 1) * DENSITY_CLAMP;
		for_each_feature(lo, lo + Vector3(1, 1, 1) * (CHUNK - 1 + 2 * DENSITY_CLAMP), [&](const FeatureInstance &p_instance, const Vector3 &p_shift) {
			near.emplace_back(&p_instance, p_shift);
		});
	}
	if (y0 > p_info.high + clamped && near.empty()) {
		r_chunk.uniform_density = -DENSITY_LIMIT; // sky
		r_chunk.uniform_material = MAT_STONE; // (as store_chunk() makes far air)
		return;
	}
	if (y1 < p_info.low - deep) {
		r_chunk.uniform_density = DENSITY_LIMIT; // rock below the deepest tunnels
		r_chunk.uniform_material = MAT_STONE;
		return;
	}
	thread_local std::vector<int8_t> density;
	thread_local std::vector<MaterialId> materials;
	density.resize(CHUNK_POINTS);
	materials.resize(CHUNK_POINTS);
	const Vector3i o = wrap_chunk(p_chunk) * CHUNK;
	size_t n = 0;
	for (int z = 0; z < CHUNK; z++) {
		for (int y = 0; y < CHUNK; y++) {
			for (int x = 0; x < CHUNK; x++, n++) {
				const size_t c = (size_t)z * CHUNK + x;
				float d = terrain_value(o.x + x, o.y + y, o.z + z, p_info.heights[c], p_info.slopes[c], &materials[n]);
				if (!near.empty() && d < DENSITY_CLAMP) {
					const Vector3 p((float)(o.x + x), (float)(o.y + y), (float)(o.z + z));
					for (const auto &[instance, shift] : near) {
						MaterialId m;
						const float v = feature_value(*instance, p - shift, &m);
						if (v > d) {
							d = v;
							materials[n] = m;
						}
					}
				}
				density[n] = quantize_density(d);
			}
		}
	}
	store_chunk(density, materials, r_chunk);
}

// --- Features ---------------------------------------------------------------------
//
// Each terrain region (FEATURE_REGION voxels square) or island (its cell)
// gets its features planned once, from the seed: every feature's tries pick
// spots, find the generated ground there (features don't grow on features),
// and keep the ones the feature grows on. generate() then unions the
// planned shapes into the ground wherever they reach.

namespace {

inline int wrap_index(int p_value, int p_count) {
	const int m = p_value % p_count;
	return m < 0 ? m + p_count : m;
}

inline bool boxes_meet(const Vector3 &a_lo, const Vector3 &a_hi, const Vector3 &b_lo, const Vector3 &b_hi) {
	return a_lo.x <= b_hi.x && a_hi.x >= b_lo.x && a_lo.y <= b_hi.y && a_hi.y >= b_lo.y && a_lo.z <= b_hi.z && a_hi.z >= b_lo.z;
}

} // namespace

void VoxelWorld::set_features(std::vector<FeatureShape> &&p_shapes, std::vector<FeatureDef> &&p_defs) {
	std::unique_lock<std::shared_mutex> lock(feature_lock);
	feature_shapes = std::move(p_shapes);
	feature_defs = std::move(p_defs);
	feature_regions.clear();
	feature_reach = 0.0f;
	feature_reach_xz = 0.0f;
	for (const FeatureDef &def : feature_defs) {
		for (int k : def.shapes) {
			const FeatureShape &shape = feature_shapes[k];
			const float reach = (shape.reach + def.sink) * def.scale_max + 1.0f;
			feature_reach = std::max(feature_reach, reach);
			// (Leaning with the ground, its height swings sideways.)
			feature_reach_xz = std::max(feature_reach_xz, def.follow_slope > 0.0f ? reach : shape.reach_xz * def.scale_max + 1.0f);
		}
	}
	settings_version++;
}

const VoxelWorld::FeatureRegion &VoxelWorld::feature_region(const Vector3i &p_key) const {
	// Points next to each other ask for the same few regions: a small
	// per-thread cache in front of the shared map.
	struct Slot {
		const VoxelWorld *world = nullptr;
		uint32_t version = 0;
		Vector3i key;
		const FeatureRegion *region = nullptr;
	};
	constexpr int SLOTS = 256;
	thread_local Slot cache[SLOTS];
	Slot &slot = cache[((uint32_t)p_key.x * 73856093u ^ (uint32_t)p_key.y * 19349663u ^ (uint32_t)p_key.z * 83492791u) & (SLOTS - 1)];
	if (slot.world == this && slot.version == settings_version && slot.key == p_key) {
		return *slot.region;
	}
	const FeatureRegion *found = nullptr;
	{
		std::shared_lock<std::shared_mutex> lock(feature_lock);
		auto it = feature_regions.find(p_key);
		if (it != feature_regions.end()) {
			found = it->second.get();
		}
	}
	if (found == nullptr) {
		// Planned outside the lock (another thread may plan it too: the
		// same, as it's all from the seed; the first one kept).
		std::unique_ptr<FeatureRegion> planned = std::make_unique<FeatureRegion>();
		plan_features(p_key, *planned);
		std::unique_lock<std::shared_mutex> lock(feature_lock);
		std::unique_ptr<FeatureRegion> &entry = feature_regions[p_key];
		if (entry == nullptr) {
			entry = std::move(planned);
		}
		found = entry.get();
	}
	slot.world = this;
	slot.version = settings_version;
	slot.key = p_key;
	slot.region = found;
	return *found;
}

void VoxelWorld::plan_features(const Vector3i &p_key, FeatureRegion &r_region) const {
	const bool terrain = settings.world_type == WORLD_TERRAIN;
	Island island;
	if (!terrain && !island_in_cell(p_key, island)) {
		return;
	}
	const float G = (float)FEATURE_REGION;
	const float area = terrain ? G * G : (float)Math_PI * island.radius * island.radius;
	std::vector<Vector3> taken; // where this region's features stand
	for (const FeatureDef &def : feature_defs) {
		if (!(def.world_types & (1u << settings.world_type)) || def.shapes.empty()) {
			continue;
		}
		uint64_t state = (uint64_t)settings.seed * 0x9E3779B97F4A7C15ull ^ def.id_hash ^ (uint64_t)(uint32_t)p_key.x * 0xD6E8FEB86659FD93ull ^
				(uint64_t)(uint32_t)p_key.y * 0xC2B2AE3D27D4EB4Full ^ (uint64_t)(uint32_t)p_key.z * 0x165667B19E3779F9ull;
		auto random = [&state]() { return (float)(mix64(state) >> 40) / (float)(1 << 24); };
		const float expected = def.per_region * area / (G * G);
		const int tries = (int)expected + (random() < expected - std::floor(expected) ? 1 : 0);
		for (int t = 0; t < tries; t++) {
			// (All drawn every time, so one spot failing leaves the rest where they were.)
			const float u = random(), v = random(), turn = random(), size = random(), pick = random();
			float x, z, top, bottom;
			if (terrain) {
				x = (p_key.x + u) * G;
				z = (p_key.z + v) * G;
				top = terrain_top();
				bottom = terrain_bottom();
			} else {
				const float r = island.radius * 0.95f * std::sqrt(u), a = v * (float)Math_TAU;
				x = island.centre.x + r * std::cos(a);
				z = island.centre.z + r * std::sin(a);
				top = island.centre.y + settings.detail + 3.0f;
				bottom = island.centre.y - island.depth;
			}
			Vector3 ground, normal;
			MaterialId material;
			if (!find_ground(x, z, top, bottom, ground, normal, material) || normal.y < def.min_up) {
				continue;
			}
			if (!def.grows_on.empty() && std::find(def.grows_on.begin(), def.grows_on.end(), material) == def.grows_on.end()) {
				continue;
			}
			if (terrain && ground.y < settings.sea_level + 1.0f) {
				continue; // (under the sea)
			}
			bool crowded = false;
			for (const Vector3 &other : taken) {
				crowded = crowded || other.distance_to(ground) < def.spacing;
			}
			if (crowded) {
				continue;
			}
			FeatureInstance instance;
			instance.shape = def.shapes[std::min((int)(pick * def.shapes.size()), (int)def.shapes.size() - 1)];
			const FeatureShape &shape = feature_shapes[instance.shape];
			instance.scale = def.scale_min + (def.scale_max - def.scale_min) * size;
			const Vector3 up = Vector3(0, 1, 0).slerp(normal, std::clamp(def.follow_slope, 0.0f, 1.0f)).normalized();
			Basis basis = up.is_equal_approx(Vector3(0, 1, 0)) ? Basis() : Basis(Quaternion(Vector3(0, 1, 0), up));
			if (def.random_turn) {
				basis = basis * Basis(Vector3(0, 1, 0), turn * (float)Math_TAU);
			}
			basis = basis * Basis::from_scale(Vector3(1, 1, 1) * instance.scale);
			instance.origin = ground - up * def.sink * instance.scale;
			instance.to_local = basis.inverse();
			instance.lo = Vector3(1e30f, 1e30f, 1e30f);
			instance.hi = -instance.lo;
			const Vector3 extent = Vector3(shape.size - Vector3i(1, 1, 1)) * shape.voxel_size;
			for (int k = 0; k < 8; k++) {
				const Vector3 corner = shape.origin + Vector3(k & 1 ? extent.x : 0.0f, k & 2 ? extent.y : 0.0f, k & 4 ? extent.z : 0.0f);
				const Vector3 at = instance.origin + basis.xform(corner);
				instance.lo = instance.lo.min(at);
				instance.hi = instance.hi.max(at);
			}
			r_region.lo = r_region.lo.min(instance.lo);
			r_region.hi = r_region.hi.max(instance.hi);
			r_region.instances.push_back(instance);
			taken.push_back(ground);
		}
	}
}

bool VoxelWorld::find_ground(float p_x, float p_z, float p_top, float p_bottom, Vector3 &r_point, Vector3 &r_normal, MaterialId &r_material) const {
	const int x = wrap_xz((int)std::floor(p_x)), z = wrap_xz((int)std::floor(p_z));
	if (settings.world_type == WORLD_TERRAIN) {
		// The surface is within the overhang noise's reach of the column's height.
		float h, slope;
		terrain_height(x, z, h, slope);
		const float reach = OVERHANG * std::sqrt(1.0f + slope * slope) + 3.0f;
		p_top = std::min(p_top, h + reach);
		p_bottom = std::max(p_bottom, h - reach);
	}
	auto density = [&](int px, int py, int pz, MaterialId *r_m) {
		return generate_ground(px, py, pz, settings.octaves, DENSITY_CLAMP, 1, r_m);
	};
	int y = (int)std::ceil(p_top);
	float above = density(x, y, z, nullptr);
	if (above > 0.0f) {
		return false; // (buried: no top in range)
	}
	while (y > p_bottom) {
		const int step = std::max((int)(-above * 0.9f), 1);
		const int below_y = y - step;
		MaterialId m;
		const float below = density(x, below_y, z, &m);
		if (below > 0.0f) {
			// Solid: back to one voxel steps for the crossing.
			if (step > 1) {
				for (int yy = y - 1; yy >= below_y; yy--) {
					const float d = density(x, yy, z, &m);
					if (d > 0.0f) {
						const float upper = density(x, yy + 1, z, nullptr);
						r_point = Vector3(p_x, (float)yy + d / (d - upper), p_z);
						y = yy;
						break;
					}
				}
			} else {
				r_point = Vector3(p_x, (float)below_y + below / (below - above), p_z);
				y = below_y;
			}
			r_material = m;
			const Vector3 grad(density(x + 1, y, z, nullptr) - density(x - 1, y, z, nullptr),
					density(x, y + 1, z, nullptr) - density(x, y - 1, z, nullptr),
					density(x, y, z + 1, nullptr) - density(x, y, z - 1, nullptr));
			r_normal = grad.length_squared() > 1e-8f ? -grad.normalized() : Vector3(0, 1, 0);
			return true;
		}
		y = below_y;
		above = below;
	}
	return false;
}

template <typename F>
void VoxelWorld::for_each_feature(const Vector3 &p_lo, const Vector3 &p_hi, F p_visit) const {
	if (feature_defs.empty()) {
		return;
	}
	const bool terrain = settings.world_type == WORLD_TERRAIN;
	const int G = terrain ? FEATURE_REGION : settings.cell_size;
	const int n = std::max(settings.world_size / G, 1);
	const float r = terrain ? feature_reach_xz : feature_reach;
	const int x0 = floor_div((int)std::floor(p_lo.x - r), G), x1 = floor_div((int)std::floor(p_hi.x + r), G);
	const int z0 = floor_div((int)std::floor(p_lo.z - r), G), z1 = floor_div((int)std::floor(p_hi.z + r), G);
	const int y0 = terrain ? 0 : floor_div((int)std::floor(p_lo.y - r), G), y1 = terrain ? 0 : floor_div((int)std::floor(p_hi.y + r), G);
	for (int cz = z0; cz <= z1; cz++) {
		for (int cy = y0; cy <= y1; cy++) {
			for (int cx = x0; cx <= x1; cx++) {
				const Vector3i key(wrap_index(cx, n), cy, wrap_index(cz, n));
				const FeatureRegion &region = feature_region(key);
				if (region.instances.empty()) {
					continue;
				}
				const Vector3 shift((float)((cx - key.x) * G), 0.0f, (float)((cz - key.z) * G));
				const Vector3 lo = p_lo - shift, hi = p_hi - shift;
				if (!boxes_meet(lo, hi, region.lo, region.hi)) {
					continue;
				}
				for (const FeatureInstance &instance : region.instances) {
					if (boxes_meet(lo, hi, instance.lo, instance.hi)) {
						p_visit(instance, shift);
					}
				}
			}
		}
	}
}

float VoxelWorld::feature_value(const FeatureInstance &p_instance, const Vector3 &p, MaterialId *r_material) const {
	const FeatureShape &shape = feature_shapes[p_instance.shape];
	const Vector3 g = (p_instance.to_local.xform(p - p_instance.origin) - shape.origin) / shape.voxel_size;
	const Vector3 last = Vector3(shape.size - Vector3i(1, 1, 1));
	const Vector3 c = g.clamp(Vector3(), last);
	const int x0 = std::min((int)c.x, shape.size.x - 2), y0 = std::min((int)c.y, shape.size.y - 2), z0 = std::min((int)c.z, shape.size.z - 2);
	const float tx = c.x - x0, ty = c.y - y0, tz = c.z - z0;
	const int sx = shape.size.x, sxy = shape.size.x * shape.size.y;
	const float *d = shape.density.data() + ((size_t)z0 * shape.size.y + y0) * sx + x0;
	auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
	const float v = lerp(lerp(lerp(d[0], d[1], tx), lerp(d[sx], d[sx + 1], tx), ty),
			lerp(lerp(d[sxy], d[sxy + 1], tx), lerp(d[sxy + sx], d[sxy + sx + 1], tx), ty), tz);
	if (r_material != nullptr) {
		*r_material = shape.materials[((size_t)std::lround(c.z) * shape.size.y + std::lround(c.y)) * sx + std::lround(c.x)];
	}
	const float k = shape.voxel_size * p_instance.scale;
	const float inside = v * k;
	const float out = (g - c).length() * k;
	if (out <= 0.0f) {
		return inside;
	}
	// Outside its grid: no nearer than the grid's edge, nor than the
	// surface is from that edge point (the solid is all inside the box).
	return inside > 0.0f ? -out : -std::sqrt(out * out + inside * inside);
}

float VoxelWorld::add_features(int x, int y, int z, float p_density, float p_accuracy, MaterialId *r_material) const {
	if (p_density >= p_accuracy) {
		return p_density; // (solid past what's stored: nothing to add)
	}
	// The features near a block of points (16 apart, at the points' spacing)
	// are found once and kept (per thread): nearly every point's block has
	// none, and the points around it come next.
	struct Block {
		const VoxelWorld *world = nullptr;
		uint32_t version = 0;
		Vector3i key;
		float accuracy = -1.0f;
		std::vector<std::pair<const FeatureInstance *, Vector3>> near;
	};
	constexpr int SLOTS = 64;
	thread_local Block blocks[SLOTS];
	const int B = std::max((int)std::ceil(p_accuracy / DENSITY_CLAMP), 1) * 16;
	const Vector3i key(floor_div(x, B), floor_div(y, B), floor_div(z, B));
	Block &block = blocks[((uint32_t)key.x * 73856093u ^ (uint32_t)key.y * 19349663u ^ (uint32_t)key.z * 83492791u ^ (uint32_t)B * 2654435761u) & (SLOTS - 1)];
	if (block.world != this || block.version != settings_version || block.key != key || block.accuracy != p_accuracy) {
		block.world = this;
		block.version = settings_version;
		block.key = key;
		block.accuracy = p_accuracy;
		block.near.clear();
		const Vector3 lo = Vector3(key * B) - Vector3(1, 1, 1) * p_accuracy;
		for_each_feature(lo, lo + Vector3(1, 1, 1) * (B + 2.0f * p_accuracy), [&](const FeatureInstance &p_instance, const Vector3 &p_shift) {
			block.near.emplace_back(&p_instance, p_shift);
		});
	}
	if (block.near.empty()) {
		return p_density;
	}
	const Vector3 p((float)x, (float)y, (float)z);
	float best = p_density;
	MaterialId material = MAT_STONE;
	bool won = false;
	for (const auto &[instance, shift] : block.near) {
		const Vector3 q = p - shift;
		if (q.x < instance->lo.x - p_accuracy || q.x > instance->hi.x + p_accuracy || q.y < instance->lo.y - p_accuracy ||
				q.y > instance->hi.y + p_accuracy || q.z < instance->lo.z - p_accuracy || q.z > instance->hi.z + p_accuracy) {
			continue;
		}
		MaterialId m;
		const float v = feature_value(*instance, q, &m);
		if (v > best) {
			best = v;
			material = m;
			won = true;
		}
	}
	if (won && r_material != nullptr) {
		*r_material = material;
	}
	return best;
}

bool VoxelWorld::features_touch_chunk(const Vector3i &p_chunk, float p_margin) const {
	const Vector3 lo = Vector3(p_chunk * CHUNK) - Vector3(1, 1, 1) * p_margin;
	bool touched = false;
	for_each_feature(lo, lo + Vector3(1, 1, 1) * (CHUNK - 1 + 2.0f * p_margin), [&](const FeatureInstance &, const Vector3 &) {
		touched = true;
	});
	return touched;
}

// --- Loading -------------------------------------------------------------------

namespace {

// Points of a chunk from its every-other-point samples (16^3, as mip 1 holds
// them; p_get(x, y, z) for x, y, z in 0..15), interpolated trilinearly in
// between (held flat past the last sample).
template <typename F>
void upsample_half(F p_get, int8_t *r_out) {
	constexpr int C = VoxelWorld::CHUNK, H = C / 2;
	for (int z = 0; z < C; z++) {
		for (int y = 0; y < C; y++) {
			for (int x = 0; x < C; x++) {
				const int x0 = x >> 1, y0 = y >> 1, z0 = z >> 1;
				const int x1 = std::min(x0 + (x & 1), H - 1), y1 = std::min(y0 + (y & 1), H - 1), z1 = std::min(z0 + (z & 1), H - 1);
				const float v = (p_get(x0, y0, z0) + p_get(x1, y0, z0) + p_get(x0, y1, z0) + p_get(x1, y1, z0) +
										p_get(x0, y0, z1) + p_get(x1, y0, z1) + p_get(x0, y1, z1) + p_get(x1, y1, z1)) /
						8.0f;
				r_out[((size_t)z * C + y) * C + x] = (int8_t)std::clamp((int)std::lround(v), -DENSITY_LIMIT, DENSITY_LIMIT);
			}
		}
	}
}

} // namespace

void VoxelWorld::load_chunk(const Vector3i &p_chunk, Chunk &r_chunk) const {
	// Edited: its points from the save (its mips are already in memory);
	// otherwise generated.
	if (r_chunk.edited) {
		if (!load_full(p_chunk, r_chunk)) {
			// Unreadable: rebuild it from its mips as well as possible (mip
			// 1, interpolated) rather than leave a hole.
			r_chunk.full.resize(CHUNK_POINTS);
			r_chunk.full_materials.resize(CHUNK_POINTS);
			const int n = CHUNK >> 1;
			upsample_half([&](int x, int y, int z) { return (float)r_chunk.mips[((size_t)z * n + y) * n + x]; }, r_chunk.full.data());
			for (int z = 0; z < CHUNK; z++) {
				for (int y = 0; y < CHUNK; y++) {
					for (int x = 0; x < CHUNK; x++) {
						r_chunk.full_materials[((size_t)z * CHUNK + y) * CHUNK + x] = r_chunk.mip_materials[((z >> 1) * n + (y >> 1)) * n + (x >> 1)];
					}
				}
			}
		}
		r_chunk.loaded = true;
		note_loaded(p_chunk);
		return;
	}
	Chunk made;
	if (settings.world_type == WORLD_TERRAIN) {
		const Vector3i w = wrap_chunk(p_chunk);
		generate_terrain_chunk(p_chunk, cached_column_info(w.x, w.z), made);
	} else {
		generate_chunk(p_chunk, made);
	}
	r_chunk.uniform_density = made.uniform_density;
	r_chunk.uniform_material = made.uniform_material;
	r_chunk.full = std::move(made.full);
	r_chunk.full_materials = std::move(made.full_materials);
	r_chunk.loaded = true;
	note_loaded(p_chunk);
}

void VoxelWorld::note_loaded(const Vector3i &p_chunk) const {
	std::lock_guard<std::mutex> lock(loaded_lock);
	loaded_set.insert(wrap_chunk(p_chunk));
}

const VoxelWorld::Chunk *VoxelWorld::loaded_chunk(const Vector3i &p_chunk) const {
	const Chunk *chunk = chunk_at(p_chunk);
	if (chunk != nullptr && !chunk->loaded) {
		load_chunk(p_chunk, *const_cast<Chunk *>(chunk));
	}
	return chunk;
}

int8_t VoxelWorld::generated_point(int x, int y, int z, MaterialId *r_material) const {
	MaterialId m;
	const int8_t d = quantize_density(generate(x, y, z, settings.octaves, DENSITY_CLAMP, 1, &m));
	if (r_material != nullptr) {
		// As store_chunk() keeps far air.
		*r_material = d == -DENSITY_LIMIT ? (MaterialId)MAT_STONE : m;
	}
	return d;
}

namespace {

// Chebyshev distance between a chunk's centre and a position, the shorter
// way round the wrapping world in x and z.
float wrapped_distance(const Vector3i &p_chunk, const Vector3 &p_position, int p_world_size) {
	const float half = VoxelWorld::CHUNK * 0.5f, W = (float)p_world_size;
	float d = std::abs(p_chunk.y * VoxelWorld::CHUNK + half - p_position.y);
	for (int a : { 0, 2 }) {
		float delta = std::fmod(p_chunk[a] * VoxelWorld::CHUNK + half - p_position[a], W);
		if (delta < 0.0f) {
			delta += W;
		}
		d = std::max(d, std::min(delta, W - delta));
	}
	return d;
}

} // namespace

int VoxelWorld::load_near(const Vector3 &p_center, float p_radius, double p_budget_ms) {
	if (chunks.empty()) {
		return 0;
	}
	const auto start = std::chrono::steady_clock::now();
	// The unloaded chunks within the radius, nearest first.
	const int r = (int)std::ceil(p_radius / CHUNK) + 1;
	const Vector3i centre = chunk_of((int)std::floor(p_center.x), (int)std::floor(p_center.y), (int)std::floor(p_center.z));
	std::vector<std::pair<float, Vector3i>> wanted;
	for (int z = -r; z <= r; z++) {
		for (int y = -r; y <= r; y++) {
			for (int x = -r; x <= r; x++) {
				const Vector3i c = centre + Vector3i(x, y, z);
				const Chunk *chunk = chunk_at(c);
				if (chunk == nullptr || chunk->loaded) {
					continue;
				}
				const float d = wrapped_distance(wrap_chunk(c), p_center, settings.world_size);
				if (d <= p_radius) {
					wanted.push_back({ d, c });
				}
			}
		}
	}
	std::sort(wanted.begin(), wanted.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
	int loaded = 0;
	// A finished background batch: take in what still wants it (a chunk may
	// have been loaded on demand, or edited, meanwhile).
	if (load_batch && load_batch->finished) {
		load_batch->thread.join();
		LoadBatch &batch = *load_batch;
		for (size_t i = 0; i < batch.chunks.size(); i++) {
			Chunk *chunk = chunk_at(batch.chunks[i]);
			// Loaded on demand meanwhile, or edited since it was generated.
			if (chunk == nullptr || chunk->loaded || (bool)chunk->edited != (bool)batch.edited[i]) {
				continue;
			}
			if (batch.edited[i]) {
				if (!batch.read_ok[i]) {
					loaded_chunk(batch.chunks[i]); // here, with its error and fallback
					loaded++;
					continue;
				}
				chunk->full = std::move(batch.made[i].full);
				chunk->full_materials = std::move(batch.made[i].full_materials);
				repair_doubled(*chunk);
			} else {
				chunk->uniform_density = batch.made[i].uniform_density;
				chunk->uniform_material = batch.made[i].uniform_material;
				chunk->full = std::move(batch.made[i].full);
				chunk->full_materials = std::move(batch.made[i].full_materials);
			}
			chunk->loaded = true;
			note_loaded(batch.chunks[i]);
			loaded++;
		}
		load_batch.reset();
	}
	// The next unloaded chunks, nearest first, go to a new batch if none is
	// running: generated or (edited) read from the save, all on its thread,
	// so neither the noise nor a slow disk holds up a frame.
	if (!load_batch && !wanted.empty()) {
		std::unique_ptr<LoadBatch> batch = std::make_unique<LoadBatch>();
		const bool terrain = settings.world_type == WORLD_TERRAIN;
		for (const auto &entry : wanted) {
			if ((int)batch->chunks.size() >= LOAD_BATCH) {
				break;
			}
			const Vector3i c = entry.second;
			const bool edited = chunk_at(c)->edited;
			batch->chunks.push_back(c);
			batch->edited.push_back(edited);
			int column = -1;
			if (terrain && !edited) {
				const Vector3i w = wrap_chunk(c);
				const Vector3i key(w.x, 0, w.z);
				for (size_t k = 0; k < batch->columns.size() && column < 0; k++) {
					if (batch->columns[k] == key) {
						column = (int)k;
					}
				}
				if (column < 0) {
					column = (int)batch->columns.size();
					batch->columns.push_back(key);
				}
			}
			batch->column_of.push_back(column);
		}
		const size_t count = batch->chunks.size();
		batch->made.resize(count);
		batch->read_ok.assign(count, 0);
		batch->infos.resize(batch->columns.size());
		LoadBatch *raw = batch.get();
		raw->thread = std::thread([this, raw, terrain]() {
			const int threads = std::clamp((int)std::thread::hardware_concurrency() / 2, 1, 4);
			parallel_for((int)raw->columns.size(), threads, [&](int k) {
				column_info(raw->columns[k].x, raw->columns[k].z, raw->infos[k]);
			});
			parallel_for((int)raw->chunks.size(), threads, [&](int i) {
				Chunk &made = raw->made[i];
				if (raw->edited[i]) {
					raw->read_ok[i] = read_full(raw->chunks[i], made.full, made.full_materials);
				} else if (terrain) {
					generate_terrain_chunk(raw->chunks[i], raw->infos[raw->column_of[i]], made);
				} else {
					generate_chunk(raw->chunks[i], made);
				}
			});
			raw->finished = true;
		});
		load_batch = std::move(batch);
	}
	(void)p_budget_ms;
	return loaded;
}

void VoxelWorld::unload_far(const Vector3 &p_center, float p_radius) {
	// Edits farther out are saved before their points are let go (their
	// mips stay, for distant views). Only chunks known to be loaded are
	// looked at: going through every chunk of the world (256 x 64 x 256 by
	// default) every second was a hitch of its own.
	std::vector<Vector3i> loaded;
	{
		std::lock_guard<std::mutex> lock(loaded_lock);
		loaded.assign(loaded_set.begin(), loaded_set.end());
	}
	std::unordered_set<Vector3i, Vector3iHash> to_save;
	std::vector<Vector3i> far, stale;
	for (const Vector3i &c : loaded) {
		const Chunk *chunk = chunk_at(c);
		if (chunk == nullptr || !chunk->loaded) {
			stale.push_back(c);
			continue;
		}
		if (wrapped_distance(c, p_center, settings.world_size) <= p_radius) {
			continue;
		}
		far.push_back(c);
		if (chunk->dirty) {
			to_save.insert(region_of(c));
		}
	}
	{
		std::lock_guard<std::mutex> lock(loaded_lock);
		for (const Vector3i &c : stale) {
			loaded_set.erase(c);
		}
	}
	// Written in the background; a chunk's points are let go once its
	// region's write is done (a later call), so reading it back never finds
	// the old file.
	poll_writes();
	for (const Vector3i &region : to_save) {
		queue_region_write(region);
	}
	for (const Vector3i &c : far) {
		Chunk &chunk = *chunk_at(c);
		if (chunk.dirty || pending_writes.count(region_of(c))) {
			continue;
		}
		chunk.full = std::vector<int8_t>();
		chunk.full_materials = std::vector<MaterialId>();
		chunk.loaded = false;
		std::lock_guard<std::mutex> lock(loaded_lock);
		loaded_set.erase(c);
	}
}

// --- Reading -----------------------------------------------------------------

int8_t VoxelWorld::lod_texel(int x, int y, int z, int p_level, MaterialId &r_material) const {
	const Vector3i c = chunk_of(x, y, z);
	const Chunk *chunk = chunk_at(c);
	// Level 0 reads what's stored (loading edited chunks: their points are
	// only in the save); coarser levels read the mips of edited chunks.
	// Everything else is generated (the same values; coarse levels with
	// distances beyond a stored byte).
	if (chunk != nullptr && p_level == 0 && chunk->edited) {
		chunk = loaded_chunk(c);
	}
	if (chunk != nullptr && ((p_level == 0 && chunk->loaded) || (p_level > 0 && chunk->edited))) {
		const int lx = x - c.x * CHUNK, ly = y - c.y * CHUNK, lz = z - c.z * CHUNK;
		if (p_level == 0) {
			const size_t i = ((size_t)lz * CHUNK + ly) * CHUNK + lx;
			r_material = chunk_material(*chunk, i);
			return chunk_density(*chunk, i);
		}
		const int m = std::clamp(p_level, 1, MIP_LEVELS);
		const int n = CHUNK >> m;
		const size_t i = mip_offset(m) + ((lz >> m) * n + (ly >> m)) * n + (lx >> m);
		r_material = chunk->mip_materials[i];
		return encode_texel(chunk->mips[i] * DENSITY_STEP, p_level);
	}
	const int s = 1 << p_level;
	return encode_texel(generate(x, y, z, octaves_for_scale(s), DENSITY_CLAMP * s, s, &r_material), p_level);
}

void VoxelWorld::edited_chunks_in(const Vector3i &p_min, const Vector3i &p_max, std::vector<Vector3i> &r_chunks) const {
	r_chunks.clear();
	// Each edited chunk, in every repeat of the world the box reaches.
	const int W = settings.world_size;
	auto ceil_div = [](int a, int b) { return -floor_div(-a, b); };
	for (const Vector3i &c : edited_list) {
		if (c.y * CHUNK > p_max.y || c.y * CHUNK + CHUNK - 1 < p_min.y) {
			continue;
		}
		const int kx0 = ceil_div(p_min.x - c.x * CHUNK - (CHUNK - 1), W), kx1 = floor_div(p_max.x - c.x * CHUNK, W);
		const int kz0 = ceil_div(p_min.z - c.z * CHUNK - (CHUNK - 1), W), kz1 = floor_div(p_max.z - c.z * CHUNK, W);
		for (int kz = kz0; kz <= kz1; kz++) {
			for (int kx = kx0; kx <= kx1; kx++) {
				r_chunks.push_back(Vector3i(c.x + kx * (W / CHUNK), c.y, c.z + kz * (W / CHUNK)));
			}
		}
	}
}

int8_t VoxelWorld::point(int x, int y, int z) const {
	const Vector3i c = chunk_of(x, y, z);
	const Chunk *chunk = chunk_at(c);
	if (chunk != nullptr && !chunk->loaded) {
		// Generated points are computed where they're needed (a ray may
		// cross hundreds of chunks); only edits have to be loaded.
		if (!chunk->edited) {
			return generated_point(x, y, z, nullptr);
		}
		chunk = loaded_chunk(c);
	}
	if (chunk == nullptr) {
		// As generate() continues the field past the limits.
		const int steps = 16;
		if (y >= y_max()) {
			const int edge = std::min<int>(point(x, y_max() - 1, z), steps / 2);
			return (int8_t)std::max(edge - steps * (y - (y_max() - 1)), -DENSITY_LIMIT);
		}
		const int edge = point(x, y_min(), z), beyond = steps * (y_min() - y);
		return (int8_t)std::clamp(edge > 0 ? edge + beyond : edge - beyond, -DENSITY_LIMIT, DENSITY_LIMIT);
	}
	return chunk_density(*chunk, ((size_t)(z - c.z * CHUNK) * CHUNK + (y - c.y * CHUNK)) * CHUNK + (x - c.x * CHUNK));
}

MaterialId VoxelWorld::point_material(int x, int y, int z) const {
	const Vector3i c = chunk_of(x, y, z);
	const Chunk *chunk = chunk_at(c);
	if (chunk == nullptr) {
		return MAT_STONE;
	}
	if (!chunk->loaded) {
		if (!chunk->edited) {
			MaterialId m;
			generated_point(x, y, z, &m);
			return m;
		}
		chunk = loaded_chunk(c);
	}
	return chunk_material(*chunk, ((size_t)(z - c.z * CHUNK) * CHUNK + (y - c.y * CHUNK)) * CHUNK + (x - c.x * CHUNK));
}

float VoxelWorld::sample(const Vector3 &p) const {
	const float fx = std::floor(p.x), fy = std::floor(p.y), fz = std::floor(p.z);
	const int x = (int)fx, y = (int)fy, z = (int)fz;
	const float tx = p.x - fx, ty = p.y - fy, tz = p.z - fz;
	float c[8];
	for (int k = 0; k < 8; k++) {
		c[k] = point(x + (k & 1), y + ((k >> 1) & 1), z + ((k >> 2) & 1)) * DENSITY_STEP;
	}
	auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
	return lerp(lerp(lerp(c[0], c[1], tx), lerp(c[2], c[3], tx), ty),
			lerp(lerp(c[4], c[5], tx), lerp(c[6], c[7], tx), ty), tz);
}

bool VoxelWorld::edited_data(const Vector3i &p_chunk, int p_level, const int8_t *&r_data, const MaterialId *&r_materials, int &r_side) const {
	const Chunk *chunk = chunk_at(p_chunk);
	if (chunk == nullptr) {
		return false;
	}
	if (p_level == 0) {
		if (chunk->edited) {
			chunk = loaded_chunk(p_chunk);
		}
		if (!chunk->loaded || chunk->uniform()) {
			return false;
		}
		r_data = chunk->full.data();
		r_materials = chunk->full_materials.data();
		r_side = CHUNK;
		return true;
	}
	if (!chunk->edited) {
		return false;
	}
	const int m = std::clamp(p_level, 1, MIP_LEVELS);
	r_data = chunk->mips.data() + mip_offset(m);
	r_materials = chunk->mip_materials.data() + mip_offset(m);
	r_side = CHUNK >> m;
	return true;
}

bool VoxelWorld::edited_unloaded(const Vector3i &p_chunk, const int8_t *&r_mip1, const MaterialId *&r_mip1_materials) const {
	const Chunk *chunk = chunk_at(p_chunk);
	if (chunk == nullptr || !chunk->edited || chunk->loaded || chunk->mips.size() < (size_t)MIP_BYTES) {
		return false;
	}
	r_mip1 = chunk->mips.data() + mip_offset(1);
	r_mip1_materials = chunk->mip_materials.data() + mip_offset(1);
	return true;
}

void VoxelWorld::generate_materials(const Vector3i &p_chunk, MaterialId *r_full) const {
	const Vector3i o = p_chunk * CHUNK;
	size_t i = 0;
	for (int z = 0; z < CHUNK; z++) {
		for (int y = 0; y < CHUNK; y++) {
			for (int x = 0; x < CHUNK; x++, i++) {
				generate(o.x + x, o.y + y, o.z + z, settings.octaves, DENSITY_CLAMP, 1, &r_full[i]);
			}
		}
	}
}

void VoxelWorld::read_points(const Vector3i &p_min, const Vector3i &p_max, float *r_out, int p_threads, MaterialId *r_materials) const {
	(void)p_threads; // everything is in memory already
	const Vector3i n = p_max - p_min + Vector3i(1, 1, 1);
	const Vector3i c0 = chunk_of(p_min.x, p_min.y, p_min.z), c1 = chunk_of(p_max.x, p_max.y, p_max.z);
	for (int cz = c0.z; cz <= c1.z; cz++) {
		for (int cy = c0.y; cy <= c1.y; cy++) {
			for (int cx = c0.x; cx <= c1.x; cx++) {
				const Vector3i c(cx, cy, cz);
				const Chunk *chunk = loaded_chunk(c);
				Vector3i lo, hi; // the part of the chunk inside the box
				for (int a = 0; a < 3; a++) {
					lo[a] = std::max(c[a] * CHUNK, p_min[a]);
					hi[a] = std::min(c[a] * CHUNK + CHUNK - 1, p_max[a]);
				}
				for (int z = lo.z; z <= hi.z; z++) {
					for (int y = lo.y; y <= hi.y; y++) {
						const size_t out = ((size_t)(z - p_min.z) * n.y + (y - p_min.y)) * n.x + (lo.x - p_min.x);
						const int count = hi.x - lo.x + 1;
						if (chunk == nullptr) {
							// Past the limits (see point()).
							for (int k = 0; k < count; k++) {
								r_out[out + k] = point(lo.x + k, y, z) * DENSITY_STEP;
							}
							if (r_materials != nullptr) {
								std::fill(r_materials + out, r_materials + out + count, (MaterialId)MAT_STONE);
							}
							continue;
						}
						if (chunk->uniform()) {
							const float d = chunk->uniform_density * DENSITY_STEP;
							const MaterialId m = chunk->uniform_material;
							std::fill(r_out + out, r_out + out + count, d);
							if (r_materials != nullptr) {
								std::fill(r_materials + out, r_materials + out + count, m);
							}
							continue;
						}
						const size_t in = ((size_t)(z - c.z * CHUNK) * CHUNK + (y - c.y * CHUNK)) * CHUNK + (lo.x - c.x * CHUNK);
						for (int k = 0; k < count; k++) {
							r_out[out + k] = chunk->full[in + k] * DENSITY_STEP;
						}
						if (r_materials != nullptr) {
							std::memcpy(r_materials + out, chunk->full_materials.data() + in, count * sizeof(MaterialId));
						}
					}
				}
			}
		}
	}
}

// --- Editing -----------------------------------------------------------------

VoxelWorld::ChunkData VoxelWorld::edit_chunk(const Vector3i &p_chunk) {
	Chunk *chunk = const_cast<Chunk *>(loaded_chunk(p_chunk));
	if (chunk == nullptr) {
		return { nullptr, nullptr };
	}
	if (track_changes && !suppress_changes) {
		const Vector3i key = wrap_chunk(p_chunk);
		if (changes.find(key) == changes.end()) {
			ChangedChunk &before = changes[key];
			before.chunk = key;
			before.density.resize(CHUNK_POINTS);
			before.materials.resize(CHUNK_POINTS);
			for (size_t i = 0; i < (size_t)CHUNK_POINTS; i++) {
				before.density[i] = chunk_density(*chunk, i);
				before.materials[i] = chunk_material(*chunk, i);
			}
		}
	}
	if (chunk->uniform()) {
		chunk->full.assign(CHUNK_POINTS, chunk->uniform_density);
		chunk->full_materials.assign(CHUNK_POINTS, chunk->uniform_material);
	}
	if (!chunk->edited) {
		chunk->edited = true;
		edited_list.push_back(wrap_chunk(p_chunk));
		chunk->mips.resize(MIP_BYTES);
		chunk->mip_materials.resize(MIP_BYTES);
	}
	chunk->dirty = true;
	return { chunk->full.data(), chunk->full_materials.data() };
}

void VoxelWorld::update_mips(const Vector3i &p_chunk) {
	Chunk *chunk = chunk_at(p_chunk);
	if (chunk == nullptr || !chunk->edited || !chunk->loaded) {
		return;
	}
	if (!chunk->uniform()) {
		// Fold back to one value if the edit left it all alike.
		bool uniform = true;
		for (int i = 1; i < CHUNK_POINTS && uniform; i++) {
			uniform = chunk->full[i] == chunk->full[0] && chunk->full_materials[i] == chunk->full_materials[0];
		}
		if (uniform) {
			chunk->uniform_density = chunk->full[0];
			chunk->uniform_material = chunk->full_materials[0];
			chunk->full = std::vector<int8_t>();
			chunk->full_materials = std::vector<MaterialId>();
		}
	}
	if (chunk->uniform()) {
		std::fill(chunk->mips.begin(), chunk->mips.end(), chunk->uniform_density);
		std::fill(chunk->mip_materials.begin(), chunk->mip_materials.end(), chunk->uniform_material);
		return;
	}
	// Densities: point-sampled. Materials: what shows at a distance, so of
	// the solid point nearest the surface within each mip point's block
	// (the point's own if the block has no surface).
	for (int m = 1; m <= MIP_LEVELS; m++) {
		const int n = CHUNK >> m, s = 1 << m;
		int8_t *mip = &chunk->mips[mip_offset(m)];
		MaterialId *mip_materials = &chunk->mip_materials[mip_offset(m)];
		for (int z = 0; z < n; z++) {
			for (int y = 0; y < n; y++) {
				for (int x = 0; x < n; x++) {
					const size_t corner = ((size_t)(z << m) * CHUNK + (y << m)) * CHUNK + (x << m);
					mip[(z * n + y) * n + x] = chunk->full[corner];
					MaterialId material = chunk->full_materials[corner];
					int nearest = DENSITY_LIMIT + 1;
					for (int bz = 0; bz < s; bz++) {
						for (int by = 0; by < s; by++) {
							const size_t row = ((size_t)((z << m) + bz) * CHUNK + ((y << m) + by)) * CHUNK + (x << m);
							for (int bx = 0; bx < s; bx++) {
								const int v = chunk->full[row + bx];
								if (v >= 0 && v < nearest) {
									nearest = v;
									material = chunk->full_materials[row + bx];
								}
							}
						}
					}
					mip_materials[(z * n + y) * n + x] = material;
				}
			}
		}
	}
}

// --- Disk ----------------------------------------------------------------------

String VoxelWorld::region_path(const Vector3i &p_region) const {
	return dir.path_join(vformat("r.%d.%d.%d.bin", p_region.x, p_region.y, p_region.z));
}

bool VoxelWorld::write_settings() {
	Ref<ConfigFile> config;
	config.instantiate();
	config->set_value("world", "format", SETTINGS_FORMAT);
	config->set_value("world", "type", settings.world_type == WORLD_TERRAIN ? "terrain" : "islands");
	config->set_value("world", "seed", settings.seed);
	config->set_value("world", "size", settings.world_size);
	config->set_value("world", "height", settings.world_height);
	config->set_value("terrain", "height", settings.terrain_height);
	config->set_value("terrain", "scale", settings.terrain_scale);
	config->set_value("terrain", "caves", settings.caves);
	config->set_value("terrain", "sea_level", settings.sea_level);
	config->set_value("terrain", "version", settings.terrain_version);
	config->set_value("islands", "cell_size", settings.cell_size);
	config->set_value("islands", "island_chance", settings.island_chance);
	config->set_value("islands", "min_radius", settings.min_radius);
	config->set_value("islands", "max_radius", settings.max_radius);
	config->set_value("islands", "detail", settings.detail);
	config->set_value("islands", "detail_frequency", settings.detail_frequency);
	config->set_value("islands", "octaves", settings.octaves);
	const String path = dir.path_join("world.cfg");
	if (config->save(path + String(".tmp")) != OK || DirAccess::rename_absolute(path + String(".tmp"), path) != OK) {
		last_error = "Could not write " + path;
		return false;
	}
	return true;
}

bool VoxelWorld::read_settings() {
	Ref<ConfigFile> config;
	config.instantiate();
	const String path = dir.path_join("world.cfg");
	if (config->load(path) != OK) {
		return false;
	}
	if ((int)config->get_value("world", "format", 0) != SETTINGS_FORMAT) {
		last_error = path + String(" is an older, endless world, which can't be opened as a wrapping one (start a new world)");
		return false;
	}
	IslandSettings s;
	s.world_type = String(config->get_value("world", "type", "islands")) == "terrain" ? WORLD_TERRAIN : WORLD_ISLANDS;
	s.seed = config->get_value("world", "seed", s.seed);
	s.world_size = config->get_value("world", "size", s.world_size);
	s.world_height = config->get_value("world", "height", s.world_height);
	s.terrain_height = (float)(double)config->get_value("terrain", "height", s.terrain_height);
	s.terrain_scale = (float)(double)config->get_value("terrain", "scale", s.terrain_scale);
	s.caves = config->get_value("terrain", "caves", s.caves);
	s.sea_level = (float)(double)config->get_value("terrain", "sea_level", 0.0);
	s.terrain_version = config->get_value("terrain", "version", 1); // worlds from before versions were 1
	s.cell_size = config->get_value("islands", "cell_size", s.cell_size);
	s.island_chance = (float)(double)config->get_value("islands", "island_chance", s.island_chance);
	s.min_radius = (float)(double)config->get_value("islands", "min_radius", s.min_radius);
	s.max_radius = (float)(double)config->get_value("islands", "max_radius", s.max_radius);
	s.detail = (float)(double)config->get_value("islands", "detail", s.detail);
	s.detail_frequency = (float)(double)config->get_value("islands", "detail_frequency", s.detail_frequency);
	s.octaves = config->get_value("islands", "octaves", s.octaves);
	configure(s);
	return true;
}

bool VoxelWorld::adopt_dir(const String &p_dir) {
	if (!dir.is_empty() || p_dir.is_empty()) {
		last_error = "adopt_dir: this world already saves somewhere (or no directory given)";
		return false;
	}
	const String target = ProjectSettings::get_singleton()->globalize_path(p_dir);
	if (DirAccess::make_dir_recursive_absolute(target) != OK) {
		last_error = "Could not create " + target;
		return false;
	}
	// An older save there (of an earlier session) goes: this copy replaces it.
	const PackedStringArray files = DirAccess::get_files_at(target);
	for (int64_t i = 0; i < files.size(); i++) {
		const String name = files[i];
		if ((name.begins_with("r.") && name.ends_with(".bin")) || name == "world.cfg" || name.ends_with(".tmp")) {
			DirAccess::remove_absolute(target.path_join(name));
		}
	}
	stop_writer(); // (writes for the old directory, if any)
	dir = target;
	{
		std::lock_guard<std::mutex> lock(region_lock);
		region_files.clear();
	}
	if (!write_settings()) {
		dir = String();
		return false;
	}
	for (Chunk &chunk : chunks) {
		if (chunk.edited) {
			chunk.dirty = true;
		}
	}
	return true;
}

bool VoxelWorld::open(const String &p_dir, int p_threads) {
	close();
	bool ok = true;
	if (!p_dir.is_empty()) {
		dir = ProjectSettings::get_singleton()->globalize_path(p_dir);
		if (DirAccess::make_dir_recursive_absolute(dir) != OK) {
			last_error = "Could not create " + dir;
			ok = false;
		} else {
			const bool existing = FileAccess::file_exists(dir.path_join("world.cfg"));
			if (existing ? !read_settings() : !write_settings()) {
				if (last_error.is_empty()) {
					last_error = "Could not read " + dir.path_join("world.cfg");
				}
				ok = false;
			}
		}
		if (!ok) {
			dir = String();
		}
	}
	(void)p_threads;
	// A slot for every chunk of the world, none loaded yet (see load_near()).
	chunk_counts = Vector3i(settings.world_size / CHUNK, settings.world_height / CHUNK, settings.world_size / CHUNK);
	chunks.assign((size_t)chunk_counts.x * chunk_counts.y * chunk_counts.z, Chunk());
	{
		std::lock_guard<std::mutex> lock(loaded_lock);
		loaded_set.clear();
	}
	edited_list.clear();
	column_cache.clear();
	// Which chunks are edited, and their mips (their points stay on disk
	// until needed).
	if (!dir.is_empty()) {
		const PackedStringArray files = DirAccess::get_files_at(dir);
		for (int64_t i = 0; i < files.size(); i++) {
			const String name = files[i];
			if (name.ends_with(".tmp")) {
				DirAccess::remove_absolute(dir.path_join(name)); // an interrupted save
				continue;
			}
			const PackedStringArray parts = name.split(".");
			if (parts.size() == 5 && parts[0] == "r" && parts[4] == "bin") {
				const Vector3i r(parts[1].to_int(), parts[2].to_int(), parts[3].to_int());
				region_files.insert(r);
				load_region(r);
			}
		}
	}
	return ok;
}

void VoxelWorld::wait_load_batch() {
	if (load_batch && load_batch->thread.joinable()) {
		load_batch->thread.join();
	}
	load_batch.reset();
}

VoxelWorld::~VoxelWorld() {
	wait_load_batch();
	stop_writer();
}

void VoxelWorld::close() {
	wait_load_batch();
	stop_writer();
	pending_writes.clear();
	chunks.clear();
	{
		std::lock_guard<std::mutex> lock(loaded_lock);
		loaded_set.clear();
	}
	edited_list.clear();
	column_cache.clear();
	region_index.clear();
	region_files.clear();
	dir = String();
	last_error = String();
}

bool VoxelWorld::read_region_index(const String &p_path, std::vector<RegionEntry> &r_entries, int *r_version) const {
	r_entries.clear();
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::READ);
	if (file.is_null()) {
		return false;
	}
	uint8_t head[8];
	const uint32_t magic = file->get_buffer(head, 8) == 8 ? get_u32(head) : 0;
	if (magic != REGION_MAGIC && magic != REGION_MAGIC_3 && magic != REGION_MAGIC_2 && magic != REGION_MAGIC_1) {
		return false;
	}
	if (r_version != nullptr) {
		*r_version = magic == REGION_MAGIC ? 4 : (magic == REGION_MAGIC_3 ? 3 : (magic == REGION_MAGIC_2 ? 2 : 1));
	}
	const uint32_t count = get_u32(head + 4);
	if (count > (uint32_t)(REGION * REGION * REGION)) {
		return false;
	}
	std::vector<uint8_t> table(count * 12);
	if (file->get_buffer(table.data(), table.size()) != table.size()) {
		return false;
	}
	uint64_t offset = 8 + table.size();
	for (uint32_t i = 0; i < count; i++) {
		RegionEntry e;
		e.local = table[i * 12] | table[i * 12 + 1] << 8;
		e.full_size = get_u32(&table[i * 12 + 4]);
		e.mips_size = get_u32(&table[i * 12 + 8]);
		e.full_offset = offset;
		e.mips_offset = offset + e.full_size;
		offset += (uint64_t)e.full_size + e.mips_size;
		r_entries.push_back(e);
	}
	return offset <= file->get_length();
}

const VoxelWorld::RegionIndex *VoxelWorld::cached_region_index(const Vector3i &p_region) const {
	auto it = region_index.find(p_region);
	if (it != region_index.end()) {
		return &it->second;
	}
	RegionIndex index;
	if (!region_files.count(p_region) || !read_region_index(region_path(p_region), index.entries, &index.version)) {
		return nullptr;
	}
	return &(region_index[p_region] = std::move(index));
}

void VoxelWorld::repair_doubled(Chunk &r_chunk) const {
	// A bug once loaded edited chunks reached past the world's wrap from the
	// wrong file, rebuilt them from mip 1 by repeating each of its points
	// twice along every axis (2-voxel terraces), and saved them so. Such a
	// chunk has every point equal to the even point at or below it; real
	// terrain never does (unless it's all one value). Interpolate those
	// points instead, and save it so.
	const int8_t *d = r_chunk.full.data();
	bool doubled = true, varied = false;
	for (int z = 0; z < CHUNK && doubled; z++) {
		for (int y = 0; y < CHUNK && doubled; y++) {
			for (int x = 0; x < CHUNK; x++) {
				const int8_t v = d[((size_t)z * CHUNK + y) * CHUNK + x];
				if (v != d[((size_t)(z & ~1) * CHUNK + (y & ~1)) * CHUNK + (x & ~1)]) {
					doubled = false;
					break;
				}
				varied |= v != d[0];
			}
		}
	}
	if (!doubled || !varied) {
		return;
	}
	std::vector<int8_t> even = r_chunk.full;
	upsample_half([&](int x, int y, int z) { return (float)even[((size_t)(2 * z) * CHUNK + 2 * y) * CHUNK + 2 * x]; }, r_chunk.full.data());
	r_chunk.dirty = true;
	repaired_chunks++;
}

bool VoxelWorld::read_full(const Vector3i &p_unwrapped_chunk, std::vector<int8_t> &r_full, std::vector<MaterialId> &r_materials) const {
	// Saved by the chunk's own coordinates, whichever repeat of the world it
	// was reached in.
	const Vector3i p_chunk = wrap_chunk(p_unwrapped_chunk);
	const Vector3i r = region_of(p_chunk);
	const Vector3i l = p_chunk - r * REGION;
	const int local = (l.z * REGION + l.y) * REGION + l.x;
	PackedByteArray compressed;
	int version = 0;
	{
		// The index and the read together: a save replaces the file and
		// forgets its index under the same lock.
		std::lock_guard<std::mutex> lock(region_lock);
		const RegionIndex *index = cached_region_index(r);
		if (index == nullptr) {
			return false;
		}
		for (const RegionEntry &e : index->entries) {
			if (e.local != local) {
				continue;
			}
			Ref<FileAccess> file = FileAccess::open(region_path(r), FileAccess::READ);
			if (file.is_null()) {
				return false;
			}
			file->seek(e.full_offset);
			compressed = file->get_buffer(e.full_size);
			version = index->version;
			break;
		}
	}
	if (version == 0) {
		return false;
	}
	const int64_t expected = (int64_t)chunk_bytes_size(CHUNK_POINTS, version);
	const PackedByteArray full = compressed.decompress(expected, COMPRESSION);
	if (full.size() != expected) {
		return false;
	}
	r_full.resize(CHUNK_POINTS);
	r_materials.resize(CHUNK_POINTS);
	std::memcpy(r_full.data(), full.ptr(), CHUNK_POINTS);
	if (version >= 2) {
		read_stored_materials(full.ptr(), CHUNK_POINTS, version, r_materials.data());
	} else {
		generate_materials(p_chunk, r_materials.data());
	}
	return true;
}

bool VoxelWorld::load_full(const Vector3i &p_unwrapped_chunk, Chunk &r_chunk) const {
	if (!read_full(p_unwrapped_chunk, r_chunk.full, r_chunk.full_materials)) {
		last_error = "Could not read chunk in " + region_path(region_of(wrap_chunk(p_unwrapped_chunk)));
		return false;
	}
	repair_doubled(r_chunk);
	return true;
}

void VoxelWorld::load_region(const Vector3i &p_region) {
	const String path = region_path(p_region);
	const RegionIndex *index = cached_region_index(p_region);
	Ref<FileAccess> file = FileAccess::open(path, FileAccess::READ);
	if (file.is_null() || index == nullptr) {
		last_error = "Could not read " + path;
		return;
	}
	const int64_t expected = (int64_t)chunk_bytes_size(MIP_BYTES, index->version);
	for (const RegionEntry &e : index->entries) {
		const Vector3i c = p_region * REGION + Vector3i(e.local % REGION, e.local / REGION % REGION, e.local / (REGION * REGION));
		Chunk *chunk = chunk_at(c);
		if (chunk == nullptr || wrap_chunk(c) != c) {
			last_error = "Chunk outside the world in " + path;
			continue;
		}
		if (!chunk->edited) {
			edited_list.push_back(c);
		}
		chunk->edited = true;
		chunk->dirty = false;
		chunk->loaded = false;
		chunk->mips.resize(MIP_BYTES);
		chunk->mip_materials.resize(MIP_BYTES);
		file->seek(e.mips_offset);
		const PackedByteArray mips = file->get_buffer(e.mips_size).decompress(expected, COMPRESSION);
		if (index->version == 4 && mips.size() == expected) {
			std::memcpy(chunk->mips.data(), mips.ptr(), MIP_BYTES);
			read_stored_materials(mips.ptr(), MIP_BYTES, index->version, chunk->mip_materials.data());
		} else {
			// Older or unreadable mips: from the points.
			load_chunk(c, *chunk);
			update_mips(c);
			chunk->full = std::vector<int8_t>();
			chunk->full_materials = std::vector<MaterialId>();
			chunk->loaded = false;
		}
	}
}

void VoxelWorld::queue_region_write(const Vector3i &p_region) {
	if (dir.is_empty()) {
		return;
	}
	RegionWrite job;
	job.region = p_region;
	job.path = region_path(p_region);
	for (int local = 0; local < REGION * REGION * REGION; local++) {
		const Vector3i c = p_region * REGION + Vector3i(local % REGION, local / REGION % REGION, local / (REGION * REGION));
		Chunk *chunk = chunk_at(c);
		if (chunk == nullptr || !chunk->edited || wrap_chunk(c) != c) {
			continue;
		}
		RegionWrite::Entry e;
		e.chunk = c;
		e.local = local;
		e.mips = chunk->mips;
		e.mip_materials = chunk->mip_materials;
		if (!chunk->loaded) {
			e.from_file = true; // never dirty: what's in the file is current
		} else if (chunk->uniform()) {
			e.full.assign(CHUNK_POINTS, chunk->uniform_density);
			e.full_materials.assign(CHUNK_POINTS, chunk->uniform_material);
		} else {
			e.full = chunk->full;
			e.full_materials = chunk->full_materials;
		}
		chunk->dirty = false; // dirty again if the write fails (poll_writes())
		job.entries.push_back(std::move(e));
	}
	if (job.entries.empty()) {
		return;
	}
	pending_writes[p_region]++;
	{
		std::lock_guard<std::mutex> lock(write_mutex);
		write_queue.push_back(std::move(job));
		if (!writer.joinable()) {
			writer_stopping = false;
			writer = std::thread([this]() { writer_loop(); });
		}
	}
	write_wake.notify_one();
}

void VoxelWorld::writer_loop() {
	std::unique_lock<std::mutex> lock(write_mutex);
	while (true) {
		write_wake.wait(lock, [&] { return writer_stopping || !write_queue.empty(); });
		if (write_queue.empty()) {
			return; // stopping, and nothing left
		}
		RegionWrite job = std::move(write_queue.front());
		write_queue.pop_front();
		writer_busy = true;
		lock.unlock();
		const bool ok = write_region_now(job);
		lock.lock();
		writer_busy = false;
		if (!ok) {
			failed_writes.push_back({ job.region, "Could not write " + job.path });
		}
		finished_writes.push_back(job.region);
		write_done.notify_all();
	}
}

bool VoxelWorld::write_region_now(RegionWrite &r_write) {
	PackedByteArray table, blobs;
	uint32_t count = 0;
	for (RegionWrite::Entry &e : r_write.entries) {
		if (e.from_file && !read_full(e.chunk, e.full, e.full_materials)) {
			return false; // rather than drop the chunk from the region
		}
		const PackedByteArray full = chunk_bytes(e.full.data(), e.full_materials.data(), CHUNK_POINTS).compress(COMPRESSION);
		const PackedByteArray mips = chunk_bytes(e.mips.data(), e.mip_materials.data(), MIP_BYTES).compress(COMPRESSION);
		table.push_back((uint8_t)(e.local & 0xFF));
		table.push_back((uint8_t)(e.local >> 8));
		table.push_back(0);
		table.push_back(0);
		put_u32(table, (uint32_t)full.size());
		put_u32(table, (uint32_t)mips.size());
		blobs.append_array(full);
		blobs.append_array(mips);
		count++;
	}
	PackedByteArray out;
	put_u32(out, REGION_MAGIC);
	put_u32(out, count);
	out.append_array(table);
	out.append_array(blobs);
	// Write a temporary file, then rename it over the old one: a crash leaves
	// either the old region or the new one, never half of each. Under
	// region_lock: a background read mustn't use the old index on the new file.
	std::lock_guard<std::mutex> lock(region_lock);
	const String tmp = r_write.path + String(".tmp");
	{
		Ref<FileAccess> file = FileAccess::open(tmp, FileAccess::WRITE);
		if (file.is_null() || !file->store_buffer(out)) {
			return false;
		}
		file->flush();
	}
	if (DirAccess::rename_absolute(tmp, r_write.path) != OK) {
		return false;
	}
	region_files.insert(r_write.region);
	region_index.erase(r_write.region); // the offsets changed
	return true;
}

bool VoxelWorld::poll_writes() {
	std::vector<Vector3i> finished;
	std::vector<std::pair<Vector3i, String>> failed;
	{
		std::lock_guard<std::mutex> lock(write_mutex);
		finished.swap(finished_writes);
		failed.swap(failed_writes);
	}
	for (const Vector3i &r : finished) {
		auto it = pending_writes.find(r);
		if (it != pending_writes.end() && --it->second <= 0) {
			pending_writes.erase(it);
		}
	}
	for (const auto &[region, error] : failed) {
		last_error = error;
		// Its chunks get written again next time.
		for (int local = 0; local < REGION * REGION * REGION; local++) {
			Chunk *chunk = chunk_at(region * REGION + Vector3i(local % REGION, local / REGION % REGION, local / (REGION * REGION)));
			if (chunk != nullptr && chunk->edited) {
				chunk->dirty = true;
			}
		}
	}
	return failed.empty();
}

void VoxelWorld::flush_writes() {
	if (writer.joinable()) {
		std::unique_lock<std::mutex> lock(write_mutex);
		write_done.wait(lock, [&] { return write_queue.empty() && !writer_busy; });
	}
}

void VoxelWorld::stop_writer() {
	if (!writer.joinable()) {
		return;
	}
	{
		std::lock_guard<std::mutex> lock(write_mutex);
		writer_stopping = true;
	}
	write_wake.notify_all();
	writer.join(); // after writing what's queued
	poll_writes();
}

bool VoxelWorld::save(bool p_wait) {
	// Only edited chunks are ever dirty: going through those, not every
	// chunk of the world (4 million by default: a 35 ms hitch each autosave).
	std::unordered_set<Vector3i, Vector3iHash> dirty_regions;
	for (const Vector3i &c : edited_list) {
		const Chunk *chunk = chunk_at(c);
		if (chunk != nullptr && chunk->dirty) {
			dirty_regions.insert(region_of(c));
		}
	}
	for (const Vector3i &r : dirty_regions) {
		queue_region_write(r);
	}
	if (!p_wait) {
		return poll_writes();
	}
	flush_writes();
	return poll_writes();
}

void VoxelWorld::set_change_tracking(bool p_on) {
	track_changes = p_on;
	if (!p_on) {
		changes.clear();
	}
}

void VoxelWorld::take_changes(std::vector<ChangedChunk> &r_changes) {
	r_changes.clear();
	r_changes.reserve(changes.size());
	for (auto &[key, before] : changes) {
		r_changes.push_back(std::move(before));
	}
	changes.clear();
}

void VoxelWorld::edited_chunk_list(std::vector<Vector3i> &r_chunks) const {
	r_chunks = edited_list;
}

bool VoxelWorld::read_chunk(const Vector3i &p_chunk, std::vector<int8_t> &r_density, std::vector<MaterialId> &r_materials) const {
	const Chunk *chunk = loaded_chunk(p_chunk);
	if (chunk == nullptr) {
		return false;
	}
	r_density.resize(CHUNK_POINTS);
	r_materials.resize(CHUNK_POINTS);
	for (size_t i = 0; i < (size_t)CHUNK_POINTS; i++) {
		r_density[i] = chunk_density(*chunk, i);
		r_materials[i] = chunk_material(*chunk, i);
	}
	return true;
}

int VoxelWorld::get_edited_chunk_count() const {
	return (int)edited_list.size();
}

int VoxelWorld::get_full_chunk_count() const {
	int count = 0;
	for (const Chunk &chunk : chunks) {
		count += chunk.loaded && !chunk.uniform();
	}
	return count;
}

int VoxelWorld::get_loaded_chunk_count() const {
	int count = 0;
	for (const Chunk &chunk : chunks) {
		count += chunk.loaded;
	}
	return count;
}

int VoxelWorld::get_dirty_chunk_count() const {
	int count = 0;
	for (const Vector3i &c : edited_list) {
		const Chunk *chunk = chunk_at(c);
		count += chunk != nullptr && chunk->dirty;
	}
	return count;
}

int64_t VoxelWorld::get_memory_bytes() const {
	int64_t bytes = (int64_t)chunks.capacity() * sizeof(Chunk);
	for (const Chunk &chunk : chunks) {
		bytes += (int64_t)chunk.full.capacity() + (int64_t)chunk.full_materials.capacity() * sizeof(MaterialId);
		bytes += (int64_t)chunk.mips.capacity() + (int64_t)chunk.mip_materials.capacity() * sizeof(MaterialId);
	}
	return bytes;
}
