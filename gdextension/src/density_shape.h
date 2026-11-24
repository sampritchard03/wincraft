#pragma once

#include "voxel_world.h"

#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/basis.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/vector4.hpp>

#include <cstdint>
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
// them (1.5 = halfway from key 1 to key 2), and a brush edits all of them
// at once (or one: see DensityBody3D.edit_key). bake_meshes() makes keys
// from triangle meshes (e.g. an animated mesh's poses).
class DensityShape : public Resource {
	GDCLASS(DensityShape, Resource)

public:
	static constexpr int MAX_SIZE = 64; // the drawing atlas's slot (SculptTerrain::GRID_SLOT)

	// Saved form: densities as signed bytes (1/16 voxel), materials as
	// 16-bit ids (little endian).
	void set_size(const Vector3i &p_size);
	Vector3i get_size() const { return size; }
	void set_origin(const Vector3 &p_origin) { origin = p_origin; }
	Vector3 get_origin() const { return origin; }
	void set_voxel_size(double p_value) { voxel_size = std::max(p_value, 0.01); }
	double get_voxel_size() const { return voxel_size; }
	void set_keys(const Array &p_keys);
	Array get_keys() const;
	void set_materials(const PackedByteArray &p_bytes);
	PackedByteArray get_materials() const;
	void set_edited(bool p_value) { edited = p_value; }
	bool is_edited() const { return edited; }
	int get_key_count() const { return (int)keys.size(); }

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
	double get_density(const Vector3 &p_local, double p_pose) const;
	Vector3 get_normal(const Vector3 &p_local, double p_pose) const;
	int get_material_at(const Vector3 &p_local) const;
	// How heavy the material at p_local is (a colour 1, stone heavier...).
	double get_material_density_at(const Vector3 &p_local) const;
	// Sculpting, in the body's space: as SculptTerrain.apply_brush() and
	// paint_color(). p_key: which key (-1: all). Adding near the edge grows
	// the grid (up to MAX_SIZE). Returns whether anything changed.
	bool apply_brush(const Vector3 &p_center, double p_radius, double p_strength, int p_mode, int p_material, int p_key = -1);
	bool paint_color(const Vector3 &p_center, double p_radius, const Color &p_color, double p_opacity, double p_hardness);
	// Grab and snake hook, as SculptTerrain.warp() (one centre, body space):
	// begin_grab() keeps the shape as it is; each warp() with p_grab moves it
	// by p_delta (the whole move since then) from that; a snake hook step
	// pulls the current shape along by p_delta.
	void begin_grab();
	bool warp(const Vector3 &p_center, const Vector3 &p_delta, double p_radius, bool p_grab, int p_key = -1);
	void end_grab();
	// How much solid it has (key 0), in cubic voxels of its space.
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
	// Collision probes of key p_key: points just inside the surface, each a
	// sphere (xyz in body space, w radius).
	const std::vector<Vector4> &probes(int p_key) const;
	// All of key p_key's skin points (probes() takes every n-th of them).
	std::vector<Vector4> skin_points(int p_key) const;
	// Solid points of key 0: their centroid, and the inertia tensor per unit
	// mass about it (body space).
	void mass_properties(Vector3 &r_centroid, Basis &r_inertia) const;
	// One z slice of key p_key for the atlas: MAX_SIZE^2 RGB8 texels like
	// the clipmap's (R density + 128, G B material, see render_bytes()).
	void fill_slice(int p_key, int p_z, uint8_t *r_texels) const;
	// Keys around a pose and the blend between them.
	void pose_keys(double p_pose, int &r_a, int &r_b, float &r_t) const;
	// Replaces everything with one key (distances are repaired).
	void init_grid(const Vector3i &p_size, const Vector3 &p_origin, double p_voxel_size, std::vector<float> &&p_density, std::vector<MaterialId> &&p_materials);

protected:
	static void _bind_methods();

private:
	Vector3i size;
	Vector3 origin;
	double voxel_size = 1.0;
	std::vector<std::vector<float>> keys; // densities, voxels of the grid
	std::vector<MaterialId> materials;
	bool edited = false;
	int64_t version = 0;
	std::vector<std::vector<float>> grab_keys; // the keys when a grab began
	std::vector<MaterialId> grab_materials;
	mutable std::vector<std::vector<Vector4>> probe_cache;
	mutable int64_t probe_version = -1;

	size_t count() const { return (size_t)size.x * size.y * size.z; }
	size_t index(int x, int y, int z) const { return ((size_t)z * size.y + y) * size.x + x; }
	// Trilinear density of one key at a grid position (voxels from the
	// first point), and beyond the grid, the edge's value minus how far out.
	float sample_key(int p_key, const Vector3 &p_grid) const;
	float sample_pose(const Vector3 &p_grid, double p_pose) const;
	// Makes the grid cover [p_lo, p_hi] (grid points, may be outside it now),
	// as far as MAX_SIZE allows, keeping what's there.
	void grow(Vector3i p_lo, Vector3i p_hi);
	void changed();
};

} // namespace godot
