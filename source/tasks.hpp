#pragma once

#include <functional>

#include "yt/innertube.hpp"

// Blocking network work runs on a small pool of worker threads, each with its
// own HTTP connection. Results are handed back to the main thread via post(),
// and run during pump() at the start of each frame.
namespace tasks {

void start(int workers = 2);
void stop();

// Runs `job` on a worker thread.
void run(std::function<void(yt::InnerTube&)> job);
// Queues `fn` to run on the main thread.
void post(std::function<void()> fn);
// Main thread: runs posted callbacks.
void pump();

}  // namespace tasks
