#include "media/player.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
}

#include <algorithm>
#include <cmath>
#include <cstring>

#include "media/block_stream.hpp"
#include "net/http.hpp"

struct Player::Source {
	std::unique_ptr<BlockStream> stream;
	AVFormatContext* fmt = nullptr;
	AVIOContext* avio = nullptr;
	AVCodecContext* codec = nullptr;
	int index = -1;
	double tb = 0;  // seconds per stream time-base tick
};

static void avLogCallback(void*, int level, const char* fmt, va_list vl) {
	if (level > AV_LOG_WARNING) return;
	char buf[512];
	vsnprintf(buf, sizeof buf, fmt, vl);
	size_t n = strlen(buf);
	if (n && buf[n - 1] == '\n') buf[n - 1] = 0;
	util::logf("ffmpeg: %s", buf);
}

Player::Player() {
	static bool logInit = false;
	if (!logInit) {
		av_log_set_callback(avLogCallback);
		logInit = true;
	}
}

Player::~Player() { close(); }

// ---------------------------------------------------------------- stream selection

static bool isAvc(const yt::Stream& s) { return !s.audio && s.codecs.rfind("avc1", 0) == 0 && s.mime == "video/mp4"; }

static int audioScore(const yt::Stream& s) {
	if (!s.audio) return -1;
	int score = 0;
	if (s.codecs.rfind("mp4a.40.2", 0) == 0) score = 300;       // AAC-LC: cheapest to decode
	else if (s.codecs.rfind("mp4a", 0) == 0) score = 200;       // HE-AAC
	else if (s.codecs == "opus") score = 100;
	else return -1;
	if (s.drc) score -= 50;
	return score + (int)std::min<long>(s.bitrate / 10000, 40);
}

bool Player::open(const yt::PlayerData& data, const Options& opt, double startAt) {
	close();
	opt_ = opt;
	userAgent_ = data.userAgent;
	videoId_ = data.videoId;
	{
		LockGuard g(refreshLock_);
		refreshed_ = yt::PlayerData();
		refreshedAt_ = 0;
	}
	duration_ = data.lengthSec;
	videoStream_ = yt::Stream();
	audioStream_ = yt::Stream();

	// Highest H.264 rendition at or below the requested height (prefer <=30fps).
	if (!opt.audioOnly) {
		const yt::Stream* best = nullptr;
		const yt::Stream* smallest = nullptr;
		for (const auto& s : data.streams) {
			if (!isAvc(s) || s.height <= 0) continue;
			if (!smallest || s.height < smallest->height) smallest = &s;
			if (s.height > opt.maxHeight) continue;
			if (!best || s.height > best->height || (s.height == best->height && s.fps < best->fps)) best = &s;
		}
		if (!best) best = smallest;
		if (best) videoStream_ = *best;
	}
	int bestScore = -1;
	for (const auto& s : data.streams) {
		if (!g_audioAvailable) break;
		int sc = audioScore(s);
		if (sc > bestScore) {
			bestScore = sc;
			audioStream_ = s;
		}
	}
	if (!videoStream_.itag && !audioStream_.itag) {
		fail("No compatible streams (needs H.264 or AAC/Opus)");
		return false;
	}
	quality_ = videoStream_.itag ? std::to_string(videoStream_.height) + "p" : "Audio";
	util::logf("open: video itag %d (%dx%d) audio itag %d (%s)", videoStream_.itag, videoStream_.width,
	           videoStream_.height, audioStream_.itag, audioStream_.codecs.c_str());

	quit_ = false;
	lateDrops_ = 0;
	userPaused_ = false;
	seekSerial_ = 0;
	videoReadySerial_ = -1;
	videoDone_ = !videoStream_.itag;
	audioStarted_ = false;
	buffering_ = false;
	ended_ = false;
	seekTarget_ = startAt;
	lastClock_ = startAt;
	wallRunning_ = false;
	wallBase_ = startAt;
	{
		LockGuard g(lock_);
		error_.clear();
	}
	open_ = true;

	bool n3ds = util::isNew3DS();
	if (audioStream_.itag) {
		// Audio decode is light; on core 1 it never competes with video decoding.
		audioThread_ = util::startThread(audioEntry, this, 96 * 1024, util::PRIO_AUDIO, 1);
	}
	if (videoStream_.itag) {
		videoThread_ = util::startThread(videoEntry, this, 256 * 1024, util::PRIO_VIDEO, n3ds ? 2 : 0);
	}
	return true;
}

