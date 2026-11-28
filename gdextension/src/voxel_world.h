#pragma once

#include "noise.h"

#include <godot_cpp/variant/basis.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector3.hpp>
#include <godot_cpp/variant/vector3i.hpp>

#include <cmath>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace godot {

// Densities are stored as one signed byte per lattice point, in 1/16-voxel
// steps: roughly the signed distance to the surface (positive = solid),
// clamped to +-127/16 voxels. Values farther from the surface don't change
// how it looks.
constexpr float DENSITY_STEP = 1.0f / 16.0f;
constexpr int DENSITY_LIMIT = 127;
constexpr float DENSITY_CLAMP = DENSITY_LIMIT * DENSITY_STEP;

inline int8_t quantize_density(float p_voxels) {
	const float q = std::nearbyint(p_voxels * 16.0f);
	return (int8_t)(q > DENSITY_LIMIT ? DENSITY_LIMIT : (q < -DENSITY_LIMIT ? -DENSITY_LIMIT : q));
}

// Each point also has a material: a 16-bit id (MaterialId), so mods can add
// as many as they like. A solid point's material is what it is made of; an
// air point's is the material of the surface it faces, so a lookup near the
// surface finds the right one from either side.
//
// Ids below MATERIAL_COUNT are the built-in blocks, whose textures are
// res://assets/block/<name>.png (MATERIAL_NAMES). Ids with the top bit set
// (MATERIAL_COLOR_BIT) are plain colors instead: the other 15 bits are red,
// green and blue, 5 bits each (red highest), see color_material(). The GPU
// gets two bytes per point (render_bytes()).
using MaterialId = uint16_t;
enum BlockMaterial : MaterialId {
	MAT_GRASS,
	MAT_DIRT,
	MAT_STONE,
	MAT_SAND,
	MAT_WOOD,
	MAT_COBBLESTONE,
	MAT_CLOTH,
	MAT_SPONGE,
	MATERIAL_COUNT,
};
extern const char *const MATERIAL_NAMES[MATERIAL_COUNT];

constexpr MaterialId MATERIAL_COLOR_BIT = 0x8000;
inline bool is_color_material(MaterialId p_material) {
	return (p_material & MATERIAL_COLOR_BIT) != 0;
}
// A color material from 5-bit channels (0..31 each).
inline MaterialId color_material(int p_red, int p_green, int p_blue) {
	return (MaterialId)(MATERIAL_COLOR_BIT | (p_red & 31) << 10 | (p_green & 31) << 5 | (p_blue & 31));
}

// The texture layer the GPU gets for a block (unknown ids show as stone for
// now). Under 128: the GPU's byte keeps its top bit for colors.
inline uint8_t render_layer(MaterialId p_material) {
	return p_material < MATERIAL_COUNT && p_material < 128 ? (uint8_t)p_material : (uint8_t)MAT_STONE;
}

// What the GPU gets for a point's material, two bytes: a block's texture
// layer and 0, or for a color 0x80 | its top 7 bits and its low 8 bits (the
// 15 color bits, red highest).
inline void render_bytes(MaterialId p_material, uint8_t *r_bytes) {
	if (is_color_material(p_material)) {
		r_bytes[0] = (uint8_t)(0x80 | ((p_material >> 8) & 0x7F));
		r_bytes[1] = (uint8_t)(p_material & 0xFF);
	} else {
		r_bytes[0] = render_layer(p_material);
		r_bytes[1] = 0;
	}
}

struct Vector3iHash {
	size_t operator()(const Vector3i &v) const {
		return ((size_t)(uint32_t)v.x * 73856093u) ^ ((size_t)(uint32_t)v.y * 19349663u) ^ ((size_t)(uint32_t)v.z * 83492791u);
	}
};

enum WorldType {
	WORLD_ISLANDS, // floating islands in every direction
	WORLD_TERRAIN, // Minecraft-like ground: hills, mountains, caves
};

