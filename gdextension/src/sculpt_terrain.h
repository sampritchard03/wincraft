#pragma once

#include "brush_options.h"
#include "clipmap.h"
#include "density_shape.h"
#include "voxel_world.h"

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/texture2d_array.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/aabb.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/vector3i.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>

#include <chrono>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>

namespace godot {

class DensityBody3D;

// Endless, sculptable floating-island terrain (see VoxelWorld), drawn by a
// raymarching shader (terrain.gdshader) that reads a Clipmap of it. One voxel
// = one world unit.
//
// open_world() loads the world saved at save_path, or starts a new one with
// the island settings below (a saved world keeps the settings it was made
// with). Call update_view() every frame with the camera position: it moves the
// clipmap, uploads what changed to `material`, unloads far-away data and
// autosaves. The world is also saved when this node leaves the tree.
class SculptTerrain : public Node {
	GDCLASS(SculptTerrain, Node)

public:
	enum BrushMode {
		BRUSH_ADD, // raise the density: the surface grows outward
		BRUSH_SUBTRACT, // lower it: carve
		BRUSH_SMOOTH, // blend each point towards the average of its neighbours
		BRUSH_FLATTEN, // blend towards a plane through the centre, facing the surface normal there
		BRUSH_PAINT, // change the material of the surface, not its shape
		BRUSH_INFLATE, // move the surface out along its own normals (negative strength: in)
		BRUSH_PINCH, // pull the surface towards the brush's middle, into a crease (negative: spread it)
		BRUSH_LAYER, // raise the surface up to brush_layer_height above where it was when the stroke began (negative: lower)
		BRUSH_CLAY, // clay strips: fill up to a plane a little above the surface, in square strips along the stroke (negative: scrape below)
		BRUSH_CLONE, // copy the ground from brush_clone_offset away
	};
	enum WorldType {
		WORLD_ISLANDS = godot::WORLD_ISLANDS,
		WORLD_TERRAIN = godot::WORLD_TERRAIN,
	};
	static constexpr int MAX_CLIP_LEVELS = 8; // MATERIAL_LEVEL / LEVEL_SAMPLER uses in terrain_common.gdshaderinc

	// Size of a new world (a saved one keeps its own): x and z wrap every
	// world_size voxels (a power of 2); world_height voxels tall, centred on 0.
	void set_world_size(int p_value) { island_settings.world_size = p_value; }
	int get_world_size() const { return island_settings.world_size; }
	void set_world_height(int p_value) { island_settings.world_height = p_value; }
	int get_world_height() const { return island_settings.world_height; }
	void set_world_type(WorldType p_value) { island_settings.world_type = p_value; }
	WorldType get_world_type() const { return (WorldType)island_settings.world_type; }
	void set_terrain_height(double p_value) { island_settings.terrain_height = (float)p_value; }
	double get_terrain_height() const { return island_settings.terrain_height; }
	void set_terrain_scale(double p_value) { island_settings.terrain_scale = (float)p_value; }
	double get_terrain_scale() const { return island_settings.terrain_scale; }
	void set_caves(bool p_value) { island_settings.caves = p_value; }
	bool get_caves() const { return island_settings.caves; }
	void set_seed(int64_t p_value) { island_settings.seed = p_value; }
	int64_t get_seed() const { return island_settings.seed; }
	void set_cell_size(int p_value) { island_settings.cell_size = p_value; }
	int get_cell_size() const { return island_settings.cell_size; }
	void set_island_chance(double p_value) { island_settings.island_chance = (float)p_value; }
	double get_island_chance() const { return island_settings.island_chance; }
	void set_min_radius(double p_value) { island_settings.min_radius = (float)p_value; }
	double get_min_radius() const { return island_settings.min_radius; }
	void set_max_radius(double p_value) { island_settings.max_radius = (float)p_value; }
	double get_max_radius() const { return island_settings.max_radius; }
	void set_detail(double p_value) { island_settings.detail = (float)p_value; }
	double get_detail() const { return island_settings.detail; }
	void set_detail_frequency(double p_value) { island_settings.detail_frequency = (float)p_value; }
	double get_detail_frequency() const { return island_settings.detail_frequency; }
	void set_octaves(int p_value) { island_settings.octaves = p_value; }
	int get_octaves() const { return island_settings.octaves; }
	void set_view_distance(double p_value);
	double get_view_distance() const { return view_distance; }
	void set_save_path(const String &p_path) { save_path = p_path; }
	String get_save_path() const { return save_path; }
	void set_autosave_interval(double p_seconds) { autosave_interval = p_seconds; }
	double get_autosave_interval() const { return autosave_interval; }
	void set_material(const Ref<ShaderMaterial> &p_material);
	Ref<ShaderMaterial> get_material() const { return material; }
	// Gets the same clipmap parameters as `material` (terrain_prepass.gdshader).
	void set_prepass_material(const Ref<ShaderMaterial> &p_material);
	Ref<ShaderMaterial> get_prepass_material() const { return prepass_material; }
	// More materials that read the clipmap (the sun shadow maps' passes): they
	// get the same clipmap parameters as `material`.
	void set_extra_materials(const Array &p_materials);
	Array get_extra_materials() const { return extra_materials; }
	// Changes whenever the terrain does (any edit, undo, another player's
	// change, a received chunk): for things computed from it to redo.
	int64_t get_edit_serial() const { return edit_serial; }
	void set_clip_size(int p_size);
	int get_clip_size() const { return clip_size; }
	void set_clip_levels(int p_levels);
	int get_clip_levels() const { return clip_levels; }
	// The area kept in memory around the camera (and that future physics
	// will run in), voxels (Chebyshev). Farther chunks are generated (or
	// read) only when something needs them, and let go again.
	void set_simulation_radius(double p_value) { simulation_radius = std::max(p_value, 32.0); }
	double get_simulation_radius() const { return simulation_radius; }
	// Blocky mode: every tool's result is snapped to blocks of this many
	// voxels (each block the tool changed takes its average value and its
	// most solid point's material, on a grid fixed to the world), for
	// stepped, terraced shapes.
	// 0 = off; 2..8.
	void set_blocky_size(int p_value) { blocky_size = p_value < 2 ? 0 : std::min(p_value, 8); }
	int get_blocky_size() const { return blocky_size; }
	void set_thread_count(int p_value) { thread_count = p_value; }
	int get_thread_count() const { return thread_count; }

