#pragma once

#include "voxel_world.h"

#include <godot_cpp/variant/vector3.hpp>
#include <godot_cpp/variant/vector3i.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace godot {

// The copy of the density grid the raymarching shader reads: a few cubes of
// size^3 texels ("levels") centred on the camera. Level l has one texel every
// 2^l voxels, so each level covers twice the distance of the one before at half
// the detail, and the shader uses the finest level that reaches a point.
//
// Storage wraps around (toroidal): world texel w (= position / scale) lives at
// w mod size on each axis, and the shader samples with repeat wrapping. When a
// level moves, texels still inside it stay where they are and only the newly
// covered slabs are computed, on a worker thread.
//
// Each level is uploaded as an Image::FORMAT_RGBA8 2D texture array with one
// layer per z slice (storage z = world texel z mod size), x fastest, then y:
// layers that changed can be uploaded on their own, a few per frame. The
// shader interpolates between layers itself: a GPU texel holds the density
// of its slice (R) and of the next one up (G), so one read gives both, then
// the material (B, A). Here a texel is three bytes: R, the
// density at its point in 1/16 of the level's texel size
// (VoxelWorld::lod_texel) + 128, so coarse levels hold larger distances; G
// and B, its material (render_bytes(): a texture layer, or a color).
//
// Separately, per BRICK^3 block of texels: how many bricks away the nearest
// brick containing surface (a texel within DENSITY_CLAMP of it) is, Chebyshev,
// up to MAX_BRICK_DISTANCE. The shader turns that into a distance rays can
// safely jump through open air or rock. These are small (size / BRICK cubed)
// and all levels share one texture.
class Clipmap {
public:
	static constexpr int BRICK = 8;
	// A level moves once the camera is this many texels from its centre. A
	// multiple of BRICK, so bricks line up across moves.
	static constexpr int RECENTER = 16;
	// Brick distances stop counting here (it means "this far or more"), so a
	// change to one brick only affects distances within this many bricks.
	static constexpr int MAX_BRICK_DISTANCE = 8;
	static constexpr int TEXEL_BYTES = 3;

	// The shader uses level l within this many texels of the camera (largest of
	// |dx|, |dy|, |dz|), which the level always covers: size / 2 - RECENTER -
	// margin. Passed to terrain_common.gdshaderinc as clip_reach.
	int reach() const { return size / 2 - RECENTER - 3; }

	struct Level {
		int scale = 1; // voxels per texel
		Vector3i origin; // first texel, in world texels (world position / scale)
		bool valid = false;
		std::vector<uint8_t> texels; // TEXEL_BYTES per texel: density + 128, material (wrapped)
		// Levels 1 and up: each texel's density as read from the world (+ 128,
		// wrapped like texels), before fix_pending() lowered it.
		std::vector<uint8_t> raw;
		// Texels whose distances fix_pending() still has to work out again
		// (world texels, inclusive boxes). Kept apart unless one pass over
		// both costs less than two (see queue_fix()), so edits far apart
		// don't make one pass over everything between them.
		std::vector<std::pair<Vector3i, Vector3i>> fix_boxes;
		// Per storage layer: changed since uploaded, 0 = no; 2 = by an edit or
		// a fix (goes up first: it's where the player is looking), 1 = by a move.
		std::vector<uint8_t> layer_dirty;
		// The brick distances on the GPU. While some layers aren't up yet, the
		// GPU holds texels of older and newer contents at once: each brick
		// goes up as the least of what it said and says now (safe for both;
		// with only the new ones, rays skipped surface still shown: gaps
		// while sculpting), and exactly again once every layer is up.
		std::vector<uint8_t> bricks_gpu;
		bool bricks_conservative = false;
		// Where the GPU copy is placed. After a move it stays at the old origin
		// until every changed layer is uploaded (see SculptTerrain); meanwhile
		// the shader uses only where the old and new boxes overlap, whose
		// texels keep their storage slot (the rest of the old box's slots may
		// already hold the new box's texels).
		Vector3i published_origin;
		bool published = false;
		std::vector<uint8_t> has_surface; // per brick (wrapped)
		std::vector<uint8_t> bricks; // per brick distance (wrapped)
		bool bricks_dirty = false; // changed since uploaded
		std::vector<uint8_t> brick_layers_dirty; // which storage layers of bricks (when bricks_dirty)
		// has_surface changed: bricks need update_brick_distances(), done
		// once a frame by update_stale_distances() however many changes.
		bool distances_stale = false;
		// Edits made while a move was running, to re-apply when it finishes.
		std::vector<std::pair<Vector3i, Vector3i>> pending_edits;

		Vector3i world_origin() const { return origin * scale; }
	};

	~Clipmap() { cancel_jobs(); }
	// Discards all levels. p_size must be a multiple of BRICK.
	void configure(int p_size, int p_level_count);
	int get_size() const { return size; }
	int get_brick_count() const { return size / BRICK; }
	int get_level_count() const { return (int)levels.size(); }
	Level &level(int p_index) { return levels[p_index]; }
	const Level &level(int p_index) const { return levels[p_index]; }