// Settings of the generated (unedited) world. Saved with the world: edited
// chunks are stored whole, so the generator must give the same values next
// time or unedited ground wouldn't meet them.
struct IslandSettings {
	int world_type = WORLD_ISLANDS;
	int64_t seed = 1;
	int world_size = 8192; // x and z wrap around every this many voxels (a power of 2, >= 256)
	int world_height = 2048; // voxels from the bottom limit to the top (a multiple of 32), centred on 0
	int cell_size = 256; // space is split into cubes this big, each with at most one island (divides world_size)
	float island_chance = 0.5f; // of a cell having an island
	float min_radius = 20.0f; // island radius range, voxels
	float max_radius = 100.0f;
	float detail = 5.0f; // how far the rocky noise moves the surface, voxels
	float detail_frequency = 0.05f; // of the largest rocky features, per voxel
	int octaves = 3;
	// WORLD_TERRAIN (ground around height 0):
	float terrain_height = 80.0f; // how far hills and mountains rise and dip (kept within the world's height)
	float terrain_scale = 200.0f; // width of the main hills, voxels (smaller = steeper)
	bool caves = true;
	float sea_level = 0.0f; // the static water surface's height (terrain worlds)
	// Terrain generator revision, saved with the world so an older world
	// keeps generating exactly as it did. 1: original; 2: deep sea basins.
	int terrain_version = 2;
};

// World features (trees, mushrooms...): small shapes the generator puts on
// the ground, as much a part of the generated world as its noise (so not
// saved unless their chunk is edited). The game's scripts define them
// (SculptTerrain.set_features()); where each goes follows from the seed.
struct FeatureShape {
	Vector3i size;
	Vector3 origin; // of the first grid point, in the shape's space (its foot at 0)
	float voxel_size = 1.0f;
	std::vector<float> density; // in the grid's voxels, positive inside (x fastest, then y, z)
	std::vector<MaterialId> materials;
	float reach = 0.0f; // farthest grid corner from the foot
	float reach_xz = 0.0f; // and sideways (in x and z)
};
struct FeatureDef {
	uint64_t id_hash = 0; // seeds its placements
	std::vector<int> shapes; // into the world's feature shapes: one is picked per placement
	float per_region = 1.0f; // placements tried per 64 x 64 voxels of ground, on average
	std::vector<MaterialId> grows_on; // empty: any ground
	float min_up = 0.0f; // cos of the steepest slope it grows on
	float sink = 1.0f; // voxels into the ground (times its size)
	float follow_slope = 0.0f; // 0 upright .. 1 square to the ground
	bool random_turn = true;
	float scale_min = 1.0f, scale_max = 1.0f;
	float spacing = 0.0f; // from other features of its region
	uint32_t world_types = ~0u; // bit per WorldType
};

// A world of fixed size that wraps around in x and z (a torus: walking
// world_size voxels east brings you back) and stops at hard limits in y
// (world_height voxels, centred on 0). Coordinates outside it are fine
// everywhere: x and z are wrapped, and beyond the y limits there is nothing
// (Terrain: solid rock below the bottom, open air above the top; Islands:
// open air both ways).
//
// The whole world is always in memory, as 32^3 chunks, so it can all be
// simulated at once. A chunk that is the same everywhere (all air or all
// rock, one material: most of them) is kept as that one value; the rest
// hold their points (1 byte density + 2 bytes material each). open()
// generates every chunk (in parallel), then applies the edited ones from
// the save; only edited chunks are saved, in region files of 8^3 chunks,
// written to a temporary file and renamed over the old one (so a crash mid-save
// leaves the previous version intact).
//
// Generators: see generate(). Both tile across the wrap (periodic noise;
// island cells that divide world_size).
//
// Edited chunks also keep point-sampled reduced copies ("mips" 1..5: every
// 2nd, 4th ... 32nd point) for the clipmap's coarse levels; unedited ones
// are generated for those directly, with distances beyond what a stored
// byte can hold.
class VoxelWorld {
public:
	static constexpr int CHUNK = 32;
	static constexpr int REGION = 8; // chunks per region side
	static constexpr int MIP_LEVELS = 5; // mips 1..5 (16^3 .. 1^3)
	static constexpr int CHUNK_POINTS = CHUNK * CHUNK * CHUNK;
	static constexpr int MIP_BYTES = 16 * 16 * 16 + 8 * 8 * 8 + 4 * 4 * 4 + 2 * 2 * 2 + 1;

