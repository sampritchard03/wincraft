#include "noise.h"

#include <algorithm>
#include <cmath>

using namespace godot;

namespace {

uint64_t splitmix(uint64_t &r_state) {
	uint64_t z = (r_state += 0x9E3779B97F4A7C15ull);
	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
	return z ^ (z >> 31);
}

inline int fast_floor(float x) {
	const int i = (int)x;
	return x < (float)i ? i - 1 : i;
}

inline float grad(int hash, float x, float y, float z) {
	// 12 gradient directions (cube edge midpoints), as in the reference.
	const int h = hash & 15;
	const float u = h < 8 ? x : y;
	const float v = h < 4 ? y : (h == 12 || h == 14 ? x : z);
	return ((h & 1) ? -u : u) + ((h & 2) ? -v : v);
}

} // namespace

void SimplexNoise::set_seed(uint64_t p_seed) {
	uint64_t state = p_seed;
	uint8_t p[256];
	for (int i = 0; i < 256; i++) {
		p[i] = (uint8_t)i;
	}
	for (int i = 255; i > 0; i--) { // Fisher-Yates
		const int j = (int)(splitmix(state) % (uint64_t)(i + 1));
		const uint8_t t = p[i];
		p[i] = p[j];
		p[j] = t;
	}
	for (int i = 0; i < 512; i++) {
		perm[i] = p[i & 255];
	}
}

float SimplexNoise::noise(float x, float y, float z) const {
	const float F3 = 1.0f / 3.0f;
	const float G3 = 1.0f / 6.0f;
	const float s = (x + y + z) * F3;
	const int i = fast_floor(x + s), j = fast_floor(y + s), k = fast_floor(z + s);
	const float t = (float)(i + j + k) * G3;
	const float x0 = x - ((float)i - t), y0 = y - ((float)j - t), z0 = z - ((float)k - t);

	int i1, j1, k1, i2, j2, k2; // offsets of the second and third simplex corners
	if (x0 >= y0) {
		if (y0 >= z0) {
			i1 = 1, j1 = 0, k1 = 0, i2 = 1, j2 = 1, k2 = 0;
		} else if (x0 >= z0) {
			i1 = 1, j1 = 0, k1 = 0, i2 = 1, j2 = 0, k2 = 1;
		} else {
			i1 = 0, j1 = 0, k1 = 1, i2 = 1, j2 = 0, k2 = 1;
		}
	} else {
		if (y0 < z0) {
			i1 = 0, j1 = 0, k1 = 1, i2 = 0, j2 = 1, k2 = 1;
		} else if (x0 < z0) {
			i1 = 0, j1 = 1, k1 = 0, i2 = 0, j2 = 1, k2 = 1;
		} else {
			i1 = 0, j1 = 1, k1 = 0, i2 = 1, j2 = 1, k2 = 0;
		}
	}
	const float x1 = x0 - i1 + G3, y1 = y0 - j1 + G3, z1 = z0 - k1 + G3;
	const float x2 = x0 - i2 + 2.0f * G3, y2 = y0 - j2 + 2.0f * G3, z2 = z0 - k2 + 2.0f * G3;
	const float x3 = x0 - 1.0f + 3.0f * G3, y3 = y0 - 1.0f + 3.0f * G3, z3 = z0 - 1.0f + 3.0f * G3;
	const int ii = i & 255, jj = j & 255, kk = k & 255;

	float n = 0.0f;
	float t0 = 0.6f - x0 * x0 - y0 * y0 - z0 * z0;
	if (t0 > 0.0f) {
		t0 *= t0;
		n += t0 * t0 * grad(perm[ii + perm[jj + perm[kk]]], x0, y0, z0);
	}
	float t1 = 0.6f - x1 * x1 - y1 * y1 - z1 * z1;
	if (t1 > 0.0f) {
		t1 *= t1;
		n += t1 * t1 * grad(perm[ii + i1 + perm[jj + j1 + perm[kk + k1]]], x1, y1, z1);
	}
	float t2 = 0.6f - x2 * x2 - y2 * y2 - z2 * z2;
	if (t2 > 0.0f) {
		t2 *= t2;
		n += t2 * t2 * grad(perm[ii + i2 + perm[jj + j2 + perm[kk + k2]]], x2, y2, z2);
	}
	float t3 = 0.6f - x3 * x3 - y3 * y3 - z3 * z3;
	if (t3 > 0.0f) {
		t3 *= t3;
		n += t3 * t3 * grad(perm[ii + 1 + perm[jj + 1 + perm[kk + 1]]], x3, y3, z3);
	}
	return 32.0f * n;
}