	// Opens the world at save_path (empty = don't save). Returns false (and
	// prints why) if it can't, in which case the world still runs, unsaved.
	bool open_world();
	// Multiplayer: a world opened unsaved (a joined player's copy) starts
	// saving at p_path (emptied first), all of it written now. False (and
	// nothing changed) if it already saves somewhere.
	bool save_world_as(const String &p_path);

	// --- Sharing the world (multiplayer) --------------------------------------
	// Every generator setting (world type, seed, sizes, noise...), as the
	// open world has them: set them on another player's terrain before its
	// open_world() and it generates the same ground.
	Dictionary get_world_settings() const;
	void set_world_settings(const Dictionary &p_settings);
	// The chunks holding edits (wrapped chunk coordinates: multiply by 32 for
	// voxels), and one chunk's points, zstd-compressed (densities, then
	// materials as 16-bit little-endian ids). set_chunk_data() replaces a
	// chunk with such data and redraws it; it isn't an undo step.
	TypedArray<Vector3i> get_edited_chunks() const;
	PackedByteArray get_chunk_data(const Vector3i &p_chunk) const;
	bool set_chunk_data(const Vector3i &p_chunk, const PackedByteArray &p_data);
	// Change tracking: while on, take_changes() returns what every edit since
	// the last call changed (any tool, undo and redo too), as
	// [chunk, change, chunk, change...]: each change the points that differ
	// (their new density and material), compressed. apply_change() writes one
	// (on another player's computer) without it counting as a change there,
	// or as an undo step.
	void set_change_tracking(bool p_on) { world.set_change_tracking(p_on); }
	bool is_change_tracking() const { return world.is_change_tracking(); }
	Array take_changes();
	// p_remote: another player's change (true), or one of this computer's own
	// being put back on top (false). Another player's change that lands
	// during a stroke counts as having been there before it, for undo.
	bool apply_change(const Vector3i &p_chunk, const PackedByteArray &p_change, bool p_remote = true);
	// Writes all changes now. Returns false if a write failed.
	bool save_world();
	// Moves the clipmap to follow p_camera_position, uploads changed levels,
	// unloads far data and autosaves when due. Distant levels are generated in
	// the background and appear a little later, unless p_wait.
	void update_view(const Vector3 &p_camera_position, bool p_wait = false);
	// Edits the points within p_radius of p_center, with a smooth falloff to 0
	// at the edge. For ADD / SUBTRACT p_strength is how far the surface moves
	// at the centre, in voxels; for SMOOTH / FLATTEN it is the blend amount at
	// the centre (0..1). Scale it by the frame time for a steady rate.
	// p_material: what ADD builds and PAINT paints (a material id); -1 = ADD
	// keeps the materials already there. Ground that SMOOTH / FLATTEN raise
	// takes the material of the ground next to it.
	void apply_brush(const Vector3 &p_center, double p_radius, double p_strength, int p_mode, int p_material = -1);
	// Material of the ground at (or right next to) a position, e.g. where a
	// raycast hit; -1 if there is no solid ground there.
	int get_block_material(const Vector3 &p_pos);

