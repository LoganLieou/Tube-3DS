#include "thumbnails.hpp"

#include <algorithm>
#include <cstring>

#include "gfx_util.hpp"
#include "net/http.hpp"
#include "third_party/stb_image.h"

Thumbnails g_thumbs;

static constexpr int kTexW = 256, kTexH = 256;
static constexpr size_t kMaxCached = 40;      // 128 KB each
static constexpr u64 kStaleMs = 1500;         // skip requests not renewed this long

void Thumbnails::start() {
	quit_ = false;
	for (auto& t : threads_) t = util::startThread(workerEntry, this, 64 * 1024, util::PRIO_WORKER, -2);
}

void Thumbnails::stop() {
	{
		LockGuard g(lock_);
		quit_ = true;
		work_.broadcast();
	}
	for (auto& t : threads_) {
		if (t) {
			threadJoin(t, U64_MAX);
			threadFree(t);
			t = nullptr;
		}
	}
	for (auto& kv : cache_) texDeleteLocked(&kv.second->tex);
	cache_.clear();
}

const C2D_Image* Thumbnails::get(const std::string& id) {
	if (id.empty()) return nullptr;
	auto it = cache_.find(id);
	if (it != cache_.end()) {
		it->second->lastUse = frame_;
		return &it->second->img;
	}
	u64 now = util::nowMs();
	auto f = failedAt_.find(id);
	if (f != failedAt_.end() && now - f->second < 20000) return nullptr;

	LockGuard g(lock_);
	auto w = wantedAt_.find(id);
	if (w == wantedAt_.end()) {
		if (!inflight_.count(id)) {
			wanted_.push_back(id);
			work_.signal();
		}
	} else if (now - w->second > 500) {
		// Re-requested after a while (scrolled back): move to the top of the stack.
		auto pos = std::find(wanted_.begin(), wanted_.end(), id);
		if (pos != wanted_.end()) {
			wanted_.erase(pos);
			wanted_.push_back(id);
		}
	}
	wantedAt_[id] = now;
	return nullptr;
}

void Thumbnails::update() {
	frame_++;
	std::vector<Decoded> done;
	{
		LockGuard g(lock_);
		// Upload a few per frame to keep frame times smooth.
		size_t n = std::min<size_t>(done_.size(), 3);
		for (size_t i = 0; i < n; i++) {
			inflight_.erase(done_[i].id);  // only now, so get() can't re-queue it meanwhile
			done.push_back(std::move(done_[i]));
		}
		done_.erase(done_.begin(), done_.begin() + n);
	}
	for (auto& d : done) {
		if (!d.ok) {
			failedAt_[d.id] = util::nowMs();
			continue;
		}
		if (cache_.count(d.id)) continue;
		auto e = std::make_unique<Entry>();
		if (!texInitLocked(&e->tex, kTexW, kTexH, GPU_RGB565)) {
			util::logf("thumb texture alloc failed");
			continue;
		}
		C3D_TexSetFilter(&e->tex, GPU_LINEAR, GPU_LINEAR);
		C3D_TexSetWrap(&e->tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
		memcpy(e->tex.data, d.pixels.data(), std::min<size_t>(e->tex.size, d.pixels.size() * 2));
		C3D_TexFlush(&e->tex);
		e->sub = makeSubTexture(kW, kH, kTexW, kTexH);
		e->img.tex = &e->tex;
		e->img.subtex = &e->sub;
		e->lastUse = frame_;
		cache_[d.id] = std::move(e);
	}

	// Evict least-recently-drawn textures beyond the cap (never ones drawn recently).
	while (cache_.size() > kMaxCached) {
		auto oldest = cache_.end();
		for (auto it = cache_.begin(); it != cache_.end(); ++it)
			if (oldest == cache_.end() || it->second->lastUse < oldest->second->lastUse) oldest = it;
		if (oldest == cache_.end() || frame_ - oldest->second->lastUse < 2) break;
		deferTexFree(oldest->second->tex);
		cache_.erase(oldest);
	}
}

void Thumbnails::worker() {
	net::Client http;
	TiledLayout layout;
	layout.init(kTexW, kTexH);

	while (true) {
		std::string id;
		{
			LockGuard g(lock_);
			while (!quit_) {
				u64 now = util::nowMs();
				// Most recent request first; drop ones that went stale.
				while (!wanted_.empty()) {
					std::string cand = wanted_.back();
					wanted_.pop_back();
					auto w = wantedAt_.find(cand);
					bool stale = w == wantedAt_.end() || now - w->second > kStaleMs;
					if (stale) {
						wantedAt_.erase(cand);
						continue;
					}
					id = cand;
					break;
				}
				if (!id.empty()) break;
				work_.wait(lock_);
			}
			if (quit_) return;
			inflight_[id] = true;
			wantedAt_.erase(id);
		}

		Decoded d;
		d.id = id;
		net::Response r = http.get("https://i.ytimg.com/vi/" + id + "/mqdefault.jpg");
		if (r.ok()) {
			int w = 0, h = 0, comp = 0;
			u8* rgb = stbi_load_from_memory((const u8*)r.body.data(), (int)r.body.size(), &w, &h, &comp, 3);
			if (rgb) {
				d.pixels.assign(kTexW * kTexH, 0);
				// mqdefault is 320x180 (16:9); crop anything taller (4:3 letterboxing).
				int srcH = std::min(h, w * 9 / 16);
				int y0 = (h - srcH) / 2;
				rgbToTiled565(rgb + y0 * w * 3, w, srcH, kW, kH, d.pixels.data(), layout);
				stbi_image_free(rgb);
				d.ok = true;
			}
		}

		LockGuard g(lock_);
		done_.push_back(std::move(d));
	}
}
