// Bones for skinned_bodies.gdshader (skinning.h): posed grid bodies, drawn
// in a pass of their own after the terrain, so this weight stays out of the
// terrain shader (in it, Intel's driver compiled for minutes and then drew
// garbage, everything else included). Needs terrain_common and
// terrain_shadow first (the grid uniforms and grid_rest_density()).

// Bones (skinning.h): a grid body posed by its skeleton draws its rest
// grid through the bones, the space round it bent. grid_bones[i]: x its
// first bone below, y how many (0: no bones). Per bone, in the rest grid's
// voxels: bone_rows* its matrix (rest grid point -> posed grid point:
// dot(row, vec4(q, 1))), bone_head (xyz, w the envelope's radius),
// bone_tail (xyz, w its blend), bone_soft.x its softness. 6 vec4s a bone:
// with the rest, the material's uniforms stay under the 16 KB a uniform
// block is sure to have on any GPU (GLES 3) - mind that adding more.
const int MAX_DRAWN_BONES = 64; // SculptTerrain::MAX_DRAWN_BONES
uniform vec4 grid_bones[MAX_GRID_BODIES];
uniform vec4 bone_rows0[MAX_DRAWN_BONES];
uniform vec4 bone_rows1[MAX_DRAWN_BONES];
uniform vec4 bone_rows2[MAX_DRAWN_BONES];
uniform vec4 bone_head[MAX_DRAWN_BONES];
uniform vec4 bone_tail[MAX_DRAWN_BONES];
uniform vec4 bone_soft[MAX_DRAWN_BONES];
// A bone (index into the above) whose weight tints the body: the object
// editor's weight preview (-1: none).
uniform int bone_preview = -1;

// --- Bones (as skinning.cpp does it; keep the two alike) ----------------------
// (Loops take their counts in the condition, with no continue or early
// break: Mali's compiler got those wrong.)
//
// Mind the size: GLSL compilers inline every call, so a function called
// from n places is n copies, and one calling another multiplies them. With
// the skin's loops in 50-odd copies, Intel's driver took over ten minutes to
// compile this shader (a black screen). So each step below is called from
// one place, inside a loop: the solver's iterations, the two guesses, the
// normal's four samples, the march and its refinement. And their fixed
// counts are times loop_unit (a uniform, always 1), so the compiler can't
// see them and unroll the loops into copies of their bodies after all.

vec3 bone_move(int b, vec3 q) {
	vec4 q4 = vec4(q, 1.0);
	return vec3(dot(bone_rows0[b], q4), dot(bone_rows1[b], q4), dot(bone_rows2[b], q4));
}

mat3 bone_basis(int b) {
	return mat3(vec3(bone_rows0[b].x, bone_rows1[b].x, bone_rows2[b].x), vec3(bone_rows0[b].y, bone_rows1[b].y, bone_rows2[b].y),
			vec3(bone_rows0[b].z, bone_rows1[b].z, bone_rows2[b].z));
}

// Softened distance to bone b's segment, and the way away from it.
float bone_distance(int b, vec3 q, out vec3 r_away) {
	vec3 a = bone_head[b].xyz;
	vec3 ab = bone_tail[b].xyz - a;
	float len2 = dot(ab, ab);
	float t = len2 > 1e-10 ? clamp(dot(q - a, ab) / len2, 0.0, 1.0) : 0.0;
	r_away = q - (a + ab * t);
	return sqrt(dot(r_away, r_away) + 0.0625);
}

// Its envelope: 1 within the radius, easing to 0 over the blend.
float bone_gate(int b, float d) {
	float blend = bone_tail[b].w;
	float x = blend > 1e-6 ? clamp((d - bone_head[b].w) / blend, 0.0, 1.0) : (d <= bone_head[b].w ? 0.0 : 1.0);
	return 1.0 - x * x * x * (x * (x * 6.0 - 15.0) + 10.0);
}