	// Color painting, like a paint program's brush: lays p_color over the
	// surface skin within p_radius with p_opacity (0..1), fully inside
	// p_hardness of the radius and fading out beyond. Within one stroke
	// (begin_stroke() .. end_stroke()) going over a point again doesn't build
	// up: each point gets the stroke's color over what it was when the stroke
	// began, at the most coverage the brush has given it. Whatever it was
	// (a color, or a block, whose color is its texture's average: see
	// set_material_colors()) becomes a color material.
	void paint_color(const Vector3 &p_center, double p_radius, const Color &p_color, double p_opacity, double p_hardness);
	// The blocks' average colors (get_material_names() order), which color
	// painting starts from on them. world.gd sets them from the textures.
	void set_material_colors(const PackedColorArray &p_colors);
	// A material id's color: a color material's own, or a block's average.
	Color get_material_color(int p_material) const;
	// The color material nearest a color (5 bits a channel).
	static int color_to_material(const Color &p_color);
	// The materials' names, by index: their textures are
	// res://assets/block/<name>.png.
	static PackedStringArray get_material_names();
	// Grab and snake hook (one entry per symmetry copy).
	//   Grab (p_grab): material within p_radius of p_centers[i] (where the
	//   grab began) moves by p_deltas[i] (the whole offset since begin_grab(),
	//   passed again every frame), fading out towards the radius. Values come
	//   from a snapshot taken at begin_grab(), so dragging back undoes it.
	//   Snake hook: material within p_radius of p_centers[i] (the brush's new
	//   position) is pulled along by this step's p_deltas[i] (up to ~8
	//   voxels; split bigger moves), keeping a tendril's thickness.
	void begin_grab();
	void warp(const PackedVector3Array &p_centers, const PackedVector3Array &p_deltas, double p_radius, bool p_grab);
	void end_grab();
	// Rotate and scale (between begin_grab() and end_grab(), like a grab):
	// material within p_radius of p_centers[i] is turned and scaled about it
	// by p_bases[i] (a rotation times a uniform scale: the whole change since
	// begin_grab(), passed again every frame), all of it at the centre and
	// fading out towards the radius. One entry per symmetry copy.
	void twist(const PackedVector3Array &p_centers, const Array &p_bases, double p_radius);
	// Undo history, one entry per stroke: everything edited between
	// begin_stroke() and end_stroke() (e.g. mouse press to release) undoes
	// together. Starting a new stroke clears the redo history. undo() / redo()
	// return false when there is nothing to undo / redo. The history lives in
	// memory only, up to MAX_UNDO_STEPS strokes or MAX_UNDO_BYTES.
	void begin_stroke();
	void end_stroke();
	bool undo();
	bool redo();
	int get_undo_count() const { return (int)undo_stack.size(); }
	int get_redo_count() const { return (int)redo_stack.size(); }
	// First point where a ray enters the solid, or null.
	Variant raycast(const Vector3 &p_from, const Vector3 &p_dir, double p_max_distance);
	// Interpolated density at a position (> 0 = solid), in voxels.
	double get_density(const Vector3 &p_pos);
	// How far p_pos is from the nearest ground at least, read from level
	// p_level's coarse data (points 2^p_level apart, which hold distances up
	// to ~8 * 2^p_level voxels, where the full detail stops at ~8): for
	// telling quickly that a big thing is clear of the ground. Conservative:
	// never more than the real distance; 0 when it can't tell.
	double clear_distance(const Vector3 &p_pos, int p_level);
	// Outward surface normal at a position, from densities p_spread away
	// (wider: the general lie of the ground rather than its bumps).
	Vector3 get_normal(const Vector3 &p_pos, double p_spread = 0.5);
	// Flatten moves the ground towards this plane until cleared (set it
	// when a stroke starts, so the whole stroke flattens to one plane).
	void set_flatten_plane(const Vector3 &p_point, const Vector3 &p_normal) {
		flatten_point = p_point;
		flatten_normal = p_normal.normalized();
		flatten_plane_set = true;
	}
	void clear_flatten_plane() { flatten_plane_set = false; }
	// The brush's settings (brush_options.h; bodies' brushes use them too).
	//   falloff: BrushFalloff (smooth, linear, sharp, round, constant).
	//   shape: 0 sphere, 1 cube.
	//   flatten_mode: 0 both, 1 fill only (raise up to the plane), 2 scrape only.
	//   layer_height: how far Layer raises, voxels (up to 7.5: the stored
	//   distances stop at DENSITY_CLAMP, so a stroke can raise no further).
	//   clone_offset: Clone copies from a point plus this.
	//   stroke_direction: which way the stroke runs (Clay strips' strips).
	void set_brush_falloff(int p_value) { brush_options().falloff = std::clamp(p_value, 0, (int)FALLOFF_COUNT - 1); }
	int get_brush_falloff() const { return brush_options().falloff; }
	void set_brush_shape(int p_value) { brush_options().shape = std::clamp(p_value, 0, 1); }
	int get_brush_shape() const { return brush_options().shape; }
	void set_flatten_mode(int p_value) { brush_options().flatten_mode = std::clamp(p_value, 0, 2); }
	int get_flatten_mode() const { return brush_options().flatten_mode; }
	void set_brush_layer_height(double p_value) { brush_options().layer_height = (float)std::clamp(p_value, 0.0, 7.5); }
	double get_brush_layer_height() const { return brush_options().layer_height; }
	void set_brush_clone_offset(const Vector3 &p_value) { brush_options().clone_offset = p_value; }
	Vector3 get_brush_clone_offset() const { return brush_options().clone_offset; }
	void set_brush_stroke_direction(const Vector3 &p_value) { brush_options().stroke_direction = p_value; }
	Vector3 get_brush_stroke_direction() const { return brush_options().stroke_direction; }
	// The middle of the top of the island nearest p_near (within a few
	// cells), or null.
	Variant find_island(const Vector3 &p_near) const;

