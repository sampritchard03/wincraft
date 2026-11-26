#pragma once

#include "density_shape.h"

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/variant/basis.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/packed_vector4_array.hpp>
#include <godot_cpp/variant/transform3d.hpp>

#include <cstdint>
#include <vector>

namespace godot {

class SculptTerrain;

// A physics body that collides through the terrain's density field
// (SculptTerrain.get_density()) rather than collision meshes: no Godot
// physics at all. Its shape is either
//   - `shape`, a small density grid of its own (DensityShape): drawn by the
//     terrain's raymarcher, sculpted with the terrain's brushes
//     (apply_brush()), posed by `pose` (its keys, like shape keys); or
//   - without one, a cluster of spheres (`spheres`, each Vector4(x, y, z,
//     radius) in the node's own space), e.g. the player's capsule.
// Either way it collides through spheres, which is all a density field can
// answer cheaply: a sphere at c with radius r is inside the ground by
// r + density(c), pushed out along the density's gradient. A grid's spheres
// ("probes") are points of its skin. Against another body, each probe asks
// that body's own density (distance_at()).
//
// SculptTerrain steps every body in the tree once per physics frame (in its
// own _physics_process, which runs after its parent's): gravity, the
// terrain, and the other bodies. Move one yourself by setting its position
// (that counts as a teleport and wakes it) or its velocities, or by applying
// impulses and forces. A body with `drawn` on is drawn by the terrain's own
// raymarching shader, sphere by sphere, lit and shadowed like the ground.
//
// A frozen body doesn't move by itself, but others bounce off it as if it
// moved at its linear_velocity (DensityCharacter3D, the player, is one).
class DensityBody3D : public Node3D {
	GDCLASS(DensityBody3D, Node3D)
	friend class DensityCharacter3D; // reads other bodies' spheres to stand on them

public:
	struct Contact {
		Vector3 position;
		Vector3 normal; // away from what was hit
		uint64_t body = 0; // the other body's instance id, 0 for the terrain
		int sphere = 0; // which of ours
	};

	DensityBody3D();
	~DensityBody3D();

