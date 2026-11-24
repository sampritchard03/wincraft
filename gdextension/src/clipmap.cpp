#include "clipmap.h"

#include "parallel.h"

#include <algorithm>
#include <cmath>

using namespace godot;

void Clipmap::configure(int p_size, int p_level_count) {
	cancel_jobs();
	size = p_size;
	levels.assign(p_level_count, Level());
	for (int l = 0; l < p_level_count; l++) {
		levels[l].scale = 1 << l;
	}
	jobs.clear();
	jobs.resize(p_level_count);
}

void Clipmap::cancel_jobs() {
	for (std::unique_ptr<Job> &job : jobs) {
		if (job && job->thread.joinable()) {
			job->thread.join();
		}
		job.reset();
	}
}

void Clipmap::shift(VoxelWorld &r_world, const Vector3i &p_offset) {
	for (int l = 0; l < (int)levels.size(); l++) {
		Level &level = levels[l];
		const int extent = size * level.scale;
		bool keeps = true;
		for (int a = 0; a < 3; a++) {
			keeps = keeps && p_offset[a] % extent == 0;
		}
		if (jobs[l]) {
			if (keeps) {
				// Its storage slots are the same either way.
				jobs[l]->shift += p_offset / level.scale;
			} else {
				finish_job(l, r_world); // the level is rebuilt below anyway
			}
		}
		if (!keeps) {
			level.valid = false;
			level.published = false;
			level.pending_edits.clear();
			continue;
		}
		const Vector3i texels = p_offset / level.scale;
		level.origin += texels;
		level.published_origin += texels;
		for (auto &[lo, hi] : level.pending_edits) {
			lo += p_offset;
			hi += p_offset;
		}
	}
}

bool Clipmap::has_pending_jobs() const {
	for (const std::unique_ptr<Job> &job : jobs) {
		if (job) {
			return true;
		}
	}
	return false;
}

float Clipmap::texel_value(const Level &p_level, const Vector3i &p_texel) const {
	return ((int)p_level.texels[texel_index(p_texel) * TEXEL_BYTES] - 128) * DENSITY_STEP * p_level.scale;
}

int Clipmap::brick_value(const Level &p_level, const Vector3i &p_texel) const {
	return p_level.bricks[brick_index(Vector3i(floor_div(p_texel.x, BRICK), floor_div(p_texel.y, BRICK), floor_div(p_texel.z, BRICK)))];
}

void Clipmap::mark_layers(Level &r_level, int p_z0, int p_z1) {
	if (p_z1 - p_z0 + 1 >= size) {
		std::fill(r_level.layer_dirty.begin(), r_level.layer_dirty.end(), 1);
		return;
	}
	for (int z = p_z0; z <= p_z1; z++) {
		r_level.layer_dirty[wrap(z, size)] = 1;
	}
}

// --- Texels ----------------------------------------------------------------------

bool Clipmap::update_has_surface(Level &r_level, const Vector3i &p_min, const Vector3i &p_max) {
	bool changed = false;
	const Vector3i b0(floor_div(p_min.x, BRICK), floor_div(p_min.y, BRICK), floor_div(p_min.z, BRICK));
	const Vector3i b1(floor_div(p_max.x, BRICK), floor_div(p_max.y, BRICK), floor_div(p_max.z, BRICK));
	for (int bz = b0.z; bz <= b1.z; bz++) {
		for (int by = b0.y; by <= b1.y; by++) {
			for (int bx = b0.x; bx <= b1.x; bx++) {
				bool surface = false;
				for (int z = bz * BRICK; z < (bz + 1) * BRICK && !surface; z++) {
					for (int y = by * BRICK; y < (by + 1) * BRICK && !surface; y++) {
						const uint8_t *row = &r_level.texels[texel_index(Vector3i(bx * BRICK, y, z)) * TEXEL_BYTES]; // a brick row never wraps
						for (int x = 0; x < BRICK; x++) {
							const int q = (int)row[x * TEXEL_BYTES] - 128;
							surface |= q > -DENSITY_LIMIT && q < DENSITY_LIMIT;
						}
					}
				}
				uint8_t &flag = r_level.has_surface[brick_index(Vector3i(bx, by, bz))];
				changed |= flag != (uint8_t)surface;
				flag = surface;
			}
		}
	}
	return changed;
}

