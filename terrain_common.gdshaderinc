// Shared by terrain.gdshader and terrain_prepass.gdshader: reading the
// SculptTerrain density clipmap (gdextension/src/clipmap.h) and marching rays
// through it.

// Set by SculptTerrain. Level l has 2^l voxels per texel, stored as a 2D
// texture array with one layer per z slice (see Clipmap). Each texel (RGB8):
// R is the density in 1/16ths of the level's texel size + 128, so a level
// holds distances up to CLAMP of its own texels; G and B the material (see
// material_at): G under 128 is a block's texture layer, otherwise the point
// is a plain color whose 15 bits (5 red, 5 green, 5 blue, red highest) are
// G's low 7 bits and B. Storage wraps around: world texel w (= position /
// 2^l) is at w mod clip_size, hence repeat.
uniform sampler2DArray clip_texture0 : filter_linear, repeat_enable;
uniform sampler2DArray clip_texture1 : filter_linear, repeat_enable;
uniform sampler2DArray clip_texture2 : filter_linear, repeat_enable;
uniform sampler2DArray clip_texture3 : filter_linear, repeat_enable;
uniform sampler2DArray clip_texture4 : filter_linear, repeat_enable;
uniform sampler2DArray clip_texture5 : filter_linear, repeat_enable;
uniform sampler2DArray clip_texture6 : filter_linear, repeat_enable;
uniform sampler2DArray clip_texture7 : filter_linear, repeat_enable;
// Brick distances of every level (see far_distance): layer l * bricks + z is
// slice z of level l's bricks, which wrap around like the texels.
uniform sampler2DArray clip_bricks : filter_nearest, repeat_enable;
uniform float clip_brick_count = 32.0; // bricks per level side
uniform int clip_count = 0;
uniform vec3 clip_origin[8]; // world position of each level's first texel
uniform float clip_size = 256.0;
// Level l is used within clip_reach * 2^l voxels of the camera (largest of
// |dx|, |dy|, |dz|); Clipmap::reach(). Its box always reaches 3 texels beyond.
uniform float clip_reach = 109.0;
uniform vec3 view_position; // the camera position the clipmap was last centred on
uniform float view_distance_limit = 1000.0; // SculptTerrain.view_distance

const float CLAMP = 127.0 / 16.0; // DENSITY_CLAMP, in texels of the level
const float BRICK = 8.0; // Clipmap::BRICK
const float STEP_CONE = 1.5; // smallest step, in pixel widths
const float BLEND = 16.0; // texels over which a level hands over to the next
const int MAX_STEPS = 160;
// The most a drawn ray may pass from a surface and still count as hitting it,
// in voxels, however wide a pixel is out there. It must stay under the
// smallest distance open air can be stored as: an edited chunk's air (clamped
// at CLAMP voxels, ~7.9) and the generator's bound near island cell faces
// (its margin, 9). Far away, where a pixel is wider than that, rays would
// otherwise "hit" that air, drawing a grid of cell faces and boxes around
// edited chunks.
const float HIT_LIMIT = 4.0;

bool inside(vec3 q) {
	// Keep a texel of margin for interpolation and the normal's samples.
	return all(greaterThanEqual(q, vec3(1.0))) && all(lessThanEqual(q, vec3(clip_size - 2.0)));
}

// Whether a density read from a level with `scale` voxels per texel is an
// actual distance (true near the surface) rather than the clamped "at least
// CLAMP texels away".
bool near_surface(float d, float scale) {
	return d > (0.01 - CLAMP) * scale;
}