	// For tests and profiling.
	int64_t take_upload_bytes();
	// The slowest time (ms) each part of update_view() took since the last
	// call, and how many layers went up in all (for finding hitches).
	Dictionary take_view_times();
	Vector3i get_clip_origin(int p_level) const;
	// What level p_level holds at texel p_texel (0..clip_size-1 from its
	// origin): the density in voxels, and the brick distance.
	double get_clip_texel(int p_level, const Vector3i &p_texel) const;
	int get_clip_brick(int p_level, const Vector3i &p_texel) const;
	// Density of a lattice point (a multiple of 2^p_level) as level p_level sees it.
	double get_lod_density(const Vector3i &p_point, int p_level);
	// Counts of edited / loaded / unsaved chunks, region files, and the last error.
	Dictionary get_stats() const;
	// The object editor's weight preview: a bone of a body whose share tints
	// it (p_bone -1, or a null body: none).
	void set_bone_preview(Object *p_body, int p_bone);
	// The posed bodies' material (skinned_bodies.gdshader, on a screen quad
	// of its own drawn after the terrain's): bodies with bones are drawn by
	// it, the others by `material`. world.gd sets it.
	void set_body_material(const Ref<ShaderMaterial> &p_material) { body_material = p_material; }
	Ref<ShaderMaterial> get_body_material() const { return body_material; }
	// Moves the view's frame of reference by p_offset voxels (a multiple of
	// world_size in x and z, 0 in y): the same place on the wrapping world,
	// so the clipmap is relabelled, not rebuilt, and every DensityBody3D
	// moves with it. For keeping the player near the origin as they go round
	// the world (world.gd).
	void shift_origin(const Vector3i &p_offset);

