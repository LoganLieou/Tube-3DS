#include "tasks.hpp"

#include <deque>
#include <vector>

#include "net/http.hpp"
#include "util.hpp"

namespace tasks {

static Mutex g_lock;
static Cond g_cond;
static std::deque<std::function<void(yt::InnerTube&)>> g_jobs;
static std::vector<std::function<void()>> g_posted;
static std::vector<Thread> g_threads;
static bool g_quit = false;

static void workerMain(void*) {
	net::Client http;
	yt::InnerTube it(http);
	while (true) {
		std::function<void(yt::InnerTube&)> job;
		{
			LockGuard g(g_lock);
			while (!g_quit && g_jobs.empty()) g_cond.wait(g_lock);
			if (g_quit) return;
			job = std::move(g_jobs.front());
			g_jobs.pop_front();
		}
		job(it);
	}
}

void start(int workers) {
	g_quit = false;
	for (int i = 0; i < workers; i++) {
		Thread t = util::startThread(workerMain, nullptr, 128 * 1024, util::PRIO_WORKER, -2);
		if (t) g_threads.push_back(t);
	}
}

void stop() {
	{
		LockGuard g(g_lock);
		g_quit = true;
		g_jobs.clear();
		g_cond.broadcast();
	}
	// A worker may be stuck in a slow request; don't hold up exiting for it.
	for (Thread t : g_threads)
		if (R_SUCCEEDED(threadJoin(t, 3000000000ULL))) threadFree(t);
	g_threads.clear();
	g_posted.clear();
}

void run(std::function<void(yt::InnerTube&)> job) {
	LockGuard g(g_lock);
	g_jobs.push_back(std::move(job));
	g_cond.signal();
}

void post(std::function<void()> fn) {
	LockGuard g(g_lock);
	g_posted.push_back(std::move(fn));
}

void pump() {
	std::vector<std::function<void()>> fns;
	{
		LockGuard g(g_lock);
		fns.swap(g_posted);
	}
	for (auto& f : fns) f();
}

}  // namespace tasks
