shader_type spatial;
render_mode unshaded, cull_disabled, depth_draw_always, shadows_disabled;

// Raymarches the SculptTerrain density clipmap. Drawn on a quad that covers
// the screen; each pixel steps along its view ray through the finest clipmap
// level that contains the current point, then shades the hit with a sun
// shadow ray and ambient occlusion through the same data. The physics
// bodies' spheres (DensityBody3D) are drawn here too, lit the same way;
// posed ones (bones) by skinned_bodies.gdshader, after this.
// Writes depth so ordinary meshes (the brush preview) sit in the scene.
//
// terrain_prepass.gdshader marches a half-resolution image first, with rays
// wide enough to cover each 2x2 block of pixels here: a block it says is sky
// needs no march at all, and the others start where it first came close to a
// surface.

#include "res://shaders/terrain_common.gdshaderinc"
#include "res://shaders/terrain_shadow.gdshaderinc"
#include "res://shaders/terrain_look.gdshaderinc"

// R: where to start (voxels along the ray / START_SCALE), G: 1 if the
// block is all sky.
uniform sampler2D prepass_texture : filter_nearest;
uniform bool use_prepass = false;


// The sea (terrain worlds): still water at sea_level wherever open air
// there has nothing solid straight above it (so caves and overhangs stay
// dry, while a pit dug below sea level in the open fills).
uniform bool has_sea = false;
uniform float sea_level = 0.0;
uniform vec3 water_tint : source_color = vec3(0.24, 0.42, 0.86);
// Light through water falls to 1/e over this many voxels: how far down the
// sea floor shows, and how far you can see underwater.
uniform float water_clarity = 7.0;
// Ripples on the surface (res://assets/block/water.png, set by world.gd).
uniform sampler2D water_texture : filter_nearest_mipmap, repeat_enable;

// The selection (SculptTerrain.select_*()): a mask over its bounding box,
// a texel per selection_cell points from selection_origin, layer = z.
uniform bool selection_on = false;
uniform sampler2DArray selection_mask : filter_nearest;
uniform vec3 selection_origin;
uniform float selection_cell = 1.0;
uniform vec3 selection_dims;
uniform vec3 selection_tint : source_color = vec3(1.0, 0.55, 0.12);

// Whether the terrain at p (normal n) is selected: the lattice point just
// inside the surface.
bool selected_at(vec3 p, vec3 n) {
	vec3 q = floor((floor(p - n * 0.6 + 0.5) - selection_origin) / selection_cell);
	if (any(lessThan(q, vec3(0.0))) || any(greaterThanEqual(q, selection_dims))) {
		return false;
	}
	return texelFetch(selection_mask, ivec3(q), 0).r > 0.5;
}

// Edited chunks store open air as at most DENSITY_CLAMP (127/16) voxels from
// a surface, at every level, though it may be much farther. To shadows and
// ambient occlusion that would look like a surface just out of reach all
// over an edited chunk, darkening it once coarse levels (whose samples reach
// farther than that) are used there. So distances approaching that limit
// count less and less as a nearby surface.
const float EDITED_AIR = 127.0 / 16.0;
float nearby(float d) {
	return 1.0 - smoothstep(EDITED_AIR - 2.0, EDITED_AIR - 0.05, -d);
}

