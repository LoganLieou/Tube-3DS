#include "media/frame_queue.hpp"

extern "C" {
#include <libavutil/frame.h>
}

#include <cstring>

FrameQueue::~FrameQueue() { shutdown(); }

bool FrameQueue::init(int w, int h, int count) {
	int texW = (int)nextPow2((u32)w), texH = (int)nextPow2((u32)h);
	if (texW > 1024 || texH > 1024) return false;
	w_ = w;
	h_ = h;
	layout_.init(texW, texH);
	slots_.resize(count);
	for (auto& s : slots_) {
		if (!texInitLocked(&s.tex, (u16)texW, (u16)texH, GPU_RGB565)) {
			util::logf("frame texture alloc failed (%dx%d)", texW, texH);
			slots_.clear();
			return false;
		}
		C3D_TexSetFilter(&s.tex, GPU_LINEAR, GPU_LINEAR);
		C3D_TexSetWrap(&s.tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
		memset(s.tex.data, 0, s.tex.size);
		C3D_TexFlush(&s.tex);
		s.sub = makeSubTexture(w, h, texW, texH);
		s.st = St::Free;
	}
	ready_ = true;
	return true;
}

int FrameQueue::acquire(s64 timeoutNs) {
	LockGuard g(lock_);
	for (int attempt = 0; attempt < 2; attempt++) {
		for (size_t i = 0; i < slots_.size(); i++) {
			if (slots_[i].st == St::Free) {
				slots_[i].st = St::Decoding;
				return (int)i;
			}
		}
		if (attempt == 0) freed_.waitFor(lock_, timeoutNs);
	}
	return -1;
}

void FrameQueue::write(int slot, const AVFrame* f) {
	Slot& s = slots_[slot];
	yuv420ToTiled565(f->data[0], f->linesize[0], f->data[1], f->linesize[1], f->data[2], f->linesize[2],
	                 f->width < w_ ? f->width : w_, f->height < h_ ? f->height : h_, (u16*)s.tex.data, layout_);
	GSPGPU_FlushDataCache(s.tex.data, s.tex.size);
}

void FrameQueue::publish(int slot, double pts, int serial) {
	LockGuard g(lock_);
	slots_[slot].pts = pts;
	slots_[slot].serial = serial;
	slots_[slot].st = St::Ready;
}

void FrameQueue::release(int slot) {
	LockGuard g(lock_);
	slots_[slot].st = St::Free;
	freed_.broadcast();
}

void FrameQueue::clearReady() {
	LockGuard g(lock_);
	for (auto& s : slots_)
		if (s.st == St::Ready) s.st = St::Free;
	freed_.broadcast();
}

int FrameQueue::readyCount() {
	LockGuard g(lock_);
	int n = 0;
	for (auto& s : slots_)
		if (s.st == St::Ready) n++;
	return n;
}

bool FrameQueue::present(double clock, int serial, C2D_Image& out) {
	if (!ready_) return false;
	LockGuard g(lock_);
	bool freedAny = false;
	int best = -1;
	for (size_t i = 0; i < slots_.size(); i++) {
		Slot& s = slots_[i];
		if (s.st == St::Retired) {
			s.st = St::Free;
			freedAny = true;
		} else if (s.st == St::Ready) {
			if (s.serial != serial) {
				s.st = St::Free;  // left over from before a seek
				freedAny = true;
			} else if (s.pts <= clock + 0.02 && (best < 0 || s.pts > slots_[best].pts)) {
				best = (int)i;
			}
		}
	}
	if (best >= 0) {
		for (auto& s : slots_) {
			if (s.st == St::Shown) s.st = St::Retired;
			// Anything older than the chosen frame is late: drop it.
			else if (s.st == St::Ready && s.pts < slots_[best].pts) {
				s.st = St::Free;
				freedAny = true;
				dropped_++;
			}
		}
		slots_[best].st = St::Shown;
		presented_++;
	}
	if (freedAny) freed_.broadcast();

	for (auto& s : slots_) {
		if (s.st == St::Shown) {
			out.tex = &s.tex;
			out.subtex = &s.sub;
			return true;
		}
	}
	return false;
}

void FrameQueue::shutdown() {
	LockGuard g(lock_);
	for (auto& s : slots_) deferTexFree(s.tex);
	slots_.clear();
	ready_ = false;
}