	// Physics (density_body.h): every DensityBody3D in the tree is stepped
	// here once per physics frame, colliding through the density field
	// (get_density() / get_normal()) and with each other. No meshes: a body's
	// shape is spheres, and drawn ones are drawn by `material`'s raymarch,
	// the MAX_BODY_SPHERES nearest the camera (see update_view()).
	void set_gravity(double p_value) { gravity = p_value; }
	double get_gravity() const { return gravity; }
	void set_physics_enabled(bool p_value) { physics_enabled = p_value; }
	bool is_physics_enabled() const { return physics_enabled; }
	bool is_world_open() const { return opened; }
	// The SculptTerrain in the tree (the last to enter), for bodies.
	static SculptTerrain *get_singleton() { return singleton; }
	static constexpr int MAX_BODY_SPHERES = 32; // terrain_shadow.gdshaderinc
	// Bodies with a DensityShape: at most MAX_GRID_BODIES grids drawn, each
	// pose key a run of layers (one per z) of a texture array GRID_SLOT
	// square, GRID_SLOTS * GRID_SLOT deep.
	static constexpr int MAX_GRID_BODIES = 48; // terrain_shadow.gdshaderinc
	// Bones drawn in all (terrain_shadow.gdshaderinc): a body whose bones
	// don't fit draws unposed.
	static constexpr int MAX_DRAWN_BONES = 64;
	// Posed bodies' shadow capsules, one a bone (terrain_shadow.gdshaderinc).
	static constexpr int MAX_CAPSULES = 64;
	static constexpr int GRID_SLOT = DensityShape::MAX_SIZE;
	static constexpr int GRID_SLOTS = 24;

