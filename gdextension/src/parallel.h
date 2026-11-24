#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace godot {

// Runs p_body(i) for i in [0, p_count) on up to p_threads threads (including this one).
template <typename F>
void parallel_for(int p_count, int p_threads, const F &p_body) {
	std::atomic<int> next{ 0 };
	auto worker = [&]() {
		for (int i = next++; i < p_count; i = next++) {
			p_body(i);
		}
	};
	std::vector<std::thread> pool;
	for (int t = 1; t < std::min(p_threads, p_count); t++) {
		pool.emplace_back(worker);
	}
	worker();
	for (std::thread &t : pool) {
		t.join();
	}
}

// Like parallel_for(), on threads kept waiting between calls: for small
// jobs many times a second (posing bodies), where starting threads each time
// would cost a good part of the work. One job at a time: a call while it's
// busy (from another thread) runs on the caller alone. shutdown() stops the
// threads (the library unloading).
class WorkerPool {
public:
	static WorkerPool &get() {
		static WorkerPool *pool = new WorkerPool(); // (never deleted: shutdown() joins)
		return *pool;
	}

	template <typename F>
	void run(int p_count, const F &p_body) {
		std::unique_lock<std::mutex> busy(job_mutex, std::try_to_lock);
		if (!busy.owns_lock() || p_count <= 1) {
			for (int i = 0; i < p_count; i++) {
				p_body(i);
			}
			return;
		}
		start();
		const std::function<void(int)> body = [&p_body](int i) { p_body(i); };
		{
			std::lock_guard<std::mutex> lock(mutex);
			job = &body;
			count = p_count;
			next = 0;
			working = (int)threads.size();
			generation++;
		}
		wake.notify_all();
		for (int i = next++; i < p_count; i = next++) {
			body(i);
		}
		std::unique_lock<std::mutex> lock(mutex);
		finished.wait(lock, [&] { return working == 0; });
		job = nullptr;
	}

	void shutdown() {
		{
			std::lock_guard<std::mutex> lock(mutex);
			stopping = true;
		}
		wake.notify_all();
		for (std::thread &t : threads) {
			t.join();
		}
		threads.clear();
		stopping = false;
	}

private:
	std::mutex job_mutex; // one job at a time
	std::mutex mutex;
	std::condition_variable wake, finished;
	std::vector<std::thread> threads;
	const std::function<void(int)> *job = nullptr;
	std::atomic<int> next{ 0 };
	int count = 0;
	int working = 0;
	uint64_t generation = 0;
	bool stopping = false;

	void start() {
		if (!threads.empty()) {
			return;
		}
		const int n = std::clamp((int)std::thread::hardware_concurrency() - 1, 1, 7);
		for (int t = 0; t < n; t++) {
			threads.emplace_back([this] { loop(); });
		}
	}

	void loop() {
		uint64_t seen = 0;
		while (true) {
			const std::function<void(int)> *body;
			int total;
			{
				std::unique_lock<std::mutex> lock(mutex);
				wake.wait(lock, [&] { return stopping || generation != seen; });
				if (stopping) {
					return;
				}
				seen = generation;
				body = job;
				total = count;
			}
			for (int i = next++; i < total; i = next++) {
				(*body)(i);
			}
			{
				std::lock_guard<std::mutex> lock(mutex);
				working--;
			}
			finished.notify_one();
		}
	}
};

// p_requested, or one per CPU core when it is 0 or less.
inline int resolve_thread_count(int p_requested) {
	const int threads = p_requested > 0 ? p_requested : (int)std::thread::hardware_concurrency();
	return std::clamp(threads, 1, 64);
}

inline int floor_div(int a, int b) {
	return a >= 0 ? a / b : -((-a + b - 1) / b);
}

} // namespace godot
