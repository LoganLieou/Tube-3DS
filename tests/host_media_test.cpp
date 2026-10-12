// Host-side test of the streaming path the 3DS player uses: InnerTube player
// response -> BlockStream (HTTP range block cache) -> FFmpeg custom I/O ->
// demux + decode, including a seek into the middle of the video.
//
// It mirrors the glue in source/media/player.cpp (avRead/avSeek, stream
// selection) so the network/demux logic can be exercised without hardware.
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include <cstdio>
#include <string>

#include "media/block_stream.hpp"
#include "net/http.hpp"
#include "yt/innertube.hpp"

std::string g_lastBody;
static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("  FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

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

// Mirrors Player::refreshUrl.
static std::string refreshUrl(const std::string& videoId, const yt::Stream& s) {
	net::Client http;
	yt::InnerTube it(http);
	yt::PlayerData pd = it.player(videoId, true);
	for (const auto& r : pd.streams)
		if (r.itag == s.itag && r.audio == s.audio && r.drc == s.drc && r.size == s.size) return r.url;
	return "";
}

static void testStream(const std::string& videoId, const yt::Stream& st, const std::string& ua, bool video,
                       double duration) {
	printf("\n== %s itag %d %s %dx%d size=%llu\n", video ? "video" : "audio", st.itag, st.codecs.c_str(), st.width,
	       st.height, (unsigned long long)st.size);
	u64 t0 = osGetTime();
	BlockStream bs(st.url, st.size, ua, video ? 256 * 1024 : 128 * 1024, video ? 10 : 8);
	bs.setRefresher([videoId, st] { return refreshUrl(videoId, st); });
	CHECK(bs.start(), "BlockStream start");

	const int ioSize = 32 * 1024;
	AVIOContext* avio = avio_alloc_context((uint8_t*)av_malloc(ioSize), ioSize, 0, &bs, avRead, nullptr, avSeek);
	AVFormatContext* fmt = avformat_alloc_context();
	fmt->pb = avio;
	fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
	const AVInputFormat* ifmt = av_find_input_format(st.mime.find("webm") != std::string::npos ? "matroska" : "mov");
	int r = avformat_open_input(&fmt, nullptr, ifmt, nullptr);
	CHECK(r >= 0, "avformat_open_input failed (%d) %s", r, bs.error().c_str());
	if (r < 0) return;
	printf("  opened in %llu ms\n", (unsigned long long)(osGetTime() - t0));

	int idx = -1;
	for (unsigned i = 0; i < fmt->nb_streams; i++)
		if (fmt->streams[i]->codecpar->codec_type == (video ? AVMEDIA_TYPE_VIDEO : AVMEDIA_TYPE_AUDIO)) idx = (int)i;
	CHECK(idx >= 0, "no track");
	AVStream* avs = fmt->streams[idx];
	double tb = av_q2d(avs->time_base);
	const AVCodec* dec = avcodec_find_decoder(avs->codecpar->codec_id);
	AVCodecContext* cc = avcodec_alloc_context3(dec);
	avcodec_parameters_to_context(cc, avs->codecpar);
	CHECK(avcodec_open2(cc, dec, nullptr) >= 0, "decoder open");
	printf("  codec %s %dx%d rate=%d\n", dec->name, cc->width, cc->height, cc->sample_rate);

	AVPacket* pkt = av_packet_alloc();
	AVFrame* fr = av_frame_alloc();
	auto decodeFrames = [&](int want, double& firstPts, double& lastPts) {
		int got = 0;
		firstPts = lastPts = -1;
		while (got < want) {
			int rr = avcodec_receive_frame(cc, fr);
			if (rr == 0) {
				if (video && fr->format != AV_PIX_FMT_YUV420P && fr->format != AV_PIX_FMT_YUVJ420P) {
					CHECK(false, "decoded pix_fmt %d isn't 4:2:0 (the 3DS converter expects it)", fr->format);
				}
				int64_t ts = fr->best_effort_timestamp != AV_NOPTS_VALUE ? fr->best_effort_timestamp : fr->pts;
				double pts = ts * tb;
				if (firstPts < 0) firstPts = pts;
				lastPts = pts;
				got++;
				av_frame_unref(fr);
				continue;
			}
			if (rr != AVERROR(EAGAIN)) break;
			rr = av_read_frame(fmt, pkt);
			if (rr < 0) {
				avcodec_send_packet(cc, nullptr);
				continue;
			}
			if (pkt->stream_index == idx) avcodec_send_packet(cc, pkt);
			av_packet_unref(pkt);
		}
		return got;
	};

	double a, b;
	u64 t1 = osGetTime();
	int n = decodeFrames(video ? 90 : 200, a, b);
	printf("  decoded %d frames, pts %.3f .. %.3f in %llu ms\n", n, a, b, (unsigned long long)(osGetTime() - t1));
	CHECK(n > 20, "too few frames decoded from start");
	CHECK(a >= -0.1 && a < 1.0, "first pts %.3f not near 0", a);

	// Seek to 60% of the way in, like dragging the seek bar.
	double target = duration * 0.6;
	u64 t2 = osGetTime();
	r = av_seek_frame(fmt, idx, (int64_t)(target / tb), AVSEEK_FLAG_BACKWARD);
	CHECK(r >= 0, "seek failed %d", r);
	avcodec_flush_buffers(cc);
	n = decodeFrames(video ? 30 : 50, a, b);
	printf("  after seek to %.1f: %d frames, pts %.3f .. %.3f in %llu ms (stream pos %llu)\n", target, n, a, b,
	       (unsigned long long)(osGetTime() - t2), (unsigned long long)bs.position());
	CHECK(n > 5, "few frames after seek");
	CHECK(a <= target + 0.5 && a > target - 10, "post-seek pts %.2f not near target %.2f", a, target);

	// And back to the start (block 0 must still be cached/refetched).
	r = av_seek_frame(fmt, idx, 0, AVSEEK_FLAG_BACKWARD);
	avcodec_flush_buffers(cc);
	n = decodeFrames(10, a, b);
	printf("  after seek to 0: %d frames, pts %.3f\n", n, a);
	CHECK(n > 3 && a < 1.0, "seek back to start failed");

	av_frame_free(&fr);
	av_packet_free(&pkt);
	avcodec_free_context(&cc);
	avformat_close_input(&fmt);
	av_freep(&avio->buffer);
	avio_context_free(&avio);
	bs.abort();
	CHECK(!bs.failed(), "stream failed: %s", bs.error().c_str());
}

int main(int argc, char** argv) {
	net::globalInit();
	net::setVerifyTls(false);
	net::Client http;
	yt::InnerTube it(http);
	std::string id = argc > 1 ? argv[1] : "aqz-KE-bpKQ";  // Big Buck Bunny (~10 min)
	yt::PlayerData pd = it.player(id);
	printf("player %s: '%s' %ds, %zu streams, error='%s'\n", id.c_str(), pd.title.c_str(), pd.lengthSec,
	       pd.streams.size(), pd.error.c_str());
	CHECK(pd.error.empty(), "player error");

	// Same picks the 3DS makes: 144p H.264 + AAC-LC (non-DRC).
	const yt::Stream* v = nullptr;
	const yt::Stream* a = nullptr;
	for (const auto& s : pd.streams) {
		if (!s.audio && s.codecs.rfind("avc1", 0) == 0 && s.height <= 144 && (!v || s.height > v->height)) v = &s;
		if (s.audio && s.codecs.rfind("mp4a.40.2", 0) == 0 && !s.drc) a = &s;
	}
	CHECK(v && a, "missing 144p avc1 or AAC stream");
	if (v) testStream(id, *v, pd.userAgent, true, pd.lengthSec);
	if (a) testStream(id, *a, pd.userAgent, false, pd.lengthSec);

	printf("\n%s (%d failures)\n", failures ? "FAILED" : "ALL OK", failures);
	return failures ? 1 : 0;
}