	// Selection (selection.cpp): a set of points of the terrain, shown
	// tinted. While anything is selected, brushes change only selected
	// material (and air next to it, which joins it when filled).
	//   select_sphere(): the ground within a radius (or, p_add false, deselect).
	//   select_connected(): the solid connected to a point, within a radius
	//   (a whole floating island, say).
	//   select_similar(): the same, but only material whose color is within
	//   p_threshold (0..1.7, RGB distance) of the point's.
	void select_sphere(const Vector3 &p_center, double p_radius, bool p_add);
	int select_connected(const Vector3 &p_from, double p_radius, bool p_add);
	int select_similar(const Vector3 &p_from, double p_radius, double p_threshold, bool p_add);
	void clear_selection();
	int64_t get_selected_count() const { return selected_count; }
	bool has_selection() const { return selected_count > 0; }
	AABB get_selection_bounds() const;
	// The selection as a DensityShape (its colors and materials), centred on
	// get_selection_center(); coarser than a voxel if it's bigger than
	// DensityShape::MAX_SIZE. p_carve: also take it out of the terrain (one
	// undo step).
	Ref<DensityShape> selection_to_shape(bool p_carve);
	Vector3 get_selection_center() const { return selection_center; }
	// Undo for the selection: a snapshot of it (an Array), and putting one
	// back (it follows the world's origin shifts since it was taken). The
	// serial counts changes, so a caller can tell when it has a new one.
	Array get_selection_state() const;
	void set_selection_state(const Array &p_state);
	int64_t get_selection_serial() const { return selection_serial; }
	// Adds a shape (its key at p_pose) to the terrain where p_transform puts
	// it (any rotation and scale), as solid unioned with what's there:
	// pasting, placing a transformed selection, the geometry tool's solids.
	// p_material >= 0 paints it all that material (else the shape's own).
	// p_select: what it adds becomes selected.
	void stamp_shape(const Ref<DensityShape> &p_shape, const Transform3D &p_transform, int p_material = -1, bool p_select = true, double p_pose = 0.0);
	// World features (trees, mushrooms...): shapes the generator grows on
	// the ground, part of the generated world like its noise (so saved only
	// where their chunks are edited). Each a Dictionary:
	//   shapes        Array of DensityShape (one picked per placement; its
	//                 key 0, its local origin where it meets the ground)
	//   id            String: seeds where it goes
	//   per_region    float: placements tried per 64 x 64 voxels of ground
	//   grows_on      PackedInt32Array of materials (empty: any ground)
	//   max_slope     degrees
	//   sink          voxels into the ground (times its size)
	//   follow_slope  0 (upright) .. 1 (square to the ground)
	//   random_turn   bool
	//   scale_min, scale_max
	//   spacing       voxels from the other features of its region
	//   world_types   PackedInt32Array of WorldType (empty: all)
	// Call before open_world() (every player must give the same ones).
	void set_features(const Array &p_features);
	// Selects (or with p_add false deselects) the terrain inside a shape
	// placed by p_transform: the geometry tool's selection.
	int select_inside_shape(const Ref<DensityShape> &p_shape, const Transform3D &p_transform, bool p_add);
	// The opposite of stamp_shape(): takes the shape's solid out of the
	// terrain (a mould of it, a doorway, a tunnel).
	void carve_shape(const Ref<DensityShape> &p_shape, const Transform3D &p_transform, double p_pose = 0.0);
	// Extrude: begin_extrude() takes the selection as it is; extrude() then
	// sweeps it p_distance along p_direction (each call replaces the last,
	// like a grab), and what it adds is selected; end_extrude() finishes.
	bool begin_extrude();
	// Negative: intrude (push it in). p_radius > 0: only within that of
	// p_center, fading out towards it (like a grab).
	void extrude(const Vector3 &p_direction, double p_distance, const Vector3 &p_center = Vector3(), double p_radius = 0.0);
	void end_extrude();
	// While held, end_stroke() doesn't end the stroke: several edits (lifting
	// a selection, then placing it) undo as one.
	void set_stroke_held(bool p_held);
	bool is_stroke_held() const { return stroke_held; }
	// Counts strokes put on the undo stack (not undo / redo), so a caller can
	// tell one happened.
	int64_t get_undo_serial() const { return undo_serial; }

protected:
	static void _bind_methods();
	void _notification(int p_what);

private:
	IslandSettings island_settings;
	String save_path = "user://world";
	double autosave_interval = 30.0;
	Ref<ShaderMaterial> material;
	Ref<ShaderMaterial> prepass_material;
	Array extra_materials;
	int64_t edit_serial = 0;
	// material, prepass_material and extra_materials (the valid ones).
	std::vector<Ref<ShaderMaterial>> clip_materials() const;
	int clip_size = 256;
	int clip_levels = 3;
	// 109, 218, 436, 872, 1744, 3488, 6976, 13952
	double view_distance = 872.0;
	int thread_count = 0; // 0 = one per CPU core
	double simulation_radius = 160.0;
	int blocky_size = 0;
	// Blocky mode (see set_blocky_size): makes each block the tool changed
	// (after != before) one value, its average.
	void snap_to_blocks(const Vector3i &lo, const Vector3i &hi, const Vector3i &rlo, const Vector3i &n,
			const std::vector<float> &before, std::vector<float> &after, std::vector<MaterialId> &materials_after) const;
	int frames_since_unload = 0;
	// See take_view_times(): per part, the slowest since the last take.
	enum ViewPart { VIEW_FOLLOW, VIEW_FIX, VIEW_DISTANCES, VIEW_UPLOAD, VIEW_LOAD, VIEW_UNLOAD, VIEW_BODIES, VIEW_SELECTION, VIEW_SAVE, VIEW_PART_COUNT };
	double view_times[VIEW_PART_COUNT] = {};
	int64_t layers_uploaded = 0;
	// The upload's parts: this frame's (summed over levels), and the slowest
	// frame's since take_view_times().
	enum UploadPart { UPLOAD_BRICKS, UPLOAD_PACK, UPLOAD_GPU, UPLOAD_BIND, UPLOAD_OTHER, UPLOAD_PART_COUNT };
	double upload_ms[UPLOAD_PART_COUNT] = {};
	double upload_ms_max[UPLOAD_PART_COUNT] = {};
	// apply_brush()'s parts: summed over this frame's dabs, and the slowest
	// frame's since take_view_times().
	enum BrushPart { BRUSH_T_READ, BRUSH_T_BRUSH, BRUSH_T_MASK, BRUSH_T_REDISTANCE, BRUSH_T_WRITE, BRUSH_T_REFRESH, BRUSH_T_MARK, BRUSH_PART_COUNT };
	double brush_frame_ms[BRUSH_PART_COUNT] = {};
	double brush_max_ms[BRUSH_PART_COUNT] = {};
	int brush_frame_dabs = 0, brush_max_dabs = 0;
	std::chrono::steady_clock::time_point brush_t;
	void brush_lap(BrushPart p_part) {
		const auto now = std::chrono::steady_clock::now();
		brush_frame_ms[p_part] += std::chrono::duration<double, std::milli>(now - brush_t).count();
		brush_t = now;
	}
	// Color painting (see paint_color()).
	std::vector<Color> material_colors;
	// Per chunk (wrapped) the current stroke has painted: how much coverage
	// each point has had so far, 0..255.
	std::unordered_map<Vector3i, std::vector<uint8_t>, Vector3iHash> stroke_coverage;
	// A point's material when the current stroke began (the undo recording
	// has it if the stroke changed its chunk; otherwise it's unchanged).
	MaterialId stroke_start_material(int x, int y, int z, MaterialId p_now) const;
	// The same for its density (Layer measures from it).
	float stroke_start_density(int x, int y, int z, float p_now) const;
	// The brushes' exact (unrounded) values this stroke, per chunk (wrapped;
	// NaN: none): each dab carries on from them rather than from the stored
	// 1/16-voxel steps, so weak dabs add up exactly instead of rounding (and
	// random rounding piling up into noise, as low-power smoothing did).
	std::unordered_map<Vector3i, std::vector<float>, Vector3iHash> stroke_exact;
	void read_exact(const Vector3i &p_lo, const Vector3i &p_hi, const Vector3i &p_n, std::vector<float> &r_values) const;
	// Physics (see set_gravity()).
	double gravity = 360.0;
	bool physics_enabled = true;
	static SculptTerrain *singleton;
	bool stroke_held = false;
	bool flatten_plane_set = false;
	Vector3 flatten_point, flatten_normal;
	int64_t undo_serial = 0;
	Ref<DensityShape> extrude_shape; // the selection when the extrude began, centred on extrude_center
	Vector3 extrude_center;
	std::unordered_map<Vector3i, std::vector<uint64_t>, Vector3iHash> selection;
	int64_t selected_count = 0;
	int64_t selection_serial = 0;
	Vector3i selection_shift; // sum of the origin shifts (for get_selection_state)
	bool selection_changed = false;
	Vector3 selection_center;
	Ref<Texture2DArray> selection_texture;
	static constexpr int MASK_SIZE = 128;
	bool is_selected(int x, int y, int z) const;
	void set_selected(int x, int y, int z, bool p_on);
	template <typename F>
	void for_selected(F &&p_f) const;
	bool selection_bounds(Vector3i &r_lo, Vector3i &r_hi) const;
	int flood_select(const Vector3 &p_from, double p_radius, double p_threshold, bool p_by_color, bool p_add);
	// Undoes what an edit did to points the selection doesn't allow (see
	// above); r_grown gets the points it may fill, to select them.
	void mask_to_selection(const Vector3i &lo, const Vector3i &hi, const Vector3i &rlo, const Vector3i &n,
			const std::vector<float> &before, std::vector<float> &after, const std::vector<MaterialId> &materials_before,
			std::vector<MaterialId> &materials_after, bool p_any_air, std::vector<Vector3i> &r_grown) const;
	void select_grown(const std::vector<Vector3i> &p_points);
	void update_selection_mask();
	void shift_selection(const Vector3i &p_offset);
	// Lattice points physics has read (see cached_point()).
	struct CachedPoint {
		int32_t x = 0, y = 0, z = 0;
		uint32_t generation = 0; // valid if cache_generation
		int8_t value = 0;
		uint8_t level = 0; // clipmap level the value is at (see clear_distance())
	};
	std::vector<CachedPoint> point_cache;
	uint32_t cache_generation = 1;
	static constexpr size_t POINT_CACHE_SIZE = 1u << 20;
	int8_t cached_point(int x, int y, int z, int p_level = 0);
	float cached_sample(const Vector3 &p_position);
	// Wakes sleeping bodies near what an edit (or undo) changed.
	void mark_edited(const Vector3i &p_min, const Vector3i &p_max);
	// Sends the drawn bodies nearest p_camera to `material`.
	void bind_bodies(const Vector3 &p_camera);
	uint64_t preview_body = 0;
	Ref<ShaderMaterial> body_material;
	int preview_bone = -1;
	Ref<Texture2DArray> body_atlas;
	struct GridSlot {
		uint64_t shape = 0; // its instance id
		int key = -1; // frame
		int64_t version = -1;
		uint64_t used = 0;
		int base = 0, count = 0; // its layers
	};
	std::vector<GridSlot> grid_slots;
	uint64_t slot_clock = 0;
	uint64_t frame_clock = 0; // slot_clock when this frame's binding began
	// The first atlas layer holding p_body's p_shape's key p_key, uploading
	// it if it isn't there or is out of date; -1 if there's no room (what
	// this frame already uses stays).
	int grid_slot(DensityBody3D *p_body, const Ref<DensityShape> &p_shape, int p_key);
	// Milliseconds a frame may spend loading chunks around the camera.
	static constexpr double LOAD_BUDGET_MS = 3.0;

