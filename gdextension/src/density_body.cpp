#include "density_body.h"

#include "sculpt_terrain.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/object.hpp>

#include <algorithm>
#include <cmath>

using namespace godot;

namespace {

// How far a body may sink into what it touches before being pushed out:
// a little, so resting contacts stay touching from one step to the next.
constexpr double SLOP = 0.05;
// Below this approach speed (voxels per second, at least) a contact doesn't
// bounce: things come to rest instead of buzzing.
constexpr double REST_SPEED_MIN = 4.0;
// A step is split so nothing moves more than this fraction of its smallest
// sphere's radius at a time (and can't pass through thin ground), into at
// most MAX_SUBSTEPS.
constexpr double SUBSTEP_TRAVEL = 0.4;
constexpr int MAX_SUBSTEPS = 12;
// A body this slow (voxels / radians per second) for SLEEP_TIME seconds
// while touching something stops being simulated until something wakes it.
constexpr double SLEEP_LINEAR = 3.0;
constexpr double SLEEP_ANGULAR = 0.6;
constexpr double SLEEP_TIME = 0.6;
// A sleeping body woken by a contact must be hit at least this fast, or
// pushed in this deep, so one at rest against something can stay asleep.
constexpr double WAKE_SPEED = 1.0;
constexpr int MAX_CONTACTS = 16;
// The player counts as standing when its lowest sphere is this close to a floor.
constexpr double SKIN = 0.3;
// The player's mass per cubic voxel: about what bodies made from the ground
// weigh (DensityShape's MASS_PER_VOXEL), so pushing a boulder the size of
// it is hard work and a pebble isn't.
constexpr double PLAYER_MASS_PER_VOXEL = 0.01;

// The player (a frozen capsule) walking into something pushes it itself,
// in DensityCharacter3D::push_bodies(), as hard as it can.
bool is_character(const DensityBody3D *p_body) {
	return p_body != nullptr && p_body->is_freeze() && Object::cast_to<DensityCharacter3D>(const_cast<DensityBody3D *>(p_body)) != nullptr;
}

Basis zero_basis() {
	return Basis(Vector3(), Vector3(), Vector3());
}

} // namespace

// --- DensityBody3D -----------------------------------------------------------------

std::vector<DensityBody3D *> &DensityBody3D::all() {
	static std::vector<DensityBody3D *> bodies;
	return bodies;
}

DensityBody3D::DensityBody3D() {
	spheres.push_back(Vector4(0, 0, 0, 2));
}

DensityBody3D::~DensityBody3D() {
	auto &list = all();
	list.erase(std::remove(list.begin(), list.end(), this), list.end());
}

void DensityBody3D::_notification(int p_what) {
	auto &list = all();
	if (p_what == NOTIFICATION_ENTER_TREE) {
		if (std::find(list.begin(), list.end(), this) == list.end()) {
			list.push_back(this);
		}
		has_written = false;
		world_centers.clear();
	} else if (p_what == NOTIFICATION_EXIT_TREE) {
		list.erase(std::remove(list.begin(), list.end(), this), list.end());
	}
}

void DensityBody3D::set_spheres(const PackedVector4Array &p_spheres) {
	spheres = p_spheres;
	inertia_dirty = true;
	world_centers.clear();
	wake();
}

void DensityBody3D::set_mass(double p_value) {
	mass = std::max(p_value, 1e-6);
	inertia_dirty = true;
}

void DensityBody3D::set_center_of_mass(const Vector3 &p_value) {
	center_of_mass = p_value;
	inertia_dirty = true;
}

void DensityBody3D::set_freeze(bool p_value) {
	freeze = p_value;
	wake();
}

void DensityBody3D::set_can_sleep(bool p_value) {
	can_sleep = p_value;
	if (!can_sleep) {
		wake();
	}
}

void DensityBody3D::set_sleeping(bool p_value) {
	if (p_value) {
		sleeping = true;
		linear_velocity = Vector3();
		angular_velocity = Vector3();
	} else {
		wake();
	}
}

void DensityBody3D::set_linear_velocity(const Vector3 &p_value) {
	linear_velocity = p_value;
	wake();
}

void DensityBody3D::set_angular_velocity(const Vector3 &p_value) {
	angular_velocity = p_value;
	wake();
}

void DensityBody3D::wake() {
	sleeping = false;
	still_time = 0.0;
}

bool DensityBody3D::shape_changed() const {
	if (shape.is_null()) {
		return false;
	}
	return shape->get_version() != shape_version || nearest_frame() != probe_frame;
}

int DensityBody3D::nearest_frame() const {
	if (shape.is_null() || shape->get_key_count() == 0) {
		return -1;
	}
	return get_blend().frames[0];
}

const DensityShape::Blend &DensityBody3D::get_blend() const {
	if (shape.is_null() || shape->get_key_count() == 0) {
		shown = DensityShape::Blend();
		return shown;
	}
	DensityShape::Blend want;
	shape->pose_blend(pose, key_weights, want);
	// Frames not worked out yet are worked out in the background; meanwhile
	// it looks as it did (or as much of the new look as is ready), rather
	// than stopping the game for them.
	DensityShape::Blend ready;
	ready.count = 0;
	float total = 0.0f;
	for (int i = 0; i < want.count; i++) {
		if (shape->frame_ready(want.frames[i])) {
			ready.frames[ready.count] = want.frames[i];
			ready.weights[ready.count] = want.weights[i];
			total += want.weights[i];
			ready.count++;
		} else {
			shape->request_frame(want.frames[i]);
		}
	}
	if (ready.count == want.count) {
		shown = want;
		shown_shape = shape.ptr();
		return shown;
	}
	bool old_ready = shown_shape == shape.ptr() && shown.count > 0;
	for (int i = 0; i < shown.count && old_ready; i++) {
		old_ready = shape->frame_ready(shown.frames[i]);
	}
	if (old_ready) {
		return shown;
	}
	if (ready.count > 0 && total > 1e-4f) {
		for (int i = 0; i < ready.count; i++) {
			ready.weights[i] /= total;
		}
		shown = ready;
	} else {
		shown = want; // nothing to show yet: worked out now, when drawn
	}
	shown_shape = shape.ptr();
	return shown;
}

void DensityBody3D::update_inertia() {
	inertia_dirty = false;
	local_offsets.clear();
	local_radii.clear();
	Basis inertia = zero_basis();
	if (shape.is_valid() && shape->get_key_count() > 0) {
		const DensityShape *grid = shape.ptr();
		// A grid: its solid's mass spread (key 0), its skin as probes.
		shape_version = grid->get_version();
		probe_frame = nearest_frame();
		if (auto_material_properties) {
			grid->physical_properties(mass, bounce, friction);
			mass *= scale * scale * scale;
		}
		Vector3 centroid;
		Basis unit;
		grid->mass_properties(centroid, unit);
		com_local = auto_center_of_mass ? centroid : center_of_mass;
		const Vector3 shift = (centroid - com_local) * scale; // parallel axis
		for (int a = 0; a < 3; a++) {
			for (int b = 0; b < 3; b++) {
				inertia.rows[a][b] = mass * (unit.rows[a][b] * scale * scale + (a == b ? shift.length_squared() : 0.0) - shift[a] * shift[b]);
			}
		}
		for (const Vector4 &p : grid->probes(probe_frame)) {
			local_offsets.push_back((Vector3(p.x, p.y, p.z) - com_local) * scale);
			local_radii.push_back(p.w * (float)scale);
		}
	} else {
		com_local = center_of_mass;
		double total = 0.0;
		for (int i = 0; i < (int)spheres.size(); i++) {
			total += std::pow(std::max((double)spheres[i].w, 1e-3) * scale, 3.0);
		}
		for (int i = 0; i < (int)spheres.size(); i++) {
			const Vector4 &s = spheres[i];
			const double r = std::max((double)s.w, 1e-3) * scale;
			const Vector3 d = (Vector3(s.x, s.y, s.z) - center_of_mass) * scale;
			local_offsets.push_back(d);
			local_radii.push_back((float)r);
			// Each sphere a solid ball (its share of the mass by volume), moved
			// out to where it sits (parallel axis theorem).
			const double m = mass * r * r * r / total;
			const double diagonal = 0.4 * m * r * r + m * d.length_squared();
			for (int a = 0; a < 3; a++) {
				for (int b = 0; b < 3; b++) {
					inertia.rows[a][b] += (a == b ? diagonal : 0.0) - m * d[a] * d[b];
				}
			}
		}
	}
	finish_offsets();
	inverse_inertia_local = std::abs(inertia.determinant()) > 1e-12 ? inertia.inverse() : Basis();
}

