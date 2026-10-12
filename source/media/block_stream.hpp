#pragma once

#include <3ds.h>

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "util.hpp"

// A seekable byte stream over HTTP range requests.
//
// The resource is split into fixed-size blocks. A background thread downloads
// the block under the read cursor first, then prefetches the following blocks.
// Blocks far behind or ahead of the cursor are evicted, so memory stays bounded
// no matter how long the video is. Seeking anywhere is cheap.
class BlockStream {
public:
	BlockStream(std::string url, u64 size, std::string userAgent, size_t blockSize, int prefetchBlocks);
	~BlockStream();

	bool start();

	// Called on the download thread when the server refuses the URL (403/410).
	// Returns a replacement URL for the same stream, or "" to give up.
	using UrlRefresher = std::function<std::string()>;
	void setRefresher(UrlRefresher r) { refresher_ = std::move(r); }

	// Asks the server for the resource size with a 1-byte range request.
	static u64 probeSize(const std::string& url, const std::string& userAgent);

	// Blocking read at the current position. Returns bytes read, 0 at EOF,
	// -1 on fatal error or after abort().
	int read(u8* dst, int len);
	// whence: SEEK_SET / SEEK_CUR / SEEK_END. Returns the new position or -1.
	s64 seek(s64 offset, int whence);

	u64 size() const { return size_; }
	u64 position() const { return pos_; }
	// Bytes available contiguously from the read position without waiting.
	u64 bufferedAhead();

	void abort();
	bool failed() const { return failed_; }
	std::string error();

private:
	struct Block {
		u64 index;
		std::vector<u8> data;
	};

	std::string url_;
	std::string fetchUrl_;  // url_ after redirects; googlevideo bounces every request otherwise
	std::string ua_;
	UrlRefresher refresher_;
	int refreshes_ = 0;
	u64 size_;
	size_t blockSize_;
	int prefetch_;
	u64 numBlocks_;

	Mutex lock_;
	Cond dataReady_;
	Cond wantData_;
	std::vector<std::unique_ptr<Block>> blocks_;
	u64 pos_ = 0;
	s64 fetching_ = -1;
	std::atomic<bool> quit_{false};
	std::atomic<bool> failed_{false};
	std::string error_;
	Thread thread_ = nullptr;

	Block* findBlock(u64 index);
	bool needed(u64 index) const;  // must hold lock
	void evict();
	void run();
	static void threadMain(void* arg);
};
