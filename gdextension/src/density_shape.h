#pragma once

#include "voxel_world.h"

#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/basis.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/vector4.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace godot {

// sculpt_terrain.cpp's distance repair, for grids too (x fastest, then y, z).
void density_redistance(std::vector<float> &r_values, const Vector3i &p_size);

// A small density grid that moves with a DensityBody3D: the body's shape,
// drawn by the terrain's raymarcher and sculpted with the same brushes as the
// terrain. Up to MAX_SIZE points a side, voxel_size apart, the first at
// `origin` (in the body's own space); densities like the terrain's (signed
// distance in voxels of the grid, positive inside, clamped to DENSITY_CLAMP).
//
// It can hold several poses ("keys", like a mesh's shape keys), all the same
// size and sharing one set of materials: a body's `pose` blends between
// them (1.5 = halfway from key 1 to key 2).
//
// The shape is a rest shape (`rest`, with the materials) and, per key, a
// motion and a residual. The motion says where each point of the rest
// shape goes in that pose (grab and snake hook on one key record it), so
// blending two keys moves the parts between them: a leg swings from one
// key's place to the other's instead of fading out of one and into the
// other. The residual is density added in that pose (what a brush adds to
// one key only, or what a key from an older file was beyond the rest).
//
// Brushes on "all keys" (edit_key -1) edit the rest shape at the point of it
// under the brush in the pose shown, so a hat sculpted on a posed frog's
// head stays on the head in every pose (new material moves as what it was
// added onto does). Brushes on one key edit that key only.
//
// Besides `pose`, a body can weigh keys in on top ("key weights", like a
// mesh's blend shapes): key 2 at 1 and key 3 at 1 shows both keys' motions
// and residuals added together (a mouth open and a leg out at once). Key 0
// is usually the plain shape, the pose the others are added to.
//
// Drawing and collision use "frames": resolved grids at the keys and at
// in-betweens (enough that no part moves more than about a voxel between
// two), each worked out when first needed and kept until the shape
// changes. Between two frames the density is blended as before. Mixes of
// key weights get frames of their own (weights rounded to such steps),
// the most recently used kept.
class DensityShape : public Resource {
	GDCLASS(DensityShape, Resource)

public:
	~DensityShape();
	static constexpr int MAX_SIZE = 64; // the drawing atlas's slot (SculptTerrain::GRID_SLOT)
	// Building past MAX_SIZE points a side makes the voxels coarser (twice
	// as big each time) rather than stopping, up to this voxel size.
	static constexpr double MAX_VOXEL_SIZE = 16.0;

	// Saved form: densities as signed bytes (1/16 voxel), materials as
	// 16-bit ids (little endian).
	void set_size(const Vector3i &p_size);
	Vector3i get_size() const { return size; }
	void set_origin(const Vector3 &p_origin) { origin = p_origin; }
	Vector3 get_origin() const { return origin; }
	void set_voxel_size(double p_value) { voxel_size = std::max(p_value, 0.01); }
	double get_voxel_size() const { return voxel_size; }
	// Keys as whole densities (as older files saved them): the first becomes
	// the rest shape, the others residuals over it. get_keys(): each key
	// resolved.
	void set_keys(const Array &p_keys);
	Array get_keys() const;
	// Saved form of the rest shape and the keys' motion and residual.
	void set_rest(const PackedByteArray &p_bytes);
	PackedByteArray get_rest() const;
	void set_key_data(const Array &p_data);
	Array get_key_data() const;
	void set_materials(const PackedByteArray &p_bytes);
	PackedByteArray get_materials() const;
	void set_edited(bool p_value) { edited = p_value; }
	bool is_edited() const { return edited; }
	int get_key_count() const { return (int)keys.size(); }
	// Whether any key has motion (else keys blend as plain densities).
	bool has_motion() const;
	// A new last key, as the shape looks at p_pose with key weights
	// p_weights (their motions and residuals added up). Returns its index.
	int add_key(double p_pose, const PackedFloat32Array &p_weights = PackedFloat32Array());
	void remove_key(int p_key);
	// Moves key p_from to index p_to (the others shift up or down).
	void move_key(int p_from, int p_to);
	// Clears a key's motion and residual (it looks like the rest shape).
	void reset_key(int p_key);
	// A key's name ("": none; get_key_label() then says "Key 2").
	void set_key_name(int p_key, const String &p_name);
	String get_key_name(int p_key) const;
	String get_key_label(int p_key) const;
	void set_key_names(const PackedStringArray &p_names);
	PackedStringArray get_key_names() const;

