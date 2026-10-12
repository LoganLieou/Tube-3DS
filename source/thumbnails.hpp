#pragma once

#include <3ds.h>
#include <citro2d.h>

#include <atomic>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "util.hpp"

// Downloads and caches video thumbnails as 256x144 GPU textures.
//
// get() is called while drawing; anything not yet cached is queued. Background
// threads always work on the most recently requested thumbnails first and skip
// ones that scrolled out of view, so lists fill in quickly as you browse.
class Thumbnails {
public:
	static constexpr int kW = 256, kH = 144;

	void start();
	void stop();

	// Main thread. Returns nullptr until the thumbnail has loaded.
	const C2D_Image* get(const std::string& videoId);
	// Main thread, once per frame: uploads finished downloads, evicts old textures.
	void update();

private:
	struct Entry {
		C3D_Tex tex;
		Tex3DS_SubTexture sub;
		C2D_Image img;
		u64 lastUse = 0;
	};
	struct Decoded {
		std::string id;
		std::vector<u16> pixels;  // tiled RGB565, 256x256
		bool ok = false;
	};

	std::unordered_map<std::string, std::unique_ptr<Entry>> cache_;
	std::unordered_map<std::string, u64> failedAt_;
	u64 frame_ = 0;

	Mutex lock_;
	Cond work_;
	std::vector<std::string> wanted_;                 // LIFO request stack
	std::unordered_map<std::string, u64> wantedAt_;   // id -> last time requested (ms)
	std::unordered_map<std::string, bool> inflight_;
	std::vector<Decoded> done_;
	std::atomic<bool> quit_{false};
	Thread threads_[2] = {nullptr, nullptr};

	void worker();
	static void workerEntry(void* p) { static_cast<Thumbnails*>(p)->worker(); }
};

extern Thumbnails g_thumbs;
