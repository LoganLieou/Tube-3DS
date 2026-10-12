#pragma once

#include <3ds.h>
#include <citro2d.h>

#include <vector>

#include "gfx_util.hpp"
#include "util.hpp"

struct AVFrame;

// Decoded video frames live directly in GPU textures. The decoder thread fills
// free slots; the main thread picks whichever ready frame is due according to
// the playback clock.
//
// Slot lifecycle: Free -> Decoding -> Ready -> Shown -> Retired -> Free.
// A slot is only recycled one presented frame after it stopped being shown, so
// the GPU is never reading a texture the decoder is overwriting.
class FrameQueue {
public:
	~FrameQueue();

	// Decoder thread. Allocates `count` textures large enough for w x h.
	bool init(int w, int h, int count);
	bool ready() const { return ready_; }
	int acquire(s64 timeoutNs);  // -1 if no slot freed up in time
	void write(int slot, const AVFrame* frame);
	void publish(int slot, double pts, int serial);
	void release(int slot);
	void clearReady();
	int readyCount();

	// Main thread. Updates which frame is shown; returns false if none yet.
	bool present(double clock, int serial, C2D_Image& out);
	void shutdown();  // main thread, after the decoder thread has exited

	int width() const { return w_; }
	// Stats for the debug log.
	u32 presented() const { return presented_; }
	u32 dropped() const { return dropped_; }
	int height() const { return h_; }

private:
	enum class St { Free, Decoding, Ready, Shown, Retired };
	struct Slot {
		C3D_Tex tex;
		Tex3DS_SubTexture sub;
		double pts = 0;
		int serial = 0;
		St st = St::Free;
	};
	std::vector<Slot> slots_;
	TiledLayout layout_;
	Mutex lock_;
	Cond freed_;
	int w_ = 0, h_ = 0;
	volatile bool ready_ = false;
	u32 presented_ = 0, dropped_ = 0;
};