	void set_spheres(const PackedVector4Array &p_spheres);
	PackedVector4Array get_spheres() const { return spheres; }
	void set_mass(double p_value);
	double get_mass() const { return mass; }
	void set_bounce(double p_value) { bounce = p_value; }
	double get_bounce() const { return bounce; }
	void set_friction(double p_value) { friction = p_value; }
	double get_friction() const { return friction; }
	void set_gravity_scale(double p_value) { gravity_scale = p_value; }
	double get_gravity_scale() const { return gravity_scale; }
	void set_linear_damp(double p_value) { linear_damp = p_value; }
	double get_linear_damp() const { return linear_damp; }
	void set_angular_damp(double p_value) { angular_damp = p_value; }
	double get_angular_damp() const { return angular_damp; }
	// Extra angular damping while touching something, so rolling things
	// come to rest (a ball on flat ground would otherwise roll for ages).
	void set_rolling_resistance(double p_value) { rolling_resistance = p_value; }
	double get_rolling_resistance() const { return rolling_resistance; }
	void set_center_of_mass(const Vector3 &p_value);
	Vector3 get_center_of_mass() const { return center_of_mass; }
	void set_freeze(bool p_value);
	bool is_freeze() const { return freeze; }
	void set_can_sleep(bool p_value);
	bool is_able_to_sleep() const { return can_sleep; }
	void set_sleeping(bool p_value);
	bool is_sleeping() const { return sleeping; }
	void set_collide_with_bodies(bool p_value) { collide_with_bodies = p_value; }
	bool get_collide_with_bodies() const { return collide_with_bodies; }
	void set_linear_velocity(const Vector3 &p_value);
	Vector3 get_linear_velocity() const { return linear_velocity; }
	void set_angular_velocity(const Vector3 &p_value);
	Vector3 get_angular_velocity() const { return angular_velocity; }
	void set_drawn(bool p_value) { drawn = p_value; }
	bool is_drawn() const { return drawn; }
	void set_color(const Color &p_value) { color = p_value; }
	Color get_color() const { return color; }
	// A block's texture layer (SculptTerrain.get_material_names() index) to
	// draw the spheres with; -1 = plain `color`. (A shape has its own.)
	void set_material(int p_value) { material = p_value; }
	int get_material() const { return material; }
	void set_shape(const Ref<DensityShape> &p_shape);
	Ref<DensityShape> get_shape() const { return shape; }
	// Which of the shape's keys it looks (and collides) like: 0 = the first,
	// 1.25 = a quarter of the way from key 1 to key 2. Animate it like any
	// property (an AnimationPlayer track on "pose").
	void set_pose(double p_value);
	double get_pose() const { return pose; }
	// Keys weighed in on top of the pose, one weight per key (0: none, 1:
	// all of it; see DensityShape): several at once add up. Also properties
	// "key_weight/0", "key_weight/1"... (one per key of the shape) to
	// animate each on a track of its own.
	void set_key_weights(const PackedFloat32Array &p_weights);
	PackedFloat32Array get_key_weights() const { return key_weights; }
	void set_key_weight(int p_key, double p_weight);
	double get_key_weight(int p_key) const;
	// The shape's centre of mass is its solid's centroid (off: center_of_mass).
	void set_auto_center_of_mass(bool p_value);
	bool is_auto_center_of_mass() const { return auto_center_of_mass; }
	// Which key brushes edit: -1 = all of them (the usual: sculpting a frog
	// changes the shape itself, where the brush is on it in the pose shown,
	// so it shows in every pose), or one, to sculpt a single pose (grab and
	// snake hook move its parts; see DensityShape).
	void set_edit_key(int p_value) { edit_key = p_value; }
	int get_edit_key() const { return edit_key; }
	// A shape's mass, bounce and friction come from its materials (stone
	// heavy, sponge bouncy...: DensityShape.get_mass()); off: the properties.
	void set_auto_material_properties(bool p_value);
	bool is_auto_material_properties() const { return auto_material_properties; }
	// Whether raycast_bodies() finds it (brushes, the physics gun). Off for
	// previews.
	void set_pickable(bool p_value) { pickable = p_value; }
	bool is_pickable() const { return pickable; }
	// After cutting: the parts no longer joined to the biggest, as shapes in
	// this body's space (this one keeps the biggest). Make bodies of them
	// with this body's transform.
	Array split_disconnected();

	// Sculpting the shape, in world space (as SculptTerrain's brushes). The
	// first edit gives the body its own copy of a shared shape.
	bool apply_brush(const Vector3 &p_center, double p_radius, double p_strength, int p_mode, int p_material = -1);
	bool paint_color(const Vector3 &p_center, double p_radius, const Color &p_color, double p_opacity, double p_hardness);
	// Grab and snake hook on the shape, in world space (as
	// SculptTerrain.warp(), one centre): begin_grab(), then warp() each frame.
	void begin_grab();
	bool warp(const Vector3 &p_center, const Vector3 &p_delta, double p_radius, bool p_grab);
	// The same with a centre and delta per symmetry copy (world space).
	bool warp_copies(const PackedVector3Array &p_centers, const PackedVector3Array &p_deltas, double p_radius, bool p_grab);
	void end_grab();
	// Where a ray first enters the body, or null.
	Variant raycast(const Vector3 &p_from, const Vector3 &p_direction, double p_max_distance);
	// The nearest body (not characters) a ray hits within p_max_distance:
	// {body, position, distance}, or empty.
	static Dictionary raycast_bodies(const Vector3 &p_from, const Vector3 &p_direction, double p_max_distance);
	// How far inside the body a world point is (negative: outside), and the
	// way out there.
	double distance_at(const Vector3 &p_point, Vector3 &r_normal);
	// The same without the normal (cheaper), and the normal alone.
	double density_at(const Vector3 &p_point);
	Vector3 normal_at(const Vector3 &p_point);
	double get_density_at(const Vector3 &p_point);
	// Reach of its shape from the centre of mass (for drawing and culling).
	double get_bound_radius();

