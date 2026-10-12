#pragma once

#include <3ds.h>

// False when ndspInit failed (usually: no DSP firmware dump on the SD card).
extern bool g_audioAvailable;

// Streams 16-bit stereo PCM through one NDSP channel using a ring of wave
// buffers. Doubles as the playback master clock: each buffer remembers the
// media timestamp of its first sample.
//
// Only the audio thread touches buffers; clock() may be called from any thread.
class AudioOut {
public:
	static constexpr int kBuffers = 16;
	static constexpr int kFrames = 2048;  // stereo frames per buffer (~46 ms at 44.1 kHz)

	bool init(int sampleRate);
	void shutdown();

	// Returns the next free buffer (kFrames stereo s16 frames) or nullptr if all are queued.
	s16* acquire();
	void submit(int frames, double pts);

	int queued() const;  // buffers waiting or playing
	void flush();        // drop everything queued (used when seeking)
	void setPaused(bool paused);
	bool paused() const { return paused_; }

	// Media time currently audible. Returns false if nothing has played yet.
	bool clock(double& out) const;
	void setVolume(float v);
	int rate() const { return rate_; }

private:
	int ch_ = 0;
	int rate_ = 0;
	bool ready_ = false;
	bool paused_ = true;
	s16* mem_ = nullptr;
	ndspWaveBuf wb_[kBuffers];
	double pts_[kBuffers];
	volatile double endPts_ = -1;  // pts just past the most recently submitted sample
	int next_ = 0;
};
