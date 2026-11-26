shader_type spatial;
render_mode unshaded, cull_disabled, depth_draw_never, depth_test_disabled, shadows_disabled;

// Half-resolution prepass for terrain.gdshader ("cone marching"). Each pixel
// here covers a 2x2 block of the full image, and its ray counts as touching a
// surface anywhere within the block's width (plus room for the density's
// slope), so:
//   G = 1: no ray of the block can hit anything; it is sky.
//   R: no ray of the block hits anything before this distance (divided by
//      START_SCALE); they start marching there.
// Must render before the full image (it is a viewport inside that one) into
// a float target (use_hdr_2d), with the same camera.

#include "res://terrain_common.gdshaderinc"

void vertex() {
	POSITION = vec4(VERTEX.xy, 0.0, 1.0);
}

void fragment() {
	vec3 rd = view_ray(SCREEN_UV, INV_PROJECTION_MATRIX, INV_VIEW_MATRIX);
	// In VR each eye's rays start at that eye (EYE_OFFSET, view space; zero
	// without multiview), so the two views have the right parallax.
	vec3 ro = CAMERA_POSITION_WORLD + mat3(INV_VIEW_MATRIX) * EYE_OFFSET;
	// One pixel here is two of the full image. The full image's rays are
	// within 0.71 of its pixels of this ray, and count a surface within 1 of
	// its pixels as a hit; the density can change ~1.7x faster than distance.
	// abs(): the projection is flipped vertically when rendering into a texture.
	float full_pixel = 1.0 / (abs(PROJECTION_MATRIX[1][1]) * VIEWPORT_SIZE.y);
	float hit_cone = (1.0 + 0.71 * 1.7) * full_pixel * 1.25;
	float t = 0.0;
	float t_prev, d_prev, d, scale;
	// No HIT_LIMIT: a hit here only sets where the full image starts, so a
	// wide one just starts it a little early.
	int result = march_core(ro, rd, hit_cone, 1e30, hit_cone, t, t_prev, d_prev, d, scale);
	float sky = result == MARCH_LEFT_VIEW ? 1.0 : 0.0;
	// Back off a little: the full image re-checks from here.
	float start = result == MARCH_HIT ? max(t_prev - 2.0 * scale, 0.0) : t_prev;
	// R holds distance / START_SCALE, kept under 1 (see START_SCALE).
	ALBEDO = vec3(min(start / START_SCALE, 0.999), sky, 0.0);
}