bool Clipmap::write_texels(Level &r_level, int p_level, VoxelWorld &r_world, const Vector3i &p_min, const Vector3i &p_max) {
	const int s = r_level.scale;
	r_world.prepare(p_min * s, p_max * s, p_level);
	// Chunk by chunk: an edited chunk's stored data is read directly (these
	// are mostly what an edit refreshes); the rest goes through lod_texel.
	const int C = VoxelWorld::CHUNK;
	const Vector3i c0 = VoxelWorld::chunk_of(p_min.x * s, p_min.y * s, p_min.z * s);
	const Vector3i c1 = VoxelWorld::chunk_of(p_max.x * s, p_max.y * s, p_max.z * s);
	for (int cz = c0.z; cz <= c1.z; cz++) {
		for (int cy = c0.y; cy <= c1.y; cy++) {
			for (int cx = c0.x; cx <= c1.x; cx++) {
				const Vector3i c(cx, cy, cz);
				Vector3i t0, t1; // texels of this level inside the chunk and the box
				bool empty = false;
				for (int a = 0; a < 3; a++) {
					t0[a] = std::max(-floor_div(-c[a] * C, s), p_min[a]);
					t1[a] = std::min(floor_div(c[a] * C + C - 1, s), p_max[a]);
					empty |= t0[a] > t1[a];
				}
				if (empty) {
					continue;
				}
				const int8_t *data;
				const MaterialId *materials;
				int side;
				if (r_world.edited_data(c, p_level, data, materials, side)) {
					// Stored points are every 2^m apart; texels every s >= 2^m.
					const int m = C / side == 1 ? 0 : (int)std::lround(std::log2((double)(C / side)));
					const int stride = s >> m;
					for (int z = t0.z; z <= t1.z; z++) {
						for (int y = t0.y; y <= t1.y; y++) {
							const size_t first = ((size_t)((z * s - c.z * C) >> m) * side + ((y * s - c.y * C) >> m)) * side + ((t0.x * s - c.x * C) >> m);
							const int8_t *in = &data[first];
							const MaterialId *in_material = &materials[first];
							for (int x = t0.x; x <= t1.x; x++, in += stride, in_material += stride) {
								const int8_t q = p_level == 0 ? *in : VoxelWorld::encode_texel(*in * DENSITY_STEP, p_level);
								uint8_t *texel = &r_level.texels[texel_index(Vector3i(x, y, z)) * TEXEL_BYTES];
								texel[0] = (uint8_t)(q + 128);
								render_bytes(*in_material, texel + 1);
							}
						}
					}
				} else {
					for (int z = t0.z; z <= t1.z; z++) {
						for (int y = t0.y; y <= t1.y; y++) {
							for (int x = t0.x; x <= t1.x; x++) {
								uint8_t *texel = &r_level.texels[texel_index(Vector3i(x, y, z)) * TEXEL_BYTES];
								MaterialId material = MAT_STONE;
								texel[0] = (uint8_t)(r_world.lod_texel(x * s, y * s, z * s, p_level, material) + 128);
								render_bytes(material, texel + 1);
							}
						}
					}
				}
			}
		}
	}
	mark_layers(r_level, p_min.z, p_max.z);
	return update_has_surface(r_level, p_min, p_max);
}

void Clipmap::patch_edited(Level &r_level, int p_level, VoxelWorld &r_world, const Vector3i &p_min, const Vector3i &p_max) {
	const int s = r_level.scale;
	r_world.prepare(p_min * s, p_max * s, p_level);
	std::vector<Vector3i> edited;
	r_world.edited_chunks_in(p_min * s, p_max * s + Vector3i(s - 1, s - 1, s - 1), edited);
	const int C = VoxelWorld::CHUNK;
	for (const Vector3i &c : edited) {
		Vector3i t0, t1;
		bool empty = false;
		for (int a = 0; a < 3; a++) {
			t0[a] = std::max(-floor_div(-c[a] * C, s), p_min[a]); // first texel at or after the chunk
			t1[a] = std::min(floor_div(c[a] * C + C - 1, s), p_max[a]);
			empty |= t0[a] > t1[a];
		}
		if (!empty) {
			write_texels(r_level, p_level, r_world, t0, t1);
		}
	}
}