// Where grid body i's rest point q goes (posed grid point), and the
// skin's Jacobian there. The bones' weights: their gates say how much moves
// at all; a softmax of distance / softness splits it between them.
vec3 skin_forward(int i, vec3 q, out mat3 r_jacobian) {
	int first = int(grid_bones[i].x);
	int last = first + int(grid_bones[i].y);
	// Pass 1: what no gate covers, and the softmax's sums (online: rescaled
	// whenever a nearer bone turns up).
	float none = 1.0;
	float least = 1e30;
	float sum = 0.0;
	vec3 sum_de = vec3(0.0);
	for (int b = first; b < last; b++) {
		vec3 away;
		float d = bone_distance(b, q, away);
		float g = bone_gate(b, d);
		float soft = bone_soft[b].x;
		float e = d / soft;
		none *= 1.0 - g;
		if (g > 0.0) {
			float lower = min(least, e);
			float k = exp(lower - least);
			float s = g * exp(lower - e);
			sum = sum * k + s;
			sum_de = sum_de * k + away / (d * soft) * s;
			least = lower;
		}
	}
	if (sum <= 1e-12) {
		r_jacobian = mat3(1.0);
		return q;
	}
	float covered = 1.0 - none;
	vec3 mean_de = sum_de / sum;
	// Pass 2: each bone's share of the move, and how it changes.
	vec3 p = q * none;
	mat3 j = mat3(none);
	for (int b = first; b < last; b++) {
		vec3 away;
		float d = bone_distance(b, q, away);
		float g = bone_gate(b, d);
		float soft = bone_soft[b].x;
		float w = g > 0.0 ? g * exp(least - d / soft) / sum : 0.0;
		vec3 dw = (mean_de - away / (d * soft)) * (w * covered);
		w *= covered;
		vec3 moved = bone_move(b, q);
		p += moved * w;
		j += bone_basis(b) * w + outerProduct(moved, dw);
	}
	r_jacobian = j;
	return p;
}

// Newton from q towards the rest point that goes to g; r_miss: how far
// that misses.
vec3 skin_solve(int i, vec3 g, vec3 q0, out float r_miss) {
	// Each pass looks at one trial point: better, it's taken and the next
	// Newton step worked out from there; worse, the step is halved.
	vec3 q = q0;
	vec3 trial = q0;
	vec3 step = vec3(0.0);
	float miss = 1e30;
	for (int k = 0; k < 9 * loop_unit && miss > 1e-3; k++) {
		mat3 j;
		vec3 f = skin_forward(i, trial, j);
		float m = length(g - f);
		if (m < miss) {
			q = trial;
			miss = m;
			step = abs(determinant(j)) > 1e-6 ? inverse(j) * (g - f) : vec3(0.0);
			float len = length(step);
			step *= len > 4.0 ? 4.0 / len : 1.0;
		} else {
			step *= 0.5;
		}
		trial = q + step;
	}
	r_miss = miss;
	return q;
}

// Grid body i's density at posed grid point g, through its bones: from the
// two bones likeliest to own g (each one's rigid guess, Newton from there),
// the more solid (a fold's union), less how far each misses. r_rest: the
// rest point it came from.
float skin_density(int i, vec3 g, out vec3 r_rest) {
	int first = int(grid_bones[i].x);
	int last = first + int(grid_bones[i].y);
	float score1 = 0.0;
	float score2 = 0.0;
	vec3 guess1 = g;
	vec3 guess2 = g;
	for (int b = first; b < last; b++) {
		mat3 m = bone_basis(b);
		vec3 q = abs(determinant(m)) > 1e-9 ? inverse(m) * (g - vec3(bone_rows0[b].w, bone_rows1[b].w, bone_rows2[b].w)) : g;
		vec3 away;
		float d = bone_distance(b, q, away);
		float score = bone_gate(b, d) * exp(-d / bone_soft[b].x);
		if (score > score1) {
			score2 = score1;
			guess2 = guess1;
			score1 = score;
			guess1 = q;
		} else if (score > score2) {
			score2 = score;
			guess2 = q;
		}
	}
	float d = -1e30;
	r_rest = g;
	for (int c = 0; c < 2 * loop_unit; c++) { // (the best bone's guess, then the second's or staying put)
		float miss;
		vec3 a = skin_solve(i, g, c == 0 ? guess1 : guess2, miss);
		float here = grid_rest_density(i, a) - miss;
		if (here > d) {
			d = here;
			r_rest = a;
		}
	}
	return d;
}