void DensityBody3D::finish_offsets() {
	// Group by octant (stored together, so a group is a range).
	{
		std::vector<std::pair<int, size_t>> order;
		for (size_t i = 0; i < local_offsets.size(); i++) {
			const Vector3 &o = local_offsets[i];
			order.push_back({ (o.x > 0.0f) | (o.y > 0.0f) << 1 | (o.z > 0.0f) << 2, i });
		}
		std::stable_sort(order.begin(), order.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
		std::vector<Vector3> offsets;
		std::vector<float> radii;
		groups.clear();
		for (size_t k = 0; k < order.size(); k++) {
			if (k == 0 || order[k].first != order[k - 1].first) {
				groups.push_back({ Vector3(), 0.0f, (int)k, (int)k });
			}
			offsets.push_back(local_offsets[order[k].second]);
			radii.push_back(local_radii[order[k].second]);
			groups.back().end = (int)k + 1;
		}
		local_offsets = std::move(offsets);
		local_radii = std::move(radii);
		for (SphereGroup &g : groups) {
			Vector3 sum;
			for (int i = g.begin; i < g.end; i++) {
				sum += local_offsets[i];
			}
			g.center = sum / (float)(g.end - g.begin);
			for (int i = g.begin; i < g.end; i++) {
				g.radius = std::max(g.radius, (local_offsets[i] - g.center).length() + local_radii[i]);
			}
		}
	}
	bound = 0.0f;
	smallest = 1e9f;
	for (size_t i = 0; i < local_offsets.size(); i++) {
		bound = std::max(bound, local_offsets[i].length() + local_radii[i]);
		smallest = std::min(smallest, local_radii[i]);
	}
	if (local_offsets.empty()) {
		smallest = 1.0f;
	}
	// Probes are small, but a grid body can't slip through much less than
	// about its own size in a substep: count its reach, not its probes.
	if (shape.is_valid()) {
		smallest = std::max(smallest, std::min(bound * 0.5f, 3.0f));
	}
}

Basis DensityBody3D::inverse_inertia_world() const {
	return rotation * inverse_inertia_local * rotation.transposed();
}

void DensityBody3D::begin_step(bool p_teleports) {
	const Transform3D t = get_global_transform();
	if (p_teleports && has_written && !t.is_equal_approx(written)) {
		wake(); // moved by something else: a teleport
	}
	const double s = t.basis.get_column(0).length();
	const double new_scale = s > 1e-6 ? s : 1.0;
	if (std::abs(new_scale - scale) > 1e-6) {
		scale = new_scale;
		inertia_dirty = true;
	}
	rotation = t.basis.orthonormalized();
	if (inertia_dirty || shape_changed()) {
		update_inertia();
	}
	com = t.origin + rotation.xform(com_local * scale);
	place_spheres();
	stepped = true;
}

void DensityBody3D::place_spheres() {
	const int count = (int)local_offsets.size();
	world_centers.resize(count);
	world_radii.resize(count);
	for (int i = 0; i < count; i++) {
		world_centers[i] = com + rotation.xform(local_offsets[i]);
		world_radii[i] = local_radii[i];
	}
}

void DensityBody3D::integrate(double p_h, const Vector3 &p_gravity) {
	linear_velocity += p_gravity * gravity_scale * p_h;
	if (linear_damp > 0.0) {
		linear_velocity *= std::max(0.0, 1.0 - linear_damp * p_h);
	}
	if (angular_damp > 0.0) {
		angular_velocity *= std::max(0.0, 1.0 - angular_damp * p_h);
	}
	com += linear_velocity * p_h;
	const double w = angular_velocity.length();
	if (w > 1e-9) {
		rotation = Basis(angular_velocity / w, w * p_h) * rotation;
		rotation.orthonormalize();
	}
	place_spheres();
}

Vector3 DensityBody3D::point_velocity(const Vector3 &p_point) const {
	return linear_velocity + angular_velocity.cross(p_point - com);
}

void DensityBody3D::push(const Vector3 &p_impulse, const Vector3 &p_point) {
	linear_velocity += p_impulse / mass;
	angular_velocity += inverse_inertia_world().xform((p_point - com).cross(p_impulse));
}

double DensityBody3D::response(const Vector3 &p_point, const Vector3 &p_dir) const {
	const Vector3 r = p_point - com;
	return 1.0 / mass + p_dir.dot(inverse_inertia_world().xform(r.cross(p_dir)).cross(r));
}

void DensityBody3D::add_contact(const Vector3 &p_position, const Vector3 &p_normal, uint64_t p_body, int p_sphere) {
	// One per sphere and thing touched (the latest), however many substeps.
	for (Contact &c : contacts) {
		if (c.body == p_body && c.sphere == p_sphere) {
			c.position = p_position;
			c.normal = p_normal;
			return;
		}
	}
	if ((int)contacts.size() < MAX_CONTACTS) {
		contacts.push_back({ p_position, p_normal, p_body, p_sphere });
	}
}

// A contact at p_point between a and b (null: the terrain), p_normal
// pointing from b towards a, p_depth deep: pushes them apart, then an
// impulse stops them approaching (bouncing back by their bounce, if fast
// enough) and friction resists sliding.
void DensityBody3D::separate(DensityBody3D *a, DensityBody3D *b, const Vector3 &p_normal, double p_depth) {
	const bool a_on = a->moving(), b_on = b != nullptr && b->moving();
	const double inv_a = a_on ? 1.0 / a->mass : 0.0, inv_b = b_on ? 1.0 / b->mass : 0.0;
	const double fix = std::max(p_depth - SLOP, 0.0);
	if (fix <= 0.0 || inv_a + inv_b <= 0.0) {
		return;
	}
	// Out of each other, the lighter one moving more.
	const double share = fix / (inv_a + inv_b);
	if (a_on) {
		a->com += p_normal * (share * inv_a);
		a->place_spheres();
	}
	if (b_on) {
		b->com -= p_normal * (share * inv_b);
		b->place_spheres();
	}
}

void DensityBody3D::resolve(DensityBody3D *a, DensityBody3D *b, const Vector3 &p_point, const Vector3 &p_normal, double p_depth,
		double p_rest_speed, int p_sphere_a, int p_sphere_b, bool p_separate) {
	// The player's own push is already done (DensityCharacter3D::push_bodies()):
	// here it only stops what it's in the way of, not shoving it at full
	// walking speed whatever it weighs (its mass counts as infinite here).
	const bool character = is_character(b);
	auto b_velocity = [&]() {
		Vector3 v = b != nullptr ? b->point_velocity(p_point) : Vector3();
		if (character) {
			const double towards = v.dot(p_normal);
			if (towards > 0.0) {
				v -= p_normal * towards;
			}
		}
		return v;
	};
	const double approach = (a->point_velocity(p_point) - b_velocity()).dot(p_normal);
	// Something moving (or frozen, so pushing) wakes a sleeper it hits.
	const bool hard = p_depth > 2.0 * SLOP || approach < -WAKE_SPEED;
	if (b != nullptr && hard) {
		if (a->dynamic() && a->sleeping && (b->moving() || b->freeze)) {
			a->wake();
		}
		if (b->dynamic() && b->sleeping && (a->moving() || a->freeze)) {
			b->wake();
		}
	}
	const bool a_on = a->moving(), b_on = b != nullptr && b->moving();
	const double inv_a = a_on ? 1.0 / a->mass : 0.0, inv_b = b_on ? 1.0 / b->mass : 0.0;
	a->add_contact(p_point, p_normal, b != nullptr ? b->get_instance_id() : 0, p_sphere_a);
	if (b != nullptr) {
		b->add_contact(p_point, -p_normal, a->get_instance_id(), p_sphere_b);
	}
	if (inv_a + inv_b <= 0.0) {
		return;
	}
	if (p_separate) {
		separate(a, b, p_normal, p_depth);
	}
	auto relative = [&]() {
		return a->point_velocity(p_point) - b_velocity();
	};
	Vector3 v = relative();
	const double vn = v.dot(p_normal);
	if (vn >= 0.0) {
		return; // already separating
	}
	const double bounce = b != nullptr ? std::max(a->bounce, b->bounce) : a->bounce;
	const double e = -vn > p_rest_speed ? bounce : 0.0;
	const double k = (a_on ? a->response(p_point, p_normal) : 0.0) + (b_on ? b->response(p_point, p_normal) : 0.0);
	const double jn = -(1.0 + e) * vn / k;
	if (a_on) {
		a->push(p_normal * jn, p_point);
	}
	if (b_on) {
		b->push(-p_normal * jn, p_point);
	}
	// Friction: up to friction * the push, against the sliding. Spheres
	// rubbing along the ground this way start rolling.
	v = relative();
	const Vector3 slide = v - p_normal * v.dot(p_normal);
	const double speed = slide.length();
	if (speed < 1e-6) {
		return;
	}
	const Vector3 t = slide / speed;
	const double kt = (a_on ? a->response(p_point, t) : 0.0) + (b_on ? b->response(p_point, t) : 0.0);
	const double mu = b != nullptr ? std::sqrt(a->friction * b->friction) : a->friction;
	const double jt = std::min(speed / kt, mu * jn);
	if (a_on) {
		a->push(-t * jt, p_point);
	}
	if (b_on) {
		b->push(t * jt, p_point);
	}
}

void DensityBody3D::collide_terrain(SculptTerrain *p_terrain, double p_rest_speed) {
	// A grid has many small probes: each one's impulse, but one push out of
	// the ground (along the deepest), or neighbouring probes would each push
	// it the whole way and it would never rest.
	const bool grid = shape.is_valid();
	double deepest = 0.0;
	Vector3 deepest_normal;
	for (const SphereGroup &g : groups) {
		if (groups.size() > 1) {
			// Clear of the ground (the density is at least the distance to it,
			// up to its clamp): nothing in this group touches.
			const Vector3 c = com + rotation.xform(g.center);
			if (-p_terrain->get_density(c) > g.radius * 1.15 + 0.5) {
				continue;
			}
		}
		for (int i = g.begin; i < g.end; i++) {
			const Vector3 c = world_centers[i];
			const double r = world_radii[i];
			// The density is (about) the signed distance to the surface,
			// positive inside: a sphere is r + density deep in the ground.
			const double depth = r + p_terrain->get_density(c);
			if (depth <= 0.0) {
				continue;
			}
			const Vector3 n = p_terrain->get_normal(c);
			resolve(this, nullptr, c - n * r, n, depth, p_rest_speed, i, -1, !grid);
			if (depth > deepest) {
				deepest = depth;
				deepest_normal = n;
			}
		}
	}
	if (grid && deepest > 0.0) {
		separate(this, nullptr, deepest_normal, deepest);
	}
}

void DensityBody3D::end_step(double p_delta) {
	if (!moving()) {
		return;
	}
	if (!contacts.empty() && rolling_resistance > 0.0) {
		angular_velocity *= std::max(0.0, 1.0 - rolling_resistance * p_delta);
	}
	const Transform3D t(rotation * Basis::from_scale(Vector3(scale, scale, scale)), com - rotation.xform(com_local * scale));
	set_global_transform(t);
	written = get_global_transform();
	has_written = true;
	if (can_sleep && !contacts.empty() && linear_velocity.length() < SLEEP_LINEAR && angular_velocity.length() < SLEEP_ANGULAR) {
		still_time += p_delta;
		if (still_time >= SLEEP_TIME) {
			set_sleeping(true);
		}
	} else {
		still_time = 0.0;
	}
}

void DensityBody3D::step_all(SculptTerrain *p_terrain, double p_delta, const Vector3 &p_gravity) {
	std::vector<DensityBody3D *> bodies;
	for (DensityBody3D *b : all()) {
		if (b->is_inside_tree() && (!b->spheres.is_empty() || b->shape.is_valid())) {
			bodies.push_back(b);
		}
	}
	if (bodies.empty() || p_delta <= 0.0) {
		return;
	}
	for (DensityBody3D *b : bodies) {
		// A sleeper keeps the contacts it fell asleep with: it is still
		// lying on that ground (scripts asking "am I on the ground?" see it).
		if (!b->sleeping) {
			b->contacts.clear();
		}
		b->begin_step();
	}
	// Forces act over the whole step. Each body is split into substeps by
	// its own speed (so one fast body doesn't make a resting pile do all its
	// collision work many times over): the step has as many as the fastest
	// needs, and a slower body moves every sub_stride of them.
	std::vector<int> needs(bodies.size(), 0);
	int substeps = 1;
	for (size_t i = 0; i < bodies.size(); i++) {
		DensityBody3D *b = bodies[i];
		if (b->moving()) {
			b->linear_velocity += b->force / b->mass * p_delta;
			b->angular_velocity += b->inverse_inertia_world().xform(b->torque) * p_delta;
			const double reach = (b->linear_velocity.length() + b->angular_velocity.length() * b->bound) * p_delta;
			needs[i] = std::clamp((int)std::ceil(reach / b->smallest / SUBSTEP_TRAVEL), 1, MAX_SUBSTEPS);
			substeps = std::max(substeps, needs[i]);
		}
		b->force = Vector3();
		b->torque = Vector3();
	}
	for (size_t i = 0; i < bodies.size(); i++) {
		bodies[i]->sub_stride = needs[i] > 0 ? std::max(1, substeps / needs[i]) : 0;
		bodies[i]->sub_last = 0;
	}
	const double h = p_delta / substeps;
	auto rest_speed_for = [&](int p_stride) {
		return std::max(REST_SPEED_MIN, 3.0 * p_gravity.length() * h * std::max(p_stride, 1));
	};

	// Pairs that could touch this step: one of them moving (or a frozen
	// body that could push a sleeping one), close enough to meet.
	std::vector<std::pair<DensityBody3D *, DensityBody3D *>> pairs;
	std::vector<std::pair<int, int>> pair_index; // the same, as indices into bodies
	for (size_t i = 0; i < bodies.size(); i++) {
		DensityBody3D *a = bodies[i];
		if (!a->collide_with_bodies) {
			continue;
		}
		for (size_t j = i + 1; j < bodies.size(); j++) {
			DensityBody3D *b = bodies[j];
			if (!b->collide_with_bodies || (a->freeze && b->freeze)) {
				continue;
			}
			if (!a->moving() && !b->moving() && !a->freeze && !b->freeze) {
				continue; // both asleep
			}
			const double reach = a->bound + b->bound + (a->linear_velocity.length() + b->linear_velocity.length()) * p_delta + 1.0;
			if (a->com.distance_squared_to(b->com) <= reach * reach) {
				// The dynamic one first: resolve() treats a as the one hit.
				pairs.push_back(a->freeze ? std::make_pair(b, a) : std::make_pair(a, b));
				pair_index.push_back(a->freeze ? std::make_pair((int)j, (int)i) : std::make_pair((int)i, (int)j));
			}
		}
	}

	std::vector<char> due(bodies.size()); // moved this substep (or the last one, for still ones)

	for (int s = 0; s < substeps; s++) {
		const bool last = s == substeps - 1;
		for (size_t i = 0; i < bodies.size(); i++) {
			DensityBody3D *b = bodies[i];
			due[i] = 0;
			if (b->moving() && b->sub_stride > 0) {
				if (s + 1 - b->sub_last >= b->sub_stride || last) {
					b->integrate(h * (s + 1 - b->sub_last), p_gravity);
					b->sub_last = s + 1;
					due[i] = 1;
				}
			} else if (last) {
				due[i] = 1; // still or frozen: in place all step, checked once
			}
		}
		for (size_t k = 0; k < pairs.size(); k++) {
			DensityBody3D *a = pairs[k].first, *b = pairs[k].second;
			const int ia = pair_index[k].first, ib = pair_index[k].second;
			if (!due[ia] && !due[ib]) {
				continue;
			}
			if (!a->moving() && !b->moving() && !a->freeze && !b->freeze) {
				continue;
			}
			const bool a_was = a->moving(), b_was = b->moving();
			const int fine = std::min(a->sub_stride > 0 ? a->sub_stride : substeps, b->sub_stride > 0 ? b->sub_stride : substeps);
			const double rest_speed = rest_speed_for(fine);
			bool touched = false;
			// Each of a's probes against b's density; for two grids, both
			// ways round (a grid's probes are only its skin points).
			auto probe = [&](DensityBody3D *p_from, DensityBody3D *p_into) {
				const bool grid = p_from->shape.is_valid();
				double deepest = 0.0;
				Vector3 deepest_normal;
				for (const SphereGroup &g : p_from->groups) {
					const double group_reach = p_into->bound + g.radius;
					if ((p_from->com + p_from->rotation.xform(g.center)).distance_squared_to(p_into->com) > group_reach * group_reach) {
						continue;
					}
					for (int i = g.begin; i < g.end; i++) {
						const Vector3 c = p_from->world_centers[i];
						const double r = p_from->world_radii[i];
						if (c.distance_squared_to(p_into->com) > (p_into->bound + r) * (p_into->bound + r)) {
							continue;
						}
						const double depth = r + p_into->density_at(c);
						if (depth <= 0.0) {
							continue;
						}
						const Vector3 n = p_into->normal_at(c);
						resolve(p_from, p_into, c - n * r, n, depth, rest_speed, i, -1, !grid);
						touched = true;
						if (depth > deepest) {
							deepest = depth;
							deepest_normal = n;
						}
					}
				}
				if (grid && deepest > 0.0) {
					separate(p_from, p_into, deepest_normal, deepest);
				}
			};
			probe(a, b);
			if (a->shape.is_valid() && b->shape.is_valid()) {
				probe(b, a);
			}
			if (touched) {
				// Bodies in contact move together from here (one hit by a fast
				// one could be going fast now), from where they are now.
				for (DensityBody3D *p : { a, b }) {
					if (!p->moving()) {
						continue;
					}
					const bool woken = !(p == a ? a_was : b_was) || p->sub_stride == 0;
					if (woken) {
						p->sub_last = s + 1;
						p->sub_stride = fine;
					} else if (fine < p->sub_stride) {
						p->sub_stride = fine;
					}
				}
			}
		}
		// The ground last: it can't move, so nothing may end a step pushed into it.
		for (size_t i = 0; i < bodies.size(); i++) {
			DensityBody3D *b = bodies[i];
			if (due[i] && b->moving()) {
				b->collide_terrain(p_terrain, rest_speed_for(b->sub_stride));
			}
		}
	}
	for (DensityBody3D *b : bodies) {
		b->end_step(p_delta);
	}
}

void DensityBody3D::wake_near(const Vector3 &p_min, const Vector3 &p_max, int p_world_size) {
	const double size = p_world_size;
	const Vector3 centre = (p_min + p_max) * 0.5, half = (p_max - p_min) * 0.5;
	for (DensityBody3D *b : all()) {
		if (!b->sleeping || !b->is_inside_tree()) {
			continue;
		}
		Vector3 d = b->get_global_center_of_mass() - centre;
		for (int a : { 0, 2 }) {
			d[a] = std::fmod(d[a] + size * 0.5, size);
			if (d[a] < 0.0) {
				d[a] += size;
			}
			d[a] -= size * 0.5;
		}
		const double margin = b->bound + 2.0;
		if (std::abs(d.x) <= half.x + margin && std::abs(d.y) <= half.y + margin && std::abs(d.z) <= half.z + margin) {
			b->wake();
		}
	}
}

void DensityBody3D::shift_all(const Vector3 &p_offset) {
	for (DensityBody3D *b : all()) {
		if (!b->is_inside_tree()) {
			continue;
		}
		b->set_global_position(b->get_global_position() + p_offset);
		b->com += p_offset;
		for (Vector3 &c : b->world_centers) {
			c += p_offset;
		}
		if (b->has_written) {
			b->written = b->get_global_transform(); // not a teleport
		}
	}
}

Vector3 DensityBody3D::get_global_center_of_mass() const {
	const Transform3D t = get_global_transform();
	return t.xform(com_local);
}

void DensityBody3D::apply_central_impulse(const Vector3 &p_impulse) {
	linear_velocity += p_impulse / mass;
	wake();
}

void DensityBody3D::apply_impulse(const Vector3 &p_impulse, const Vector3 &p_position) {
	// Between steps the transform is the truth (a script may have moved it).
	const Transform3D t = get_global_transform();
	rotation = t.basis.orthonormalized();
	if (inertia_dirty || shape_changed()) {
		update_inertia();
	}
	const Vector3 offset = p_position - t.basis.xform(com_local);
	linear_velocity += p_impulse / mass;
	angular_velocity += inverse_inertia_world().xform(offset.cross(p_impulse));
	wake();
}

void DensityBody3D::apply_torque_impulse(const Vector3 &p_impulse) {
	rotation = get_global_transform().basis.orthonormalized();
	if (inertia_dirty || shape_changed()) {
		update_inertia();
	}
	angular_velocity += inverse_inertia_world().xform(p_impulse);
	wake();
}

void DensityBody3D::apply_central_force(const Vector3 &p_force) {
	force += p_force;
	wake();
}

void DensityBody3D::apply_force(const Vector3 &p_force, const Vector3 &p_position) {
	const Vector3 offset = p_position - get_global_transform().basis.xform(com_local);
	force += p_force;
	torque += offset.cross(p_force);
	wake();
}

void DensityBody3D::apply_torque(const Vector3 &p_torque) {
	torque += p_torque;
	wake();
}

Vector3 DensityBody3D::get_contact_position(int p_index) const {
	ERR_FAIL_INDEX_V(p_index, (int)contacts.size(), Vector3());
	return contacts[p_index].position;
}

Vector3 DensityBody3D::get_contact_normal(int p_index) const {
	ERR_FAIL_INDEX_V(p_index, (int)contacts.size(), Vector3());
	return contacts[p_index].normal;
}

Object *DensityBody3D::get_contact_body(int p_index) const {
	ERR_FAIL_INDEX_V(p_index, (int)contacts.size(), nullptr);
	return contacts[p_index].body != 0 ? ObjectDB::get_instance(contacts[p_index].body) : nullptr;
}

bool DensityBody3D::is_touching_terrain() const {
	for (const Contact &c : contacts) {
		if (c.body == 0) {
			return true;
		}
	}
	return false;
}

Vector3 DensityBody3D::to_local(const Vector3 &p_point) const {
	const Vector3 origin_world = com - rotation.xform(com_local * scale);
	return rotation.xform_inv(p_point - origin_world) / (float)scale;
}

void DensityBody3D::set_shape(const Ref<DensityShape> &p_shape) {
	shape = p_shape;
	own_shape = false; // shared until sculpted
	inertia_dirty = true;
	wake();
}

void DensityBody3D::set_pose(double p_value) {
	pose = std::max(p_value, 0.0);
	// A new nearest key's probes: the next step picks them up (shape_changed()).
}

void DensityBody3D::set_key_weights(const PackedFloat32Array &p_weights) {
	key_weights = p_weights;
}

void DensityBody3D::set_key_weight(int p_key, double p_weight) {
	ERR_FAIL_COND(p_key < 0 || p_key > 4096);
	if (p_key >= key_weights.size()) {
		if (p_weight == 0.0) {
			return;
		}
		key_weights.resize(p_key + 1); // (new ones 0)
	}
	key_weights.set(p_key, (float)p_weight);
}

double DensityBody3D::get_key_weight(int p_key) const {
	return p_key >= 0 && p_key < key_weights.size() ? key_weights[p_key] : 0.0;
}

bool DensityBody3D::_set(const StringName &p_name, const Variant &p_value) {
	const String name = p_name;
	if (!name.begins_with("key_weight/")) {
		return false;
	}
	set_key_weight((int)name.get_slicec('/', 1).to_int(), (double)p_value);
	return true;
}

bool DensityBody3D::_get(const StringName &p_name, Variant &r_ret) const {
	const String name = p_name;
	if (!name.begins_with("key_weight/")) {
		return false;
	}
	r_ret = get_key_weight((int)name.get_slicec('/', 1).to_int());
	return true;
}

void DensityBody3D::_get_property_list(List<PropertyInfo> *p_list) const {
	// One per key (saved as key_weights).
	const int keys = shape.is_valid() ? shape->get_key_count() : 0;
	for (int k = 0; k < keys; k++) {
		p_list->push_back(PropertyInfo(Variant::FLOAT, vformat("key_weight/%d", k), PROPERTY_HINT_RANGE, "-1,2,0.001", PROPERTY_USAGE_EDITOR));
	}
}

void DensityBody3D::set_auto_material_properties(bool p_value) {
	auto_material_properties = p_value;
	inertia_dirty = true;
}

Array DensityBody3D::split_disconnected() {
	if (shape.is_null()) {
		return Array();
	}
	if (!own_shape) {
		shape = shape->duplicate_shape();
		own_shape = true;
	}
	const Array parts = shape->split_parts();
	if (!parts.is_empty()) {
		inertia_dirty = true;
		wake();
	}
	return parts;
}

void DensityBody3D::set_auto_center_of_mass(bool p_value) {
	auto_center_of_mass = p_value;
	inertia_dirty = true;
}

double DensityBody3D::distance_at(const Vector3 &p_point, Vector3 &r_normal) {
	if (!stepped || inertia_dirty || shape_changed()) {
		begin_step(false);
	}
	if (shape.is_valid()) {
		const Vector3 local = to_local(p_point);
		const DensityShape::Blend &b = get_blend();
		r_normal = rotation.xform(shape->get_blend_normal(local, b));
		return shape->get_blend_density(local, b) * scale;
	}
	if (capsule_radius > 0.0) {
		Vector3 d;
		const double inside = capsule_inside(p_point, d);
		r_normal = d;
		return inside;
	}
	double best = -1e30;
	r_normal = Vector3(0, 1, 0);
	for (size_t i = 0; i < world_centers.size(); i++) {
		const Vector3 d = p_point - world_centers[i];
		const double dist = d.length();
		const double inside = world_radii[i] - dist;
		if (inside > best) {
			best = inside;
			r_normal = dist > 1e-6 ? d / dist : Vector3(0, 1, 0);
		}
	}
	return best;
}

double DensityBody3D::density_at(const Vector3 &p_point) {
	if (!stepped || inertia_dirty || shape_changed()) {
		begin_step(false);
	}
	if (shape.is_valid()) {
		const Vector3 local = to_local(p_point);
		return shape->get_blend_density(local, get_blend()) * scale;
	}
	if (capsule_radius > 0.0) {
		Vector3 d;
		return capsule_inside(p_point, d);
	}
	double best = -1e30;
	for (size_t i = 0; i < world_centers.size(); i++) {
		best = std::max(best, (double)world_radii[i] - (p_point - world_centers[i]).length());
	}
	return best;
}

double DensityBody3D::capsule_inside(const Vector3 &p_point, Vector3 &r_normal) const {
	const Vector3 local = to_local(p_point);
	const Vector3 ab = capsule_b - capsule_a;
	const float len2 = ab.length_squared();
	const float t = len2 > 0.0f ? std::clamp((local - capsule_a).dot(ab) / len2, 0.0f, 1.0f) : 0.0f;
	const Vector3 d = local - (capsule_a + ab * t);
	const double dist = d.length();
	r_normal = dist > 1e-6 ? rotation.xform(d / (float)dist) : Vector3(0, 1, 0);
	return (capsule_radius - dist) * scale;
}

Vector3 DensityBody3D::normal_at(const Vector3 &p_point) {
	Vector3 n;
	distance_at(p_point, n);
	return n;
}

double DensityBody3D::get_density_at(const Vector3 &p_point) {
	return density_at(p_point);
}

double DensityBody3D::get_bound_radius() {
	if (!stepped || inertia_dirty || shape_changed()) {
		begin_step(false);
	}
	return bound;
}

bool DensityBody3D::apply_brush(const Vector3 &p_center, double p_radius, double p_strength, int p_mode, int p_material) {
	if (shape.is_null()) {
		return false;
	}
	if (!own_shape) {
		shape = shape->duplicate_shape();
		own_shape = true;
	}
	const Transform3D t = get_global_transform();
	const double s = std::max((double)t.basis.get_column(0).length(), 1e-6);
	const Transform3D inv = t.affine_inverse();
	const Vector3 local = inv.xform(p_center);
	// The brush's settings in the shape's grid (its units: voxel_size).
	DensityShape::BrushLocal &b = shape->brush_local;
	const float vs = (float)shape->get_voxel_size();
	const Vector3 origin = shape->get_origin();
	b.plane_set = flatten_plane_set;
	if (flatten_plane_set) {
		b.plane_point = (inv.xform(flatten_point) - origin) / vs;
		b.plane_normal = inv.basis.xform(flatten_normal).normalized();
	}
	b.clone_offset = inv.basis.xform(brush_options().clone_offset) / vs;
	b.stroke_direction = inv.basis.xform(brush_options().stroke_direction);
	const bool any = shape->apply_brush(local, p_radius / s, p_strength / s, p_mode, p_material, edit_key, pose, key_weights);
	if (any) {
		inertia_dirty = true;
		wake();
	}
	return any;
}

bool DensityBody3D::paint_color(const Vector3 &p_center, double p_radius, const Color &p_color, double p_opacity, double p_hardness, double p_scatter, int p_seed) {
	if (shape.is_null()) {
		return false;
	}
	if (!own_shape) {
		shape = shape->duplicate_shape();
		own_shape = true;
	}
	const Transform3D t = get_global_transform();
	const double s = std::max((double)t.basis.get_column(0).length(), 1e-6);
	const Vector3 local = t.affine_inverse().xform(p_center);
	return shape->paint_color(local, p_radius / s, p_color, p_opacity, p_hardness, pose, key_weights, p_scatter, p_seed);
}

int DensityBody3D::paint_fill(const Vector3 &p_center, double p_threshold, int p_material, const Color &p_color, double p_opacity, double p_scatter, int p_seed) {
	if (shape.is_null()) {
		return 0;
	}
	if (!own_shape) {
		shape = shape->duplicate_shape();
		own_shape = true;
	}
	const Vector3 local = get_global_transform().affine_inverse().xform(p_center);
	return shape->paint_fill(local, p_threshold, p_material, p_color, p_opacity, p_scatter, p_seed, pose, key_weights);
}

void DensityBody3D::begin_grab() {
	if (shape.is_null()) {
		return;
	}
	if (!own_shape) {
		shape = shape->duplicate_shape();
		own_shape = true;
	}
	shape->begin_grab();
}

bool DensityBody3D::warp(const Vector3 &p_center, const Vector3 &p_delta, double p_radius, bool p_grab) {
	PackedVector3Array centers, deltas;
	centers.push_back(p_center);
	deltas.push_back(p_delta);
	return warp_copies(centers, deltas, p_radius, p_grab);
}

bool DensityBody3D::warp_copies(const PackedVector3Array &p_centers, const PackedVector3Array &p_deltas, double p_radius, bool p_grab) {
	if (shape.is_null()) {
		return false;
	}
	if (!own_shape) {
		shape = shape->duplicate_shape();
		own_shape = true;
	}
	const Transform3D t = get_global_transform();
	const Transform3D inv = t.affine_inverse();
	const double s = std::max((double)t.basis.get_column(0).length(), 1e-6);
	std::vector<Vector3> centers, deltas;
	for (int64_t i = 0; i < std::min(p_centers.size(), p_deltas.size()); i++) {
		centers.push_back(inv.xform(p_centers[i]));
		deltas.push_back(inv.basis.xform(p_deltas[i]));
	}
	const bool any = shape->warp(centers, deltas, p_radius / s, p_grab, edit_key, pose, key_weights);
	if (any) {
		inertia_dirty = true;
	}
	return any;
}

bool DensityBody3D::twist(const PackedVector3Array &p_centers, const Array &p_bases, double p_radius) {
	if (shape.is_null()) {
		return false;
	}
	if (!own_shape) {
		shape = shape->duplicate_shape();
		own_shape = true;
	}
	const Transform3D t = get_global_transform();
	const Transform3D inv = t.affine_inverse();
	const double s = std::max((double)t.basis.get_column(0).length(), 1e-6);
	std::vector<Vector3> centers;
	std::vector<Basis> bases;
	for (int64_t i = 0; i < std::min<int64_t>(p_centers.size(), p_bases.size()); i++) {
		centers.push_back(inv.xform(p_centers[i]));
		const Basis b = p_bases[i];
		bases.push_back(inv.basis * b * t.basis); // (the same turn and scale, about the body's axes)
	}
	const bool any = shape->twist(centers, bases, p_radius / s, edit_key, pose, key_weights);
	if (any) {
		inertia_dirty = true;
	}
	return any;
}

void DensityBody3D::end_grab() {
	if (shape.is_valid()) {
		shape->end_grab();
	}
	inertia_dirty = true;
	wake();
}

Variant DensityBody3D::raycast(const Vector3 &p_from, const Vector3 &p_direction, double p_max_distance) {
	if (p_direction.length_squared() < 1e-12f || (spheres.is_empty() && shape.is_null())) {
		return Variant();
	}
	const Vector3 dir = p_direction.normalized();
	const double radius = get_bound_radius() + 0.5;
	const Vector3 centre = com;
	// Only between where the ray enters and leaves the bounding sphere.
	const Vector3 oc = p_from - centre;
	const double b = oc.dot(dir), c = oc.length_squared() - radius * radius, h = b * b - c;
	if (h < 0.0) {
		return Variant();
	}
	const double t_out = std::min(-b + std::sqrt(h), p_max_distance);
	double t = std::max(-b - std::sqrt(h), 0.0), prev = t;
	for (int i = 0; i < 160 && t <= t_out; i++) {
		const double d = density_at(p_from + dir * (float)t);
		if (d > 0.0) {
			double lo = prev, hi = t; // outside at lo, inside at hi
			for (int j = 0; j < 10; j++) {
				const double m = (lo + hi) * 0.5;
				(density_at(p_from + dir * (float)m) > 0.0 ? hi : lo) = m;
			}
			return p_from + dir * (float)hi;
		}
		prev = t;
		t += std::max(-d * 0.8, 0.05);
	}
	return Variant();
}

Dictionary DensityBody3D::raycast_bodies(const Vector3 &p_from, const Vector3 &p_direction, double p_max_distance) {
	Dictionary best;
	double best_distance = p_max_distance;
	for (DensityBody3D *b : all()) {
		if (!b->is_inside_tree() || !b->pickable || Object::cast_to<DensityCharacter3D>(b) != nullptr) {
			continue;
		}
		const Variant hit = b->raycast(p_from, p_direction, best_distance);
		if (hit.get_type() == Variant::VECTOR3) {
			const Vector3 p = hit;
			best_distance = p.distance_to(p_from);
			best["body"] = b;
			best["position"] = p;
			best["distance"] = best_distance;
		}
	}
	return best;
}

void DensityBody3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_spheres", "spheres"), &DensityBody3D::set_spheres);
	ClassDB::bind_method(D_METHOD("get_spheres"), &DensityBody3D::get_spheres);
	ClassDB::bind_method(D_METHOD("set_mass", "mass"), &DensityBody3D::set_mass);
	ClassDB::bind_method(D_METHOD("get_mass"), &DensityBody3D::get_mass);
	ClassDB::bind_method(D_METHOD("set_bounce", "bounce"), &DensityBody3D::set_bounce);
	ClassDB::bind_method(D_METHOD("get_bounce"), &DensityBody3D::get_bounce);
	ClassDB::bind_method(D_METHOD("set_friction", "friction"), &DensityBody3D::set_friction);
	ClassDB::bind_method(D_METHOD("get_friction"), &DensityBody3D::get_friction);
	ClassDB::bind_method(D_METHOD("set_gravity_scale", "scale"), &DensityBody3D::set_gravity_scale);
	ClassDB::bind_method(D_METHOD("get_gravity_scale"), &DensityBody3D::get_gravity_scale);
	ClassDB::bind_method(D_METHOD("set_linear_damp", "damp"), &DensityBody3D::set_linear_damp);
	ClassDB::bind_method(D_METHOD("get_linear_damp"), &DensityBody3D::get_linear_damp);
	ClassDB::bind_method(D_METHOD("set_angular_damp", "damp"), &DensityBody3D::set_angular_damp);
	ClassDB::bind_method(D_METHOD("get_angular_damp"), &DensityBody3D::get_angular_damp);
	ClassDB::bind_method(D_METHOD("set_rolling_resistance", "damp"), &DensityBody3D::set_rolling_resistance);
	ClassDB::bind_method(D_METHOD("get_rolling_resistance"), &DensityBody3D::get_rolling_resistance);
	ClassDB::bind_method(D_METHOD("set_center_of_mass", "position"), &DensityBody3D::set_center_of_mass);
	ClassDB::bind_method(D_METHOD("get_center_of_mass"), &DensityBody3D::get_center_of_mass);
	ClassDB::bind_method(D_METHOD("set_freeze", "freeze"), &DensityBody3D::set_freeze);
	ClassDB::bind_method(D_METHOD("is_freeze"), &DensityBody3D::is_freeze);
	ClassDB::bind_method(D_METHOD("set_can_sleep", "enabled"), &DensityBody3D::set_can_sleep);
	ClassDB::bind_method(D_METHOD("is_able_to_sleep"), &DensityBody3D::is_able_to_sleep);
	ClassDB::bind_method(D_METHOD("set_sleeping", "sleeping"), &DensityBody3D::set_sleeping);
	ClassDB::bind_method(D_METHOD("is_sleeping"), &DensityBody3D::is_sleeping);
	ClassDB::bind_method(D_METHOD("set_collide_with_bodies", "enabled"), &DensityBody3D::set_collide_with_bodies);
	ClassDB::bind_method(D_METHOD("get_collide_with_bodies"), &DensityBody3D::get_collide_with_bodies);
	ClassDB::bind_method(D_METHOD("set_linear_velocity", "velocity"), &DensityBody3D::set_linear_velocity);
	ClassDB::bind_method(D_METHOD("get_linear_velocity"), &DensityBody3D::get_linear_velocity);
	ClassDB::bind_method(D_METHOD("set_angular_velocity", "velocity"), &DensityBody3D::set_angular_velocity);
	ClassDB::bind_method(D_METHOD("get_angular_velocity"), &DensityBody3D::get_angular_velocity);
	ClassDB::bind_method(D_METHOD("set_drawn", "drawn"), &DensityBody3D::set_drawn);
	ClassDB::bind_method(D_METHOD("is_drawn"), &DensityBody3D::is_drawn);
	ClassDB::bind_method(D_METHOD("set_color", "color"), &DensityBody3D::set_color);
	ClassDB::bind_method(D_METHOD("get_color"), &DensityBody3D::get_color);
	ClassDB::bind_method(D_METHOD("set_material", "material"), &DensityBody3D::set_material);
	ClassDB::bind_method(D_METHOD("get_material"), &DensityBody3D::get_material);
	ClassDB::bind_method(D_METHOD("apply_central_impulse", "impulse"), &DensityBody3D::apply_central_impulse);
	ClassDB::bind_method(D_METHOD("apply_impulse", "impulse", "position"), &DensityBody3D::apply_impulse, DEFVAL(Vector3()));
	ClassDB::bind_method(D_METHOD("apply_torque_impulse", "impulse"), &DensityBody3D::apply_torque_impulse);
	ClassDB::bind_method(D_METHOD("apply_central_force", "force"), &DensityBody3D::apply_central_force);
	ClassDB::bind_method(D_METHOD("apply_force", "force", "position"), &DensityBody3D::apply_force, DEFVAL(Vector3()));
	ClassDB::bind_method(D_METHOD("apply_torque", "torque"), &DensityBody3D::apply_torque);
	ClassDB::bind_method(D_METHOD("get_contact_count"), &DensityBody3D::get_contact_count);
	ClassDB::bind_method(D_METHOD("get_contact_position", "index"), &DensityBody3D::get_contact_position);
	ClassDB::bind_method(D_METHOD("get_contact_normal", "index"), &DensityBody3D::get_contact_normal);
	ClassDB::bind_method(D_METHOD("get_contact_body", "index"), &DensityBody3D::get_contact_body);
	ClassDB::bind_method(D_METHOD("is_touching_terrain"), &DensityBody3D::is_touching_terrain);
	ClassDB::bind_method(D_METHOD("get_global_center_of_mass"), &DensityBody3D::get_global_center_of_mass);
	ClassDB::bind_method(D_METHOD("set_shape", "shape"), &DensityBody3D::set_shape);
	ClassDB::bind_method(D_METHOD("get_shape"), &DensityBody3D::get_shape);
	ClassDB::bind_method(D_METHOD("set_pose", "pose"), &DensityBody3D::set_pose);
	ClassDB::bind_method(D_METHOD("get_pose"), &DensityBody3D::get_pose);
	ClassDB::bind_method(D_METHOD("set_key_weights", "weights"), &DensityBody3D::set_key_weights);
	ClassDB::bind_method(D_METHOD("get_key_weights"), &DensityBody3D::get_key_weights);
	ClassDB::bind_method(D_METHOD("set_key_weight", "key", "weight"), &DensityBody3D::set_key_weight);
	ClassDB::bind_method(D_METHOD("get_key_weight", "key"), &DensityBody3D::get_key_weight);
	ClassDB::bind_method(D_METHOD("set_auto_center_of_mass", "enabled"), &DensityBody3D::set_auto_center_of_mass);
	ClassDB::bind_method(D_METHOD("is_auto_center_of_mass"), &DensityBody3D::is_auto_center_of_mass);
	ClassDB::bind_method(D_METHOD("set_edit_key", "key"), &DensityBody3D::set_edit_key);
	ClassDB::bind_method(D_METHOD("get_edit_key"), &DensityBody3D::get_edit_key);
	ClassDB::bind_method(D_METHOD("apply_brush", "center", "radius", "strength", "mode", "material"), &DensityBody3D::apply_brush, DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("set_flatten_plane", "point", "normal"), &DensityBody3D::set_flatten_plane);
	ClassDB::bind_method(D_METHOD("clear_flatten_plane"), &DensityBody3D::clear_flatten_plane);
	ClassDB::bind_method(D_METHOD("paint_color", "center", "radius", "color", "opacity", "hardness", "scatter", "seed"), &DensityBody3D::paint_color, DEFVAL(0.0), DEFVAL(0));
	ClassDB::bind_method(D_METHOD("paint_fill", "center", "threshold", "material", "color", "opacity", "scatter", "seed"), &DensityBody3D::paint_fill);
	ClassDB::bind_method(D_METHOD("raycast", "from", "direction", "max_distance"), &DensityBody3D::raycast);
	ClassDB::bind_method(D_METHOD("begin_grab"), &DensityBody3D::begin_grab);
	ClassDB::bind_method(D_METHOD("warp", "center", "delta", "radius", "grab"), &DensityBody3D::warp);
	ClassDB::bind_method(D_METHOD("warp_copies", "centers", "deltas", "radius", "grab"), &DensityBody3D::warp_copies);
	ClassDB::bind_method(D_METHOD("end_grab"), &DensityBody3D::end_grab);
	ClassDB::bind_method(D_METHOD("twist", "centers", "bases", "radius"), &DensityBody3D::twist);
	ClassDB::bind_static_method("DensityBody3D", D_METHOD("raycast_bodies", "from", "direction", "max_distance"), &DensityBody3D::raycast_bodies);
	ClassDB::bind_method(D_METHOD("get_density_at", "position"), &DensityBody3D::get_density_at);
	ClassDB::bind_method(D_METHOD("get_bound_radius"), &DensityBody3D::get_bound_radius);
	ClassDB::bind_method(D_METHOD("set_auto_material_properties", "enabled"), &DensityBody3D::set_auto_material_properties);
	ClassDB::bind_method(D_METHOD("is_auto_material_properties"), &DensityBody3D::is_auto_material_properties);
	ClassDB::bind_method(D_METHOD("set_pickable", "pickable"), &DensityBody3D::set_pickable);
	ClassDB::bind_method(D_METHOD("is_pickable"), &DensityBody3D::is_pickable);
	ClassDB::bind_method(D_METHOD("set_highlighted", "highlighted"), &DensityBody3D::set_highlighted);
	ClassDB::bind_method(D_METHOD("is_highlighted"), &DensityBody3D::is_highlighted);
	ClassDB::bind_method(D_METHOD("split_disconnected"), &DensityBody3D::split_disconnected);
	ClassDB::bind_method(D_METHOD("get_drawn_shape"), &DensityBody3D::get_drawn_shape);

	ADD_GROUP("Shape", "");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "shape", PROPERTY_HINT_RESOURCE_TYPE, "DensityShape"), "set_shape", "get_shape");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "pose", PROPERTY_HINT_RANGE, "0,64,0.001,or_greater"), "set_pose", "get_pose");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_FLOAT32_ARRAY, "key_weights", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE), "set_key_weights", "get_key_weights");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "edit_key", PROPERTY_HINT_RANGE, "-1,64,1,or_greater"), "set_edit_key", "get_edit_key");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_VECTOR4_ARRAY, "spheres"), "set_spheres", "get_spheres");
	ADD_GROUP("Physics", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "mass", PROPERTY_HINT_RANGE, "0.001,1000,0.001,or_greater"), "set_mass", "get_mass");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "bounce", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_bounce", "get_bounce");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "friction", PROPERTY_HINT_RANGE, "0,2,0.01"), "set_friction", "get_friction");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "gravity_scale", PROPERTY_HINT_RANGE, "-8,8,0.001,or_greater,or_less"), "set_gravity_scale", "get_gravity_scale");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "linear_damp", PROPERTY_HINT_RANGE, "0,10,0.001,or_greater"), "set_linear_damp", "get_linear_damp");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "angular_damp", PROPERTY_HINT_RANGE, "0,10,0.001,or_greater"), "set_angular_damp", "get_angular_damp");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "rolling_resistance", PROPERTY_HINT_RANGE, "0,10,0.001,or_greater"), "set_rolling_resistance", "get_rolling_resistance");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "auto_material_properties"), "set_auto_material_properties", "is_auto_material_properties");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "auto_center_of_mass"), "set_auto_center_of_mass", "is_auto_center_of_mass");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "pickable"), "set_pickable", "is_pickable");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "highlighted", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NONE), "set_highlighted", "is_highlighted");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "center_of_mass", PROPERTY_HINT_NONE, "suffix:voxels"), "set_center_of_mass", "get_center_of_mass");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "freeze"), "set_freeze", "is_freeze");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "can_sleep"), "set_can_sleep", "is_able_to_sleep");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "sleeping"), "set_sleeping", "is_sleeping");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "collide_with_bodies"), "set_collide_with_bodies", "get_collide_with_bodies");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "linear_velocity", PROPERTY_HINT_NONE, "suffix:voxels/s"), "set_linear_velocity", "get_linear_velocity");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "angular_velocity", PROPERTY_HINT_NONE, "suffix:rad/s"), "set_angular_velocity", "get_angular_velocity");
	ADD_GROUP("Drawing", "");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "drawn"), "set_drawn", "is_drawn");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "color", PROPERTY_HINT_COLOR_NO_ALPHA), "set_color", "get_color");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "material", PROPERTY_HINT_RANGE, "-1,127,1"), "set_material", "get_material");
}