// How far a ray at world texel w of level L, CLAMP or more texels from the
// surface, can safely jump, in voxels. The brick holding w is k bricks
// (Chebyshev) from one with surface, so the surface is at least k - 1 bricks
// of texels away (2 fewer for interpolation). Surface beyond this level's
// reach is the next level's business (and unknown to this one), so the jump
// stops just past it: reach_left is how far that is, in texels. Both depend
// only on the camera and the world, not on where the level's box sits.
float far_distance(vec3 w, float level, float reach_left, float scale) {
	vec3 brick = floor(w / BRICK);
	float layer = level * clip_brick_count + mod(brick.z, clip_brick_count);
	float k = textureLod(clip_bricks, vec3((brick.xy + 0.5) / clip_brick_count, layer), 0.0).r * 255.0;
	float jump = min((k - 1.0) * BRICK - 2.0, reach_left + 2.0);
	// Beyond the bricks' reach, the clamped density itself says the surface is
	// at least ~CLAMP texels away; take the same 0.6 safety factor as near it.
	return max(jump, 0.6 * CLAMP) * scale;
}

// One level's density at p (and in r_far a safe step there), each with its
// own sampler: indexing the uniform arrays or choosing a sampler with a
// variable is several times slower on Intel GPUs.
//
// Layers are z slices, so the hardware filters within a layer and the shader
// blends the two layers around the point (storage wraps: layer = z mod size).
#define LEVEL_SAMPLER(NAME, TEX, LEVEL, SCALE) \
float NAME(vec3 p, float reach_left, out float r_far) { \
	vec3 w = p / SCALE; \
	vec2 uv = (w.xy + 0.5) / clip_size; \
	float z0 = floor(w.z); \
	float below = textureLod(TEX, vec3(uv, mod(z0, clip_size)), 0.0).r; \
	float above = textureLod(TEX, vec3(uv, mod(z0 + 1.0, clip_size)), 0.0).r; \
	float d = (mix(below, above, w.z - z0) * 255.0 - 128.0) / 16.0 * SCALE; \
	r_far = near_surface(d, SCALE) ? -d : far_distance(w, LEVEL, reach_left, SCALE); \
	return d; \
}
LEVEL_SAMPLER(level0, clip_texture0, 0.0, 1.0)
LEVEL_SAMPLER(level1, clip_texture1, 1.0, 2.0)
LEVEL_SAMPLER(level2, clip_texture2, 2.0, 4.0)
LEVEL_SAMPLER(level3, clip_texture3, 3.0, 8.0)
LEVEL_SAMPLER(level4, clip_texture4, 4.0, 16.0)
LEVEL_SAMPLER(level5, clip_texture5, 5.0, 32.0)
LEVEL_SAMPLER(level6, clip_texture6, 6.0, 64.0)
LEVEL_SAMPLER(level7, clip_texture7, 7.0, 128.0)