	// As RigidBody3D's. p_position (apply_impulse / apply_force): where,
	// relative to the body's origin, in global orientation.
	void apply_central_impulse(const Vector3 &p_impulse);
	void apply_impulse(const Vector3 &p_impulse, const Vector3 &p_position = Vector3());
	void apply_torque_impulse(const Vector3 &p_impulse);
	// Forces last for the next physics step only (call every frame to keep pushing).
	void apply_central_force(const Vector3 &p_force);
	void apply_force(const Vector3 &p_force, const Vector3 &p_position = Vector3());
	void apply_torque(const Vector3 &p_torque);
	// Contacts during the last physics step (with the terrain or bodies);
	// a sleeping body keeps the ones it fell asleep with.
	int get_contact_count() const { return (int)contacts.size(); }
	Vector3 get_contact_position(int p_index) const;
	Vector3 get_contact_normal(int p_index) const;
	// The other body, or null for the terrain.
	Object *get_contact_body(int p_index) const;
	bool is_touching_terrain() const;
	// Where the centre of mass is now, in world space.
	Vector3 get_global_center_of_mass() const;

	// The grid it draws (its shape).
	Ref<DensityShape> get_drawn_shape() { return shape; }

	// --- Used by SculptTerrain ---------------------------------------------
	// Every body in the tree (any viewport).
	static std::vector<DensityBody3D *> &all();
	// Advances every body by p_delta seconds.
	static void step_all(SculptTerrain *p_terrain, double p_delta, const Vector3 &p_gravity);
	// Wakes sleeping bodies within reach of an edited box (x and z compared
	// the short way round a world p_world_size wide).
	static void wake_near(const Vector3 &p_min, const Vector3 &p_max, int p_world_size);
	// Moves every body by p_offset (see SculptTerrain.shift_origin()).
	static void shift_all(const Vector3 &p_offset);

protected:
	static void _bind_methods();
	bool _set(const StringName &p_name, const Variant &p_value);
	bool _get(const StringName &p_name, Variant &r_ret) const;
	void _get_property_list(List<PropertyInfo> *p_list) const;
	void _notification(int p_what);

	PackedVector4Array spheres;
	double mass = 1.0;
	double bounce = 0.5;
	double friction = 0.5;
	double gravity_scale = 1.0;
	double linear_damp = 0.0;
	double angular_damp = 0.5;
	double rolling_resistance = 1.0;
	Vector3 center_of_mass;
	bool freeze = false;
	bool can_sleep = true;
	bool sleeping = false;
	bool collide_with_bodies = true;
	Vector3 linear_velocity;
	Vector3 angular_velocity;
	bool drawn = false;
	Color color = Color(1, 1, 1);
	int material = -1;

	Ref<DensityShape> shape;
	double pose = 0.0;
	bool auto_center_of_mass = true;
	bool auto_material_properties = true;
	bool pickable = true;
	int edit_key = -1;
	bool own_shape = false;
	int64_t shape_version = -1; // the shape's, when the probes were made
	int probe_frame = -1; // the shape frame the probes are of
	PackedFloat32Array key_weights;
	// The shape frame nearest how it looks now (see DensityShape::pose_frames()).
	int nearest_frame() const;
	bool stepped = false; // the step state below has been filled in
	Vector3 com_local; // centre of mass in the node's space (unscaled)

	Vector3 force, torque; // for the next step
	std::vector<Contact> contacts;

