#pragma once

#include <godot_cpp/variant/basis.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <vector>

namespace godot {

// Bones that warp a density grid (DensityShape's skeleton, DensityBody3D's
// pose of it), Blender style: each bone a segment from head to tail with an
// envelope round it, posed by a rotation, location and scale relative to
// its rest place (and its parent's pose).
//
// The grid itself never changes: the space round it bends. A point q of the
// rest shape goes to F(q) = w0 q + sum_b w_b(q) M_b q (linear blend
// skinning), M_b = pose_b * rest_b^-1. The weights:
//   - each bone's envelope (its "gate") says how much of the point bones
//     move at all: 1 within `radius` of its segment, easing to 0 over the
//     next `blend` (a quintic ease); what no gate covers (w0) stays with
//     the body;
//   - that share is split between the bones by a softmax of their
//     distances: exp(-d_b / softness_b). So a point well inside one bone's
//     part moves rigidly with it, and the hand-over between two bones is a
//     smooth band about `softness` wide round where they're equally near
//     (a joint). Summing envelopes instead (Blender's way) blends a limb
//     along its whole length: bent far, it folds over itself, and the joint
//     looks broken.
//
// Drawing and collision ask the other way: which rest point is at p? F is
// inverted numerically (inverse_candidates()): from each bone's rigid guess,
// Newton steps with F's full Jacobian (the weights' change included). On
// the inside of a strong bend F folds, and several rest points land on p:
// the caller takes the most solid of them (a union), so the folded flesh
// merges smoothly rather than flickering between them. The terrain shader
// does the same (terrain_shadow.gdshaderinc, skin_density()); keep them
// alike.

// A bone of the rest skeleton (shape space).
struct SkinBone {
	String name;
	int parent = -1; // index, before it in the list (-1: a root)
	Vector3 head, tail;
	float roll = 0.0f; // radians about the head->tail axis
	float radius = 4.0f; // gate: full within this of the segment...
	float blend = 2.0f; // ...easing to none over this much more
	float softness = 1.5f; // width of the hand-over to the next bone at a joint
};

// A bone's pose, relative to its rest place (in the bone's own axes: y
// along it, Blender's convention).
struct BonePose {
	Basis rotation; // (from a Quaternion)
	Vector3 location;
	Vector3 scale = Vector3(1, 1, 1);
	bool is_rest() const;
};

// One bone ready to skin with: its envelope and its matrix this pose.
struct SkinJoint {
	Vector3 head, tail;
	float radius = 0.0f, blend = 0.0f, softness = 1.0f;
	Transform3D matrix; // rest point -> posed point (M_b)
	Basis inverse_basis; // of matrix.basis (for the rigid guess)
};

class Skin {
public:
	static constexpr int MAX_BONES = 32; // per shape (SculptTerrain sends at most MAX_DRAWN_BONES in all)
	static constexpr int MAX_CANDIDATES = 3;

	// The rest frame of a bone: origin at the head, y along it, x and z
	// turned by its roll.
	static Transform3D rest_frame(const SkinBone &p_bone);
	// Pose matrices for a skeleton in a pose. Bones whose parent comes
	// later than themselves are treated as roots.
	void build(const std::vector<SkinBone> &p_bones, const std::vector<BonePose> &p_poses);
	void clear() {
		joints.clear();
		frames.clear();
	}
	bool active() const { return !joints.empty(); }
	int size() const { return (int)joints.size(); }
	const SkinJoint &joint(int p_index) const { return joints[p_index]; }
	// Posed bone frames (shape space): origin at the posed head, y along it.
	const Transform3D &frame(int p_index) const { return frames[p_index]; }

	// Every bone's weight at a rest point; returns w0 (staying put). With
	// r_gradients, each weight's gradient too (the gates' own slopes left
	// out: they're gentle, and only steer Newton's steps).
	float weights(const Vector3 &p_q, float *r_weights, Vector3 *r_gradients = nullptr) const;
	// Where a rest point goes, and F's Jacobian there.
	Vector3 forward(const Vector3 &p_q, Basis *r_jacobian = nullptr) const;
	// The rest points at p (up to MAX_CANDIDATES, each with how far F of it
	// misses p): one from each of the bones likeliest to own p, and p
	// itself. Returns how many.
	struct Candidate {
		Vector3 rest;
		float miss = 0.0f;
	};
	int inverse_candidates(const Vector3 &p_p, Candidate *r_candidates) const;
	// The best single one (least miss).
	Vector3 inverse(const Vector3 &p_p, float *r_miss = nullptr) const;

private:
	std::vector<SkinJoint> joints;
	std::vector<Transform3D> frames;
	Vector3 solve(const Vector3 &p_p, Vector3 p_q, float &r_miss) const;
};

} // namespace godot