template <typename F>
void Clipmap::for_each_repeat(const VoxelWorld &p_world, const Vector3i &p_min, const Vector3i &p_max, const Vector3i &p_lo, const Vector3i &p_hi, int p_scale, F p_function) const {
	// The world repeats every W in x and z: the voxel box [p_min, p_max]
	// shows up shifted by whole multiples of W, and each copy that falls in
	// the level's texel box [p_lo, p_hi] is passed on in texels.
	const int W = p_world.world_size(), s = p_scale;
	const int kx0 = -floor_div(-(p_lo.x * s - p_max.x), W), kx1 = floor_div(p_hi.x * s - p_min.x, W);
	const int kz0 = -floor_div(-(p_lo.z * s - p_max.z), W), kz1 = floor_div(p_hi.z * s - p_min.z, W);
	for (int kz = kz0; kz <= kz1; kz++) {
		for (int kx = kx0; kx <= kx1; kx++) {
			const Vector3i offset(kx * W, 0, kz * W);
			Vector3i t0, t1;
			bool empty = false;
			for (int a = 0; a < 3; a++) {
				t0[a] = std::max(-floor_div(-(p_min[a] + offset[a]), s), p_lo[a]); // ceil(min / s)
				t1[a] = std::min(floor_div(p_max[a] + offset[a], s), p_hi[a]);
				empty |= t0[a] > t1[a];
			}
			if (!empty) {
				p_function(t0, t1);
			}
		}
	}
}

void Clipmap::refresh(VoxelWorld &r_world, const Vector3i &p_min, const Vector3i &p_max) {
	for (int l = 0; l < (int)levels.size(); l++) {
		Level &level = levels[l];
		if (!level.valid) {
			continue;
		}
		const int s = level.scale;
		Vector3i lo = level.origin, hi = level.origin + Vector3i(size - 1, size - 1, size - 1);
		const Job *job = jobs[l].get();
		if (job != nullptr) {
			// A move is running: redo this edit once it finishes. Until then
			// only texels in both the old and the new box are safe to write; the
			// worker owns the others (a reset writes its own copy).
			level.pending_edits.push_back({ p_min, p_max });
			if (!job->reset) {
				for (int a = 0; a < 3; a++) {
					lo[a] = std::max(lo[a], job->origin[a] + job->shift[a]);
					hi[a] = std::min(hi[a], job->origin[a] + job->shift[a] + size - 1);
				}
			}
		}
		// Distances only change when a brick gains or loses surface, which a
		// brush moving an existing surface rarely causes.
		bool bricks_changed = false;
		for_each_repeat(r_world, p_min, p_max, lo, hi, s, [&](const Vector3i &t0, const Vector3i &t1) {
			bricks_changed |= write_texels(level, l, r_world, t0, t1);
		});
		if (bricks_changed) {
			update_brick_distances(level);
		}
	}
}

// --- Bricks ----------------------------------------------------------------------

void Clipmap::update_brick_distances(Level &r_level) {
	// Breadth-first search over the 26 neighbours, in the level's own brick
	// coordinates, gives the Chebyshev distance.
	const int nb = size / BRICK;
	const Vector3i first(floor_div(r_level.origin.x, BRICK), floor_div(r_level.origin.y, BRICK), floor_div(r_level.origin.z, BRICK));
	auto local_index = [nb](int x, int y, int z) { return ((size_t)z * nb + y) * nb + x; };
	std::vector<uint8_t> distance((size_t)nb * nb * nb, MAX_BRICK_DISTANCE);
	std::vector<int> queue;
	for (int z = 0; z < nb; z++) {
		for (int y = 0; y < nb; y++) {
			for (int x = 0; x < nb; x++) {
				if (r_level.has_surface[brick_index(first + Vector3i(x, y, z))]) {
					distance[local_index(x, y, z)] = 0;
					queue.push_back((int)local_index(x, y, z));
				}
			}
		}
	}
	for (size_t head = 0; head < queue.size(); head++) {
		const int i = queue[head];
		const int x = i % nb, y = i / nb % nb, z = i / (nb * nb);
		const uint8_t next = distance[i] + 1;
		if (next >= MAX_BRICK_DISTANCE) {
			continue;
		}
		for (int dz = -1; dz <= 1; dz++) {
			for (int dy = -1; dy <= 1; dy++) {
				for (int dx = -1; dx <= 1; dx++) {
					const int nx = x + dx, ny = y + dy, nz = z + dz;
					if (nx < 0 || ny < 0 || nz < 0 || nx >= nb || ny >= nb || nz >= nb) {
						continue;
					}
					const size_t j = local_index(nx, ny, nz);
					if (distance[j] > next) {
						distance[j] = next;
						queue.push_back((int)j);
					}
				}
			}
		}
	}
	for (int z = 0; z < nb; z++) {
		for (int y = 0; y < nb; y++) {
			for (int x = 0; x < nb; x++) {
				uint8_t &stored = r_level.bricks[brick_index(first + Vector3i(x, y, z))];
				const uint8_t d = distance[local_index(x, y, z)];
				r_level.bricks_dirty |= stored != d;
				stored = d;
			}
		}
	}
}