// --- DensityCharacter3D ------------------------------------------------------------

DensityCharacter3D::DensityCharacter3D() {
	freeze = true;
	can_sleep = false;
	rebuild_spheres();
}

void DensityCharacter3D::set_radius(double p_value) {
	radius = std::max(p_value, 0.5);
	rebuild_spheres();
}

void DensityCharacter3D::set_height(double p_value) {
	height = std::max(p_value, 1.0);
	rebuild_spheres();
}

int DensityCharacter3D::axis_count() const {
	return (int)samples.size();
}

Vector3 DensityCharacter3D::axis_point(const Vector3 &p_feet, int p_index) const {
	const Vector4 &q = samples[p_index];
	return p_feet + Vector3(q.x, q.y, q.z);
}

void DensityCharacter3D::rebuild_spheres() {
	// The terrain's density only says how far a surface is up to about 8
	// voxels (DENSITY_CLAMP): a sphere wider than that would always seem
	// buried. A capsule that wide is made of smaller spheres round its
	// surface instead (the lowest, straight below, first: it finds the floor).
	samples.clear();
	const double r = radius, h = std::max(height, 2.0 * r);
	if (r <= MAX_SAMPLE_RADIUS) {
		// Spheres along the axis, half a radius apart.
		const double length = h - 2.0 * r;
		const int n = (int)std::ceil(length / (r * 0.5)) + 1;
		for (int i = 0; i < n; i++) {
			samples.push_back(Vector4(0, (float)(r + (n > 1 ? length * i / (n - 1) : 0.0)), 0, (float)r));
		}
	} else {
		const double rs = MAX_SAMPLE_RADIUS, inner = r - rs, spacing = rs * 0.9;
		const float rsf = (float)rs;
		samples.push_back(Vector4(0, rsf, 0, rsf)); // the bottom
		// The two hemispheres: points spread evenly over them (a Fibonacci spiral).
		const int shell = std::max(8, (int)std::ceil(4.0 * Math_PI * inner * inner / (spacing * spacing)));
		const double golden = Math_PI * (3.0 - std::sqrt(5.0));
		for (int i = 0; i < shell; i++) {
			const double y = 1.0 - 2.0 * (i + 0.5) / shell, ring = std::sqrt(1.0 - y * y), a = golden * i;
			const Vector3 dir((float)(std::cos(a) * ring), (float)y, (float)(std::sin(a) * ring));
			const double centre = y < 0.0 ? r : h - r;
			samples.push_back(Vector4(dir.x * (float)inner, (float)centre + dir.y * (float)inner, dir.z * (float)inner, rsf));
		}
		samples.push_back(Vector4(0, (float)(h - rs), 0, rsf)); // the top
		// The side: rings of them.
		const int rings = (int)std::ceil((h - 2.0 * r) / spacing);
		const int around = std::max(6, (int)std::ceil(2.0 * Math_PI * inner / spacing));
		for (int j = 0; j <= rings; j++) {
			const double y = r + (rings > 0 ? (h - 2.0 * r) * j / rings : 0.0);
			for (int i = 0; i < around; i++) {
				const double a = 2.0 * Math_PI * (i + 0.5 * (j & 1)) / around;
				samples.push_back(Vector4((float)(std::cos(a) * inner), (float)y, (float)(std::sin(a) * inner), rsf));
			}
		}
	}
	PackedVector4Array list;
	for (const Vector4 &q : samples) {
		list.push_back(q);
	}
	set_spheres(list);
	mass = std::max(PLAYER_MASS_PER_VOXEL * Math_PI * r * r * (h - 2.0 * r + 4.0 / 3.0 * r), 1e-3);
	capsule_radius = r;
	capsule_a = Vector3(0, (float)r, 0);
	capsule_b = Vector3(0, (float)(h - r), 0);
	build_patches();
	forget_checks();
}

