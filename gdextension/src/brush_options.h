#pragma once

#include <godot_cpp/variant/vector3.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

// How the brushes are shaped, for the terrain's and the bodies' alike
// (SculptTerrain::apply_brush(), DensityShape::brush_grid()): the local
// player's brush settings (SculptTerrain's brush_* properties set them).
// Shared, not per object: one player holds one brush. Edits reach other
// players as the changed data, never replayed, so they needn't match there.
namespace godot {

enum BrushFalloff {
	FALLOFF_SMOOTH, // eases out (smoothstep); Draw / Carve stay whole spheres
	FALLOFF_LINEAR, // straight down to the edge
	FALLOFF_SHARP, // strong in the middle, little at the edge ((1 - t)^2)
	FALLOFF_ROUND, // a dome: full most of the way, dropping at the edge
	FALLOFF_CONSTANT, // the same everywhere within the radius
	FALLOFF_COUNT,
};

enum BrushShapeKind {
	BRUSH_SHAPE_SPHERE,
	BRUSH_SHAPE_CUBE, // along the axes (the world's; a body's own)
};

enum FlattenMode {
	FLATTEN_BOTH, // raise what's below the plane and cut what's above
	FLATTEN_FILL, // only raise, up to the plane
	FLATTEN_SCRAPE, // only cut, down to it
};

struct BrushOptions {
	int falloff = FALLOFF_SMOOTH;
	int shape = BRUSH_SHAPE_SPHERE;
	int flatten_mode = FLATTEN_BOTH;
	// Layer: how far it raises the surface (above where it was when the
	// stroke began), voxels.
	float layer_height = 2.0f;
	// Clone: added to a point to find where it copies from (world space;
	// a body's brush takes it into its own).
	Vector3 clone_offset;
	// Clay strips: which way the stroke runs (world space; zero: any).
	Vector3 stroke_direction;
	// Counts strokes (SculptTerrain::begin_stroke()): what a body's layer
	// brush measures from is renewed with each.
	uint32_t stroke_serial = 0;
};

inline BrushOptions &brush_options() {
	static BrushOptions options;
	return options;
}

// The brush's weight at t = distance / radius (0 middle, 1 edge).
inline float brush_falloff(float t, int p_falloff) {
	if (t >= 1.0f) {
		return 0.0f;
	}
	t = std::max(t, 0.0f);
	switch (p_falloff) {
		case FALLOFF_LINEAR:
			return 1.0f - t;
		case FALLOFF_SHARP:
			return (1.0f - t) * (1.0f - t);
		case FALLOFF_ROUND:
			return std::sqrt(1.0f - t * t);
		case FALLOFF_CONSTANT:
			return 1.0f;
		default:
			return 1.0f - t * t * (3.0f - 2.0f * t);
	}
}

inline float brush_falloff(float t) {
	return brush_falloff(t, brush_options().falloff);
}

// Inflate: full over the inner half of the brush, then the falloff out to
// its edge (a swelling, not a bump).
inline float inflate_falloff(float t) {
	return brush_falloff(std::max(t - 0.5f, 0.0f) * 2.0f);
}

// The distance the brush measures (from its middle): straight, or for a
// cube the largest along any axis.
inline float brush_distance(const Vector3 &d) {
	if (brush_options().shape == BRUSH_SHAPE_CUBE) {
		return std::max(std::abs(d.x), std::max(std::abs(d.y), std::abs(d.z)));
	}
	return d.length();
}

// Draw / Carve: their weight by the falloff, except with the default
// (smooth) one, where they build whole spheres (cubes) as they always did.
inline float sphere_brush_weight(float t) {
	const int f = brush_options().falloff;
	if (f == FALLOFF_SMOOTH) {
		return 1.0f;
	}
	return brush_falloff(t, f);
}

// Flatten's change at a point, as fill / scrape only allow.
inline float flatten_limit(float p_before, float p_after) {
	switch (brush_options().flatten_mode) {
		case FLATTEN_FILL:
			return std::max(p_before, p_after);
		case FLATTEN_SCRAPE:
			return std::min(p_before, p_after);
		default:
			return p_after;
	}
}

// Clay strips' footprint: a square in the surface's plane, one side along
// the stroke. Returns (along, across, up) coordinates of d.
inline Vector3 strip_frame(const Vector3 &d, const Vector3 &p_normal, const Vector3 &p_direction) {
	Vector3 along = p_direction - p_normal * p_direction.dot(p_normal);
	if (along.length_squared() < 1e-6f) {
		along = std::abs(p_normal.y) < 0.9f ? Vector3(0, 1, 0).cross(p_normal) : Vector3(1, 0, 0).cross(p_normal);
	}
	along = along.normalized();
	const Vector3 across = p_normal.cross(along);
	return Vector3(d.dot(along), d.dot(across), d.dot(p_normal));
}

} // namespace godot