	// A ball of p_radius, of p_material (a block, or a color material), with
	// a band of p_stripe_material round its middle (-1: none).
	static Ref<DensityShape> make_sphere(double p_radius, int p_material, int p_stripe_material = -1, double p_voxel_size = 1.0);
	// Replaces the keys with one per pose: p_poses[k] is a PackedVector3Array
	// of triangles (3 points each) in the body's space, p_colors[k] (optional)
	// a PackedColorArray with a color per point, which become the materials
	// (from the first pose). All poses share one box, p_margin beyond them;
	// the voxels grow if that would be over MAX_SIZE a side.
	void bake_meshes(const Array &p_poses, const Array &p_colors, double p_voxel_size, double p_margin);
	// Copy with its own data, to sculpt one body without changing others.
	Ref<DensityShape> duplicate_shape() const;
	// Compact form for save files (zstd).
	Dictionary to_dict() const;
	static Ref<DensityShape> from_dict(const Dictionary &p_dict);

	// At a point of the body's space, with `pose` blending the keys.
	// p_weights: the key weights (see above; empty: none).
	double get_density(const Vector3 &p_local, double p_pose, const PackedFloat32Array &p_weights = PackedFloat32Array()) const;
	Vector3 get_normal(const Vector3 &p_local, double p_pose, const PackedFloat32Array &p_weights = PackedFloat32Array()) const;
	int get_material_at(const Vector3 &p_local, double p_pose = 0.0, const PackedFloat32Array &p_weights = PackedFloat32Array()) const;
	// The point of the rest shape shown at p_local in pose p_pose, and where
	// a point of the rest shape is in key p_key (body space).
	Vector3 rest_point(const Vector3 &p_local, double p_pose, const PackedFloat32Array &p_weights = PackedFloat32Array()) const;
	Vector3 key_point(const Vector3 &p_rest, int p_key) const;
	// How heavy the material at p_local is (a colour 1, stone heavier...).
	double get_material_density_at(const Vector3 &p_local) const;
	// Sculpting, in the body's space: as SculptTerrain.apply_brush() and
	// paint_color(). p_key: which key (-1: all, through the rest shape);
	// p_pose: the pose the body is shown in (where p_center is). Adding near
	// the edge grows the grid (coarser past MAX_SIZE). Returns whether
	// anything changed.
	bool apply_brush(const Vector3 &p_center, double p_radius, double p_strength, int p_mode, int p_material, int p_key = -1, double p_pose = 0.0, const PackedFloat32Array &p_weights = PackedFloat32Array());
	bool paint_color(const Vector3 &p_center, double p_radius, const Color &p_color, double p_opacity, double p_hardness, double p_pose = 0.0, const PackedFloat32Array &p_weights = PackedFloat32Array());
	// Grab and snake hook, as SculptTerrain.warp() (a centre per symmetry
	// copy, body space): begin_grab() keeps the shape as it is; each warp()
	// with p_grab moves it by p_deltas (the whole move since then) from that;
	// a snake hook step pulls the current shape along by p_deltas. On one
	// key (p_key), they change that key's motion: the rest shape stays.
	void begin_grab();
	bool warp(const std::vector<Vector3> &p_centers, const std::vector<Vector3> &p_deltas, double p_radius, bool p_grab, int p_key = -1, double p_pose = 0.0, const PackedFloat32Array &p_weights = PackedFloat32Array());
	void end_grab();
	// How much solid it has (the rest shape), in cubic voxels of its space.
	double get_volume() const;
	// What its materials make it (see material_physics()): its mass (a
	// colour ball of radius 3 is about 1.2), and the bounce and friction of
	// its surface (averaged over its skin).
	void physical_properties(double &r_mass, double &r_bounce, double &r_friction) const;
	double get_mass() const;
	// Splits off the parts no longer connected to the biggest one (cutting
	// a body in two): returns them as shapes in the same space, and keeps
	// only the biggest here. Parts under p_min_voxels are just removed.
	Array split_parts(int p_min_voxels = 4);
	// A convex solid round some points (the hull of them, at least a voxel
	// or so thick), in their own space, of p_material.
	static Ref<DensityShape> make_hull(const PackedVector3Array &p_points, int p_material, double p_voxel_size = 1.0);
	// Changes with every edit (the drawing atlas re-uploads).
	int64_t get_version() const { return version; }