// Bone b's share of rest point q of grid body i (the editor's weight preview).
float bone_weight(int i, int b, vec3 q) {
	mat3 unused;
	int first = int(grid_bones[i].x);
	int last = first + int(grid_bones[i].y);
	float none = 1.0;
	float least = 1e30;
	float sum = 0.0;
	float mine = 0.0;
	for (int k = first; k < last; k++) {
		vec3 away;
		float d = bone_distance(k, q, away);
		float g = bone_gate(k, d);
		float e = d / bone_soft[k].x;
		none *= 1.0 - g;
		if (g > 0.0) {
			float lower = min(least, e);
			float scale = exp(lower - least);
			float s = g * exp(lower - e);
			sum = sum * scale + s;
			mine = mine * scale + (k == b ? s : 0.0);
			least = lower;
		}
	}
	return sum > 1e-12 ? mine / sum * (1.0 - none) : 0.0;
}

// Posed grid body i's density at world point p (world voxels), and the rest
// grid point it came from.
float skin_grid_density(int i, vec3 p, out vec3 r_rest) {
	return skin_density(i, grid_point(i, p), r_rest) * grid_size[i].w;
}

vec3 skin_grid_normal(int i, vec3 p) {
	float e = 0.5 * grid_size[i].w;
	vec3 g = vec3(0.0);
	for (int k = 0; k < 4 * loop_unit; k++) { // (a loop: one copy, not four)
		vec3 o = k == 0 ? vec3(1.0, -1.0, -1.0) : (k == 1 ? vec3(-1.0, -1.0, 1.0) : (k == 2 ? vec3(-1.0, 1.0, -1.0) : vec3(1.0)));
		vec3 unused;
		g += o * skin_grid_density(i, p + o * e, unused);
	}
	float len = length(g);
	return len > 1e-6 ? -g / len : vec3(0.0, 1.0, 0.0);
}

// The nearest posed body the ray hits before r_t: its index (or -1), r_t
// set to where, and r_rest the rest grid point just inside its surface there.
int skin_trace(vec3 ro, vec3 rd, inout float r_t, out vec3 r_rest) {
	int hit = -1;
	r_rest = vec3(0.0);
	int count = min(grid_count, MAX_GRID_BODIES);
	for (int i = 0; i < count; i++) {
		float t, t1;
		if (grid_span(i, ro, rd, t, t1) && t < r_t) {
			t1 = min(t1, r_t);
			float voxel = grid_size[i].w;
			// Marching until inside, then 5 halvings back to the surface
			// between the last point outside (a) and the first inside (b);
			// one loop, so one copy of the skin. (Steps at 0.6 of the
			// distance: bones squeeze space at a bend.)
			float t_prev = t;
			float a = 0.0;
			float b = 0.0;
			vec3 rest_b = vec3(0.0);
			int refining = -1;
			bool done = false;
			for (int j = 0; j < 133 && !done && (refining >= 0 || t <= t1); j++) {
				float at = refining >= 0 ? 0.5 * (a + b) : t;
				vec3 rest;
				float d = skin_grid_density(i, ro + rd * at, rest);
				if (refining >= 0) {
					if (d > 0.0) {
						b = at;
						rest_b = rest;
					} else {
						a = at;
					}
					refining++;
					done = refining >= 5;
				} else if (d > 0.0) {
					a = t_prev;
					b = t;
					rest_b = rest;
					refining = 0;
				} else {
					t_prev = t;
					t += max(-d * 0.6, 0.1 * voxel);
				}
			}
			if (done) {
				r_t = b;
				hit = i;
				r_rest = rest_b;
			}
		}
	}
	return hit;
}