// --- Moving ----------------------------------------------------------------------

void Clipmap::generate_slab(const VoxelWorld &p_world, int p_level, Slab &r_slab, uint8_t *r_texels, int p_threads) const {
	const int s = 1 << p_level;
	const int octaves = p_world.octaves_for_scale(s);
	const float accuracy = DENSITY_CLAMP * s; // beyond this the texel is clamped anyway
	const Vector3i n = r_slab.hi - r_slab.lo + Vector3i(1, 1, 1);
	parallel_for(n.z, p_threads, [&](int dz) {
		const int z = r_slab.lo.z + dz;
		for (int y = r_slab.lo.y; y <= r_slab.hi.y; y++) {
			uint8_t *row = &r_texels[texel_index(Vector3i(0, y, z)) * TEXEL_BYTES];
			for (int x = r_slab.lo.x; x <= r_slab.hi.x; x++) {
				uint8_t *texel = &row[wrap(x, size) * TEXEL_BYTES];
				MaterialId material = MAT_STONE;
				texel[0] = (uint8_t)(VoxelWorld::encode_texel(p_world.generate(x * s, y * s, z * s, octaves, accuracy, s, &material), p_level) + 128);
				render_bytes(material, texel + 1);
			}
		}
	});
	// Which of the slab's bricks hold surface.
	const Vector3i nb = n / BRICK;
	const Vector3i b0(floor_div(r_slab.lo.x, BRICK), floor_div(r_slab.lo.y, BRICK), floor_div(r_slab.lo.z, BRICK));
	r_slab.surface.assign((size_t)nb.x * nb.y * nb.z, 0);
	for (int bz = 0; bz < nb.z; bz++) {
		for (int by = 0; by < nb.y; by++) {
			for (int bx = 0; bx < nb.x; bx++) {
				bool surface = false;
				for (int z = 0; z < BRICK && !surface; z++) {
					for (int y = 0; y < BRICK && !surface; y++) {
						const uint8_t *row = &r_texels[texel_index(Vector3i((b0.x + bx) * BRICK, (b0.y + by) * BRICK + y, (b0.z + bz) * BRICK + z)) * TEXEL_BYTES];
						for (int x = 0; x < BRICK; x++) {
							const int q = (int)row[x * TEXEL_BYTES] - 128;
							surface |= q > -DENSITY_LIMIT && q < DENSITY_LIMIT;
						}
					}
				}
				r_slab.surface[((size_t)bz * nb.y + by) * nb.x + bx] = surface;
			}
		}
	}
}

void Clipmap::start_job(int p_level, const Vector3i &p_target, const VoxelWorld &p_world, bool p_wait, int p_threads) {
	Level &level = levels[p_level];
	std::unique_ptr<Job> job = std::make_unique<Job>();
	job->origin = p_target;
	job->old_origin = level.origin;
	// The new box minus the old one, as up to 6 slabs: peel off the part
	// outside the old range one axis at a time.
	Vector3i lo = p_target, hi = p_target + Vector3i(size - 1, size - 1, size - 1);
	bool overlap = level.valid;
	for (int a = 0; a < 3 && overlap; a++) {
		overlap = std::abs(p_target[a] - level.origin[a]) < size;
	}
	if (!overlap) {
		job->reset = true;
		job->texels.assign((size_t)size * size * size * TEXEL_BYTES, 0);
		job->slabs.push_back({ lo, hi, {} });
	} else {
		for (int a = 0; a < 3; a++) {
			const int old_lo = level.origin[a], old_hi = level.origin[a] + size - 1;
			if (lo[a] < old_lo) {
				Vector3i slab_hi = hi;
				slab_hi[a] = old_lo - 1;
				job->slabs.push_back({ lo, slab_hi, {} });
				lo[a] = old_lo;
			}
			if (hi[a] > old_hi) {
				Vector3i slab_lo = lo;
				slab_lo[a] = old_hi + 1;
				job->slabs.push_back({ slab_lo, hi, {} });
				hi[a] = old_hi;
			}
		}
	}
	uint8_t *target = job->reset ? job->texels.data() : level.texels.data();
	if (p_wait) {
		for (Slab &slab : job->slabs) {
			generate_slab(p_world, p_level, slab, target, p_threads);
		}
		job->finished = true;
	} else {
		Job *raw = job.get();
		raw->thread = std::thread([this, raw, target, &p_world, p_level]() {
			for (Slab &slab : raw->slabs) {
				generate_slab(p_world, p_level, slab, target, 1);
			}
			raw->finished = true;
		});
	}
	jobs[p_level] = std::move(job);
}