// Distance along the ray to the terrain, or -1. p_cone: how wide a pixel is
// per unit of distance; a ray that passes within one pixel of a surface
// counts as a hit, and steps are at least 1.5 pixels.
float march(vec3 ro, vec3 rd, float t_start, float p_cone) {
	float t = t_start;
	float t_prev, d_prev, d, scale;
	int result = march_core(ro, rd, p_cone, HIT_LIMIT, STEP_CONE * p_cone, t, t_prev, d_prev, d, scale);
	float hit = -1.0;
	if (result == MARCH_OUT_OF_STEPS) {
		// That happens to rays skimming low over the ground for a long way;
		// if one is within a few voxels of a surface, drawing that beats
		// leaving a hole. (Not just "near" one as the clipmap sees it: open
		// air can be stored a few voxels from a surface, e.g. in edited
		// chunks, and a ray running along it would draw it as ground.)
		hit = d > -1.5 * HIT_LIMIT ? t : -1.0;
	} else if (result == MARCH_HIT) {
		if (d <= 0.0) {
			hit = t; // grazing within a pixel
		} else {
			// The density is close to linear across one step: two secant
			// steps between the last point in air and this one in solid.
			float a = t_prev, da = d_prev, b = t, db = d;
			for (int j = 0; j < 2; j++) {
				float m = a + (b - a) * clamp(-da / (db - da), 0.05, 0.95);
				float dm = density_only(ro + rd * m);
				if (dm > 0.0) {
					b = m;
					db = dm;
				} else {
					a = m;
					da = dm;
				}
			}
			hit = a + (b - a) * clamp(-da / (db - da), 0.0, 1.0);
		}
	}
	return hit;
}

vec3 normal_at(vec3 p, float scale) {
	float e = 0.5 * scale;
	vec2 k = vec2(1.0, -1.0);
	vec3 g = k.xyy * density_only(p + k.xyy * e) + k.yyx * density_only(p + k.yyx * e) +
			k.yxy * density_only(p + k.yxy * e) + k.xxx * density_only(p + k.xxx * e);
	// Density grows into the solid. If the samples can't tell (all equal),
	// fall back to facing up rather than a NaN that shades as garbage.
	float len = length(g);
	return len > 1e-6 ? -g / len : vec3(0.0, 1.0, 0.0);
}

float ambient_occlusion(vec3 p, vec3 n, float scale) {
	float occlusion = 0.0;
	float weight = 1.0;
	for (int i = 1; i <= 3; i++) {
		float h = float(i) * 1.5 * scale;
		float d = density_only(p + n * h);
		occlusion += weight * nearby(d) * clamp(h + d, 0.0, h) / h; // how much closer the surface is than h
		weight *= 0.6;
	}
	return clamp(1.0 - 0.35 * occlusion, 0.0, 1.0);
}

// The topmost ground's height at a column position, interpolated between
// the four columns around it (so shorelines are smooth curves).
// Whether nothing solid is straight above p, as far as the clipmap knows:
// a march straight up until it leaves the terrain (or runs out of steps,
// which counts as open).
const int SKY_STEPS = 48;
bool open_sky(vec3 p) {
	float t = 0.5;
	for (int i = 0; i < SKY_STEPS; i++) {
		float s;
		float far;
		float d = density(p + vec3(0.0, t, 0.0), s, far);
		if (d > 0.0) {
			return false;
		}
		t += max(step_length(d, far, s), 0.5 * s + 0.02 * t);
	}
	return true;
}

// Water at p (on or below sea level): open air with open sky above.
bool in_sea(vec3 p) {
	return density_only(p) < 0.0 && open_sky(p);
}

// How much light gets through p_distance voxels of water, per colour: red
// fades fastest, blue slowest (so shallows look turquoise, depths blue).
vec3 water_transmission(float p_distance) {
	vec3 absorb = vec3(1.6, 0.75, 0.45);
	return exp(-p_distance * absorb / water_clarity);
}

vec3 sky(vec3 rd, vec3 sun) {
	return sky_base(rd) + sun_color * pow(max(dot(rd, sun), 0.0), 400.0) * 2.0;
}