	// Sets the generator; open() may replace it with the saved one.
	void configure(const IslandSettings &p_settings);
	const IslandSettings &get_settings() const { return settings; }
	// Generates the whole world, then opens the edits saved in directory
	// p_dir (creating it if new; empty = don't save). Returns false if the
	// save can't be used (the world is still generated, unsaved).
	bool open(const String &p_dir, int p_threads);
	// For a world opened without a directory (multiplayer: a joined player's
	// copy): starts saving it in p_dir, emptied first, every edited chunk to
	// be written by the next save(). False if this world already saves
	// somewhere, or p_dir can't be made.
	bool adopt_dir(const String &p_dir);
	void close();
	~VoxelWorld();

	// The world's extent: x and z wrap every world_size; y runs from
	// y_min() to y_max() - 1.
	int world_size() const { return settings.world_size; }
	int y_min() const { return -settings.world_height / 2; }
	int y_max() const { return settings.world_height / 2; }
	int wrap_xz(int p_value) const { return p_value & (settings.world_size - 1); }

	// Generated signed distance at a point, in voxels (positive = solid), with
	// p_octaves of noise. Exact only within p_accuracy voxels of the surface;
	// farther out it is a lower bound on the distance. Thread-safe.
	// p_spacing: how far apart the points being generated are (a clipmap
	// level's texel size). Islands too small to keep a solid sample at that
	// spacing are fattened just enough to keep one, so far-away small
	// islands don't vanish from coarse levels; 1 = the true shape. Features
	// far smaller than the spacing (caves) may be left out.
	// r_material (optional): the point's material. With a coarse spacing,
	// points within about a spacing of the surface get the surface's
	// material, so far-away terrain shows what's on top.
	float generate(int x, int y, int z, int p_octaves, float p_accuracy, int p_spacing = 1, MaterialId *r_material = nullptr) const;
	// The generated ground alone, without features (as generate() was).
	float generate_ground(int x, int y, int z, int p_octaves, float p_accuracy, int p_spacing = 1, MaterialId *r_material = nullptr) const;
	// Replaces the features (call before generating anything: open()).
	void set_features(std::vector<FeatureShape> &&p_shapes, std::vector<FeatureDef> &&p_defs);
	bool has_features() const { return !feature_defs.empty(); }
	// Noise octaves worth computing when points are p_scale voxels apart
	// (finer octaves would be smaller than the spacing).
	int octaves_for_scale(int p_scale) const;
	// Somewhere to stand: the centre of the top of the island nearest p_near
	// within a few cells (islands), or the ground surface straight above or
	// below p_near (terrain). False if there is none.
	bool find_island(const Vector3 &p_near, Vector3 &r_top) const;

	// A clipmap texel: the density at a point (a multiple of 2^p_level) as
	// level p_level stores it, in units of 1/16 of ITS texel (2^p_level
	// voxels), clamped to +-127, and its material. For level 0 that is
	// exactly the stored byte; coarser levels can hold larger distances.
	// Thread-safe while nothing is being edited.
	int8_t lod_texel(int x, int y, int z, int p_level, MaterialId &r_material) const;
	// Encodes a generated distance for level p_level (see lod_texel).
	static int8_t encode_texel(float p_voxels, int p_level);
	// Nothing to load any more (everything is in memory); kept for callers.
	void prepare(const Vector3i &, const Vector3i &, int) {}

	// Chunks (in the caller's unwrapped coordinates, every repeat) with points
	// in [p_min, p_max] that hold edits.
	void edited_chunks_in(const Vector3i &p_min, const Vector3i &p_max, std::vector<Vector3i> &r_chunks) const;
	// Every chunk that holds edits (wrapped coordinates), for sending the
	// world to another player.
	void edited_chunk_list(std::vector<Vector3i> &r_chunks) const;
	// A chunk's points (CHUNK_POINTS each, x fastest, then y, z), loading it
	// if needed. False outside the world.
	bool read_chunk(const Vector3i &p_chunk, std::vector<int8_t> &r_density, std::vector<MaterialId> &r_materials) const;

	// Change tracking (multiplayer): while on, the first edit_chunk() of a
	// chunk since the last take_changes() keeps a copy of its points as they
	// were, so the changes can be worked out and sent. Suppressed while
	// applying changes that came from elsewhere.
	struct ChangedChunk {
		Vector3i chunk; // wrapped
		std::vector<int8_t> density; // before the changes
		std::vector<MaterialId> materials;
	};
	void set_change_tracking(bool p_on);
	bool is_change_tracking() const { return track_changes; }
	void set_changes_suppressed(bool p_on) { suppress_changes = p_on; }
	void take_changes(std::vector<ChangedChunk> &r_changes);