	VoxelWorld world;
	bool opened = false;
	Clipmap clipmap;
	// Every level's texels in one texture: layer l * clip_size + z holds slice z
	// of level l. (One sampler, not one a level: a phone's GPU may have only 16
	// texture units, and Godot binds its own textures to the top ones.)
	Ref<Texture2DArray> clip_texture;
	Ref<Texture2DArray> brick_texture; // all levels' brick distances (see Clipmap)
	int64_t upload_bytes = 0;
	// Time spent uploading texture layers (z slices) a frame, roughly: at
	// least one batch goes up each frame, then more while under this.
	static constexpr double UPLOAD_BUDGET_MS = 2.5;
	// Layers packed side by side (on worker threads) before going up.
	static constexpr int UPLOAD_BATCH = 8;
	std::chrono::steady_clock::time_point last_save = std::chrono::steady_clock::now();
	uint32_t brush_strokes = 0; // varies the rounding dither between strokes

	int worker_count() const;
	// Uploads a level's changed layers until p_deadline (all, if it has no
	// texture yet; at least one batch a frame: r_uploaded says whether one
	// went up yet) and publishes its new origin once none are left. Returns
	// whether the material needs updating.
	// p_min_priority: only layers marked at least this (see Clipmap::Level::layer_dirty).
	bool upload_level(int p_level, std::chrono::steady_clock::time_point p_deadline, bool &r_uploaded, uint8_t p_min_priority);
	// Sends the textures and level placement to the material.
	void bind_material();
	void report_world_error(const String &p_what);
	// Writes the edit box [lo, hi] of a brush's results (`after`, over the
	// read box starting at rlo, size n) back to the world and the clipmap.
	void commit_edit(const Vector3i &lo, const Vector3i &hi, const Vector3i &rlo, const Vector3i &n,
			const std::vector<float> &before, const std::vector<float> &after, const std::vector<uint8_t> &brushed, bool p_dither,
		const std::vector<MaterialId> &materials_before, const std::vector<MaterialId> &materials_after);
	// A stroke's undo entry: each chunk it changed, as it was before (or,
	// on the redo stack, as it was after). Undo and redo swap them in.
	// A stroke's undo step: per chunk, the points it changed, with their
	// values before and after. Undo puts back only points still as the stroke
	// left them, and redo only ones still as they were before it, so undoing
	// your own stroke never wipes out edits made since by someone else
	// (multiplayer). Alone it's the same as swapping whole chunks: strokes
	// undo newest first.
	struct UndoChunk {
		Vector3i chunk;
		std::vector<int8_t> density; // while recording: the whole chunk as it was before the stroke
		std::vector<MaterialId> materials;
		std::vector<uint16_t> points; // when done: the points the stroke changed...
		std::vector<int8_t> density_before, density_after; // ... and their values
		std::vector<MaterialId> material_before, material_after;
	};
	struct UndoStep {
		std::vector<UndoChunk> chunks;
		std::unordered_map<Vector3i, size_t, Vector3iHash> index; // chunk -> position in `chunks` (while recording)
		size_t bytes() const {
			size_t total = 0;
			for (const UndoChunk &c : chunks) {
				total += c.density.size() * (1 + sizeof(MaterialId)) + c.points.size() * (2 + 2 + 2 * sizeof(MaterialId)) + 64;
			}
			return total;
		}
	};
	// Turns the recorded chunks into the points the stroke changed.
	void finish_step(UndoStep &r_step);
	static constexpr int MAX_UNDO_STEPS = 100;
	static constexpr size_t MAX_UNDO_BYTES = 256u << 20;
	std::vector<UndoStep> undo_stack, redo_stack; // most recent last
	UndoStep recording;
	size_t undo_bytes = 0;
	// Records chunk p_chunk's data (about to change) into the stroke.
	void record_chunk(const Vector3i &p_chunk, const VoxelWorld::ChunkData &p_data);
	// Undoes (p_undo) or redoes p_step's points, refreshing the view.
	void swap_step(UndoStep &r_step, bool p_undo);
	// Grab: the terrain as it was when the grab began, captured chunk by chunk.
	struct SnapshotChunk {
		std::vector<float> density;
		std::vector<MaterialId> materials;
	};
	std::unordered_map<Vector3i, SnapshotChunk, Vector3iHash> grab_snapshot;
	Vector3i grab_lo, grab_hi; // last frame's rewritten box
	bool grab_has_box = false;
	void capture_snapshot(const Vector3i &p_min, const Vector3i &p_max);
	float snapshot_point(int x, int y, int z) const;
	MaterialId snapshot_material(int x, int y, int z) const;
};

} // namespace godot

VARIANT_ENUM_CAST(SculptTerrain::BrushMode);
VARIANT_ENUM_CAST(SculptTerrain::WorldType);