void Player::close() {
	if (!open_) return;
	quit_ = true;
	{
		LockGuard g(lock_);
		for (auto* s : liveStreams_)
			if (s) s->abort();
	}
	for (Thread* t : {&videoThread_, &audioThread_}) {
		if (*t) {
			threadJoin(*t, U64_MAX);
			threadFree(*t);
			*t = nullptr;
		}
	}
	frames_.shutdown();
	open_ = false;
}

void Player::fail(const std::string& msg) {
	LockGuard g(lock_);
	if (error_.empty()) error_ = msg;
	util::logf("player error: %s", msg.c_str());
}

std::string Player::error() {
	LockGuard g(lock_);
	return error_;
}

void Player::setPaused(bool paused) {
	if (!audioStream_.itag) {
		// Freeze/unfreeze the wall clock.
		if (paused && wallRunning_) {
			wallBase_ += (util::nowMs() - wallStart_) / 1000.0;
			wallRunning_ = false;
		}
	}
	userPaused_ = paused;
}

void Player::setVolume(float v) { volume_ = v; }

void Player::seek(double t) {
	if (duration_ > 0) t = std::min(t, std::max(0.0, duration_ - 1.0));
	if (t < 0) t = 0;
	seekTarget_ = t;
	lastClock_ = t;
	ended_ = false;
	wallBase_ = t;
	wallRunning_ = false;
	seekSerial_++;
}

bool Player::clock(double& out) {
	if (audioStream_.itag) {
		if (videoReadySerial_ != seekSerial_ && !videoDone_) {
			out = seekTarget_;  // still seeking
			return true;
		}
		double c;
		if (audioStarted_ && audio_.clock(c)) {
			out = c;
			lastClock_ = c;
			return true;
		}
		out = lastClock_;
		return true;
	}
	// No audio: run a wall clock once the first frame is ready.
	if (!wallRunning_ && !userPaused_ && videoReadySerial_ == seekSerial_) {
		wallStart_ = util::nowMs();
		wallRunning_ = true;
	}
	out = wallBase_ + (wallRunning_ ? (util::nowMs() - wallStart_) / 1000.0 : 0);
	if (duration_ > 0 && out >= duration_) ended_ = true;
	return true;
}

double Player::position() {
	double c;
	clock(c);
	if (duration_ > 0) c = std::min(c, duration_);
	return std::max(0.0, c);
}

double Player::bufferedUntil() {
	LockGuard g(lock_);
	if (!liveStreams_[0] && !liveStreams_[1]) return 0;
	double until = duration_;
	const yt::Stream* streams[2] = {&videoStream_, &audioStream_};
	for (int i = 0; i < 2; i++) {
		BlockStream* s = liveStreams_[i];
		if (!s || !streams[i]->itag) continue;
		double bytesPerSec = streams[i]->size && duration_ > 0 ? streams[i]->size / duration_ : 0;
		if (bytesPerSec <= 0) continue;
		double t = (s->position() + s->bufferedAhead()) / bytesPerSec;
		until = std::min(until, t);
	}
	return until;
}

Player::State Player::state() {
	if (!open_) return State::Idle;
	{
		LockGuard g(lock_);
		if (!error_.empty()) return State::Error;
	}
	if (ended_) return State::Ended;
	bool started = audioStream_.itag ? audioStarted_.load() : videoReadySerial_ >= 0;
	if (!started) return State::Loading;
	if (userPaused_) return State::Paused;
	if (buffering_ || (videoReadySerial_ != seekSerial_ && !videoDone_)) return State::Buffering;
	return State::Playing;
}

bool Player::frame(C2D_Image& img, int& w, int& h) {
	if (!open_ || !frames_.ready()) return false;
	double c;
	clock(c);
	if (!frames_.present(c, seekSerial_, img)) return false;
	w = frames_.width();
	h = frames_.height();
	return true;
}