	// Moves levels to keep p_center near their middle. The texels a move
	// brings into view are generated on a worker thread, and edited chunks are
	// filled in on this thread once it finishes; until then the level stays
	// where it was. Level 0 is built at once the first time if p_wait_first,
	// and with p_wait everything finishes before this returns. Returns a bit
	// mask of levels that moved during this call.
	int follow(VoxelWorld &r_world, const Vector3 &p_center, int p_threads, bool p_wait_first, bool p_wait);
	// Relabels everything by p_offset voxels (a multiple of the world's
	// size in x and z: the same place on the wrapping world) without
	// regenerating: storage slots don't change when p_offset is a multiple of
	// a level's extent (size * scale); a level for which it isn't is rebuilt.
	// Running moves carry on and are moved when they finish.
	void shift(VoxelWorld &r_world, const Vector3i &p_offset);
	// Waits for running work and discards it. Call before changing the world's
	// generator settings.
	void cancel_jobs();
	bool has_pending_jobs() const;
	// Re-reads the texels whose points lie in [p_min, p_max] after an edit there.
	void refresh(VoxelWorld &r_world, const Vector3i &p_min, const Vector3i &p_max);
	// Works out the distances refresh() and moves left to do (see
	// start_fix()), once a frame: starts a pass per level with work on a
	// worker thread and writes back those that finished. p_all: all of it
	// now. Returns whether anything changed.
	bool fix_pending(const VoxelWorld &p_world, bool p_all);
	// The slowest finish_job() since the last take, per part ("raw",
	// "patch", "queue", "pending"), the slowest start_job() ("start") and
	// per level ("L0".."); then zeroed.
	std::vector<std::pair<std::string, double>> take_finish_times();
	// Works out the brick distances of levels whose bricks gained or lost
	// surface since the last call (whole level each: so at most once a frame;
	// the levels side by side on worker threads). Call before uploading.
	void update_stale_distances();
	// How far a pass reaches: CLAMP texels, rounded up, and one
	// more for the crossings next to them.
	static constexpr int FIX_REACH = 9;

	// Density stored for world texel p_texel of a level, in voxels (for tests).
	float texel_value(const Level &p_level, const Vector3i &p_texel) const;
	// Brick distance stored for the brick holding world texel p_texel (for tests).
	int brick_value(const Level &p_level, const Vector3i &p_texel) const;

private:
	// World texels [lo, hi] a move brings into view. Brick-aligned (levels
	// move in steps of RECENTER texels).
	// An edited chunk's stored data (copied on the main thread when a move
	// starts), for the worker to write over the generated texels.
	struct Patch {
		Vector3i chunk, t0, t1; // the chunk; its texels inside the slab
		int side = 0; // points per axis of data
		std::vector<int8_t> data;
		std::vector<MaterialId> materials;
		// Level 0, a chunk not in memory: the worker reads its points from
		// the save (data holds its mip 1, 16^3, in case that fails), so a
		// move doesn't wait on the disk.
		bool from_save = false;
	};
	struct Slab {
		Vector3i lo, hi;
		std::vector<uint8_t> surface; // has_surface per brick of the slab, x fastest
		std::vector<Patch> patches; // the edited chunks in it (used up by the worker)
	};
	// A move of one level, generating on a worker thread.
	struct Job {
		Vector3i origin; // the level's origin once done
		Vector3i old_origin;
		bool reset = false; // the new box doesn't overlap the old one
		std::vector<Slab> slabs;
		// A reset builds a whole new level here; otherwise the worker writes the
		// slabs straight into the level's storage slots they replace, which
		// nothing else touches while it runs (see refresh()).
		std::vector<uint8_t> texels;
		// Levels 1 and up: the worker also copies the slabs' densities into
		// the raw values (the level's, or for a reset this new copy), so the
		// main thread doesn't (strided over the whole level: up to ~100 ms).
		std::vector<uint8_t> raw;
		bool raw_done = false;
		// Texels shift() moved the world by while this ran (main thread only:
		// the worker's coordinates are applied with this once it's done).
		Vector3i shift;
		std::thread thread;
		std::atomic<bool> finished{ false };
	};

	int size = 128;
	std::vector<Level> levels;
	std::vector<std::unique_ptr<Job>> jobs; // per level, or null
	double finish_part_ms[7] = {}; // see take_finish_times(); [4]: start_job(), [5] fix_pending()'s start_fix(), [6] its apply_fix()
	double finish_level_ms[16] = {};
	// fix_pending()'s passes, one per level at a time, on a worker thread:
	// the raw values needed are copied out first, and the result written
	// back on the main thread a frame or so later.
	struct FixJob {
		Vector3i min, max; // the box asked for (world texels)
		Vector3i a, n; // read: first texel, size
		Vector3i wa, wb; // written: first and last texel
		std::vector<uint8_t> raw; // the raw values read (+ 128)
		int scale = 1, saturated = 0; // the level's; what "at least CLAMP voxels" reads as
		Vector3i chunk0, chunk_n; // the chunks the read box covers...
		std::vector<uint8_t> chunk_edited; // ...and which are edited
		std::vector<int8_t> result; // per written texel
		std::thread thread;
		std::atomic<bool> finished{ false };
		// Main thread only (the worker reads the boxes above while it runs):
		// shift() records world shifts here instead of waiting for the
		// worker, and fix_pending() applies them once it has finished.
		Vector3i shift; // texels to add to the boxes
		bool dropped = false; // the level was rebuilt: throw the result away
	};
	std::vector<std::unique_ptr<FixJob>> fix_jobs; // per level, or null