void DensityCharacter3D::build_patches() {
	patches.clear();
	small_patches.clear();
	// Groups points (sample centres, or small patch centres) into cells
	// p_cell across: each group a patch with its bounding sphere and the
	// coarse level that can vouch for it.
	auto group = [](const std::vector<Vector3> &p_points, const std::vector<double> &p_radii, double p_cell, std::vector<Patch> &r_patches) {
		std::unordered_map<Vector3i, int, Vector3iHash> index;
		for (int i = 0; i < (int)p_points.size(); i++) {
			const Vector3 &c = p_points[i];
			const Vector3i key((int)std::floor(c.x / p_cell), (int)std::floor(c.y / p_cell), (int)std::floor(c.z / p_cell));
			auto found = index.find(key);
			if (found == index.end()) {
				found = index.emplace(key, (int)r_patches.size()).first;
				r_patches.push_back(Patch());
			}
			r_patches[found->second].members.push_back(i);
		}
		for (Patch &p : r_patches) {
			Vector3 sum;
			for (int i : p.members) {
				sum += p_points[i];
			}
			p.centre = sum / (float)p.members.size();
			for (int i : p.members) {
				p.reach = std::max(p.reach, (double)(p_points[i] - p.centre).length() + p_radii[i]);
			}
			// The coarse level whose distances (at most ~0.6 * CLAMP lattice
			// steps, less a corner's offset) reach past the patch.
			p.level = 1;
			while (p.level < 7 && (0.6 * DENSITY_CLAMP - 1.75) * (1 << p.level) < p.reach + CLEAR_GAP + 1.0) {
				p.level++;
			}
		}
	};
	if (samples.size() <= 16) {
		// A normal-sized player: a few spheres, all checked.
		Patch all;
		all.level = -1;
		all.members.push_back(0);
		patches.push_back(all);
		Patch every;
		every.level = -1;
		for (int i = 0; i < (int)samples.size(); i++) {
			every.members.push_back(i);
		}
		small_patches.push_back(every);
		return;
	}
	std::vector<Vector3> points;
	std::vector<double> radii;
	for (const Vector4 &q : samples) {
		points.push_back(Vector3(q.x, q.y, q.z));
		radii.push_back(q.w);
	}
	group(points, radii, SMALL_PATCH, small_patches);
	std::vector<Vector3> centres;
	std::vector<double> reaches;
	for (const Patch &p : small_patches) {
		centres.push_back(p.centre);
		reaches.push_back(p.reach);
	}
	group(centres, reaches, std::max(SMALL_PATCH * 2.0, radius * 0.4), patches);
}