// What a ray sees, given what it hits otherwise (t_ground, -1 for sky, and
// its colour), once the sea is added. From above: the surface (where the
// ray meets sea_level, exactly), reflecting the sky over the ground below,
// which fades into the water's colour with depth. From underwater: fog up to
// the ground or the surface. r_t_seen: distance for the haze (-1 = none).
vec3 sea_view(vec3 ro, vec3 rd, float t_ground, vec3 ground_color, vec3 sun, float cone, inout float r_t_seen) {
	vec3 scatter = water_tint * (0.22 + 0.3 * max(sun.y, 0.0)); // light the water itself sends back
	if (ro.y < sea_level) {
		if (!in_sea(ro)) {
			return ground_color; // below sea level but dry (a cave)
		}
		float t_surface = rd.y > 0.0 ? (sea_level - ro.y) / rd.y : 1e6;
		bool ground_first = t_ground >= 0.0 && t_ground <= t_surface;
		r_t_seen = -1.0;
		vec3 beyond = ground_first ? ground_color : scatter * 2.0; // the surface from below: bright
		return mix(scatter, beyond, water_transmission(ground_first ? t_ground : t_surface));
	}
	if (rd.y >= 0.0) {
		return ground_color;
	}
	float t_sea = (sea_level - ro.y) / rd.y;
	if (t_ground >= 0.0 && t_ground <= t_sea) {
		// The ray hit ground before reaching the surface. Ground hits are
		// only as close as about a pixel's width, so over shallow water a
		// ray can "hit" the sea floor just short of the surface. If the
		// ray then carries on to the surface through air, that's what
		// happened; ground in between means land in front of the water.
		if (t_sea - t_ground > 4.0 * HIT_LIMIT) {
			return ground_color;
		}
		for (int k = 1; k <= 3; k++) {
			if (density_only(ro + rd * mix(t_ground, t_sea, float(k) / 4.0)) > 0.0) {
				return ground_color;
			}
		}
	}
	vec3 q = ro + rd * t_sea;
	float scale;
	float unused;
	float d = density(q, scale, unused);
	if (d >= 0.0 || !open_sky(q)) {
		return ground_color; // the ground rises through sea level here, or the sea is roofed over
	}
	// How much water the ray crosses: as far as it goes to the ground below,
	// and at least what the distance from q down to the nearest ground
	// (the density there) says, followed along the view as if the bottom
	// were level. The second is smooth where hits are rough (far away).
	float through = t_ground < 0.0 ? 1e3 : max(t_ground - t_sea, -d / max(-rd.y, 0.15));
	vec3 below = mix(scatter, ground_color, water_transmission(through));
	// The water texture as ripples, at the blocks' scale (see texture_size).
	float lod = max(log2(cone * t_sea * 16.0 / texture_size), 0.0);
	float ripple = textureLod(water_texture, q.xz / texture_size, lod).r;
	below *= 0.7 + 0.7 * ripple;
	float fresnel = 0.02 + 0.98 * pow(1.0 - abs(rd.y), 5.0);
	r_t_seen = t_sea;
	return mix(below, sky(reflect(rd, vec3(0.0, 1.0, 0.0)), sun), fresnel);
}

void vertex() {
	// The quad (2x2) covers the screen whatever the camera does.
	POSITION = vec4(VERTEX.xy, 0.0, 1.0);
}