// The material at p, from the finest level that covers it (as density()
// picks them): x is a block's texture layer, or -1 for a plain color, which
// is then yzw. Points hold one material each, 1 level texel apart, so taking
// the nearest one would draw material boundaries as texel-sized cubes.
// Instead each of the 8 points around p votes with its trilinear weight
// (solid points count more: the surface belongs to them) for its kind of
// material, a block or "colored", and the kind with the most weight wins.
// The boundary then runs where the votes balance, a smooth curve between
// the points. Colors blend: a colored result is the vote-weighted average of
// the colored points, so painted gradients come out smooth.
#define MATERIAL_LEVEL(L, TEX, SCALE) \
if (clip_count > L && reach <= clip_reach * SCALE && inside((p - clip_origin[L]) / SCALE)) { \
	vec3 w = p / SCALE; \
	vec3 b = floor(w); \
	vec3 f = w - b; \
	int kinds[8]; \
	vec3 colors[8]; \
	float votes[8]; \
	for (int k = 0; k < 8; k++) { \
		vec3 o = vec3(float(k & 1), float((k >> 1) & 1), float((k >> 2) & 1)); \
		vec3 texel = texelFetch(TEX, ivec3(mod(b + o, clip_size)), 0).rgb; \
		vec3 tw = mix(1.0 - f, f, o); \
		float solid = texel.r * 255.0 > 128.5 ? 1.0 : 0.15; \
		int g = int(texel.g * 255.0 + 0.5); \
		int bits = (g & 127) * 256 + int(texel.b * 255.0 + 0.5); \
		kinds[k] = g < 128 ? g : 128; \
		colors[k] = vec3(float((bits >> 10) & 31), float((bits >> 5) & 31), float(bits & 31)) / 31.0; \
		votes[k] = tw.x * tw.y * tw.z * solid; \
	} \
	int best = kinds[0]; \
	float best_votes = -1.0; \
	for (int k = 0; k < 8; k++) { \
		float total = 0.0; \
		for (int j = 0; j < 8; j++) { \
			total += kinds[j] == kinds[k] ? votes[j] : 0.0; \
		} \
		if (total > best_votes) { \
			best_votes = total; \
			best = kinds[k]; \
		} \
	} \
	if (best < 128) { \
		return vec4(float(best), 0.0, 0.0, 0.0); \
	} \
	vec3 sum = vec3(0.0); \
	for (int k = 0; k < 8; k++) { \
		sum += kinds[k] == 128 ? colors[k] * votes[k] : vec3(0.0); \
	} \
	return vec4(-1.0, sum / max(best_votes, 1e-6)); \
}
vec4 material_at(vec3 p) {
	vec3 rel = abs(p - view_position);
	float reach = max(max(rel.x, rel.y), rel.z);
	MATERIAL_LEVEL(0, clip_texture0, 1.0)
	MATERIAL_LEVEL(1, clip_texture1, 2.0)
	MATERIAL_LEVEL(2, clip_texture2, 4.0)
	MATERIAL_LEVEL(3, clip_texture3, 8.0)
	MATERIAL_LEVEL(4, clip_texture4, 16.0)
	MATERIAL_LEVEL(5, clip_texture5, 32.0)
	MATERIAL_LEVEL(6, clip_texture6, 64.0)
	MATERIAL_LEVEL(7, clip_texture7, 128.0)
	return vec4(2.0, 0.0, 0.0, 0.0); // stone
}

// Which level a point uses depends only on its distance from the camera:
// level l covers up to clip_reach * 2^l, handing over to level l + 1 across
// the last BLEND * 2^l. Because the choice doesn't depend on where a level's
// box happens to be, the image doesn't change when one moves, and the edge
// of a box (where a move's new layers may still be uploading) is never read.
#define TRY_LEVEL(L, NAME, NEXT, SCALE) \
if (clip_count > L && reach <= clip_reach * SCALE) { \
	vec3 q = (p - clip_origin[L]) / SCALE; \
	if (inside(q)) { \
		r_scale = SCALE; \
		float d = NAME(p, clip_reach - reach / SCALE, r_far); \
		float w = clamp((clip_reach * SCALE - reach) / (BLEND * SCALE), 0.0, 1.0); \
		if (w < 1.0 && clip_count > L + 1) { \
			vec3 q_next = (p - clip_origin[L + 1]) / (SCALE * 2.0); \
			if (inside(q_next)) { \
				float far_next; \
				d = mix(NEXT(p, clip_reach - reach / (SCALE * 2.0), far_next), d, w); \
				r_far = near_surface(d, SCALE) ? -d : min(r_far, far_next); \
			} \
		} \
		return d; \
	} \
}

// Density at p (> 0 = solid). r_far: in air, a distance that is safe to step;
// r_scale: voxels per texel of the finest level used.
float density(vec3 p, out float r_scale, out float r_far) {
	vec3 rel = abs(p - view_position);
	float reach = max(max(rel.x, rel.y), rel.z);
	TRY_LEVEL(0, level0, level1, 1.0)
	TRY_LEVEL(1, level1, level2, 2.0)
	TRY_LEVEL(2, level2, level3, 4.0)
	TRY_LEVEL(3, level3, level4, 8.0)
	TRY_LEVEL(4, level4, level5, 16.0)
	TRY_LEVEL(5, level5, level6, 32.0)
	TRY_LEVEL(6, level6, level7, 64.0)
	if (clip_count > 7 && reach <= clip_reach * 128.0) {
		vec3 q = (p - clip_origin[7]) / 128.0;
		if (inside(q)) {
			r_scale = 128.0;
			return level7(p, clip_reach - reach / 128.0, r_far);
		}
	}
	r_scale = exp2(float(max(clip_count - 1, 0)));
	r_far = CLAMP * r_scale;
	return -CLAMP * r_scale; // outside every level: treat as air
}