float SimplexNoise::fbm(float x, float y, float z, int p_octaves) const {
	float sum = 0.0f, amplitude = 1.0f, total = 0.0f;
	for (int o = 0; o < p_octaves; o++) {
		sum += amplitude * noise(x, y, z);
		total += amplitude;
		amplitude *= 0.5f;
		x = x * 2.0f + 17.3f; // offset each octave so their lattices don't line up
		y = y * 2.0f + 5.1f;
		z = z * 2.0f + 11.7f;
	}
	return total > 0.0f ? sum / total : 0.0f;
}

// --- Perlin ----------------------------------------------------------------------

namespace {

inline float fade(float t) {
	return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

inline float lerpf(float a, float b, float t) {
	return a + (b - a) * t;
}

inline float grad2(int hash, float x, float y) {
	// 8 directions: the axes and diagonals.
	switch (hash & 7) {
		case 0: return x + y;
		case 1: return -x + y;
		case 2: return x - y;
		case 3: return -x - y;
		case 4: return x;
		case 5: return -x;
		case 6: return y;
		default: return -y;
	}
}

} // namespace

void PerlinNoise::set_seed(uint64_t p_seed) {
	uint64_t state = p_seed ^ 0x5DEECE66Dull;
	uint8_t p[256];
	for (int i = 0; i < 256; i++) {
		p[i] = (uint8_t)i;
	}
	for (int i = 255; i > 0; i--) {
		const int j = (int)(splitmix(state) % (uint64_t)(i + 1));
		const uint8_t t = p[i];
		p[i] = p[j];
		p[j] = t;
	}
	for (int i = 0; i < 512; i++) {
		perm[i] = p[i & 255];
	}
}

float PerlinNoise::noise(float p_x, float p_y) const {
	const int xi = fast_floor(p_x), yi = fast_floor(p_y);
	const float x = p_x - (float)xi, y = p_y - (float)yi;
	const int X = xi & 255, Y = yi & 255;
	const float u = fade(x), v = fade(y);
	const int a = perm[X] + Y, b = perm[X + 1] + Y;
	return lerpf(lerpf(grad2(perm[a], x, y), grad2(perm[b], x - 1.0f, y), u),
				   lerpf(grad2(perm[a + 1], x, y - 1.0f), grad2(perm[b + 1], x - 1.0f, y - 1.0f), u), v) *
			0.7071f; // the diagonals reach sqrt(2) / 2 * 2
}

float PerlinNoise::noise(float p_x, float p_y, float p_z) const {
	const int xi = fast_floor(p_x), yi = fast_floor(p_y), zi = fast_floor(p_z);
	const float x = p_x - (float)xi, y = p_y - (float)yi, z = p_z - (float)zi;
	const int X = xi & 255, Y = yi & 255, Z = zi & 255;
	const float u = fade(x), v = fade(y), w = fade(z);
	const int A = perm[X] + Y, AA = perm[A] + Z, AB = perm[A + 1] + Z;
	const int B = perm[X + 1] + Y, BA = perm[B] + Z, BB = perm[B + 1] + Z;
	return lerpf(lerpf(lerpf(grad(perm[AA], x, y, z), grad(perm[BA], x - 1.0f, y, z), u),
						 lerpf(grad(perm[AB], x, y - 1.0f, z), grad(perm[BB], x - 1.0f, y - 1.0f, z), u), v),
			lerpf(lerpf(grad(perm[AA + 1], x, y, z - 1.0f), grad(perm[BA + 1], x - 1.0f, y, z - 1.0f), u),
					lerpf(grad(perm[AB + 1], x, y - 1.0f, z - 1.0f), grad(perm[BB + 1], x - 1.0f, y - 1.0f, z - 1.0f), u), v),
			w);
}

float PerlinNoise::fbm(float p_x, float p_y, int p_octaves) const {
	float sum = 0.0f, amplitude = 1.0f, total = 0.0f, f = 1.0f;
	for (int o = 0; o < p_octaves; o++) {
		// Offsetting each octave keeps their lattice zeros from lining up.
		sum += amplitude * noise(p_x * f + 17.3f * o, p_y * f - 9.1f * o);
		total += amplitude;
		amplitude *= 0.5f;
		f *= 2.0f;
	}
	return total > 0.0f ? sum / total : 0.0f;
}

float PerlinNoise::fbm(float p_x, float p_y, float p_z, int p_octaves) const {
	float sum = 0.0f, amplitude = 1.0f, total = 0.0f, f = 1.0f;
	for (int o = 0; o < p_octaves; o++) {
		sum += amplitude * noise(p_x * f + 17.3f * o, p_y * f - 9.1f * o, p_z * f + 4.7f * o);
		total += amplitude;
		amplitude *= 0.5f;
		f *= 2.0f;
	}
	return total > 0.0f ? sum / total : 0.0f;
}

// --- Periodic Perlin ---------------------------------------------------------------

namespace {

inline int wrap_period(int p_value, int p_period) {
	// Usually already in range (or one period over): no division.
	if (p_value >= 0 && p_value < p_period) {
		return p_value;
	}
	if (p_value >= p_period && p_value < 2 * p_period) {
		return p_value - p_period;
	}
	const int m = p_value % p_period;
	return m < 0 ? m + p_period : m;
}

} // namespace

float PerlinNoise::noise_periodic(float p_x, float p_y, int p_period) const {
	const int xi = fast_floor(p_x), yi = fast_floor(p_y);
	const float x = p_x - (float)xi, y = p_y - (float)yi;
	const int x0 = wrap_period(xi, p_period), x1 = wrap_period(xi + 1, p_period);
	const int y0 = wrap_period(yi, p_period), y1 = wrap_period(yi + 1, p_period);
	const float u = fade(x), v = fade(y);
	return lerpf(lerpf(grad2(hash2(x0, y0), x, y), grad2(hash2(x1, y0), x - 1.0f, y), u),
				   lerpf(grad2(hash2(x0, y1), x, y - 1.0f), grad2(hash2(x1, y1), x - 1.0f, y - 1.0f), u), v) *
			0.7071f;
}

float PerlinNoise::noise_periodic(float p_x, float p_y, float p_z, int p_period) const {
	// Periodic in x and z; y repeats every 256 as usual.
	const int xi = fast_floor(p_x), yi = fast_floor(p_y), zi = fast_floor(p_z);
	const float x = p_x - (float)xi, y = p_y - (float)yi, z = p_z - (float)zi;
	const int x0 = wrap_period(xi, p_period), x1 = wrap_period(xi + 1, p_period);
	const int y0 = yi & 255, y1 = (yi + 1) & 255;
	const int z0 = wrap_period(zi, p_period), z1 = wrap_period(zi + 1, p_period);
	const float u = fade(x), v = fade(y), w = fade(z);
	return lerpf(lerpf(lerpf(grad(hash3(x0, y0, z0), x, y, z), grad(hash3(x1, y0, z0), x - 1.0f, y, z), u),
						 lerpf(grad(hash3(x0, y1, z0), x, y - 1.0f, z), grad(hash3(x1, y1, z0), x - 1.0f, y - 1.0f, z), u), v),
			lerpf(lerpf(grad(hash3(x0, y0, z1), x, y, z - 1.0f), grad(hash3(x1, y0, z1), x - 1.0f, y, z - 1.0f), u),
					lerpf(grad(hash3(x0, y1, z1), x, y - 1.0f, z - 1.0f), grad(hash3(x1, y1, z1), x - 1.0f, y - 1.0f, z - 1.0f), u), v),
			w);
}

float PerlinNoise::fbm_periodic(float p_x, float p_y, int p_octaves, int p_period) const {
	float sum = 0.0f, amplitude = 1.0f, total = 0.0f, f = 1.0f;
	int period = p_period;
	for (int o = 0; o < p_octaves; o++) {
		// Whole-number offsets keep each octave periodic.
		sum += amplitude * noise_periodic(p_x * f + 17.0f * o, p_y * f - 9.0f * o, std::min(period, 256));
		total += amplitude;
		amplitude *= 0.5f;
		f *= 2.0f;
		period *= 2;
	}
	return total > 0.0f ? sum / total : 0.0f;
}