	void start_job(int p_level, const Vector3i &p_target, const VoxelWorld &p_world, bool p_wait, int p_threads);
	void finish_job(int p_level, VoxelWorld &r_world);
	// Copies the densities of a slab's texels into p_raw (texels + 128).
	void copy_raw(const Slab &p_slab, const uint8_t *p_texels, uint8_t *r_raw) const;
	// Worker side: generated texels of a slab into r_texels, the slab's
	// patches over them, and which of its bricks hold surface. Reads nothing
	// the main thread changes.
	void generate_slab(const VoxelWorld &p_world, int p_level, Slab &r_slab, uint8_t *r_texels, int p_threads) const;
	// Main thread: copies the data of the edited chunks in a slab into its
	// patches (edits made while the worker runs are redone by finish_job()).
	void take_patches(Slab &r_slab, int p_level, const VoxelWorld &p_world) const;
	// Any thread: an edited chunk's data (points every C / side) into texels
	// [p_t0, p_t1] of r_texels, and r_raw if not null.
	void write_chunk(int p_level, const Vector3i &p_chunk, const int8_t *p_data, const MaterialId *p_materials, int p_side, const Vector3i &p_t0, const Vector3i &p_t1, uint8_t *r_texels, uint8_t *r_raw) const;
	// Main thread: re-reads world texels [p_min, p_max] (clipped to the level)
	// through VoxelWorld::lod_texel, which knows about edits. Returns whether
	// any brick's has_surface changed.
	bool write_texels(Level &r_level, int p_level, VoxelWorld &r_world, const Vector3i &p_min, const Vector3i &p_max);
	// Calls p_function(t0, t1) with the texel box of each repeat of the
	// (wrapping) world's voxel box [p_min, p_max] inside texels [p_lo, p_hi].
	template <typename F>
	void for_each_repeat(const VoxelWorld &p_world, const Vector3i &p_min, const Vector3i &p_max, const Vector3i &p_lo, const Vector3i &p_hi, int p_scale, F p_function) const;
	// Coarse levels (1 and up) near edits. An edited chunk's mips hold its
	// points' densities, which stop at DENSITY_CLAMP voxels: a fraction of
	// these levels' texels, so rays over sculpted ground far away crept
	// along in short steps (running out of them: holes, and slow) and no
	// brick there could be skipped. And generated texels next to an edit
	// don't know about it. So texels within CLAMP texels of world texels
	// [p_min, p_max] are worked out again from the raw ones around them:
	// each gets the distance (in this level's texels, along a 26-neighbour
	// path, a little short of the straight line) to the nearest crossing of
	// the surface, where that is clearly less than it says; an edited
	// chunk's "at least CLAMP voxels" gets that distance outright (CLAMP
	// texels if there's none so near).
	//
	// Main thread: copies what a pass over [p_min, p_max] reads (null if
	// nothing of it is in the level).
	std::unique_ptr<FixJob> start_fix(Level &r_level, int p_level, const VoxelWorld &p_world, const Vector3i &p_min, const Vector3i &p_max);
	// Any thread: the pass itself, from the copy only.
	static void compute_fix(FixJob &r_job);
	// Main thread: writes the result. Returns whether a brick's has_surface changed.
	bool apply_fix(Level &r_level, const FixJob &p_job);
	static void queue_fix(Level &r_level, const Vector3i &p_min, const Vector3i &p_max);
	// Returns whether any brick's flag changed.
	bool update_has_surface(Level &r_level, const Vector3i &p_min, const Vector3i &p_max);
	void update_brick_distances(Level &r_level);
	// Marks the layers holding world texels z in [p_z0, p_z1] as changed.
	void mark_layers(Level &r_level, int p_z0, int p_z1, uint8_t p_priority);

	static int wrap(int p_value, int p_size) {
		const int m = p_value % p_size;
		return m < 0 ? m + p_size : m;
	}
	size_t texel_index(const Vector3i &p_world_texel) const {
		return ((size_t)wrap(p_world_texel.z, size) * size + wrap(p_world_texel.y, size)) * size + wrap(p_world_texel.x, size);
	}
	size_t brick_index(const Vector3i &p_world_brick) const {
		const int nb = size / BRICK;
		return ((size_t)wrap(p_world_brick.z, nb) * nb + wrap(p_world_brick.y, nb)) * nb + wrap(p_world_brick.x, nb);
	}
};

} // namespace godot