	// --- For DensityBody3D and SculptTerrain --------------------------------
	// Collision probes of frame p_frame (see pose_blend()): points just
	// inside the surface, each a sphere (xyz in body space, w radius).
	const std::vector<Vector4> &probes(int p_frame) const;
	// All of frame p_frame's skin points (probes() takes every n-th of them).
	std::vector<Vector4> skin_points(int p_frame) const;
	// Solid points of the rest shape: their centroid, and the inertia tensor
	// per unit mass about it (body space).
	void mass_properties(Vector3 &r_centroid, Basis &r_inertia) const;
	// One z slice of frame p_frame for the atlas: MAX_SIZE^2 RGB8 texels like
	// the clipmap's (R density + 128, G B material, see render_bytes()).
	void fill_slice(int p_frame, int p_z, uint8_t *r_texels) const;
	// How the shape looks in a pose (and key weights): up to MAX_BLEND
	// frames, their densities blended by weight (summing to 1), the heaviest
	// first. With several key weights between steps, the frames are the
	// corners of the simplex round the mix (so the blend moves smoothly
	// whichever weight changes).
	static constexpr int MAX_BLEND = 4;
	struct Blend {
		int frames[MAX_BLEND] = { 0, 0, 0, 0 };
		float weights[MAX_BLEND] = { 1.0f, 0.0f, 0.0f, 0.0f };
		int count = 1;
	};
	// The frames of a pose (made if new, not worked out: see frame_ready()).
	void pose_blend(double p_pose, const PackedFloat32Array &p_weights, Blend &r_blend) const;
	// Whether frame p_frame is worked out for the shape as it is now (and
	// still kept: a key weight mix's frame can be dropped for room).
	bool frame_ready(int p_frame) const;
	// Has frame p_frame worked out on a background thread, unless it's ready
	// or on its way (a body shows the frames it has meanwhile).
	void request_frame(int p_frame) const;
	// The density (and its way out) of a blend at a point of the body's space.
	double get_blend_density(const Vector3 &p_local, const Blend &p_blend) const;
	Vector3 get_blend_normal(const Vector3 &p_local, const Blend &p_blend) const;
	// Stops the background thread (the library unloading).
	static void shutdown_worker();
	// Whether some key weight is in use.
	static bool has_weights(const PackedFloat32Array &p_weights);
	int get_frame_count() const;
	// Replaces everything with one key (distances are repaired).
	void init_grid(const Vector3i &p_size, const Vector3 &p_origin, double p_voxel_size, std::vector<float> &&p_density, std::vector<MaterialId> &&p_materials);

protected:
	static void _bind_methods();

private:
	struct Key {
		std::vector<Vector3> motion; // where each rest point goes, in voxels from it (empty: nowhere)
		std::vector<float> residual; // density added in this pose, at its points (empty: none)
		String name;
	};
	// A resolved grid: the shape in one pose.
	struct Frame {
		int64_t version = -1;
		std::vector<float> density;
		std::vector<MaterialId> materials; // empty: the rest shape's
		std::vector<Vector3> to_rest; // the rest point (grid units) shown at each point; empty: the same point
	};