	// The state a step works on, read from the node's transform at the start
	// and written back at the end.
	Basis rotation; // orthonormal
	double scale = 1.0; // uniform
	Vector3 com; // world
	std::vector<Vector3> local_offsets; // sphere centres relative to the centre of mass, scaled, body space
	std::vector<float> local_radii; // scaled
	// The spheres in up to 8 groups (by octant round the centre of mass,
	// stored together): a group whose bounding sphere is clear of the
	// ground is skipped with one density lookup.
	struct SphereGroup {
		Vector3 center; // relative to the centre of mass, scaled, body space
		float radius = 0.0f;
		int begin = 0, end = 0;
	};
	std::vector<SphereGroup> groups;
	std::vector<Vector3> world_centers;
	std::vector<float> world_radii;
	float bound = 0.0f; // reach from the centre of mass
	float smallest = 1.0f; // smallest sphere radius
	Basis inverse_inertia_local; // about the centre of mass, scaled
	bool inertia_dirty = true;
	// A capsule (DensityCharacter3D): other bodies' distances to it are worked
	// out from it exactly (capsule_radius > 0), not from its spheres, of which
	// a big one has thousands. Its axis, node space.
	double capsule_radius = 0.0;
	Vector3 capsule_a, capsule_b;
	Transform3D written; // what the last step set, to spot teleports
	bool has_written = false;
	double still_time = 0.0;

	bool dynamic() const { return !freeze; }
	bool moving() const { return !freeze && !sleeping; }
	void wake();
	Basis inverse_inertia_world() const;
	void update_inertia();
	// Groups the sphere offsets by octant and measures their reach.
	void finish_offsets();
	// A world point in the body's own space (from the step state).
	Vector3 to_local(const Vector3 &p_point) const;
	// Reads the node's transform into the step state (p_teleports: a
	// transform that isn't what the last step wrote wakes it).
	void begin_step(bool p_teleports = true);
	bool shape_changed() const;
	void place_spheres();
	void end_step(double p_delta);
	void integrate(double p_h, const Vector3 &p_gravity);
	void collide_terrain(SculptTerrain *p_terrain, double p_rest_speed);
	void add_contact(const Vector3 &p_position, const Vector3 &p_normal, uint64_t p_body, int p_sphere);
	// How far inside the capsule (capsule_radius) a world point is, and the way out.
	double capsule_inside(const Vector3 &p_point, Vector3 &r_normal) const;
	// Velocity of a world point moving with the body.
	Vector3 point_velocity(const Vector3 &p_point) const;
	void push(const Vector3 &p_impulse, const Vector3 &p_point);
	// An impulse's effect on the velocity along p_dir at p_point, per unit.
	double response(const Vector3 &p_point, const Vector3 &p_dir) const;
	// p_separate: also push them apart (else the caller does it once for
	// many contacts, with separate()).
	static void resolve(DensityBody3D *a, DensityBody3D *b, const Vector3 &p_point, const Vector3 &p_normal, double p_depth,
			double p_rest_speed, int p_sphere_a, int p_sphere_b, bool p_separate = true);
	static void separate(DensityBody3D *a, DensityBody3D *b, const Vector3 &p_normal, double p_depth);
};

// The player: a frozen DensityBody3D shaped like a capsule (spheres along
// its axis, its feet at the origin) with CharacterBody3D's way of moving:
// set `velocity`, call move_and_slide() in _physics_process. It slides
// along the terrain and can stand on (but not be pushed by) other bodies;
// they bounce off it.
class DensityCharacter3D : public DensityBody3D {
	GDCLASS(DensityCharacter3D, DensityBody3D)

public:
	DensityCharacter3D();

	void set_radius(double p_value);
	double get_radius() const { return radius; }
	void set_height(double p_value);
	double get_height() const { return height; }
	void set_velocity(const Vector3 &p_value) { velocity = p_value; }
	Vector3 get_velocity() const { return velocity; }
	void set_floor_max_angle(double p_value) { floor_max_angle = p_value; }
	double get_floor_max_angle() const { return floor_max_angle; }
	void set_floor_snap_length(double p_value) { floor_snap_length = p_value; }
	double get_floor_snap_length() const { return floor_snap_length; }
	// Ledges up to this high are stepped onto while walking into them.
	void set_step_height(double p_value) { step_height = std::max(p_value, 0.0); }
	double get_step_height() const { return step_height; }