	// Full-detail density / material at a point.
	int8_t point(int x, int y, int z) const;
	MaterialId point_material(int x, int y, int z) const;
	// Trilinear density at a position, in voxels.
	float sample(const Vector3 &p_position) const;
	// Full-detail densities (voxels) of points [p_min, p_max], x fastest, then
	// y, then z, into r_out, and their materials into r_materials if given.
	void read_points(const Vector3i &p_min, const Vector3i &p_max, float *r_out, int p_threads, MaterialId *r_materials = nullptr) const;
	// A chunk's stored data as level p_level of the clipmap reads it: full
	// data for level 0 (any chunk that holds its points), mip min(level, 5)
	// above (edited chunks only; the rest are generated); side = points per
	// axis. False otherwise.
	bool edited_data(const Vector3i &p_chunk, int p_level, const int8_t *&r_data, const MaterialId *&r_materials, int &r_side) const;
	// An edited chunk whose points aren't in memory (only in the save), with
	// its mip 1 (16^3) to fall back on: level 0 reads it with read_saved()
	// on its worker instead of loading it here.
	bool edited_unloaded(const Vector3i &p_chunk, const int8_t *&r_mip1, const MaterialId *&r_mip1_materials) const;
	// A chunk's points from the save; safe on another thread (as
	// load_near()'s batch reads them). An unloaded chunk's save is current
	// (see unload_far()).
	bool read_saved(const Vector3i &p_chunk, std::vector<int8_t> &r_full, std::vector<MaterialId> &r_materials) const {
		return read_full(p_chunk, r_full, r_materials);
	}

	// The points of a chunk, ready to edit (expanded if it was uniform), and
	// marked edited and unsaved; nullptr data outside the y limits. Call
	// update_mips() when done.
	struct ChunkData {
		int8_t *density;
		MaterialId *material;
	};
	ChunkData edit_chunk(const Vector3i &p_chunk);
	// Refreshes an edited chunk's mips (and folds it back to one value if it
	// became uniform).
	void update_mips(const Vector3i &p_chunk);
	// The chunk a (possibly unwrapped) chunk coordinate refers to.
	Vector3i wrap_chunk(const Vector3i &p_chunk) const;
	bool chunk_in_world(const Vector3i &p_chunk) const;
	bool chunk_edited(const Vector3i &p_chunk) const {
		const Chunk *chunk = chunk_at(p_chunk);
		return chunk != nullptr && chunk->edited;
	}

	// Writes every changed region. p_wait: and waits until every write
	// (also earlier queued ones) is on disk, returning false if any failed;
	// otherwise the writes go on in the background (true; a failure shows
	// up in last_error and its chunks are saved again next time).
	bool save(bool p_wait = true);
	// Loads the unloaded chunks within p_radius (Chebyshev, the short way
	// round) of p_center, nearest first: edited ones (from the save) here
	// for at most about p_budget_ms, generated ones on a background thread,
	// a batch at a time, taken in on a later call. Returns how many it took in.
	int load_near(const Vector3 &p_center, float p_radius, double p_budget_ms);
	// Lets go of the points of chunks farther than p_radius (saving edits
	// first).
	void unload_far(const Vector3 &p_center, float p_radius);
	int get_loaded_chunk_count() const;
	int get_repaired_chunk_count() const { return repaired_chunks; }

	int get_edited_chunk_count() const;
	int get_full_chunk_count() const; // chunks holding their points
	int64_t get_memory_bytes() const;
	int get_region_file_count() const { return (int)region_files.size(); }
	int get_dirty_chunk_count() const;
	String get_last_error() const { return last_error; }

	static Vector3i chunk_of(int x, int y, int z);
	static Vector3i region_of(const Vector3i &p_chunk);

private:
	struct Chunk {
		// Uniform: every point is uniform_density / uniform_material, and
		// full is empty.
		int8_t uniform_density = -DENSITY_LIMIT;
		MaterialId uniform_material = MAT_STONE;
		std::vector<int8_t> full; // CHUNK_POINTS, or empty (uniform)
		std::vector<MaterialId> full_materials; // likewise
		std::vector<int8_t> mips; // MIP_BYTES, edited chunks only
		std::vector<MaterialId> mip_materials;
		bool edited = false; // differs from the generator (saved)
		bool dirty = false; // changed since last saved
		bool loaded = false; // its points are in memory (else see load_chunk())
		bool uniform() const { return full.empty(); }
	};
	struct RegionEntry {
		int local = 0; // chunk index within the region
		uint32_t full_size = 0, mips_size = 0; // compressed bytes
		uint64_t full_offset = 0, mips_offset = 0;
	};