	Vector3i size;
	Vector3 origin;
	double voxel_size = 1.0;
	std::vector<float> rest; // densities, voxels of the grid
	std::vector<Key> keys; // at least one if `rest` has points
	std::vector<MaterialId> materials; // of the rest shape
	bool edited = false;
	int64_t version = 0;
	// The shape when a grab began.
	std::vector<float> grab_rest;
	std::vector<Key> grab_keys;
	std::vector<MaterialId> grab_materials;
	Vector3 grab_box_lo, grab_box_hi; // what the grab has rewritten so far (body space)
	bool grab_has_box = false;
	std::vector<Vector3> grab_center_offsets; // brush centre (pose shown) -> where it edits, per copy
	std::vector<float> grab_key_density; // a grab on one key: that key as it looked
	// The snake hook's, kept from step to step while nothing else changes.
	std::vector<Vector3> snake_centers, snake_offsets;
	int64_t snake_version = -1;
	uint32_t paint_serial = 0; // varies paint_color()'s rounding dither between calls
	mutable std::unordered_map<int, std::vector<Vector4>> probe_cache;
	mutable int64_t probe_version = -1;
	// physical_properties() and mass_properties() of `mass_version` (they
	// read every point: bodies changing pose, or sharing the shape, reuse them).
	mutable int64_t mass_version = -1;
	mutable double cached_mass = 0.0, cached_bounce = 0.5, cached_friction = 0.5;
	mutable Vector3 cached_centroid;
	mutable Basis cached_inertia;
	// Frames: gap_frames[g] of them from key g up to (not including) key g + 1.
	mutable std::vector<Frame> frames;
	mutable std::vector<int> gap_frames;
	mutable int64_t layout_version = -1;
	mutable std::vector<int> key_steps; // a key weight is rounded to 1 / this
	// Frames of key weight mixes (ids from COMBO_BASE), the latest used kept.
	static constexpr int COMBO_BASE = 1 << 24;
	struct Combo {
		std::vector<int16_t> steps; // each key's weight, in its key_steps
		std::vector<float> weights;
		int id = 0;
		uint64_t used = 0;
		Frame frame;
	};
	mutable std::vector<Combo> combos;
	mutable int next_combo = COMBO_BASE;
	mutable uint64_t combo_clock = 0;
	mutable std::recursive_mutex frame_mutex;
	// Background work (request_frame()): a copy of what frames are made of
	// (the worker reads it while the shape is edited), and the frames on
	// their way, collected into their places when done.
	struct Source {
		Vector3i size;
		std::vector<float> rest;
		std::vector<Key> keys;
		std::vector<MaterialId> materials;
		int64_t version = -1;
	};
	struct Job {
		std::shared_ptr<const Source> source;
		std::vector<float> weights;
		int frame = 0;
		Frame result;
		std::atomic<bool> done{ false };
		std::atomic<bool> cancelled{ false };
	};
	mutable std::shared_ptr<const Source> source;
	mutable std::vector<std::shared_ptr<Job>> jobs;
	void collect_jobs() const;
	void cancel_jobs(int p_frame) const; // -1: all
	// Where frame p_frame is kept (null: dropped), and its key weights.
	Frame *frame_slot(int p_frame) const;
	std::vector<float> frame_weights(int p_frame) const;
	// Keys' weights -> the shape in that pose (see resolve_weights()), from
	// any copy of the shape.
	static void resolve_source(const Vector3i &p_size, const std::vector<float> &p_rest, const std::vector<Key> &p_keys,
			const std::vector<MaterialId> &p_materials, const std::vector<float> &p_weights, bool p_residual, Frame &r_frame);
	float sample_blend(const Vector3 &p_grid, const Blend &p_blend) const;
	// Each key's weight in a pose with key weights: r_weights (one per key).
	// Returns whether any key weight is in use.
	bool pose_weights(double p_pose, const PackedFloat32Array &p_weights, std::vector<float> &r_weights) const;
	// The frame of a mix of key weights (rounded steps), made if new (the
	// least recently used dropped for room).
	int combo_frame(const std::vector<int16_t> &p_steps) const;
	void update_mass_cache() const;
	void physical_properties_now(double &r_mass, double &r_bounce, double &r_friction) const;
	void mass_properties_now(Vector3 &r_centroid, Basis &r_inertia) const;