void Clipmap::finish_job(int p_level, VoxelWorld &r_world) {
	std::unique_ptr<Job> job = std::move(jobs[p_level]);
	if (job->thread.joinable()) {
		job->thread.join();
	}
	job->origin += job->shift;
	job->old_origin += job->shift;
	for (Slab &slab : job->slabs) {
		slab.lo += job->shift;
		slab.hi += job->shift;
	}
	Level &level = levels[p_level];
	const int nb = size / BRICK;
	if (job->reset) {
		level.texels.swap(job->texels);
		level.layer_dirty.assign(size, 1);
		level.has_surface.assign((size_t)nb * nb * nb, 0);
		level.bricks.assign(level.has_surface.size(), MAX_BRICK_DISTANCE);
		level.bricks_dirty = true;
	}
	level.origin = job->origin;
	for (const Slab &slab : job->slabs) {
		const Vector3i b0(floor_div(slab.lo.x, BRICK), floor_div(slab.lo.y, BRICK), floor_div(slab.lo.z, BRICK));
		const Vector3i n = (slab.hi - slab.lo + Vector3i(1, 1, 1)) / BRICK;
		for (int z = 0; z < n.z; z++) {
			for (int y = 0; y < n.y; y++) {
				for (int x = 0; x < n.x; x++) {
					level.has_surface[brick_index(b0 + Vector3i(x, y, z))] = slab.surface[((size_t)z * n.y + y) * n.x + x];
				}
			}
		}
		// The worker generated these from noise only.
		patch_edited(level, p_level, r_world, slab.lo, slab.hi);
		mark_layers(level, slab.lo.z, slab.hi.z);
	}
	// Edits made while it ran, now over the whole new box.
	const int s = level.scale;
	const Vector3i last = level.origin + Vector3i(size - 1, size - 1, size - 1);
	for (const auto &edit : level.pending_edits) {
		for_each_repeat(r_world, edit.first, edit.second, level.origin, last, s, [&](const Vector3i &t0, const Vector3i &t1) {
			write_texels(level, p_level, r_world, t0, t1);
		});
	}
	level.pending_edits.clear();
	update_brick_distances(level);
	level.valid = true;
}

int Clipmap::follow(VoxelWorld &r_world, const Vector3 &p_center, int p_threads, bool p_wait_first, bool p_wait) {
	int moved = 0;
	bool finished_one = false;
	for (int l = 0; l < (int)levels.size(); l++) {
		Level &level = levels[l];
		if (jobs[l]) {
			// Spread finishing over frames: at most one per call.
			if (!p_wait && (!jobs[l]->finished || finished_one)) {
				continue;
			}
			finish_job(l, r_world);
			moved |= 1 << l;
			finished_one = true;
		}
		if (level.valid && (!level.published || level.published_origin != level.origin)) {
			continue; // the last move isn't on the GPU yet
		}
		const int s = level.scale;
		bool centred = level.valid;
		for (int a = 0; a < 3 && centred; a++) {
			centred = std::abs(p_center[a] / s - (level.origin[a] + size * 0.5)) <= RECENTER;
		}
		if (centred) {
			continue;
		}
		Vector3i target;
		for (int a = 0; a < 3; a++) {
			target[a] = (int)std::lround((p_center[a] / s - size * 0.5) / RECENTER) * RECENTER;
		}
		if (level.valid && target == level.origin) {
			continue;
		}
		const bool wait = p_wait || (p_wait_first && l == 0 && !level.valid);
		start_job(l, target, r_world, wait, p_threads);
		if (wait) {
			finish_job(l, r_world);
			moved |= 1 << l;
		}
	}
	return moved;
}
