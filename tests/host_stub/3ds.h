// Minimal stand-in for libctru so platform-independent code can be tested on a
// desktop. Sync primitives and threads are backed by pthreads.
#pragma once
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/time.h>
#include <unistd.h>
#include <errno.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
typedef int8_t s8; typedef int16_t s16; typedef int32_t s32; typedef int64_t s64;
typedef s32 Result;
#define R_SUCCEEDED(r) ((r) >= 0)
#define R_FAILED(r) ((r) < 0)
#ifndef U64_MAX
#define U64_MAX UINT64_MAX
#endif
typedef pthread_mutex_t LightLock;
typedef pthread_cond_t CondVar;
typedef pthread_t* Thread;
typedef void (*ThreadFunc)(void*);
static inline void LightLock_Init(LightLock* l) { pthread_mutex_init(l, nullptr); }
static inline void LightLock_Lock(LightLock* l) { pthread_mutex_lock(l); }
static inline void LightLock_Unlock(LightLock* l) { pthread_mutex_unlock(l); }
static inline void CondVar_Init(CondVar* c) { pthread_cond_init(c, nullptr); }
static inline void CondVar_Wait(CondVar* c, LightLock* l) { pthread_cond_wait(c, l); }
static inline int CondVar_WaitTimeout(CondVar* c, LightLock* l, s64 ns) {
	timeval tv; gettimeofday(&tv, nullptr);
	s64 t = (s64)tv.tv_sec * 1000000000LL + tv.tv_usec * 1000LL + ns;
	timespec ts{(time_t)(t / 1000000000LL), (long)(t % 1000000000LL)};
	return pthread_cond_timedwait(c, l, &ts) == ETIMEDOUT ? 1 : 0;
}
static inline void CondVar_Signal(CondVar* c) { pthread_cond_signal(c); }
static inline void CondVar_Broadcast(CondVar* c) { pthread_cond_broadcast(c); }
struct HostThreadStart { ThreadFunc fn; void* arg; };
static inline void* hostThreadMain(void* p) {
	HostThreadStart s = *(HostThreadStart*)p; delete (HostThreadStart*)p; s.fn(s.arg); return nullptr;
}
static inline Thread threadCreate(ThreadFunc fn, void* arg, size_t, int, int, bool) {
	Thread t = new pthread_t;
	if (pthread_create(t, nullptr, hostThreadMain, new HostThreadStart{fn, arg}) != 0) { delete t; return nullptr; }
	return t;
}
static inline Result threadJoin(Thread t, u64) { return pthread_join(*t, nullptr) == 0 ? 0 : -1; }
static inline void threadFree(Thread t) { delete t; }
static inline Result APT_CheckNew3DS(bool* out) { *out = false; return 0; }
static inline u64 osGetTime() { timeval tv; gettimeofday(&tv, nullptr); return (u64)tv.tv_sec * 1000 + tv.tv_usec / 1000; }
static inline void svcSleepThread(s64 ns) { usleep((useconds_t)(ns / 1000)); }