void DensityCharacter3D::forget_checks() {
	patch_margin.assign(patches.size() + small_patches.size(), -1.0f);
	patch_feet.assign(patch_margin.size(), Vector3());
	sample_margin.assign(samples.size(), -1.0f);
	sample_feet.assign(samples.size(), Vector3());
}

double DensityCharacter3D::worst_gap(SculptTerrain *p_terrain, const Vector3 &p_feet, Vector3 &r_normal) const {
	double worst = 1e30;
	int deepest = -1;
	r_normal = Vector3(0, 1, 0);
	// A ground check is good while the feet stay within its margin of where
	// they were for it (the ground can't have come nearer than that; an edit
	// forgets them all): a big player re-reads only what it moved near.
	if (remembered_serial != p_terrain->get_edit_serial()) {
		remembered_serial = p_terrain->get_edit_serial();
		std::fill(patch_margin.begin(), patch_margin.end(), -1.0f);
		std::fill(sample_margin.begin(), sample_margin.end(), -1.0f);
	}
	// Whether nothing (ground or a near body) is within reach of a patch
	// (index p_slot of patch_margin / patch_feet).
	auto clear = [&](const Patch &p, int p_slot) {
		if (p.level < 0) {
			return false;
		}
		const Vector3 c = p_feet + p.centre;
		for (const DensityBody3D *b : near_bodies) {
			const double reach = b->bound + p.reach + 1.0;
			if (b->com.distance_squared_to(c) <= reach * reach) {
				return false;
			}
		}
		if (patch_margin[p_slot] > 0.0f && patch_feet[p_slot].distance_squared_to(p_feet) < patch_margin[p_slot] * patch_margin[p_slot]) {
			return true;
		}
		const double room = p_terrain->clear_distance(c, p.level) - p.reach - CLEAR_GAP;
		patch_margin[p_slot] = (float)room;
		patch_feet[p_slot] = p_feet;
		return room > 0.0;
	};
	for (int bi = 0; bi < (int)patches.size(); bi++) {
		const Patch &big = patches[bi];
		if (clear(big, bi)) {
			worst = std::min(worst, CLEAR_GAP);
			continue;
		}
		for (int s : big.members) {
			const Patch &p = small_patches[s];
			if (clear(p, (int)patches.size() + s)) {
				worst = std::min(worst, CLEAR_GAP);
				continue;
			}
			for (int k : p.members) {
				// The ground's distance only (one density read): the way out
				// (two normals, a dozen more reads) is only needed for the
				// deepest sample, below. Near a body, the whole check.
				const Vector3 c = axis_point(p_feet, k);
				double g;
				if (near_bodies.empty()) {
					if (sample_margin[k] > 0.0f && sample_feet[k].distance_squared_to(p_feet) < sample_margin[k] * sample_margin[k]) {
						worst = std::min(worst, CLEAR_GAP);
						continue;
					}
					g = -p_terrain->get_density(c) - samples[k].w;
					// Densities can overstate the distance (up to ~1.7x): half of it.
					sample_margin[k] = (float)(0.5 * (g + samples[k].w) - samples[k].w * 0.5 - CLEAR_GAP);
					sample_feet[k] = p_feet;
				} else {
					Vector3 n;
					g = gap(p_terrain, c, samples[k].w, true, n);
				}
				if (g < worst) {
					worst = g;
					deepest = k;
				}
			}
		}
	}
	if (deepest >= 0) {
		worst = gap(p_terrain, axis_point(p_feet, deepest), samples[deepest].w, true, r_normal);
	}
	return worst;
}

