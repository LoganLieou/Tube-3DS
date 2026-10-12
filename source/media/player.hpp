#pragma once

#include <3ds.h>
#include <citro2d.h>

#include <atomic>
#include <memory>
#include <string>

#include "media/audio_out.hpp"
#include "media/frame_queue.hpp"
#include "util.hpp"
#include "yt/innertube.hpp"

class BlockStream;

// Plays a YouTube video from separate DASH video (H.264) and audio (AAC/Opus)
// streams. Each stream gets its own network buffer, demuxer and decoder thread.
// Audio is the master clock; video frames are shown when their time comes and
// dropped (or skipped at the decoder) when the CPU falls behind.
class Player {
public:
	enum class State { Idle, Loading, Buffering, Playing, Paused, Ended, Error };

	struct Options {
		int maxHeight = 240;    // highest video resolution to pick
		bool fastDecode = true;  // skip the H.264 deblocking filter (big win on Old 3DS)
		bool audioOnly = false;
	};

	Player();
	~Player();

	// Starts playback asynchronously. Returns false if no usable streams exist.
	bool open(const yt::PlayerData& data, const Options& opt, double startAt = 0);
	void close();
	bool isOpen() const { return open_; }

	void setPaused(bool paused);
	bool paused() const { return userPaused_; }
	void seek(double seconds);  // absolute
	void setVolume(float v);

	State state();
	std::string error();
	double position();
	double duration() const { return duration_; }
	double bufferedUntil();
	std::string qualityLabel() const { return quality_; }
	bool hasVideo() const { return videoStream_.itag != 0; }
	u32 framesPresented() const { return frames_.presented(); }
	u32 framesDropped() const { return frames_.dropped() + lateDrops_; }

	// Main thread, once per frame: picks the frame to show. Returns false when
	// there's no frame yet (show a thumbnail/spinner instead).
	bool frame(C2D_Image& img, int& w, int& h);

private:
	struct Source;

	yt::Stream videoStream_, audioStream_;
	std::string userAgent_;
	std::string videoId_;

	// Fresh stream URLs after YouTube refuses the current ones. Both streams
	// usually fail together, so one refreshed player response serves both.
	Mutex refreshLock_;
	yt::PlayerData refreshed_;
	u64 refreshedAt_ = 0;
	std::string refreshUrl(const yt::Stream& s);
	Options opt_;
	double duration_ = 0;
	std::string quality_;
	bool open_ = false;

	Thread videoThread_ = nullptr, audioThread_ = nullptr;
	std::atomic<bool> quit_{false};
	std::atomic<bool> userPaused_{false};
	std::atomic<int> seekSerial_{0};
	std::atomic<int> videoReadySerial_{-1};  // serial of the first frame decoded after a seek
	std::atomic<bool> videoDone_{false};      // video failed or hit EOF
	std::atomic<bool> audioStarted_{false};
	std::atomic<bool> buffering_{false};
	std::atomic<bool> ended_{false};
	std::atomic<float> volume_{1.0f};
	volatile double seekTarget_ = 0;
	volatile double lastClock_ = 0;
	volatile u32 lateDrops_ = 0;  // frames the decoder skipped converting because they were late

	Mutex lock_;  // guards error_ and the live BlockStream pointers
	std::string error_;
	BlockStream* liveStreams_[2] = {nullptr, nullptr};

	// Wall clock used when there's no audio track.
	u64 wallStart_ = 0;
	double wallBase_ = 0;
	bool wallRunning_ = false;

	AudioOut audio_;
	FrameQueue frames_;

	void fail(const std::string& msg);
	bool clock(double& out);
	void videoMain();
	void audioMain();
	static void videoEntry(void* p) { static_cast<Player*>(p)->videoMain(); }
	static void audioEntry(void* p) { static_cast<Player*>(p)->audioMain(); }
	bool openSource(Source& src, const yt::Stream& s, bool video, int slot);
	void closeSource(Source& src, int slot);
};