	// Moves by velocity * the physics frame time, pushed out of the ground
	// and sliding along it. Returns whether it touched anything.
	bool move_and_slide();
	bool is_on_floor() const { return on_floor; }
	bool is_on_wall() const { return on_wall; }
	bool is_on_ceiling() const { return on_ceiling; }
	Vector3 get_floor_normal() const { return floor_normal; }

protected:
	static void _bind_methods();

private:
	double radius = 4.0;
	double height = 16.0;
	Vector3 velocity;
	double floor_max_angle = 0.785398;
	double floor_snap_length = 4.0;
	double step_height = 3.0;
	double standing_height = 0.0; // feet height when last on the floor
	bool on_floor = false, on_wall = false, on_ceiling = false;
	Vector3 floor_normal = Vector3(0, 1, 0);

	void rebuild_spheres();
	// The axis points (sphere centres) of the capsule with its feet at p_feet.
	int axis_count() const;
	Vector3 axis_point(const Vector3 &p_feet, int p_index) const;
	// How far the capsule's p_index-th sphere is from the nearest thing it
	// collides with (negative: that deep inside), and the way out.
	// p_bodies: other bodies count only from above (to stand on): those in
	// near_bodies, gathered once per move_and_slide().
	double gap(SculptTerrain *p_terrain, const Vector3 &p_center, double p_radius, bool p_bodies, Vector3 &r_normal) const;
	// The spheres it collides with (offset from the feet, radius); the
	// lowest first. See rebuild_spheres().
	std::vector<Vector4> samples;
	// Well under the ~7.9 voxels the terrain's density tells distances to: its
	// distances understate more the farther out they are, and bigger samples
	// (7 was tried) left a big player standing several voxels above the ground.
	static constexpr double MAX_SAMPLE_RADIUS = 5.0;
	std::vector<DensityBody3D *> near_bodies;
	// The samples in patches (spatially, a few dozen whatever the size): a
	// patch whose bounding sphere the ground can't reach (by the terrain's
	// coarse data, SculptTerrain.clear_distance()) and no near body touches
	// is skipped whole. A big player's samples are mostly far from anything.
	struct Patch {
		Vector3 centre; // from the feet
		double reach = 0.0; // bounding radius of its samples
		int level = 0; // coarse level whose distances cover the reach (-1: always check)
		std::vector<int> members; // samples (small patches) or small patches (big ones)
	};
	// Two tiers: big patches (a fraction of the radius across, a few dozen)
	// hold small ones (~SMALL_PATCH voxels), which hold the samples. A big
	// one clear of the ground skips all of its small ones; on the ground,
	// only the small ones near it check their samples.
	static constexpr double SMALL_PATCH = 12.0;
	std::vector<Patch> patches; // big
	std::vector<Patch> small_patches;
	// Remembered checks (worst_gap()): for each patch (big, then small) and
	// sample, where the feet were and how far they may move before it needs
	// checking again (<= 0: check).
	mutable std::vector<float> patch_margin, sample_margin;
	mutable std::vector<Vector3> patch_feet, sample_feet;
	mutable int64_t remembered_serial = -1;
	void forget_checks();
	void build_patches();
	// The smallest gap() of the capsule's samples at p_feet (and that one's
	// normal), skipping patches that are certainly clear: then it's at least
	// CLEAR_GAP.
	static constexpr double CLEAR_GAP = 0.5;
	double worst_gap(SculptTerrain *p_terrain, const Vector3 &p_feet, Vector3 &r_normal) const;
};

} // namespace godot
