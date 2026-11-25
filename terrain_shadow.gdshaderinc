// The sun's shadows, for terrain.gdshader (and any mesh that should match
// it): the terrain itself, through the clipmap (terrain_common.gdshaderinc),
// plus the physics bodies, so they cast shadows too.

const int SHADOW_STEPS = 96;
// (300 cost ~1.7 ms a frame more on an Intel HD 4400, for shadows longer
// than most scenes have.)
const float SHADOW_DISTANCE = 150.0;

// The drawn physics bodies (DensityBody3D with `drawn`), nearest the camera
// first; SculptTerrain sends them every frame. terrain.gdshader draws them;
// here they cast shadows and darken what is near them. Two kinds:
//
// Sphere clusters (bodies without a shape), drawn as exact spheres.
const int MAX_BODY_SPHERES = 32; // SculptTerrain::MAX_BODY_SPHERES
uniform vec4 body_spheres[MAX_BODY_SPHERES]; // xyz centre, w radius
uniform vec4 body_colors[MAX_BODY_SPHERES]; // rgb, w: block texture layer, or -1 for the plain colour
uniform int body_sphere_count = 0;

// Density grids (bodies with a DensityShape), marched like the terrain.
// Each pose key of a grid is a run of body_atlas's 64x64 layers, one per z
// from its first, its texels like the clipmap's (R density in 1/16 voxels +
// 128, G B material).
const int MAX_GRID_BODIES = 48; // SculptTerrain::MAX_GRID_BODIES
const float GRID_SLOT = 64.0; // DensityShape::MAX_SIZE
uniform sampler2DArray body_atlas : filter_linear;
// World point -> grid point (in voxels of the grid): dot(row, vec4(p, 1)).
uniform vec4 grid_rows0[MAX_GRID_BODIES];
uniform vec4 grid_rows1[MAX_GRID_BODIES];
uniform vec4 grid_rows2[MAX_GRID_BODIES];
uniform vec4 grid_size[MAX_GRID_BODIES]; // points a side, w: world size of a grid voxel
uniform vec4 grid_pose[MAX_GRID_BODIES]; // first layer of the key below the pose, of the one above, the blend
uniform vec4 grid_bound[MAX_GRID_BODIES]; // bounding sphere: centre, radius
uniform int grid_count = 0;
vec3 grid_point(int i, vec3 p) {
	vec4 p4 = vec4(p, 1.0);
	return vec3(dot(grid_rows0[i], p4), dot(grid_rows1[i], p4), dot(grid_rows2[i], p4));
}

// One key's density at grid point c (inside the grid), grid voxels.
float grid_slot_density(vec3 c, float layer0, float z_last) {
	float z0 = floor(c.z);
	vec2 uv = (c.xy + 0.5) / GRID_SLOT;
	float below = textureLod(body_atlas, vec3(uv, layer0 + z0), 0.0).r;
	float above = textureLod(body_atlas, vec3(uv, layer0 + min(z0 + 1.0, z_last)), 0.0).r;
	return (mix(below, above, c.z - z0) * 255.0 - 128.0) / 16.0;
}

// Grid body i's density at its grid point q (grid voxels): its pose's two
// keys blended; beyond the grid, the edge's value less how far out.
float grid_rest_density(int i, vec3 q) {
	vec3 last = grid_size[i].xyz - 1.0;
	vec3 c = clamp(q, vec3(0.0), last);
	vec4 pose = grid_pose[i];
	float d = grid_slot_density(c, pose.x, last.z);
	if (pose.z > 0.0) {
		d = mix(d, grid_slot_density(c, pose.y, last.z), pose.z);
	}
	return d - length(q - c);
}

// Grid body i's density at world point p (> 0 inside), in world voxels.
float grid_density(int i, vec3 p) {
	return grid_rest_density(i, grid_point(i, p)) * grid_size[i].w;
}

vec3 grid_normal(int i, vec3 p) {
	float e = 0.5 * grid_size[i].w;
	vec2 k = vec2(1.0, -1.0);
	vec3 g = k.xyy * grid_density(i, p + k.xyy * e) + k.yyx * grid_density(i, p + k.yyx * e) +
			k.yxy * grid_density(i, p + k.yxy * e) + k.xxx * grid_density(i, p + k.xxx * e);
	float len = length(g);
	return len > 1e-6 ? -g / len : vec3(0.0, 1.0, 0.0);
}