float density_only(vec3 p) {
	float s;
	float f;
	return density(p, s, f);
}

// How far the clipmap reaches along a ray, at most.
float view_distance() {
	// The last level ends at its reach (a cube around the camera); fade out
	// before that.
	return min(view_distance_limit, clip_reach * exp2(float(max(clip_count - 1, 0))));
}

// Near the surface the density is an estimate that can change by up to ~1.7
// per voxel, so only part of it is safe to step; far away r_far is safe as is.
float step_length(float d, float far, float scale) {
	return near_surface(d, scale) ? max(-d * 0.6, 0.1 * scale) : far;
}

// A ray through the screen pixel at SCREEN_UV, in world space.
vec3 view_ray(vec2 screen_uv, mat4 inv_projection, mat4 inv_view) {
	vec4 far_point = inv_projection * vec4(screen_uv * 2.0 - 1.0, 1.0, 1.0);
	return normalize((inv_view * vec4(far_point.xyz / far_point.w, 0.0)).xyz);
}

// The prepass stores start distances divided by this.
// The prepass's float target stores values up to 1 exactly but distorts
// larger ones (8 comes back as ~9.5), which would start rays past what they
// should hit; this keeps every distance a clipmap can reach (256 texels, 8
// levels: 13952 voxels) under 1. At 16-bit float that is a step of at most
// 8 voxels (near the far end), well inside the prepass's back-off.
const float START_SCALE = 16384.0;

const int MARCH_HIT = 0;
const int MARCH_LEFT_VIEW = 1;
const int MARCH_OUT_OF_STEPS = 2;

// Marches from r_t until the ray comes within p_hit_cone * t (at most
// p_hit_limit) of a surface
// (MARCH_HIT; r_d > 0 means it is inside the solid, r_t_prev / r_d_prev the
// last point before), leaves the view, or runs out of steps. It never steps
// less than p_step_cone * t: rays skimming past a surface would otherwise
// creep along it in tiny steps (and a GPU runs a group of pixels as slowly
// as its slowest one). The cones are angles: a pixel's width per unit of
// distance, times however many pixels.
int march_core(vec3 ro, vec3 rd, float p_hit_cone, float p_hit_limit, float p_step_cone, inout float r_t,
		out float r_t_prev, out float r_d_prev, out float r_d, out float r_scale) {
	float t_end = view_distance();
	r_t_prev = r_t;
	r_d_prev = -CLAMP;
	r_d = -CLAMP;
	r_scale = 1.0;
	for (int i = 0; i < MAX_STEPS; i++) {
		if (r_t > t_end) {
			return MARCH_LEFT_VIEW;
		}
		float far;
		r_d = density(ro + rd * r_t, r_scale, far);
		if (r_d > -min(p_hit_cone * r_t, p_hit_limit)) {
			return MARCH_HIT;
		}
		r_t_prev = r_t;
		r_d_prev = r_d;
		// Within a pixel of a surface but outside p_hit_limit: a step as long
		// as the usual minimum (1.5 pixels, tens of voxels far away) could
		// jump right over a thin island, so take shorter ones until the ray
		// either hits or moves away.
		float min_step = p_step_cone * r_t;
		if (r_d > -p_hit_cone * r_t) {
			min_step = min(min_step, 1.5 * p_hit_limit);
		}
		r_t += max(step_length(r_d, far, r_scale), min_step);
	}
	return MARCH_OUT_OF_STEPS;
}