// ---------------------------------------------------------------- demuxing

static int avRead(void* opaque, uint8_t* buf, int size) {
	int n = static_cast<BlockStream*>(opaque)->read(buf, size);
	if (n < 0) return AVERROR(EIO);
	if (n == 0) return AVERROR_EOF;
	return n;
}

static int64_t avSeek(void* opaque, int64_t offset, int whence) {
	auto* s = static_cast<BlockStream*>(opaque);
	if (whence & AVSEEK_SIZE) return (int64_t)s->size();
	return s->seek(offset, whence & ~AVSEEK_FORCE);
}

std::string Player::refreshUrl(const yt::Stream& s) {
	if (quit_ || videoId_.empty()) return "";
	LockGuard g(refreshLock_);
	if (refreshed_.streams.empty() || util::nowMs() - refreshedAt_ > 5000) {
		net::Client http;
		yt::InnerTube it(http);
		yt::PlayerData pd = it.player(videoId_, true);
		if (!pd.error.empty()) {
			util::logf("stream refresh failed: %s", pd.error.c_str());
			return "";
		}
		refreshed_ = std::move(pd);
		refreshedAt_ = util::nowMs();
	}
	for (const auto& r : refreshed_.streams)
		if (r.itag == s.itag && r.audio == s.audio && r.drc == s.drc && (!s.size || r.size == s.size)) return r.url;
	return "";
}

bool Player::openSource(Source& src, const yt::Stream& s, bool video, int slot) {
	u64 size = s.size;
	if (!size) size = BlockStream::probeSize(s.url, userAgent_);
	if (!size) {
		fail("Couldn't reach the video server");
		return false;
	}
	// Bigger blocks for video; audio is ~16 KB/s so 128 KB holds ~8 s.
	size_t block = video ? 256 * 1024 : 128 * 1024;
	int prefetch = video ? 10 : 8;
	src.stream = std::make_unique<BlockStream>(s.url, size, userAgent_, block, prefetch);
	src.stream->setRefresher([this, s] { return refreshUrl(s); });
	{
		LockGuard g(lock_);
		liveStreams_[slot] = src.stream.get();
	}
	if (quit_ || !src.stream->start()) return false;

	const int ioSize = 32 * 1024;
	u8* ioBuf = (u8*)av_malloc(ioSize);
	src.avio = avio_alloc_context(ioBuf, ioSize, 0, src.stream.get(), avRead, nullptr, avSeek);
	src.fmt = avformat_alloc_context();
	if (!src.avio || !src.fmt) {
		fail("Out of memory");
		return false;
	}
	src.fmt->pb = src.avio;
	src.fmt->flags |= AVFMT_FLAG_CUSTOM_IO;

	const AVInputFormat* ifmt = av_find_input_format(s.mime.find("webm") != std::string::npos ? "matroska" : "mov");
	int r = avformat_open_input(&src.fmt, nullptr, ifmt, nullptr);
	if (r < 0) {
		src.fmt = nullptr;  // freed by avformat_open_input on failure
		if (!quit_) fail(src.stream->failed() ? src.stream->error() : "Couldn't open the media stream");
		return false;
	}

	AVMediaType type = video ? AVMEDIA_TYPE_VIDEO : AVMEDIA_TYPE_AUDIO;
	for (unsigned i = 0; i < src.fmt->nb_streams; i++) {
		if (src.fmt->streams[i]->codecpar->codec_type == type && src.index < 0) src.index = (int)i;
		else src.fmt->streams[i]->discard = AVDISCARD_ALL;
	}
	if (src.index < 0) {
		fail("Stream has no playable track");
		return false;
	}
	AVStream* st = src.fmt->streams[src.index];
	src.tb = av_q2d(st->time_base);

	const AVCodec* dec = avcodec_find_decoder(st->codecpar->codec_id);
	if (!dec) {
		fail(std::string("Unsupported codec: ") + avcodec_get_name(st->codecpar->codec_id));
		return false;
	}
	src.codec = avcodec_alloc_context3(dec);
	avcodec_parameters_to_context(src.codec, st->codecpar);
	src.codec->thread_count = 1;
	if (video && opt_.fastDecode) {
		src.codec->skip_loop_filter = AVDISCARD_ALL;
		src.codec->flags2 |= AV_CODEC_FLAG2_FAST;
	}
	if (avcodec_open2(src.codec, dec, nullptr) < 0) {
		fail("Couldn't start the decoder");
		return false;
	}
	if (duration_ <= 0 && src.fmt->duration > 0) duration_ = src.fmt->duration / (double)AV_TIME_BASE;
	return true;
}

