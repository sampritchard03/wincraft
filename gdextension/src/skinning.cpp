#include "skinning.h"

#include <godot_cpp/variant/quaternion.hpp>

#include <algorithm>
#include <cmath>

using namespace godot;

namespace {

// The nearest point of a bone's segment, and the (softened) distance to it.
float segment_distance(const SkinJoint &p_joint, const Vector3 &p_q, Vector3 &r_closest) {
	const Vector3 ab = p_joint.tail - p_joint.head;
	const float len2 = ab.length_squared();
	const float t = len2 > 1e-10f ? std::clamp((p_q - p_joint.head).dot(ab) / len2, 0.0f, 1.0f) : 0.0f;
	r_closest = p_joint.head + ab * t;
	// (Softened near the segment, so the distance is smooth there too.)
	return std::sqrt((p_q - r_closest).length_squared() + 0.0625f);
}

// 1 within the radius, easing to 0 over the blend (1 - smootherstep).
float gate(const SkinJoint &p_joint, float p_distance) {
	if (p_distance <= p_joint.radius) {
		return 1.0f;
	}
	if (p_joint.blend <= 1e-6f) {
		return 0.0f;
	}
	const float x = (p_distance - p_joint.radius) / p_joint.blend;
	return x >= 1.0f ? 0.0f : 1.0f - x * x * x * (x * (x * 6.0f - 15.0f) + 10.0f);
}

void add_outer(Basis &r_m, const Vector3 &p_u, const Vector3 &p_v) {
	for (int r = 0; r < 3; r++) {
		r_m.rows[r] += p_v * p_u[r];
	}
}

} // namespace

bool BonePose::is_rest() const {
	return rotation.is_equal_approx(Basis()) && location.is_zero_approx() && scale.is_equal_approx(Vector3(1, 1, 1));
}

Transform3D Skin::rest_frame(const SkinBone &p_bone) {
	Vector3 y = p_bone.tail - p_bone.head;
	y = y.length_squared() > 1e-10f ? y.normalized() : Vector3(0, 1, 0);
	// The turn taking +y onto the bone (the shortest), then its roll about it.
	Basis align;
	const float d = Vector3(0, 1, 0).dot(y);
	if (d < -0.9999f) {
		align = Basis(Vector3(1, 0, 0), Math_PI);
	} else if (d < 0.9999f) {
		align = Basis(Quaternion(Vector3(0, 1, 0), y));
	}
	return Transform3D(Basis(y, p_bone.roll) * align, p_bone.head);
}

void Skin::build(const std::vector<SkinBone> &p_bones, const std::vector<BonePose> &p_poses) {
	const int n = std::min((int)p_bones.size(), MAX_BONES);
	joints.resize(n);
	frames.resize(n);
	std::vector<Transform3D> rest(n);
	for (int b = 0; b < n; b++) {
		const SkinBone &bone = p_bones[b];
		rest[b] = rest_frame(bone);
		const BonePose pose = b < (int)p_poses.size() ? p_poses[b] : BonePose();
		const Transform3D local(pose.rotation * Basis::from_scale(pose.scale), pose.location);
		const bool has_parent = bone.parent >= 0 && bone.parent < b;
		// Posed frame: the parent's posed frame carries the bone's rest place
		// relative to it; then the bone's own pose, in its own axes.
		const Transform3D base = has_parent ? frames[bone.parent] * (rest[bone.parent].affine_inverse() * rest[b]) : rest[b];
		frames[b] = base * local;
		SkinJoint &j = joints[b];
		j.head = bone.head;
		j.tail = bone.tail;
		j.radius = std::max(bone.radius, 0.0f);
		j.blend = std::max(bone.blend, 0.0f);
		j.softness = std::max(bone.softness, 0.05f);
		j.matrix = frames[b] * rest[b].affine_inverse();
		const float det = j.matrix.basis.determinant();
		j.inverse_basis = std::abs(det) > 1e-9f ? j.matrix.basis.inverse() : Basis();
	}
}

float Skin::weights(const Vector3 &p_q, float *r_weights, Vector3 *r_gradients) const {
	const int n = (int)joints.size();
	float e[MAX_BONES], g[MAX_BONES];
	Vector3 de[MAX_BONES];
	float none = 1.0f; // product of (1 - gate): what no gate covers
	float e_min = 1e30f;
	for (int b = 0; b < n; b++) {
		Vector3 c;
		const float d = segment_distance(joints[b], p_q, c);
		g[b] = gate(joints[b], d);
		e[b] = d / joints[b].softness;
		de[b] = (p_q - c) / (d * joints[b].softness);
		none *= 1.0f - g[b];
		if (g[b] > 0.0f) {
			e_min = std::min(e_min, e[b]);
		}
	}
	const float covered = 1.0f - none;
	float sum = 0.0f;
	for (int b = 0; b < n; b++) {
		r_weights[b] = g[b] > 0.0f ? g[b] * std::exp(e_min - e[b]) : 0.0f;
		sum += r_weights[b];
	}
	if (sum <= 1e-12f) {
		for (int b = 0; b < n; b++) {
			r_weights[b] = 0.0f;
			if (r_gradients) {
				r_gradients[b] = Vector3();
			}
		}
		return 1.0f;
	}
	Vector3 mean_de; // softmax-weighted mean of the distance gradients
	for (int b = 0; b < n; b++) {
		r_weights[b] /= sum;
		mean_de += de[b] * r_weights[b];
	}
	for (int b = 0; b < n; b++) {
		if (r_gradients) {
			// d/dq of softmax(-e): w (-de + mean de).
			r_gradients[b] = (mean_de - de[b]) * (r_weights[b] * covered);
		}
		r_weights[b] *= covered;
	}
	return none;
}