double DensityCharacter3D::gap(SculptTerrain *p_terrain, const Vector3 &p_center, double p_radius, bool p_bodies, Vector3 &r_normal) const {
	const double distance = -p_terrain->get_density(p_center); // to the ground, from the centre
	double best = distance - p_radius;
	r_normal = Vector3(0, 1, 0);
	if (best < 1.0) {
		// The way out, taken near the surface touched rather than at the
		// centre: in a corner (floor meeting wall) the centre is about as
		// far from both, and its gradient points diagonally, which would
		// carry the player up walls or stop it on flat ground.
		// Over a couple of voxels, so a slightly rough surface (brushed
		// ground) doesn't jostle the player about.
		const double spread = std::clamp(p_radius * 0.3, 0.5, 2.0);
		r_normal = p_terrain->get_normal(p_center, spread);
		const Vector3 near_surface = p_center - r_normal * (float)std::max(distance - 0.5, 0.0);
		r_normal = p_terrain->get_normal(near_surface, spread);
	}
	if (!p_bodies) {
		return best;
	}
	const double floor_cos = std::cos(floor_max_angle);
	for (DensityBody3D *b : near_bodies) {
		const double reach = b->bound + p_radius + 1.0;
		if (b->com.distance_squared_to(p_center) > reach * reach) {
			continue;
		}
		Vector3 n;
		const double g = -b->distance_at(p_center, n) - p_radius;
		// Only from above: something to stand on. From the side the player
		// walks into it and it gets pushed (it bounces off us).
		if (g < best && n.y >= floor_cos) {
			best = g;
			r_normal = n;
		}
	}
	return best;
}