void Player::closeSource(Source& src, int slot) {
	{
		LockGuard g(lock_);
		liveStreams_[slot] = nullptr;
	}
	if (src.codec) avcodec_free_context(&src.codec);
	if (src.fmt) avformat_close_input(&src.fmt);
	if (src.avio) {
		av_freep(&src.avio->buffer);
		avio_context_free(&src.avio);
	}
	src.stream.reset();
}

static void seekSource(AVFormatContext* fmt, int index, double tb, double t) {
	int64_t ts = (int64_t)(t / tb);
	if (av_seek_frame(fmt, index, ts, AVSEEK_FLAG_BACKWARD) < 0) av_seek_frame(fmt, index, ts, AVSEEK_FLAG_ANY);
}

// ---------------------------------------------------------------- video thread

void Player::videoMain() {
	Source src;
	AVPacket* pkt = av_packet_alloc();
	AVFrame* fr = av_frame_alloc();

	if (!openSource(src, videoStream_, true, 0)) {
		videoDone_ = true;
		closeSource(src, 0);
		av_packet_free(&pkt);
		av_frame_free(&fr);
		return;
	}
	int w = src.codec->width, h = src.codec->height;
	int slots = (w * h > 400 * 240) ? 4 : 6;
	if (!frames_.init(w, h, slots)) {
		fail("Not enough memory for this resolution");
		videoDone_ = true;
	}

	int serial = -1;
	double dropUntil = -1;
	bool eof = false;
	int lateStreak = 0;
	int readErrors = 0;

	while (!quit_ && frames_.ready()) {
		int want = seekSerial_;
		if (want != serial) {
			double t = seekTarget_;
			if (serial >= 0 || t > 0.5) {
				seekSource(src.fmt, src.index, src.tb, t);
				avcodec_flush_buffers(src.codec);
			}
			frames_.clearReady();
			serial = want;
			dropUntil = t;
			eof = false;
			lateStreak = 0;
			src.codec->skip_frame = AVDISCARD_DEFAULT;
		}
		if (eof) {
			util::sleepMs(20);
			continue;
		}

		int slot = frames_.acquire(50 * 1000000LL);
		if (slot < 0) continue;

		bool produced = false;
		while (!quit_ && seekSerial_ == serial) {
			int r = avcodec_receive_frame(src.codec, fr);
			if (r == AVERROR(EAGAIN)) {
				r = av_read_frame(src.fmt, pkt);
				if (r < 0) {
					if (src.stream->failed()) {
						fail(src.stream->error());
						quit_ = true;
						break;
					}
					// Tolerate the odd corrupt packet; drain the decoder at real EOF.
					if (r == AVERROR_EOF || ++readErrors > 50) {
						avcodec_send_packet(src.codec, nullptr);
					} else {
						util::logf("video read error %d", r);
					}
					continue;
				}
				readErrors = 0;
				if (pkt->stream_index == src.index) avcodec_send_packet(src.codec, pkt);
				av_packet_unref(pkt);
				continue;
			}
			if (r == AVERROR_EOF) {
				eof = true;
				break;
			}
			if (r < 0) {
				util::logf("video decode error %d", r);
				avcodec_flush_buffers(src.codec);
				continue;
			}

			if (fr->format != AV_PIX_FMT_YUV420P && fr->format != AV_PIX_FMT_YUVJ420P) {
				fail("Unsupported video format");
				quit_ = true;
				break;
			}
			int64_t ts = fr->best_effort_timestamp != AV_NOPTS_VALUE ? fr->best_effort_timestamp : fr->pts;
			double pts = ts != AV_NOPTS_VALUE ? ts * src.tb : 0;

			// After a seek, decode (but don't convert) frames before the target.
			if (pts < dropUntil - 0.02) {
				src.codec->skip_frame = (dropUntil - pts > 0.6) ? AVDISCARD_NONREF : AVDISCARD_DEFAULT;
				av_frame_unref(fr);
				continue;
			}
			if (dropUntil >= 0) {
				dropUntil = -1;
				src.codec->skip_frame = AVDISCARD_DEFAULT;
			} else if (videoReadySerial_ == serial && !userPaused_) {
				// Behind the audio? Drop this frame, and skip non-reference
				// frames at the decoder until we catch up.
				double c = lastClock_;
				double late = c - pts;
				if (late > 0.12) {
					lateStreak++;
					lateDrops_ = lateDrops_ + 1;
					if (late > 0.4) src.codec->skip_frame = AVDISCARD_NONREF;
					// Hopelessly behind: jump ahead to the next keyframe.
					if (late > 3.0 && lateStreak > 30) {
						util::logf("video %.1fs behind, resyncing", late);
						seekSource(src.fmt, src.index, src.tb, c + 1.5);
						avcodec_flush_buffers(src.codec);
						dropUntil = c + 1.0;
						lateStreak = 0;
					}
					av_frame_unref(fr);
					continue;
				}
				lateStreak = 0;
				if (late < 0.05) src.codec->skip_frame = AVDISCARD_DEFAULT;
			}

			frames_.write(slot, fr);
			av_frame_unref(fr);
			frames_.publish(slot, pts, serial);
			produced = true;
			if (videoReadySerial_ != serial) videoReadySerial_ = serial;
			break;
		}
		if (!produced) frames_.release(slot);
	}

	videoDone_ = true;
	closeSource(src, 0);
	av_packet_free(&pkt);
	av_frame_free(&fr);
}

