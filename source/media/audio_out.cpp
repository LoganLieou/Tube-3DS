#include "media/audio_out.hpp"

#include <cstring>

#include "gfx_util.hpp"
#include "util.hpp"

bool g_audioAvailable = false;

bool AudioOut::init(int sampleRate) {
	rate_ = sampleRate;
	lockLinearHeap();
	mem_ = (s16*)linearAlloc(sizeof(s16) * 2 * kFrames * kBuffers);
	unlockLinearHeap();
	if (!mem_) return false;
	memset(mem_, 0, sizeof(s16) * 2 * kFrames * kBuffers);

	ndspChnReset(ch_);
	ndspChnSetInterp(ch_, NDSP_INTERP_POLYPHASE);
	ndspChnSetRate(ch_, (float)sampleRate);
	ndspChnSetFormat(ch_, NDSP_FORMAT_STEREO_PCM16);
	setVolume(1.0f);

	memset(wb_, 0, sizeof wb_);
	for (int i = 0; i < kBuffers; i++) {
		wb_[i].data_pcm16 = mem_ + i * kFrames * 2;
		wb_[i].status = NDSP_WBUF_FREE;
		pts_[i] = 0;
	}
	next_ = 0;
	endPts_ = -1;
	paused_ = true;
	ndspChnSetPaused(ch_, true);
	ready_ = true;
	return true;
}

void AudioOut::shutdown() {
	if (!ready_) return;
	ndspChnWaveBufClear(ch_);
	ndspChnReset(ch_);
	// Give the DSP a moment to stop referencing our buffers before freeing them.
	util::sleepMs(20);
	lockLinearHeap();
	linearFree(mem_);
	unlockLinearHeap();
	mem_ = nullptr;
	ready_ = false;
}

void AudioOut::setVolume(float v) {
	float mix[12] = {0};
	mix[0] = mix[1] = v;
	ndspChnSetMix(ch_, mix);
}

s16* AudioOut::acquire() {
	if (!ready_) return nullptr;
	ndspWaveBuf& b = wb_[next_];
	if (b.status == NDSP_WBUF_QUEUED || b.status == NDSP_WBUF_PLAYING) return nullptr;
	return (s16*)b.data_pcm16;
}

void AudioOut::submit(int frames, double pts) {
	ndspWaveBuf& b = wb_[next_];
	b.nsamples = (u32)frames;
	b.looping = false;
	pts_[next_] = pts;
	DSP_FlushDataCache(b.data_pcm16, sizeof(s16) * 2 * frames);
	ndspChnWaveBufAdd(ch_, &b);
	endPts_ = pts + (double)frames / rate_;
	next_ = (next_ + 1) % kBuffers;
}

int AudioOut::queued() const {
	int n = 0;
	for (int i = 0; i < kBuffers; i++)
		if (wb_[i].status == NDSP_WBUF_QUEUED || wb_[i].status == NDSP_WBUF_PLAYING) n++;
	return n;
}

void AudioOut::flush() {
	if (!ready_) return;
	ndspChnWaveBufClear(ch_);
	for (int i = 0; i < kBuffers; i++) wb_[i].status = NDSP_WBUF_FREE;
	next_ = 0;
	endPts_ = -1;
	// WaveBufClear can leave the channel stopped; restore our pause state.
	ndspChnSetFormat(ch_, NDSP_FORMAT_STEREO_PCM16);
	ndspChnSetRate(ch_, (float)rate_);
	ndspChnSetPaused(ch_, paused_);
}

void AudioOut::setPaused(bool paused) {
	if (paused == paused_) return;
	paused_ = paused;
	if (ready_) ndspChnSetPaused(ch_, paused);
}

bool AudioOut::clock(double& out) const {
	if (!ready_) return false;
	// Walk buffers oldest-first, starting just after the most recently filled one.
	for (int k = 0; k < kBuffers; k++) {
		int i = (next_ + k) % kBuffers;
		if (wb_[i].status == NDSP_WBUF_PLAYING) {
			out = pts_[i] + (double)ndspChnGetSamplePos(ch_) / rate_;
			return true;
		}
	}
	for (int k = 0; k < kBuffers; k++) {
		int i = (next_ + k) % kBuffers;
		if (wb_[i].status == NDSP_WBUF_QUEUED) {
			out = pts_[i];
			return true;
		}
	}
	if (endPts_ >= 0) {
		out = endPts_;
		return true;
	}
	return false;
}