	struct Island {
		Vector3 centre; // middle of the top surface
		float radius = 0; // of the top
		float depth = 0; // from the top down to the tip
	};

	IslandSettings settings;
	uint32_t settings_version = 0; // changes with every configure() and set_features() (see generate's cache)

	// --- Features (see FeatureDef) ---
	// Terrain worlds plan them per FEATURE_REGION x FEATURE_REGION column of
	// ground, island worlds per island (its cell). Coarser clipmap levels
	// than FEATURE_MAX_SPACING leave them out.
	static constexpr int FEATURE_REGION = 64;
	static constexpr int FEATURE_MAX_SPACING = 8;
	struct FeatureInstance {
		int shape = 0;
		Vector3 origin; // the shape's foot, in the world (its region's wrapped frame)
		Basis to_local; // world offsets to the shape's space (turn and size undone)
		float scale = 1.0f;
		Vector3 lo, hi; // its grid's box in the world
	};
	struct FeatureRegion {
		std::vector<FeatureInstance> instances;
		Vector3 lo = Vector3(1e30f, 1e30f, 1e30f), hi = Vector3(-1e30f, -1e30f, -1e30f);
	};
	std::vector<FeatureShape> feature_shapes;
	std::vector<FeatureDef> feature_defs;
	float feature_reach = 0.0f; // farthest any instance's box reaches from where it stands
	float feature_reach_xz = 0.0f; // and sideways
	mutable std::shared_mutex feature_lock;
	mutable std::unordered_map<Vector3i, std::unique_ptr<FeatureRegion>, Vector3iHash> feature_regions;
	// A region's features, planned the first time it's asked for (any thread).
	const FeatureRegion &feature_region(const Vector3i &p_key) const;
	void plan_features(const Vector3i &p_key, FeatureRegion &r_region) const;
	// Where the ground is at a column (x, z wrapped), the highest within
	// [p_bottom, p_top]: its point, normal and material. False if none.
	bool find_ground(float p_x, float p_z, float p_top, float p_bottom, Vector3 &r_point, Vector3 &r_normal, MaterialId &r_material) const;
	// Calls p_visit(instance, shift) for each feature whose box meets [p_lo,
	// p_hi] (world coordinates; shift: what to take off a point there to
	// put it in the instance's frame).
	template <typename F>
	void for_each_feature(const Vector3 &p_lo, const Vector3 &p_hi, F p_visit) const;
	// An instance's density at p (in its frame), in world voxels, and its material there.
	float feature_value(const FeatureInstance &p_instance, const Vector3 &p, MaterialId *r_material) const;
	// p_density (the ground's at a point) with the features unioned in.
	float add_features(int x, int y, int z, float p_density, float p_accuracy, MaterialId *r_material) const;
	// Whether any feature comes within p_margin of a chunk.
	bool features_touch_chunk(const Vector3i &p_chunk, float p_margin) const;
	SimplexNoise noise;
	PerlinNoise perlin;
	PerlinNoise perlin_caves; // a second, independent field (caves need two)

	float generate_islands(int x, int y, int z, int p_octaves, float p_accuracy, int p_spacing, MaterialId *r_material) const;
	float generate_terrain(int x, int y, int z, float p_accuracy, int p_spacing, MaterialId *r_material) const;
	// Terrain: ground height at a column, and how steep it is there.
	void terrain_height(int x, int z, float &r_height, float &r_slope) const;
	float terrain_base_height(float p_x, float p_z) const; // (no caching)
	// generate_terrain() for a column whose height and slope are known.
	float generate_terrain_at(int x, int y, int z, float p_height, float p_slope, float p_accuracy, int p_spacing, MaterialId *r_material) const;

