#pragma once

#include <cstdint>

namespace godot {

// 3D simplex noise (after Stefan Gustavson's public-domain simplexnoise1234),
// with the permutation shuffled by a seed. Plain C++, so it is fast and safe to
// call from any thread.
class SimplexNoise {
public:
	explicit SimplexNoise(uint64_t p_seed = 0) { set_seed(p_seed); }
	void set_seed(uint64_t p_seed);
	// About -1..1.
	float noise(float p_x, float p_y, float p_z) const;
	// p_octaves octaves, each at double the frequency and half the amplitude of
	// the one before, normalised so the result stays about -1..1.
	float fbm(float p_x, float p_y, float p_z, int p_octaves) const;

private:
	uint8_t perm[512];
};

// Ken Perlin's improved gradient noise (2002), 2D and 3D, with the
// permutation shuffled by a seed. Plain C++, thread-safe to call.
class PerlinNoise {
public:
	explicit PerlinNoise(uint64_t p_seed = 0) { set_seed(p_seed); }
	void set_seed(uint64_t p_seed);
	// About -1..1 (0 at every integer lattice point).
	float noise(float p_x, float p_y) const;
	float noise(float p_x, float p_y, float p_z) const;
	// p_octaves octaves, each at double the frequency and half the amplitude,
	// normalised so the result stays about -1..1.
	float fbm(float p_x, float p_y, int p_octaves) const;
	float fbm(float p_x, float p_y, float p_z, int p_octaves) const;
	// Periodic versions: the same pattern every p_period lattice units in x
	// (and y for 2D, z for 3D); p_period <= 256. fbm octave o repeats every
	// p_period * 2^o of its own units, i.e. still every p_period of yours.
	float noise_periodic(float p_x, float p_y, int p_period) const;
	float noise_periodic(float p_x, float p_y, float p_z, int p_period) const;
	float fbm_periodic(float p_x, float p_y, int p_octaves, int p_period) const;

private:
	uint8_t perm[512];
	int hash2(int x, int y) const { return perm[(perm[x & 255] + y) & 255]; }
	int hash3(int x, int y, int z) const { return perm[(perm[(perm[x & 255] + y) & 255] + z) & 255]; }
};

} // namespace godot