// The material of grid body i at p, as material_at() gives the terrain's
// (and the same way: the 8 points round it vote by trilinear weight, solid
// ones counting more, so boundaries are smooth curves rather than cubes):
// x a block's texture layer, or -1 and the colour in yzw. q: the grid point
// (grid_point()).
vec4 grid_material(int i, vec3 q_point) {
	vec3 q = clamp(q_point, vec3(0.0), grid_size[i].xyz - 1.0);
	vec3 b = floor(q);
	vec3 f = q - b;
	vec3 last = grid_size[i].xyz - 1.0;
	float layer0 = grid_pose[i].x;
	int kinds[8];
	vec3 colors[8];
	float votes[8];
	for (int k = 0; k < 8; k++) {
		vec3 o = vec3(float(k & 1), float((k >> 1) & 1), float((k >> 2) & 1));
		vec3 c = min(b + o, last);
		vec3 texel = texelFetch(body_atlas, ivec3(int(c.x), int(c.y), int(layer0 + c.z)), 0).rgb;
		vec3 tw = mix(1.0 - f, f, o);
		float solid = texel.r * 255.0 > 128.5 ? 1.0 : 0.15;
		int g = int(texel.g * 255.0 + 0.5);
		int bits = (g & 127) * 256 + int(texel.b * 255.0 + 0.5);
		kinds[k] = g < 128 ? g : 128;
		colors[k] = vec3(float((bits >> 10) & 31), float((bits >> 5) & 31), float(bits & 31)) / 31.0;
		votes[k] = tw.x * tw.y * tw.z * solid;
	}
	int best = kinds[0];
	float best_votes = -1.0;
	for (int k = 0; k < 8; k++) {
		float total = 0.0;
		for (int j = 0; j < 8; j++) {
			total += kinds[j] == kinds[k] ? votes[j] : 0.0;
		}
		if (total > best_votes) {
			best_votes = total;
			best = kinds[k];
		}
	}
	if (best < 128) {
		return vec4(float(best), 0.0, 0.0, 0.0);
	}
	vec3 sum = vec3(0.0);
	for (int k = 0; k < 8; k++) {
		sum += kinds[k] == 128 ? colors[k] * votes[k] : vec3(0.0);
	}
	return vec4(-1.0, sum / max(best_votes, 1e-6));
}

// Where the ray ro + rd * t is inside grid body i's bounding sphere: false
// if nowhere ahead.
bool grid_span(int i, vec3 ro, vec3 rd, out float r_t0, out float r_t1) {
	vec3 oc = ro - grid_bound[i].xyz;
	float r = grid_bound[i].w;
	float b = dot(oc, rd);
	float h = b * b - (dot(oc, oc) - r * r);
	if (h < 0.0) {
		return false;
	}
	h = sqrt(h);
	r_t0 = max(-b - h, 0.0);
	r_t1 = -b + h;
	return r_t1 > 0.0;
}

// The nearest grid body the ray hits before r_t: its index (or -1), and
// r_t set to where.
int trace_grids(vec3 ro, vec3 rd, inout float r_t) {
	int hit = -1;
	for (int i = 0; i < MAX_GRID_BODIES; i++) {
		if (i >= grid_count) {
			break;
		}
		float t, t1;
		if (!grid_span(i, ro, rd, t, t1) || t >= r_t) {
			continue;
		}
		t1 = min(t1, r_t);
		float voxel = grid_size[i].w;
		float t_prev = t;
		for (int j = 0; j < 128; j++) {
			if (t > t1) {
				break;
			}
			float d = grid_density(i, ro + rd * t);
			if (d > 0.0) {
				// Back to the surface between the last point outside and this one.
				float a = t_prev, b = t;
				for (int k = 0; k < 5; k++) {
					float m = 0.5 * (a + b);
					if (grid_density(i, ro + rd * m) > 0.0) {
						b = m;
					} else {
						a = m;
					}
				}
				r_t = b;
				hit = i;
				break;
			}
			t_prev = t;
			t += max(-d * 0.8, 0.1 * voxel);
		}
	}
	return hit;
}

// Whether the ray from p towards the sun passes through a grid body.
bool grid_shadow(vec3 p, vec3 sun) {
	for (int i = 0; i < MAX_GRID_BODIES; i++) {
		if (i >= grid_count) {
			break;
		}
		float t, t1;
		if (!grid_span(i, p, sun, t, t1)) {
			continue;
		}
		float voxel = grid_size[i].w;
		for (int j = 0; j < 32; j++) {
			if (t > t1) {
				break;
			}
			float d = grid_density(i, p + sun * t);
			if (d > 0.0) {
				return true;
			}
			t += max(-d * 0.9, 0.3 * voxel);
		}
	}
	return false;
}