// ---------------------------------------------------------------- audio thread

static inline s16 f2s(float f) {
	int v = (int)lrintf(f * 32767.0f);
	if (v > 32767) v = 32767;
	if (v < -32768) v = -32768;
	return (s16)v;
}

// Converts `count` samples starting at `pos` into interleaved stereo s16.
static void convertSamples(const AVFrame* fr, int pos, int count, s16* out) {
	int ch = fr->ch_layout.nb_channels;
	int c1 = ch > 1 ? 1 : 0;
	switch (fr->format) {
	case AV_SAMPLE_FMT_FLTP: {
		const float* l = (const float*)fr->extended_data[0] + pos;
		const float* r = (const float*)fr->extended_data[c1] + pos;
		for (int i = 0; i < count; i++) {
			out[2 * i] = f2s(l[i]);
			out[2 * i + 1] = f2s(r[i]);
		}
		break;
	}
	case AV_SAMPLE_FMT_FLT: {
		const float* p = (const float*)fr->extended_data[0] + pos * ch;
		for (int i = 0; i < count; i++) {
			out[2 * i] = f2s(p[i * ch]);
			out[2 * i + 1] = f2s(p[i * ch + c1]);
		}
		break;
	}
	case AV_SAMPLE_FMT_S16P: {
		const s16* l = (const s16*)fr->extended_data[0] + pos;
		const s16* r = (const s16*)fr->extended_data[c1] + pos;
		for (int i = 0; i < count; i++) {
			out[2 * i] = l[i];
			out[2 * i + 1] = r[i];
		}
		break;
	}
	case AV_SAMPLE_FMT_S16: {
		const s16* p = (const s16*)fr->extended_data[0] + pos * ch;
		for (int i = 0; i < count; i++) {
			out[2 * i] = p[i * ch];
			out[2 * i + 1] = p[i * ch + c1];
		}
		break;
	}
	default: memset(out, 0, sizeof(s16) * 2 * count); break;
	}
}

