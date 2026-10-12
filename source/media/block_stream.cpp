#include "media/block_stream.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "net/http.hpp"

BlockStream::BlockStream(std::string url, u64 size, std::string userAgent, size_t blockSize, int prefetchBlocks)
	: url_(std::move(url)), ua_(std::move(userAgent)), size_(size), blockSize_(blockSize), prefetch_(prefetchBlocks) {
	numBlocks_ = (size_ + blockSize_ - 1) / blockSize_;
}

BlockStream::~BlockStream() {
	abort();
	if (thread_) {
		threadJoin(thread_, U64_MAX);
		threadFree(thread_);
	}
}

bool BlockStream::start() {
	thread_ = util::startThread(threadMain, this, 32 * 1024, util::PRIO_NET, -2);
	return thread_ != nullptr;
}

void BlockStream::abort() {
	LockGuard g(lock_);
	quit_ = true;
	dataReady_.broadcast();
	wantData_.broadcast();
}

std::string BlockStream::error() {
	LockGuard g(lock_);
	return error_;
}

BlockStream::Block* BlockStream::findBlock(u64 index) {
	for (auto& b : blocks_)
		if (b->index == index) return b.get();
	return nullptr;
}

bool BlockStream::needed(u64 index) const {
	u64 cur = pos_ / blockSize_;
	return index >= cur && index < cur + (u64)prefetch_ && index < numBlocks_;
}

void BlockStream::evict() {
	u64 cur = pos_ / blockSize_;
	// Keep one block behind the cursor (demuxers often step back slightly).
	for (size_t i = 0; i < blocks_.size();) {
		u64 idx = blocks_[i]->index;
		bool keep = (idx + 1 >= cur) && idx < cur + (u64)prefetch_ + 1;
		// Always keep block 0: it holds the container header (moov/EBML), which
		// demuxers revisit when seeking.
		if (idx == 0) keep = true;
		if (!keep) blocks_.erase(blocks_.begin() + i);
		else i++;
	}
}

u64 BlockStream::bufferedAhead() {
	LockGuard g(lock_);
	u64 p = pos_;
	u64 total = 0;
	while (p < size_) {
		Block* b = findBlock(p / blockSize_);
		if (!b) break;
		u64 off = p - b->index * blockSize_;
		u64 n = b->data.size() - off;
		total += n;
		p += n;
	}
	return total;
}

int BlockStream::read(u8* dst, int len) {
	LockGuard g(lock_);
	int done = 0;
	while (done < len) {
		if (quit_ || failed_) return done > 0 ? done : -1;
		if (pos_ >= size_) break;
		u64 idx = pos_ / blockSize_;
		Block* b = findBlock(idx);
		if (!b) {
			if (done > 0) break;  // return what we have; don't stall the demuxer
			wantData_.signal();
			dataReady_.wait(lock_);
			continue;
		}
		u64 off = pos_ - idx * blockSize_;
		if (off >= b->data.size()) {
			// Short block (only possible for the final block).
			break;
		}
		size_t n = (size_t)std::min<u64>((u64)(len - done), b->data.size() - off);
		memcpy(dst + done, b->data.data() + off, n);
		done += (int)n;
		u64 oldBlock = pos_ / blockSize_;
		pos_ += n;
		if (pos_ / blockSize_ != oldBlock) wantData_.signal();
	}
	return done;
}

s64 BlockStream::seek(s64 offset, int whence) {
	LockGuard g(lock_);
	s64 target;
	switch (whence) {
	case SEEK_SET: target = offset; break;
	case SEEK_CUR: target = (s64)pos_ + offset; break;
	case SEEK_END: target = (s64)size_ + offset; break;
	default: return -1;
	}
	if (target < 0) return -1;
	if ((u64)target > size_) target = (s64)size_;
	u64 oldBlock = pos_ / blockSize_;
	pos_ = (u64)target;
	if (pos_ / blockSize_ != oldBlock) wantData_.signal();
	return target;
}

u64 BlockStream::probeSize(const std::string& url, const std::string& userAgent) {
	net::Client http;
	std::vector<u8> data;
	std::string err;
	u64 total = 0;
	http.getRange(url, 0, 0, data, userAgent, nullptr, err, &total);
	return total;
}

void BlockStream::threadMain(void* arg) { static_cast<BlockStream*>(arg)->run(); }

void BlockStream::run() {
	net::Client http;
	int consecutiveErrors = 0;

	while (true) {
		u64 target = 0;
		{
			LockGuard g(lock_);
			while (!quit_) {
				evict();
				u64 cur = pos_ / blockSize_;
				bool found = false;
				for (u64 i = cur; i < cur + (u64)prefetch_ && i < numBlocks_; i++) {
					if (!findBlock(i)) {
						target = i;
						found = true;
						break;
					}
				}
				if (found) break;
				wantData_.wait(lock_);
			}
			if (quit_) return;
			fetching_ = (s64)target;
		}

		u64 start = target * blockSize_;
		u64 end = std::min<u64>(start + blockSize_, size_) - 1;
		std::vector<u8> data;
		std::string err;
		// Abort the transfer if a seek made this block irrelevant.
		auto cancel = [this, target]() {
			if (quit_) return true;
			LockGuard g(lock_);
			return !needed(target);
		};
		const std::string& from = fetchUrl_.empty() ? url_ : fetchUrl_;
		std::string effective;
		long status = http.getRange(from, start, end, data, ua_, cancel, err, nullptr, &effective);
		if ((status == 206 || status == 200) && !effective.empty()) {
			fetchUrl_ = effective;
		} else if (!fetchUrl_.empty() && err != "cancelled") {
			// The redirect target may have expired; go back through the original URL.
			fetchUrl_.clear();
			status = http.getRange(url_, start, end, data, ua_, cancel, err, nullptr, &effective);
			if ((status == 206 || status == 200) && !effective.empty()) fetchUrl_ = effective;
		}

		LockGuard g(lock_);
		fetching_ = -1;
		if (quit_) return;
		if ((status == 206 || status == 200) && data.size() == end - start + 1) {
			consecutiveErrors = 0;
			if (!findBlock(target)) {
				auto b = std::make_unique<Block>();
				b->index = target;
				b->data = std::move(data);
				blocks_.push_back(std::move(b));
			}
			dataReady_.broadcast();
			continue;
		}
		if (err == "cancelled") continue;

		util::logf("block %llu fetch failed (%ld): %s", (unsigned long long)target, status, err.c_str());
		// YouTube sometimes starts refusing a session's URLs after the first
		// request (and URLs expire after a few hours). Get a fresh URL and retry.
		if ((status == 403 || status == 410) && refresher_ && refreshes_ < 4) {
			refreshes_++;
			lock_.unlock();
			std::string fresh = refresher_();
			lock_.lock();
			if (quit_) return;
			if (!fresh.empty()) {
				util::logf("stream URL refreshed (attempt %d)", refreshes_);
				url_ = fresh;
				fetchUrl_.clear();
				continue;
			}
		}
		consecutiveErrors++;
		// 403/404/410 mean the URL expired or was rejected; retrying won't help.
		if (status == 403 || status == 404 || status == 410 || consecutiveErrors >= 6) {
			error_ = status == 403 ? "YouTube refused the stream (HTTP 403)" : ("Download failed: " + err);
			failed_ = true;
			dataReady_.broadcast();
			return;
		}
		lock_.unlock();
		util::sleepMs(500 * consecutiveErrors);
		lock_.lock();
	}
}