	void update_layout() const;
	// Frame p_frame's keys and blend.
	void frame_keys(int p_frame, int &r_a, int &r_b, float &r_t) const;
	int key_frame(int p_key) const;
	const Frame &frame(int p_frame) const;
	// The shape at keys a, b blended by t: densities (distances repaired if
	// it moves), materials and rest points (empty: the rest shape's / the
	// same). p_residual: with the keys' residuals.
	void resolve(int p_a, int p_b, float p_t, bool p_residual, Frame &r_frame) const;
	// The same for any weights (one per key).
	void resolve_weights(const std::vector<float> &p_weights, bool p_residual, Frame &r_frame) const;
	// Trilinear motion of a key's motion field at a grid position (zero if none).
	Vector3 sample_motion(const std::vector<Vector3> &p_motion, const Vector3 &p_grid) const;
	// The rest point (grid units) shown at grid position p_grid in p_pose.
	Vector3 rest_grid_point(const Vector3 &p_grid, double p_pose, const PackedFloat32Array &p_weights) const;
	// After the rest shape changed from p_before within grid box [p_lo, p_hi]:
	// points that were air and changed move as the nearest solid does, in
	// every key with motion (what's added onto a part follows it).
	void inherit_motion(const std::vector<float> &p_before, const Vector3i &p_lo, const Vector3i &p_hi);
	// A brush (apply_brush()'s modes) on one grid of this size: values and
	// materials, centre in grid units. r_lo, r_hi: the points it may have
	// changed. Returns whether anything changed.
	bool brush_grid(std::vector<float> &r_values, std::vector<MaterialId> &r_materials, const Vector3 &p_center, float p_radius, float p_strength, int p_mode, int p_material, Vector3i &r_lo, Vector3i &r_hi);

	size_t count() const { return (size_t)size.x * size.y * size.z; }
	size_t index(int x, int y, int z) const { return ((size_t)z * size.y + y) * size.x + x; }
	// Trilinear density of a grid of this size at a grid position (voxels
	// from the first point), and beyond the grid, the edge's value minus how far out.
	float sample_grid(const std::vector<float> &p_values, const Vector3 &p_grid) const;
	float sample_pose(const Vector3 &p_grid, double p_pose, const PackedFloat32Array &p_weights) const;
	// Makes the grid cover [p_lo, p_hi] (grid points, may be outside it now),
	// as far as MAX_SIZE allows, keeping what's there.
	void grow(Vector3i p_lo, Vector3i p_hi);
	// Halves the resolution: voxels twice as big, every other point kept
	// (the grab's saved shape too).
	void coarsen();
	// Makes the grid cover the body-space box [p_lo, p_hi] (p_coarsen: making
	// the voxels coarser if it wouldn't fit in MAX_SIZE; else as far as fits).
	void fit(const Vector3 &p_lo, const Vector3 &p_hi, bool p_coarsen);
	// After an edit of key values within grid box [p_lo, p_hi]: repairs the
	// distances (as the terrain's commit_edit()) where it can have changed
	// them, the box grown by the clamp. Points the edit changed take the
	// repaired value; the rest keep theirs unless the repair finds the
	// surface nearer, or on their other side.
	void repair(std::vector<float> &r_after, const std::vector<float> &p_before, const Vector3i &p_lo, const Vector3i &p_hi) const;
	void changed();
};

} // namespace godot
