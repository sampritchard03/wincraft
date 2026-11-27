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
	fix_jobs.clear();
	fix_jobs.resize(p_level_count);
}

void Clipmap::cancel_jobs() {
	for (std::unique_ptr<Job> &job : jobs) {
		if (job && job->thread.joinable()) {
			job->thread.join();
		}
		job.reset();
	}
	for (std::unique_ptr<FixJob> &job : fix_jobs) {
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
		if (l < (int)fix_jobs.size() && fix_jobs[l]) {
			FixJob &fix = *fix_jobs[l];
			if (fix.thread.joinable()) {
				fix.thread.join();
			}
			if (keeps) {
				const Vector3i texels = p_offset / level.scale;
				fix.a += texels;
				fix.wa += texels;
				fix.wb += texels;
				fix.min += texels;
				fix.max += texels;
				fix.origin += texels;
			} else {
				fix_jobs[l].reset();
			}
		}
		if (!keeps) {
			level.valid = false;
			level.published = false;
			level.pending_edits.clear();
			level.fix_lo = Vector3i(1, 1, 1);
			level.fix_hi = Vector3i();
			continue;
		}
		const Vector3i texels = p_offset / level.scale;
		level.origin += texels;
		level.published_origin += texels;
		for (auto &[lo, hi] : level.pending_edits) {
			lo += p_offset;
			hi += p_offset;
		}
		level.fix_lo += texels;
		level.fix_hi += texels;
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
	// The GPU layer for slice z also holds slice z + 1's densities (see
	// SculptTerrain::upload_level()), so the slice below changes too.
	if (p_z1 - p_z0 + 2 >= size) {
		std::fill(r_level.layer_dirty.begin(), r_level.layer_dirty.end(), 1);
		return;
	}
	for (int z = p_z0 - 1; z <= p_z1; z++) {
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
								const size_t at = texel_index(Vector3i(x, y, z));
								uint8_t *texel = &r_level.texels[at * TEXEL_BYTES];
								texel[0] = (uint8_t)(q + 128);
								if (!r_level.raw.empty()) {
									r_level.raw[at] = texel[0];
								}
								render_bytes(*in_material, texel + 1);
							}
						}
					}
				} else {
					for (int z = t0.z; z <= t1.z; z++) {
						for (int y = t0.y; y <= t1.y; y++) {
							for (int x = t0.x; x <= t1.x; x++) {
								const size_t at = texel_index(Vector3i(x, y, z));
								uint8_t *texel = &r_level.texels[at * TEXEL_BYTES];
								MaterialId material = MAT_STONE;
								texel[0] = (uint8_t)(r_world.lod_texel(x * s, y * s, z * s, p_level, material) + 128);
								if (!r_level.raw.empty()) {
									r_level.raw[at] = texel[0];
								}
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
			if (l > 0) {
				queue_fix(level, t0, t1);
			}
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

// --- Distances near edits ------------------------------------------------------

void Clipmap::queue_fix(Level &r_level, const Vector3i &p_min, const Vector3i &p_max) {
	if (r_level.fix_lo.x > r_level.fix_hi.x) {
		r_level.fix_lo = p_min;
		r_level.fix_hi = p_max;
		return;
	}
	for (int a = 0; a < 3; a++) {
		r_level.fix_lo[a] = std::min(r_level.fix_lo[a], p_min[a]);
		r_level.fix_hi[a] = std::max(r_level.fix_hi[a], p_max[a]);
	}
}

bool Clipmap::fix_pending(const VoxelWorld &p_world, bool p_all) {
	bool changed = false;
	for (int l = 1; l < (int)levels.size(); l++) {
		Level &level = levels[l];
		std::unique_ptr<FixJob> &job = fix_jobs[l];
		if (job) {
			if (!p_all && !job->finished) {
				continue;
			}
			if (job->thread.joinable()) {
				job->thread.join();
			}
			if (jobs[l] && !p_all) {
				continue; // applied once the move is done (or thrown away if it moved)
			}
			if (level.valid && !jobs[l] && level.origin == job->origin) {
				if (apply_fix(level, *job)) {
					update_brick_distances(level);
				}
				changed = true;
			} else if (level.valid) {
				queue_fix(level, job->min, job->max); // the level moved: again
			}
			job.reset();
		}
		if (level.fix_lo.x > level.fix_hi.x) {
			continue;
		}
		if (!level.valid || level.raw.empty()) {
			level.fix_lo = Vector3i(1, 1, 1);
			level.fix_hi = Vector3i();
			continue;
		}
		if (jobs[l]) {
			continue; // once the move is done (it queues its own)
		}
		job = start_fix(level, l, p_world, level.fix_lo, level.fix_hi);
		level.fix_lo = Vector3i(1, 1, 1);
		level.fix_hi = Vector3i();
		if (!job) {
			continue;
		}
		if (p_all) {
			compute_fix(*job);
			if (apply_fix(level, *job)) {
				update_brick_distances(level);
			}
			job.reset();
			changed = true;
		} else {
			FixJob *raw = job.get();
			raw->thread = std::thread([raw]() {
				compute_fix(*raw);
				raw->finished = true;
			});
		}
	}
	return changed;
}

std::unique_ptr<Clipmap::FixJob> Clipmap::start_fix(Level &r_level, int p_level, const VoxelWorld &p_world, const Vector3i &p_min, const Vector3i &p_max) {
	// Written: within CLAMP texels of the box. Read: CLAMP texels further,
	// so every crossing near a written texel is seen.
	const int W = FIX_REACH - 1, R = 2 * FIX_REACH;
	const Vector3i last = r_level.origin + Vector3i(size - 1, size - 1, size - 1);
	std::unique_ptr<FixJob> job = std::make_unique<FixJob>();
	FixJob &j = *job;
	j.min = p_min;
	j.max = p_max;
	j.origin = r_level.origin;
	Vector3i b;
	for (int k = 0; k < 3; k++) {
		j.a[k] = std::max(p_min[k] - R, r_level.origin[k]);
		b[k] = std::min(p_max[k] + R, last[k]);
		j.wa[k] = std::max(p_min[k] - W, r_level.origin[k]);
		j.wb[k] = std::min(p_max[k] + W, last[k]);
		if (j.a[k] > b[k] || j.wa[k] > j.wb[k]) {
			return nullptr;
		}
	}
	j.n = b - j.a + Vector3i(1, 1, 1);
	const Vector3i n = j.n;
	const int s = r_level.scale;
	j.saturated = std::abs((int)VoxelWorld::encode_texel(DENSITY_CLAMP, p_level));
	// The raw values, a row at a time (x wraps at most once along one).
	j.raw.resize((size_t)n.x * n.y * n.z);
	std::vector<int> wx(n.x);
	for (int x = 0; x < n.x; x++) {
		wx[x] = wrap(j.a.x + x, size);
	}
	uint8_t *out = j.raw.data();
	for (int z = 0; z < n.z; z++) {
		for (int y = 0; y < n.y; y++) {
			const uint8_t *row = &r_level.raw[((size_t)wrap(j.a.z + z, size) * size + wrap(j.a.y + y, size)) * size];
			for (int x = 0; x < n.x; x++) {
				*out++ = row[wx[x]];
			}
		}
	}
	// Which chunks are edited (only their "at least" values are clamped).
	const int C = VoxelWorld::CHUNK;
	j.scale = s;
	j.chunk0 = VoxelWorld::chunk_of(j.a.x * s, j.a.y * s, j.a.z * s);
	const Vector3i chunk1 = VoxelWorld::chunk_of(b.x * s, b.y * s, b.z * s);
	j.chunk_n = chunk1 - j.chunk0 + Vector3i(1, 1, 1);
	j.chunk_edited.resize((size_t)j.chunk_n.x * j.chunk_n.y * j.chunk_n.z);
	size_t k = 0;
	for (int z = 0; z < j.chunk_n.z; z++) {
		for (int y = 0; y < j.chunk_n.y; y++) {
			for (int x = 0; x < j.chunk_n.x; x++) {
				j.chunk_edited[k++] = p_world.chunk_edited(j.chunk0 + Vector3i(x, y, z));
			}
		}
	}
	(void)C;
	return job;
}

void Clipmap::compute_fix(FixJob &r_job) {
	const Vector3i n = r_job.n;
	// Raw values in texels, and which are only "at least" values.
	std::vector<float> v(r_job.raw.size());
	std::vector<uint8_t> clamped(v.size(), 0);
	{
		const int s = r_job.scale;
		size_t i = 0;
		for (int z = 0; z < n.z; z++) {
			for (int y = 0; y < n.y; y++) {
				for (int x = 0; x < n.x; x++, i++) {
					const int q = (int)r_job.raw[i] - 128;
					v[i] = q / 16.0f;
					if (std::abs(q) >= r_job.saturated && r_job.saturated < DENSITY_LIMIT) {
						const Vector3i w = r_job.a + Vector3i(x, y, z);
						const Vector3i c = VoxelWorld::chunk_of(w.x * s, w.y * s, w.z * s) - r_job.chunk0;
						clamped[i] = r_job.chunk_edited[((size_t)c.z * r_job.chunk_n.y + c.y) * r_job.chunk_n.x + c.x];
					}
				}
			}
		}
	}
	const float CLAMP_TEXELS = DENSITY_LIMIT / 16.0f;
	auto at = [&](int x, int y, int z) { return ((size_t)z * n.y + y) * n.x + x; };
	// Distances, padded by one "infinitely far" point all round. Seeds: the
	// raw values that are distances, and the crossings along grid edges.
	const int px = n.x + 2, py = n.y + 2, pz = n.z + 2;
	const ptrdiff_t psy = px, psz = (ptrdiff_t)px * py;
	const float INF = 1e30f;
	std::vector<float> dist((size_t)px * py * pz, INF);
	auto pat = [&](int x, int y, int z) { return (z + 1) * psz + (y + 1) * psy + (x + 1); };
	const int T = 8;
	const int tx = (n.x + T - 1) / T, ty = (n.y + T - 1) / T, tz = (n.z + T - 1) / T;
	std::vector<uint8_t> crossing((size_t)tx * ty * tz, 0);
	auto tile = [&](int x, int y, int z) { return ((size_t)(z / T) * ty + y / T) * tx + x / T; };
	const size_t step[3] = { 1, (size_t)n.x, (size_t)n.x * n.y };
	const ptrdiff_t pstep[3] = { 1, psy, psz };
	for (int z = 0; z < n.z; z++) {
		for (int y = 0; y < n.y; y++) {
			for (int x = 0; x < n.x; x++) {
				const size_t i = at(x, y, z);
				const ptrdiff_t pi = pat(x, y, z);
				const float here = v[i];
				if (!clamped[i]) {
					dist[pi] = std::min(dist[pi], std::abs(here));
				}
				const int pos[3] = { x, y, z };
				const int lim[3] = { n.x, n.y, n.z };
				for (int k = 0; k < 3; k++) {
					if (pos[k] + 1 >= lim[k]) {
						continue;
					}
					const float there = v[i + step[k]];
					if ((here > 0.0f) == (there > 0.0f)) {
						continue;
					}
					const float t = here / (here - there);
					dist[pi] = std::min(dist[pi], t);
					dist[pi + pstep[k]] = std::min(dist[pi + pstep[k]], 1.0f - t);
					crossing[tile(x, y, z)] = 1;
					int q[3] = { x, y, z };
					q[k]++;
					crossing[tile(q[0], q[1], q[2])] = 1;
				}
			}
		}
	}
	// Only tiles with surface in or next to them: the rest are at least a
	// tile (8 texels, ~CLAMP) from it.
	std::vector<uint8_t> active(crossing.size(), 0);
	for (int z = 0; z < tz; z++) {
		for (int y = 0; y < ty; y++) {
			for (int x = 0; x < tx; x++) {
				bool any = false;
				for (int dz = -1; dz <= 1 && !any; dz++) {
					for (int dy = -1; dy <= 1 && !any; dy++) {
						for (int dx = -1; dx <= 1 && !any; dx++) {
							const int ax = x + dx, ay = y + dy, az = z + dz;
							any = ax >= 0 && ay >= 0 && az >= 0 && ax < tx && ay < ty && az < tz && crossing[((size_t)az * ty + ay) * tx + ax];
						}
					}
				}
				active[((size_t)z * ty + y) * tx + x] = any;
			}
		}
	}
	// Two passes over the 13 neighbours before / after each point. Step
	// lengths a little under the true ones (1, sqrt 2, sqrt 3), so a path is
	// never longer than the straight line it stands for: the result must not
	// say the surface is farther than it is.
	ptrdiff_t offset[13];
	float length[13];
	int count = 0;
	for (int dz = -1; dz <= 0; dz++) {
		for (int dy = -1; dy <= 1; dy++) {
			for (int dx = -1; dx <= 1; dx++) {
				if (dz == 0 && (dy > 0 || (dy == 0 && dx >= 0))) {
					continue;
				}
				const int k = dx * dx + dy * dy + dz * dz;
				offset[count] = dz * psz + dy * psy + dx;
				length[count] = k == 1 ? 0.875f : (k == 2 ? 1.25f : 1.5625f);
				count++;
			}
		}
	}
	float *d = dist.data();
	for (int z = 0; z < n.z; z++) {
		for (int y = 0; y < n.y; y++) {
			for (int t = 0; t < tx; t++) {
				if (!active[((size_t)(z / T) * ty + y / T) * tx + t]) {
					continue;
				}
				for (int x = t * T; x < std::min((t + 1) * T, n.x); x++) {
					float *p = d + pat(x, y, z);
					float best = *p;
					for (int k = 0; k < 13; k++) {
						best = std::min(best, p[offset[k]] + length[k]);
					}
					*p = best;
				}
			}
		}
	}
	for (int z = n.z - 1; z >= 0; z--) {
		for (int y = n.y - 1; y >= 0; y--) {
			for (int t = tx - 1; t >= 0; t--) {
				if (!active[((size_t)(z / T) * ty + y / T) * tx + t]) {
					continue;
				}
				for (int x = std::min((t + 1) * T, n.x) - 1; x >= t * T; x--) {
					float *p = d + pat(x, y, z);
					float best = *p;
					for (int k = 0; k < 13; k++) {
						best = std::min(best, p[-offset[k]] + length[k]);
					}
					*p = best;
				}
			}
		}
	}
	// The result for the written box; a raw distance stays as it is unless
	// the surface is clearly nearer (near it, both are about the same, and
	// the raw one is finer: rewriting it would move the surface a little).
	const Vector3i wn = r_job.wb - r_job.wa + Vector3i(1, 1, 1);
	r_job.result.resize((size_t)wn.x * wn.y * wn.z);
	size_t o = 0;
	for (int z = r_job.wa.z; z <= r_job.wb.z; z++) {
		for (int y = r_job.wa.y; y <= r_job.wb.y; y++) {
			for (int x = r_job.wa.x; x <= r_job.wb.x; x++, o++) {
				const Vector3i l = Vector3i(x, y, z) - r_job.a;
				const size_t i = at(l.x, l.y, l.z);
				const float here = v[i];
				float e = std::min(d[pat(l.x, l.y, l.z)], CLAMP_TEXELS);
				if (!clamped[i] && e > std::abs(here) - 0.25f) {
					e = std::abs(here);
				}
				int q = std::clamp((int)std::lround(e * 16.0f), 0, (int)DENSITY_LIMIT);
				r_job.result[o] = (int8_t)(here > 0.0f ? std::max(q, 1) : -q);
			}
		}
	}
}

bool Clipmap::apply_fix(Level &r_level, const FixJob &p_job) {
	// Texels edited again since the copy was taken hold their new raw values
	// until the next pass (queued for them): not this pass's older ones.
	const bool requeued = r_level.fix_lo.x <= r_level.fix_hi.x;
	int z0 = INT32_MAX, z1 = INT32_MIN;
	size_t o = 0;
	std::vector<int> wx(p_job.wb.x - p_job.wa.x + 1);
	for (int x = p_job.wa.x; x <= p_job.wb.x; x++) {
		wx[x - p_job.wa.x] = wrap(x, size);
	}
	for (int z = p_job.wa.z; z <= p_job.wb.z; z++) {
		for (int y = p_job.wa.y; y <= p_job.wb.y; y++) {
			uint8_t *row = &r_level.texels[((size_t)wrap(z, size) * size + wrap(y, size)) * size * TEXEL_BYTES];
			const bool row_requeued = requeued && y >= r_level.fix_lo.y && z >= r_level.fix_lo.z && y <= r_level.fix_hi.y && z <= r_level.fix_hi.z;
			for (int x = p_job.wa.x; x <= p_job.wb.x; x++, o++) {
				if (row_requeued && x >= r_level.fix_lo.x && x <= r_level.fix_hi.x) {
					continue;
				}
				uint8_t &stored = row[wx[x - p_job.wa.x] * TEXEL_BYTES];
				const uint8_t q = (uint8_t)(p_job.result[o] + 128);
				if (stored != q) {
					stored = q;
					z0 = std::min(z0, z);
					z1 = std::max(z1, z);
				}
			}
		}
	}
	if (z0 > z1) {
		return false;
	}
	mark_layers(r_level, z0, z1);
	return update_has_surface(r_level, p_job.wa, p_job.wb);
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
	const bool coarse = p_level > 0;
	if (coarse && level.raw.size() != (size_t)size * size * size) {
		level.raw.assign((size_t)size * size * size, 128);
	}
	if (coarse && job->reset) {
		for (size_t i = 0; i < level.raw.size(); i++) {
			level.raw[i] = level.texels[i * TEXEL_BYTES];
		}
	}
	const Vector3i level_last = level.origin + Vector3i(size - 1, size - 1, size - 1);
	for (const Slab &slab : job->slabs) {
		if (coarse && !job->reset) {
			for (int z = slab.lo.z; z <= slab.hi.z; z++) {
				for (int y = slab.lo.y; y <= slab.hi.y; y++) {
					for (int x = slab.lo.x; x <= slab.hi.x; x++) {
						const size_t at = texel_index(Vector3i(x, y, z));
						level.raw[at] = level.texels[at * TEXEL_BYTES];
					}
				}
			}
		}
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
		if (coarse) {
			// Distances near edited chunks in the slab or just outside it
			// (the new texels don't know about those).
			const int s = level.scale, C = VoxelWorld::CHUNK, R = FIX_REACH;
			std::vector<Vector3i> edited;
			r_world.edited_chunks_in((slab.lo - Vector3i(R, R, R)) * s, (slab.hi + Vector3i(R + 1, R + 1, R + 1)) * s - Vector3i(1, 1, 1), edited);
			for (const Vector3i &c : edited) {
				Vector3i t0, t1;
				bool empty = false;
				for (int a = 0; a < 3; a++) {
					t0[a] = std::max(-floor_div(-c[a] * C, s), level.origin[a]);
					t1[a] = std::min(floor_div(c[a] * C + C - 1, s), level_last[a]);
					empty |= t0[a] > t1[a];
				}
				if (!empty) {
					queue_fix(level, t0, t1);
				}
			}
		}
	}
	// Edits made while it ran, now over the whole new box.
	const int s = level.scale;
	const Vector3i last = level.origin + Vector3i(size - 1, size - 1, size - 1);
	for (const auto &edit : level.pending_edits) {
		for_each_repeat(r_world, edit.first, edit.second, level.origin, last, s, [&](const Vector3i &t0, const Vector3i &t1) {
			write_texels(level, p_level, r_world, t0, t1);
			if (coarse) {
				queue_fix(level, t0, t1);
			}
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
