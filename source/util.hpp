#pragma once

#include <3ds.h>

#include <cstdint>
#include <string>
#include <vector>

// Small RAII wrappers around libctru's lightweight sync primitives.
class Mutex {
public:
	Mutex() { LightLock_Init(&lock_); }
	void lock() { LightLock_Lock(&lock_); }
	void unlock() { LightLock_Unlock(&lock_); }
	LightLock* native() { return &lock_; }

private:
	LightLock lock_;
};

class LockGuard {
public:
	explicit LockGuard(Mutex& m) : m_(m) { m_.lock(); }
	~LockGuard() { m_.unlock(); }
	LockGuard(const LockGuard&) = delete;
	LockGuard& operator=(const LockGuard&) = delete;

private:
	Mutex& m_;
};

class Cond {
public:
	Cond() { CondVar_Init(&cv_); }
	void wait(Mutex& m) { CondVar_Wait(&cv_, m.native()); }
	// Returns false on timeout.
	bool waitFor(Mutex& m, s64 ns) { return CondVar_WaitTimeout(&cv_, m.native(), ns) == 0; }
	void signal() { CondVar_Signal(&cv_); }
	void broadcast() { CondVar_Broadcast(&cv_); }

private:
	CondVar cv_;
};

namespace util {

// Thread priorities (lower number = higher priority). The main thread runs at 0x30.
constexpr int PRIO_AUDIO = 0x2A;
constexpr int PRIO_NET = 0x2C;
constexpr int PRIO_WORKER = 0x2E;
constexpr int PRIO_VIDEO = 0x31;

// Creates a thread, trying the preferred core first and falling back to any core.
Thread startThread(ThreadFunc fn, void* arg, size_t stack, int prio, int preferredCore);

bool isNew3DS();

u64 nowMs();
void sleepMs(u32 ms);

std::string formatTime(double seconds);          // 83 -> "1:23", 3700 -> "1:01:40"
std::string urlEncode(const std::string& s);
std::string jsonEscape(const std::string& s);    // without surrounding quotes
std::string trim(const std::string& s);
std::string toLower(std::string s);

// Parses "3 days ago", "Streamed 2 weeks ago", "1y ago" into an approximate age in seconds.
// Unknown strings return a large value so they sort last.
s64 parseRelativeAge(const std::string& s);

// UTF-8 helpers
u32 utf8Next(const char*& p, const char* end);   // returns codepoint, advances p

bool readFile(const std::string& path, std::string& out);
bool writeFile(const std::string& path, const std::string& data);  // atomic-ish (tmp + rename)
void mkdirs(const std::string& path);

// Data directory on the SD card.
const std::string& dataDir();

void logf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

}  // namespace util