void DensityCharacter3D::push_bodies(const Vector3 &p_feet, double p_dt, double p_gravity) {
	if (near_bodies.empty() || p_dt <= 0.0) {
		return;
	}
	const double floor_cos = std::cos(floor_max_angle);
	const Vector3 ahead = p_feet + velocity * (float)p_dt;
	// The most it can push with over this step, however light it is.
	const double most = push_strength * mass * std::max(p_gravity, 0.0) * p_dt;
	for (DensityBody3D *b : near_bodies) {
		if (!b->dynamic()) {
			continue; // frozen (or another computer's, online): not pushed
		}
		// Where the capsule, moved on by this step, goes into it deepest
		// (or comes within SKIN of it).
		double deepest = -SKIN;
		Vector3 normal, point;
		bool found = false;
		for (int k = 0; k < axis_count(); k++) {
			const Vector3 c = axis_point(ahead, k);
			const double r = samples[k].w;
			const double reach = b->bound + r + 1.0;
			if (b->com.distance_squared_to(c) > reach * reach) {
				continue;
			}
			Vector3 n;
			const double depth = b->distance_at(c, n) + r;
			if (depth > deepest && n.length_squared() > 1e-12) {
				deepest = depth;
				normal = n.normalized();
				point = c - normal * (float)r;
				found = true;
			}
		}
		// Standing on it isn't pushing it.
		if (!found || normal.y >= floor_cos) {
			continue;
		}
		const Vector3 into = -normal;
		const double vp = velocity.dot(into);
		const double vb = b->point_velocity(point).dot(into);
		if (vp <= vb) {
			continue; // not catching it up
		}
		// An inelastic collision between the two (both then move on at the
		// same speed: a light thing at nearly the player's, a heavy one
		// barely), unless that needs more than the player can push.
		const double k = b->response(point, into);
		const double j = std::min((vp - vb) / (1.0 / mass + k), most);
		if (j <= 0.0) {
			continue;
		}
		// (push() takes a world point, as response() does; apply_impulse()'s
		// position is an offset from the body's origin.)
		b->push(into * (float)j, point);
		b->wake();
		// No faster into it than it now gives way.
		velocity -= into * (float)(vp - (vb + j * k));
	}
}