	// Highest / lowest the ground can be.
	float terrain_top() const;
	float terrain_bottom() const;
	// Periodic Perlin noise for the terrain: features about p_wavelength
	// voxels across, fitted to a whole number of repeats across the world.
	float tiled_fbm(float p_x, float p_z, float p_wavelength, int p_octaves, float p_offset) const;
	float tiled_noise(const PerlinNoise &p_noise, float p_x, float p_y, float p_z, float p_wavelength, float p_y_scale) const;
	float margin = 0; // every island stays this far inside its cell

	bool island_in_cell(const Vector3i &p_cell, Island &r_island) const;
	String dir; // absolute; empty = not saving

	mutable std::vector<Chunk> chunks; // every chunk of the world (chunk_index()); loaded on demand
	bool track_changes = false;
	bool suppress_changes = false;
	std::unordered_map<Vector3i, ChangedChunk, Vector3iHash> changes; // see take_changes()
	struct RegionIndex {
		int version = 0;
		std::vector<RegionEntry> entries;
	};
	mutable std::unordered_map<Vector3i, RegionIndex, Vector3iHash> region_index; // read region headers
	// Held for region_index, region_files and the region files themselves
	// while load_near()'s background batch may read them (see read_full()).
	mutable std::mutex region_lock;
	// The caller holds region_lock (or no batch can be running: open()).
	const RegionIndex *cached_region_index(const Vector3i &p_region) const;
	// Terrain: a chunk column's ground heights (see column_info()).
	struct ColumnInfo {
		std::vector<float> heights, slopes; // 32 x 32, z then x
		float low = 0.0f, high = 0.0f, steepest = 0.0f; // including one voxel beyond
	};
	mutable std::unordered_map<int, ColumnInfo> column_cache;
	void column_info(int p_cx, int p_cz, ColumnInfo &r_info) const;
	const ColumnInfo &cached_column_info(int p_cx, int p_cz) const;
	void generate_terrain_chunk(const Vector3i &p_chunk, const ColumnInfo &p_info, Chunk &r_chunk) const;
	int8_t terrain_point(int x, int y, int z, float p_height, float p_slope, MaterialId *r_material) const;
	float terrain_value(int x, int y, int z, float p_height, float p_slope, MaterialId *r_material) const; // (not quantized)
	// A point as generated, quantized as stored (far air's material folded).
	int8_t generated_point(int x, int y, int z, MaterialId *r_material) const;
	// Fills a chunk's points: from the save if edited, else generated.
	void load_chunk(const Vector3i &p_chunk, Chunk &r_chunk) const;
	bool load_full(const Vector3i &p_chunk, Chunk &r_chunk) const;
	// Any thread: an edited chunk's saved points (taking region_lock), and
	// nothing else: no repair, no last_error. False if unreadable.
	bool read_full(const Vector3i &p_unwrapped_chunk, std::vector<int8_t> &r_full, std::vector<MaterialId> &r_materials) const;
	// Undoes an old bug's damage to a loaded chunk, if it shows (see .cpp).
	void repair_doubled(Chunk &r_chunk) const;
	mutable int repaired_chunks = 0;
	// The chunk, loaded (nullptr outside the y limits).
	const Chunk *loaded_chunk(const Vector3i &p_chunk) const;
	// The chunks (wrapped) whose points are in memory, so unload_far() looks
	// at those rather than every chunk of the world (millions). May hold
	// some no longer loaded; unload_far() drops them. Loading can happen on
	// the clipmap's worker threads, hence the lock.
	mutable std::unordered_set<Vector3i, Vector3iHash> loaded_set;
	// Every edited chunk (wrapped), in the order they became edited (a chunk
	// never stops being edited), so finding the edited ones in a box doesn't
	// mean going through every chunk of the world: a clipmap level that
	// moved did that on the main thread, 4 million chunks for the coarsest
	// level, a 40-90 ms hitch every few steps while walking.
	std::vector<Vector3i> edited_list;
	mutable std::mutex loaded_lock;
	void note_loaded(const Vector3i &p_chunk) const;
	// load_near()'s chunks being generated in the background: the worker
	// only fills made (from the settings and its own copies of the columns'
	// info); they're taken in on the main thread once it has finished.
	struct LoadBatch {
		std::vector<Vector3i> chunks;
		std::vector<uint8_t> edited; // per chunk, when the batch started: read from the save
		std::vector<uint8_t> read_ok; // per edited chunk: read_full() worked
		std::vector<Vector3i> columns; // the distinct (wrapped) columns, terrain worlds
		std::vector<int> column_of; // per chunk: its index in columns
		std::vector<ColumnInfo> infos; // per column, worked out by the worker
		std::vector<Chunk> made;
		std::thread thread;
		std::atomic<bool> finished{ false };
	};
	std::unique_ptr<LoadBatch> load_batch;
	static constexpr int LOAD_BATCH = 16;
	void wait_load_batch(); // joins it, throwing it away
	Vector3i chunk_counts; // chunks per axis
	mutable String last_error;
	std::unordered_set<Vector3i, Vector3iHash> region_files; // regions with a file on disk