void Player::audioMain() {
	Source src;
	AVPacket* pkt = av_packet_alloc();
	AVFrame* fr = av_frame_alloc();
	bool audioReady = false;

	if (openSource(src, audioStream_, false, 1)) {
		audioReady = audio_.init(src.codec->sample_rate > 0 ? src.codec->sample_rate : 44100);
		if (!audioReady) fail("Couldn't start audio");
	}

	int serial = -1;
	bool eof = false;
	bool haveFrame = false;
	int framePos = 0;       // next unread sample in fr
	double framePts = 0;    // pts of fr's first sample
	double skipUntil = -1;  // trim samples before a seek target
	bool gateOpen = false;
	int readErrors = 0;
	float curVolume = -1;
	int rate = audio_.rate();

	while (audioReady && !quit_) {
		int want = seekSerial_;
		if (want != serial) {
			double t = seekTarget_;
			if (serial >= 0 || t > 0.5) {
				seekSource(src.fmt, src.index, src.tb, t);
				avcodec_flush_buffers(src.codec);
			}
			audio_.flush();
			serial = want;
			eof = false;
			haveFrame = false;
			av_frame_unref(fr);
			skipUntil = t;
			gateOpen = false;
			buffering_ = false;
		}
		if (volume_ != curVolume) {
			curVolume = volume_;
			audio_.setVolume(curVolume);
		}

		// Start (or resume after a seek) once video has a frame at the new position
		// and enough audio is queued to ride out network hiccups.
		int queued = audio_.queued();
		if (!gateOpen) {
			bool videoOk = videoDone_ || videoReadySerial_ == serial;
			if (videoOk && (queued >= 6 || eof)) {
				gateOpen = true;
				audioStarted_ = true;
			}
		} else if (!eof && queued == 0 && !buffering_) {
			buffering_ = true;  // underrun: pause until we have a cushion again
		} else if (buffering_ && (queued >= 10 || eof)) {
			buffering_ = false;
		}
		audio_.setPaused(userPaused_ || !gateOpen || buffering_);

		if (eof && queued == 0 && gateOpen) {
			ended_ = true;
			util::sleepMs(20);
			continue;
		}

		s16* buf = eof ? nullptr : audio_.acquire();
		if (!buf) {
			util::sleepMs(5);
			continue;
		}

		int filled = 0;
		double bufPts = -1;
		while (filled < AudioOut::kFrames && !quit_ && seekSerial_ == serial) {
			if (haveFrame && framePos < fr->nb_samples) {
				double pos = framePts + (double)framePos / rate;
				if (skipUntil >= 0 && pos < skipUntil) {
					int skip = (int)((skipUntil - pos) * rate) + 1;
					framePos = std::min(fr->nb_samples, framePos + skip);
					continue;
				}
				skipUntil = -1;
				if (bufPts < 0) bufPts = pos;
				int n = std::min(fr->nb_samples - framePos, AudioOut::kFrames - filled);
				convertSamples(fr, framePos, n, buf + filled * 2);
				framePos += n;
				filled += n;
				continue;
			}
			haveFrame = false;
			int r = avcodec_receive_frame(src.codec, fr);
			if (r == 0) {
				int64_t ts = fr->best_effort_timestamp != AV_NOPTS_VALUE ? fr->best_effort_timestamp : fr->pts;
				framePts = ts != AV_NOPTS_VALUE ? ts * src.tb : (bufPts >= 0 ? bufPts + (double)filled / rate : 0);
				framePos = 0;
				haveFrame = true;
				continue;
			}
			if (r == AVERROR_EOF) {
				eof = true;
				break;
			}
			if (r != AVERROR(EAGAIN)) {
				avcodec_flush_buffers(src.codec);
				continue;
			}
			r = av_read_frame(src.fmt, pkt);
			if (r < 0) {
				if (src.stream->failed()) {
					fail(src.stream->error());
					eof = true;
					break;
				}
				if (r == AVERROR_EOF || ++readErrors > 50) avcodec_send_packet(src.codec, nullptr);
				continue;
			}
			readErrors = 0;
			if (pkt->stream_index == src.index) avcodec_send_packet(src.codec, pkt);
			av_packet_unref(pkt);
		}
		if (seekSerial_ != serial) continue;  // a seek arrived mid-fill; discard
		if (filled > 0) audio_.submit(filled, bufPts >= 0 ? bufPts : lastClock_);
	}

	if (audioReady) audio_.shutdown();
	closeSource(src, 1);
	av_packet_free(&pkt);
	av_frame_free(&fr);
}