Vector3 Skin::forward(const Vector3 &p_q, Basis *r_jacobian) const {
	float w[MAX_BONES];
	Vector3 dw[MAX_BONES];
	const float w0 = weights(p_q, w, r_jacobian ? dw : nullptr);
	Vector3 p = p_q * w0;
	Basis j = Basis().scaled(Vector3(w0, w0, w0));
	for (int b = 0; b < (int)joints.size(); b++) {
		if (w[b] <= 0.0f && (!r_jacobian || dw[b].is_zero_approx())) {
			continue;
		}
		const Transform3D &m = joints[b].matrix;
		const Vector3 moved = m.xform(p_q);
		p += moved * w[b];
		if (r_jacobian) {
			for (int r = 0; r < 3; r++) {
				j.rows[r] += m.basis.rows[r] * w[b];
			}
			add_outer(j, moved, dw[b]);
		}
	}
	if (r_jacobian) {
		*r_jacobian = j;
	}
	return p;
}

Vector3 Skin::solve(const Vector3 &p_p, Vector3 p_q, float &r_miss) const {
	Basis j;
	float miss = (p_p - forward(p_q, &j)).length();
	for (int i = 0; i < 8 && miss > 1e-3f; i++) {
		const float det = j.determinant();
		if (std::abs(det) < 1e-6f) {
			break; // (a fold's edge: no way on from here)
		}
		Vector3 step = j.inverse().xform(p_p - forward(p_q));
		const float len = step.length();
		if (len > 4.0f) {
			step *= 4.0f / len;
		}
		// Back off while a step makes it worse.
		bool better = false;
		for (int k = 0; k < 4 && !better; k++, step *= 0.5f) {
			Basis j_new;
			const Vector3 q = p_q + step;
			const float m = (p_p - forward(q, &j_new)).length();
			if (m < miss) {
				p_q = q;
				miss = m;
				j = j_new;
				better = true;
			}
		}
		if (!better) {
			break;
		}
	}
	r_miss = miss;
	return p_q;
}

int Skin::inverse_candidates(const Vector3 &p_p, Candidate *r_candidates) const {
	if (joints.empty()) {
		r_candidates[0] = { p_p, 0.0f };
		return 1;
	}
	// The bones likeliest to own p: each one's rigid guess, scored by its
	// weight there.
	float w[MAX_BONES];
	int best[2] = { -1, -1 };
	float best_w[2] = { 0.0f, 0.0f };
	Vector3 guess[2];
	for (int b = 0; b < (int)joints.size(); b++) {
		const SkinJoint &jt = joints[b];
		const Vector3 q = jt.inverse_basis.xform(p_p - jt.matrix.origin);
		weights(q, w);
		if (w[b] > best_w[0]) {
			best[1] = best[0];
			best_w[1] = best_w[0];
			guess[1] = guess[0];
			best[0] = b;
			best_w[0] = w[b];
			guess[0] = q;
		} else if (w[b] > best_w[1]) {
			best[1] = b;
			best_w[1] = w[b];
			guess[1] = q;
		}
	}
	Vector3 starts[MAX_CANDIDATES];
	int n = 0;
	for (int k = 0; k < 2; k++) {
		if (best[k] >= 0) {
			starts[n++] = guess[k];
		}
	}
	starts[n++] = p_p; // (staying put: parts no bone moves)
	int count = 0;
	for (int s = 0; s < n; s++) {
		float miss;
		const Vector3 q = solve(p_p, starts[s], miss);
		bool same = false;
		for (int c = 0; c < count && !same; c++) {
			same = r_candidates[c].rest.distance_squared_to(q) < 0.25f;
			if (same && miss < r_candidates[c].miss) {
				r_candidates[c] = { q, miss };
			}
		}
		if (!same) {
			r_candidates[count++] = { q, miss };
		}
	}
	return count;
}

Vector3 Skin::inverse(const Vector3 &p_p, float *r_miss) const {
	Candidate c[MAX_CANDIDATES];
	const int n = inverse_candidates(p_p, c);
	int best = 0;
	for (int i = 1; i < n; i++) {
		if (c[i].miss < c[best].miss) {
			best = i;
		}
	}
	if (r_miss) {
		*r_miss = c[best].miss;
	}
	return c[best].rest;
}
