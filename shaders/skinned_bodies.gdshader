shader_type spatial;
render_mode unshaded, cull_disabled, depth_draw_always, shadows_disabled;

// The posed grid bodies (those with bones: skinning.h), drawn in a pass of
// their own on a second screen quad after the terrain (world.gd makes it).
// Each pixel marches its view ray through those bodies alone, through
// their bones (skin.gdshaderinc), and writes depth, so the terrain (and the
// unposed bodies it draws) in front of one hides it and behind it doesn't:
// pixels with no posed body are discarded.
//
// Kept apart because the bones' maths made the terrain shader too much for
// Intel's driver. The grid uniforms here (grid_rows0 ...) are the posed
// bodies' own, set by SculptTerrain::bind_bodies(); the look (sun, sky,
// block textures) and the sun's shadow maps are the terrain material's,
// copied over. Lit like the terrain's bodies, but the sun's shadow from
// bodies is their bones' capsules (terrain_shadow.gdshaderinc) and the
// ambient light has no occlusion.

#include "res://shaders/terrain_common.gdshaderinc"
#include "res://shaders/terrain_shadow.gdshaderinc"
#include "res://shaders/terrain_look.gdshaderinc"
#include "res://shaders/skin.gdshaderinc"

void vertex() {
	// The quad (2x2) covers the screen whatever the camera does.
	POSITION = vec4(VERTEX.xy, 0.0, 1.0);
}

void fragment() {
	vec3 rd = view_ray(SCREEN_UV, INV_PROJECTION_MATRIX, INV_VIEW_MATRIX);
	vec3 ro = CAMERA_POSITION_WORLD + mat3(INV_VIEW_MATRIX) * EYE_OFFSET;
	vec3 sun = normalize(sun_direction);
	float cone = 2.0 / (abs(PROJECTION_MATRIX[1][1]) * VIEWPORT_SIZE.y);
	float far_view = view_distance();
	float t = far_view;
	vec3 rest;
	int grid = skin_trace(ro, rd, t, rest);
	if (grid < 0) {
		discard;
	}
	vec3 p = ro + rd * t;
	vec3 n = skin_grid_normal(grid, p);
	vec4 material = grid_material(grid, rest);
	vec3 albedo;
	if (material.x >= 0.0) {
		// Textures follow the rest shape (they stay on the skin as it bends).
		vec3 texture_normal = normalize(vec3(dot(grid_rows0[grid].xyz, n), dot(grid_rows1[grid].xyz, n), dot(grid_rows2[grid].xyz, n)));
		albedo = block_albedo(rest * grid_size[grid].w, texture_normal, int(material.x + 0.5), cone * t);
	} else {
		albedo = material.yzw;
	}
	if (bone_preview >= 0) {
		// The object editor's weight preview: the bone's share, warm.
		albedo = mix(albedo * 0.35, vec3(1.0, 0.45, 0.1), bone_weight(grid, bone_preview, rest));
	}
	if (grid_highlight[grid] > 0.5) {
		albedo = mix(albedo, vec3(1.0, 0.55, 0.12), 0.45 + 0.12 * sin(TIME * 4.0)); // (selected, as the terrain tints it)
	}
	float scale = grid_size[grid].w;
	float diffuse = max(dot(n, sun), 0.0);
	float shadow = 0.0;
	if (diffuse > 0.0) {
		bool shaded = capsule_shadow(p + n * 0.5 * scale, sun);
		shadow = shaded ? 0.0 : (sun_map_on ? sun_map_terrain(p, n, sun, scale) : 1.0);
	}
	vec3 sky_light = mix(horizon_color * 0.5, sky_color, 0.5 + 0.5 * n.y) * ambient_strength;
	vec3 color = albedo * (sun_color * diffuse * shadow + sky_light);
	// Compatibility renderer: NDC depth is -1..1, the depth buffer 0..1.
	vec4 clip = PROJECTION_MATRIX * VIEW_MATRIX * vec4(p, 1.0);
	DEPTH = clip.z / clip.w * 0.5 + 0.5;
	ALBEDO = mix(color, sky_base(rd), smoothstep(0.5 * far_view, far_view, t));
}