bool DensityCharacter3D::move_and_slide() {
	SculptTerrain *terrain = SculptTerrain::get_singleton();
	const double dt = get_physics_process_delta_time();
	Vector3 feet = get_global_position();
	const bool was_on_floor = on_floor;
	on_floor = on_wall = on_ceiling = false;
	floor_normal = Vector3(0, 1, 0);
	if (terrain == nullptr || !terrain->is_world_open()) {
		feet += velocity * dt;
		set_global_position(feet);
		linear_velocity = velocity;
		return false;
	}
	const double floor_cos = std::cos(floor_max_angle);
	// The bodies it could touch this move. gap() is asked hundreds of times
	// a move (every sphere, every iteration and substep, stepping up): it
	// used to go through every body in the world each time.
	near_bodies.clear();
	{
		const double h = std::max(height, 2.0 * radius);
		const Vector3 middle = feet + Vector3(0, (float)(h * 0.5), 0);
		const double reach = h * 0.5 + radius + velocity.length() * dt + step_height * 2.0 + floor_snap_length + std::min(radius, MAX_SAMPLE_RADIUS) + 2.0;
		for (DensityBody3D *b : all()) {
			if (b == this || !b->collide_with_bodies || !b->is_inside_tree() || !b->stepped || Object::cast_to<DensityCharacter3D>(b) != nullptr) {
				continue;
			}
			const double r = reach + b->bound;
			if (b->com.distance_squared_to(middle) <= r * r) {
				near_bodies.push_back(b);
			}
		}
	}
	push_bodies(feet, dt, terrain->get_gravity());
	bool touched = false;
	auto classify = [&](const Vector3 &n) {
		if (n.y >= floor_cos) {
			on_floor = true;
			floor_normal = n;
		} else if (n.y <= -0.7) {
			on_ceiling = true;
		} else {
			on_wall = true;
		}
	};
	// Pushes the capsule at r_feet out of what it's in, along the deepest
	// sphere's way out, a few times. p_slide: also classify the contacts and
	// take away velocity into them. Returns whether it hit a wall.
	auto depenetrate = [&](Vector3 &r_feet, bool p_slide) {
		bool wall = false;
		for (int iteration = 0; iteration < 4; iteration++) {
			Vector3 out;
			const double worst = std::min(worst_gap(terrain, r_feet, out), 0.0);
			if (worst >= -1e-3) {
				break;
			}
			if (out.y >= floor_cos) {
				// A floor: straight up, enough to clear it (as far along the
				// normal as it's in), so rough ground doesn't push sideways.
				r_feet.y += (float)(-worst / out.y);
			} else {
				r_feet += out * -worst;
			}
			wall |= out.y < floor_cos && out.y > -0.7;
			if (p_slide) {
				touched = true;
				classify(out);
				// Slide: no velocity left into what it hit.
				const double into = velocity.dot(out);
				if (into < 0.0) {
					velocity -= out * into;
				}
			}
		}
		return wall;
	};
	// How far the lowest sphere is above what's under it, following it down
	// (at most p_max): moves r_feet there and returns the gap left.
	auto settle = [&](Vector3 &r_feet, double p_max, Vector3 &r_normal) {
		const double top = r_feet.y;
		double g = 0.0;
		for (int i = 0; i < 8; i++) {
			g = gap(terrain, axis_point(r_feet, 0), samples[0].w, true, r_normal);
			if (g <= SKIN || top - r_feet.y + g > p_max) {
				break;
			}
			r_feet.y -= g; // the gap is to the nearest surface, so at most the drop
		}
		return g;
	};
	const int steps = std::clamp((int)std::ceil(velocity.length() * dt / (std::min(radius, MAX_SAMPLE_RADIUS) * 0.4)), 1, 24);
	for (int s = 0; s < steps; s++) {
		const Vector3 start = feet;
		const Vector3 velocity_before = velocity;
		feet += velocity * (dt / steps); // velocity as clipped by earlier contacts
		const bool hit_wall = depenetrate(feet, true);
		// Blocked by a low ledge while walking: step up onto it (up, across,
		// then back down onto the ledge), if there's room above and floor on it.
		Vector3 across = velocity_before * (dt / steps);
		across.y = 0.0f;
		Vector3 got = feet - start;
		got.y = 0.0f;
		if (hit_wall && step_height > 0.0 && (was_on_floor || on_floor) && across.length() > 1e-4f && got.length() < across.length() * 0.7f) {
			// Look a radius ahead, up a step, for a floor there (so the lowest
			// sphere is over the ledge, not on its corner).
			const Vector3 lift(0, (float)step_height, 0);
			Vector3 n;
			bool room = worst_gap(terrain, start + lift, n) > -0.05;
			Vector3 ahead = start + lift + across.normalized() * (float)std::max((double)across.length(), std::min(radius, MAX_SAMPLE_RADIUS));
			room = room && worst_gap(terrain, ahead, n) > -0.05;
			if (room) {
				const double g = settle(ahead, step_height * 2.0, n);
				// The rise counts from the floor last stood on, so a ledge can't be
				// climbed a step at a time.
				const double rise = ahead.y - start.y;
				const double above_floor = ahead.y - (was_on_floor ? std::min((double)start.y, standing_height) : start.y);
				if (g <= SKIN * 2.0 && n.y >= floor_cos && rise > 0.05 && above_floor <= step_height + 0.01) {
					// Up by the ledge's height, and on by this step's move.
					Vector3 stepped = start + across + Vector3(0, (float)rise + 0.05f, 0);
					depenetrate(stepped, false);
					Vector3 got_up = stepped - start;
					got_up.y = 0.0f;
					if (got_up.length() > got.length() + 1e-3f) {
						feet = stepped;
						velocity = Vector3(velocity_before.x, std::min(velocity_before.y, 0.0f), velocity_before.z);
						on_floor = true;
						on_wall = false;
						floor_normal = n;
					}
				}
			}
		}
	}
	// Standing: the lowest sphere just above a floor (velocity.y = 0 there
	// leaves no push to detect it by).
	Vector3 n;
	double g = gap(terrain, axis_point(feet, 0), samples[0].w, true, n);
	if (!on_floor && g < SKIN && n.y >= floor_cos) {
		on_floor = true;
		floor_normal = n;
	}
	// Walking off a step or down a slope: follow the ground down rather than
	// flying off it, if it is within floor_snap_length.
	if (!on_floor && was_on_floor && velocity.y <= 0.0 && floor_snap_length > 0.0) {
		Vector3 probe = feet;
		g = settle(probe, floor_snap_length, n);
		if (g <= SKIN * 2.0 && n.y >= floor_cos && feet.y - probe.y <= floor_snap_length) {
			feet = probe;
			on_floor = true;
			floor_normal = n;
			touched = true;
		}
	}
	if (on_floor) {
		standing_height = feet.y;
	}
	set_global_position(feet);
	linear_velocity = velocity; // what bodies it hits feel
	return touched;
}

void DensityCharacter3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_radius", "radius"), &DensityCharacter3D::set_radius);
	ClassDB::bind_method(D_METHOD("get_radius"), &DensityCharacter3D::get_radius);
	ClassDB::bind_method(D_METHOD("set_height", "height"), &DensityCharacter3D::set_height);
	ClassDB::bind_method(D_METHOD("get_height"), &DensityCharacter3D::get_height);
	ClassDB::bind_method(D_METHOD("set_velocity", "velocity"), &DensityCharacter3D::set_velocity);
	ClassDB::bind_method(D_METHOD("get_velocity"), &DensityCharacter3D::get_velocity);
	ClassDB::bind_method(D_METHOD("set_floor_max_angle", "radians"), &DensityCharacter3D::set_floor_max_angle);
	ClassDB::bind_method(D_METHOD("get_floor_max_angle"), &DensityCharacter3D::get_floor_max_angle);
	ClassDB::bind_method(D_METHOD("set_floor_snap_length", "voxels"), &DensityCharacter3D::set_floor_snap_length);
	ClassDB::bind_method(D_METHOD("get_floor_snap_length"), &DensityCharacter3D::get_floor_snap_length);
	ClassDB::bind_method(D_METHOD("set_step_height", "voxels"), &DensityCharacter3D::set_step_height);
	ClassDB::bind_method(D_METHOD("get_step_height"), &DensityCharacter3D::get_step_height);
	ClassDB::bind_method(D_METHOD("set_push_strength", "weights"), &DensityCharacter3D::set_push_strength);
	ClassDB::bind_method(D_METHOD("get_push_strength"), &DensityCharacter3D::get_push_strength);
	ClassDB::bind_method(D_METHOD("move_and_slide"), &DensityCharacter3D::move_and_slide);
	ClassDB::bind_method(D_METHOD("is_on_floor"), &DensityCharacter3D::is_on_floor);
	ClassDB::bind_method(D_METHOD("is_on_wall"), &DensityCharacter3D::is_on_wall);
	ClassDB::bind_method(D_METHOD("is_on_ceiling"), &DensityCharacter3D::is_on_ceiling);
	ClassDB::bind_method(D_METHOD("get_floor_normal"), &DensityCharacter3D::get_floor_normal);

	ADD_GROUP("Capsule", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "radius", PROPERTY_HINT_RANGE, "0.5,64,0.1,suffix:voxels"), "set_radius", "get_radius");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "height", PROPERTY_HINT_RANGE, "1,256,0.1,suffix:voxels"), "set_height", "get_height");
	ADD_GROUP("Movement", "");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "velocity", PROPERTY_HINT_NONE, "suffix:voxels/s"), "set_velocity", "get_velocity");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "floor_max_angle", PROPERTY_HINT_RANGE, "0,180,0.1,radians_as_degrees"), "set_floor_max_angle", "get_floor_max_angle");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "floor_snap_length", PROPERTY_HINT_RANGE, "0,32,0.1,suffix:voxels"), "set_floor_snap_length", "get_floor_snap_length");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "step_height", PROPERTY_HINT_RANGE, "0,32,0.1,suffix:voxels"), "set_step_height", "get_step_height");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "push_strength", PROPERTY_HINT_RANGE, "0,10,0.1,or_greater"), "set_push_strength", "get_push_strength");
}