	size_t chunk_index(const Vector3i &p_wrapped) const {
		return ((size_t)p_wrapped.y * chunk_counts.z + p_wrapped.z) * chunk_counts.x + p_wrapped.x;
	}
	const Chunk *chunk_at(const Vector3i &p_chunk) const; // nullptr outside the y limits
	Chunk *chunk_at(const Vector3i &p_chunk);
	// Fills a chunk from the generator (folding it to one value if uniform).
	// Chunks that are plainly all air or all rock (see uniform_chunk()) are
	// set without generating their points.
	void generate_chunk(const Vector3i &p_chunk, Chunk &r_chunk) const;
	bool uniform_chunk(const Vector3i &p_chunk, int8_t &r_density, MaterialId &r_material) const;
	// Stores a chunk's generated points (or the one value, if all alike).
	static void store_chunk(const std::vector<int8_t> &p_density, std::vector<MaterialId> &r_materials, Chunk &r_chunk);
	// A point of a chunk (uniform or not).
	static int8_t chunk_density(const Chunk &p_chunk, size_t p_index) {
		return p_chunk.uniform() ? p_chunk.uniform_density : p_chunk.full[p_index];
	}
	static MaterialId chunk_material(const Chunk &p_chunk, size_t p_index) {
		return p_chunk.uniform() ? p_chunk.uniform_material : p_chunk.full_materials[p_index];
	}

	static int mip_offset(int p_mip);
	String region_path(const Vector3i &p_region) const;
	// Marks a region's chunks edited and reads their mips.
	void load_region(const Vector3i &p_region);
	// r_version: 1 = densities only (materials come from the generator), 2 =
	// with 4-bit materials, 3 = with 16-bit materials (including the former
	// water ones), 4 = current 16-bit materials.
	bool read_region_index(const String &p_path, std::vector<RegionEntry> &r_entries, int *r_version = nullptr) const;
	// For chunks saved without materials: what the generator gives there.
	void generate_materials(const Vector3i &p_chunk, MaterialId *r_full) const;
	// Region writes, on a background thread (a region's chunks compressed
	// and written took up to ~200 ms on the main thread). queue_region_write()
	// copies what the region needs on the main thread and marks its chunks
	// saved; the writer reads edited chunks that aren't in memory from the
	// old file, compresses and replaces it under region_lock.
	struct RegionWrite {
		struct Entry {
			Vector3i chunk;
			int local = 0;
			bool from_file = false; // not in memory: read from the old file
			std::vector<int8_t> full;
			std::vector<MaterialId> full_materials;
			std::vector<int8_t> mips;
			std::vector<MaterialId> mip_materials;
		};
		Vector3i region;
		String path;
		std::vector<Entry> entries;
	};
	void queue_region_write(const Vector3i &p_region);
	void writer_loop();
	bool write_region_now(RegionWrite &r_write); // writer thread; false if it failed
	// Takes in finished writes (main thread): failures make their regions'
	// chunks dirty again. Returns false if any failed.
	bool poll_writes();
	void flush_writes(); // waits until the queue is empty (main thread)
	void stop_writer();
	std::mutex write_mutex;
	std::condition_variable write_wake, write_done;
	std::deque<RegionWrite> write_queue;
	bool writer_busy = false, writer_stopping = false;
	std::thread writer;
	std::vector<std::pair<Vector3i, String>> failed_writes; // (region, error), under write_mutex
	// Main thread: queued, not yet taken in by poll_writes() (their chunks
	// stay in memory until then).
	std::unordered_map<Vector3i, int, Vector3iHash> pending_writes;
	std::vector<Vector3i> finished_writes; // under write_mutex
	bool write_settings();
	bool read_settings();
};

} // namespace godot