// Whether the ray from p towards the sun passes through a sphere. A point
// on a sphere's own lit side starts outside it heading away, so a sphere
// doesn't shadow itself.
bool sphere_shadow(vec3 p, vec3 sun) {
	for (int i = 0; i < MAX_BODY_SPHERES; i++) {
		if (i >= body_sphere_count) {
			break;
		}
		vec3 oc = p - body_spheres[i].xyz;
		float r = body_spheres[i].w;
		float b = dot(oc, sun);
		float c = dot(oc, oc) - r * r;
		if (c > 0.0 && b < 0.0 && b * b > c) {
			return true;
		}
	}
	return false;
}

// How much sky light reaches p (normal n) past the bodies, 1 = all: each
// blocks about its solid angle (a grid body counted as a ball a little
// smaller than its bounding sphere), weighted by how much it faces n.
// p_skip_sphere / p_skip_grid: the body p is on (-1 for none).
float body_occlusion(vec3 p, vec3 n, int p_skip_sphere, int p_skip_grid) {
	float occlusion = 0.0;
	for (int i = 0; i < MAX_BODY_SPHERES; i++) {
		if (i >= body_sphere_count) {
			break;
		}
		if (i == p_skip_sphere) {
			continue;
		}
		vec3 to = body_spheres[i].xyz - p;
		float l2 = max(dot(to, to), 1e-4);
		float r = body_spheres[i].w;
		occlusion += clamp(dot(n, to) * inversesqrt(l2), 0.0, 1.0) * r * r / l2;
	}
	for (int i = 0; i < MAX_GRID_BODIES; i++) {
		if (i >= grid_count) {
			break;
		}
		vec3 to = grid_bound[i].xyz - p;
		float l2 = max(dot(to, to), 1e-4);
		float r = grid_bound[i].w * 0.6;
		if (i == p_skip_grid || l2 < r * r) {
			continue;
		}
		occlusion += clamp(dot(n, to) * inversesqrt(l2), 0.0, 1.0) * r * r / l2;
	}
	return clamp(1.0 - occlusion, 0.0, 1.0);
}

// The first sphere the ray ro + rd * t enters, in front of the camera:
// returns its index (or -1) and sets r_t.
int trace_spheres(vec3 ro, vec3 rd, out float r_t) {
	int hit = -1;
	r_t = 1e30;
	for (int i = 0; i < MAX_BODY_SPHERES; i++) {
		if (i >= body_sphere_count) {
			break;
		}
		vec3 oc = ro - body_spheres[i].xyz;
		float r = body_spheres[i].w;
		float b = dot(oc, rd);
		float h = b * b - (dot(oc, oc) - r * r);
		if (h < 0.0) {
			continue;
		}
		float t = -b - sqrt(h);
		if (t > 0.0 && t < r_t) {
			r_t = t;
			hit = i;
		}
	}
	return hit;
}

// 1 = sunlit, 0 = shadowed: hard shadows. A ray is in shadow only once a
// sample lands inside solid; passing close to a surface doesn't count
// (scoring near misses gave soft edges, but they depend on where samples
// happen to fall and showed as streaks along the sun's direction). p is on
// the surface and `scale` the texel size of the level there.
//
// Coarse levels describe a surface only to within about a texel, so a ray
// starting close to it can hit the surface it is leaving: black speckles that
// shift as you move. The ray starts 1.5 texels out (plus half along the
// normal) and ignores everything within the first 3 texels.
float sun_shadow(vec3 p, vec3 n, vec3 sun, float scale) {
	vec3 start = p + n * 0.5 * scale;
	if (sphere_shadow(start, sun) || grid_shadow(start, sun)) {
		return 0.0;
	}
	float t = 1.5 * scale;
	for (int i = 0; i < SHADOW_STEPS; i++) {
		if (t > SHADOW_DISTANCE) {
			break;
		}
		float s;
		float far;
		float d = density(start + sun * t, s, far);
		if (d > 0.0 && t >= 3.0 * scale) {
			return 0.0; // inside an occluder
		}
		// A minimum step growing with distance keeps rays from creeping along
		// a surface until they run out of steps.
		t += max(step_length(d, far, s), 0.1 * s + 0.01 * t);
	}
	return 1.0;
}

