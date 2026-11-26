shader_type canvas_item;
render_mode unshaded;

// One sun shadow map (sun_shadows.gd): a square of the plane facing the sun,
// map_extent voxels across, centred on map_origin (which sits above the
// area it covers, towards the sun). Each texel marches a ray from there in
// the sun's light direction through the clipmap and stores how far it got
// before meeting solid ground (map_depth if it didn't), over map_depth. A
// surface point is in shadow if it lies farther along its texel's ray than
// that (terrain_shadow.gdshaderinc, sun_map_shadow()).
//
// Drawn only when the map moves or the terrain changes, a strip at a time,
// rather than one ray to the sun for every pixel of every frame.

#include "res://terrain_common.gdshaderinc"

uniform vec3 map_origin;
uniform vec3 map_right; // the plane's axes (UV x, y)
uniform vec3 map_up;
uniform vec3 sun_dir; // towards the sun
uniform float map_extent = 256.0;
uniform float map_depth = 512.0;
// The rows drawn this time (UV y): a map is drawn a strip a frame, the rest
// kept (the viewport isn't cleared), so no one frame pays for all of it.
uniform float band_lo = 0.0;
uniform float band_hi = 1.0;

void fragment() {
	if (UV.y < band_lo || UV.y >= band_hi) {
		discard;
	}
	vec2 ab = (UV - 0.5) * map_extent;
	vec3 start = map_origin + map_right * ab.x + map_up * ab.y;
	float t = 0.0;
	float t_prev = 0.0;
	float d_prev = -CLAMP;
	float hit = map_depth;
	for (int i = 0; i < 160; i++) {
		if (t > map_depth) {
			break;
		}
		float s;
		float far;
		float d = density(start - sun_dir * t, s, far);
		if (d > 0.0) {
			// Where it crossed, between the last point in air and this one.
			hit = mix(t_prev, t, clamp(-d_prev / (d - d_prev), 0.0, 1.0));
			break;
		}
		t_prev = t;
		d_prev = d;
		t += max(step_length(d, far, s), 0.05 * s);
	}
	COLOR = vec4(hit / map_depth, 0.0, 0.0, 1.0);
}