void fragment() {
	vec3 rd = view_ray(SCREEN_UV, INV_PROJECTION_MATRIX, INV_VIEW_MATRIX);
	// In VR each eye's rays start at that eye (EYE_OFFSET, view space; zero
	// without multiview), so the two views have the right parallax.
	vec3 ro = CAMERA_POSITION_WORLD + mat3(INV_VIEW_MATRIX) * EYE_OFFSET;
	vec3 sun = normalize(sun_direction);

	// Projection [1][1] = 1 / tan(fov / 2): a pixel spans 2 / ([1][1] * height).
	// abs(): the projection can be flipped vertically when rendering into a texture.
	float cone = 2.0 / (abs(PROJECTION_MATRIX[1][1]) * VIEWPORT_SIZE.y);
	float t = -1.0;
	vec2 pre = use_prepass ? texelFetch(prepass_texture, ivec2(FRAGCOORD.xy) / 2, 0).rg : vec2(0.0);
	if (pre.g < 0.5) {
		t = march(ro, rd, pre.r * START_SCALE, cone);
	}
	// The physics bodies (DensityBody3D, drawn): spheres hit exactly, grids
	// marched, whichever is nearest (and nearer than the ground).
	float t_body;
	int sphere = trace_spheres(ro, rd, t_body);
	if (sphere >= 0 && (t < 0.0 || t_body < t)) {
		t = t_body;
	} else {
		sphere = -1;
	}
	float t_grid = t < 0.0 ? view_distance() : t;
	int grid = trace_grids(ro, rd, t_grid);
	if (grid >= 0) {
		t = t_grid;
		sphere = -1;
	}

	vec3 color; // what the ray sees, before haze
	float t_seen = t; // how far that is, for the haze (-1: none)
	if (t < 0.0) {
		color = sky(rd, sun);
		DEPTH = 0.0; // the far plane (the Compatibility renderer's depth is reversed)
	} else {
		vec3 p = ro + rd * t;
		float scale;
		float unused;
		density(p, scale, unused);
		vec3 n;
		vec3 albedo;
		vec4 material;
		vec3 texture_position = p; // where block textures are projected from
		if (sphere >= 0) {
			n = (p - body_spheres[sphere].xyz) / body_spheres[sphere].w;
			vec4 look = body_colors[sphere];
			material = look.w >= 0.0 ? vec4(look.w, 0.0, 0.0, 0.0) : vec4(-1.0, look.rgb);
		} else if (grid >= 0) {
			n = grid_normal(grid, p);
			// A little inside, so the solid side decides.
			vec3 q = grid_point(grid, p - n * 0.1 * grid_size[grid].w);
			material = grid_material(grid, q);
			// In the body's own frame, so its textures turn with it.
			texture_position = q * grid_size[grid].w;
		} else {
			n = normal_at(p, scale);
			// A little inside the surface, so the solid side decides.
			material = material_at(p - n * 0.1 * scale);
		}
		if (material.x >= 0.0) {
			vec3 texture_normal = n;
			if (grid >= 0) {
				texture_normal = normalize(vec3(dot(grid_rows0[grid].xyz, n), dot(grid_rows1[grid].xyz, n), dot(grid_rows2[grid].xyz, n)));
			}
			albedo = block_albedo(texture_position, texture_normal, int(material.x + 0.5), cone * t);
		} else {
			// A painted color (in the same space as the textures' texels, so
			// paint over a block matches its average).
			albedo = material.yzw;
		}

		if ((selection_on && grid < 0 && sphere < 0 && selected_at(p, n)) || (grid >= 0 && grid_highlight[grid] > 0.5)) {
			// Tinted, with a slow pulse so it reads as a selection, not paint.
			albedo = mix(albedo, selection_tint, 0.45 + 0.12 * sin(TIME * 4.0));
		}
		float diffuse = max(dot(n, sun), 0.0);
		// Sun shadows fade out far away: there islands are only a few coarse
		// texels across, and their self-shadowing is noise (haze is taking
		// over by then anyway), and past the fade no shadow ray is cast. In
		// voxels: tied to clip_reach, the fade moved past the view distance
		// when the clipmap grew, and every pixel paid for a shadow ray.
		float shadow_fade = smoothstep(300.0, 700.0, t);
		float shadow = 0.0;
		if (diffuse > 0.0) {
			shadow = shadow_fade < 1.0 ? mix(sun_map_on ? sun_map_shadow(p, n, sun, scale) : sun_shadow(p, n, sun, scale), 1.0, shadow_fade) : 1.0;
		}
		float ao = ambient_occlusion(p, n, scale) * body_occlusion(p, n, sphere, grid);
		vec3 sky_light = mix(horizon_color * 0.5, sky_color, 0.5 + 0.5 * n.y) * ambient_strength;
		color = albedo * (sun_color * diffuse * shadow + sky_light * ao);

		// Compatibility renderer: NDC depth is -1..1, the depth buffer 0..1.
		vec4 clip = PROJECTION_MATRIX * VIEW_MATRIX * vec4(p, 1.0);
		DEPTH = clip.z / clip.w * 0.5 + 0.5;
	}
	if (has_sea) {
		color = sea_view(ro, rd, t, color, sun, cone, t_seen);
	}
	// Haze towards the sky behind, fully opaque at the view distance.
	float far_view = view_distance();
	ALBEDO = t_seen < 0.0 ? color : mix(color, sky_base(rd), smoothstep(0.5 * far_view, far_view, t_seen));
}
