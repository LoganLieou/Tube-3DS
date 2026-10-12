// Host-side smoke test for the InnerTube data layer against live YouTube.
//   make -C tests        (or see the Makefile there)
#include <cstdio>
#include <string>

#include "net/http.hpp"
#include "yt/innertube.hpp"

static int failures = 0;
std::string g_lastBody;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("  FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static void printPage(const char* label, const yt::VideoPage& p) {
	printf("%s: %zu items, continuation=%s, error='%s'\n", label, p.items.size(),
	       p.continuation.empty() ? "no" : "yes", p.error.c_str());
	for (size_t i = 0; i < p.items.size() && i < 4; i++) {
		const auto& v = p.items[i];
		printf("  [%s] %s | %s (%s) | %s | %s | %s%s\n", v.id.c_str(), v.title.c_str(), v.channel.c_str(),
		       v.channelId.c_str(), v.views.c_str(), v.published.c_str(), v.duration.c_str(), v.live ? " LIVE" : "");
	}
}

int main(int argc, char** argv) {
	net::globalInit();
	net::setVerifyTls(false);  // romfs CA bundle isn't available on the host
	net::Client http;
	yt::InnerTube it(http);

	yt::VideoPage s = it.search(argc > 1 ? argv[1] : "nintendo 3ds");
	printPage("search", s);
	CHECK(s.items.size() >= 10, "search returned few items");
	CHECK(!s.continuation.empty(), "search has no continuation");
	for (const auto& v : s.items) {
		CHECK(!v.title.empty() && !v.channel.empty(), "item %s missing title/channel", v.id.c_str());
		if (v.title.empty()) {
			FILE* f = fopen("bad_search.json", "w");
			fwrite(g_lastBody.data(), 1, g_lastBody.size(), f);
			fclose(f);
		}
	}
	if (!s.continuation.empty()) {
		yt::VideoPage s2 = it.search("", s.continuation);
		printPage("search page 2", s2);
		CHECK(s2.items.size() >= 5, "search continuation returned few items");
	}

	std::string vid = s.items.empty() ? "jNQXAC9IVRw" : s.items[0].id;
	yt::PlayerData pd = it.player(vid);
	printf("player %s: error='%s' title='%s' len=%d streams=%zu\n", vid.c_str(), pd.error.c_str(), pd.title.c_str(),
	       pd.lengthSec, pd.streams.size());
	int avc = 0, aac = 0;
	for (const auto& st : pd.streams) {
		if (st.codecs.rfind("avc1", 0) == 0) avc++;
		if (st.codecs.rfind("mp4a", 0) == 0) aac++;
		printf("  itag %d %s %s %dx%d size=%llu drc=%d\n", st.itag, st.mime.c_str(), st.codecs.c_str(), st.width,
		       st.height, (unsigned long long)st.size, st.drc);
	}
	CHECK(pd.error.empty(), "player error: %s", pd.error.c_str());
	CHECK(avc > 0 && aac > 0, "player missing avc1/mp4a streams");

	yt::WatchData wd = it.next(vid);
	printf("next: title='%s' channel='%s' (%s) subs='%s' views='%s' date='%s' desc=%zu chars\n", wd.title.c_str(),
	       wd.channel.c_str(), wd.channelId.c_str(), wd.subscribers.c_str(), wd.views.c_str(), wd.date.c_str(),
	       wd.description.size());
	printPage("related", wd.related);
	CHECK(!wd.title.empty() && !wd.channelId.empty(), "next missing title/channel");
	CHECK(wd.related.items.size() >= 5, "few related videos");
	if (!wd.related.continuation.empty()) {
		yt::VideoPage r2 = it.nextMore(wd.related.continuation);
		printPage("related page 2", r2);
		CHECK(r2.items.size() >= 3, "related continuation returned few items");
	}

	yt::ChannelData cd = it.channel(wd.channelId.empty() ? "UCBR8-60-B28hp2BmDPdntcQ" : wd.channelId);
	printf("channel: name='%s' handle='%s' subs='%s' videos='%s' error='%s'\n", cd.name.c_str(), cd.handle.c_str(),
	       cd.subscribers.c_str(), cd.videoCount.c_str(), cd.error.c_str());
	printPage("channel videos", cd.videos);
	CHECK(!cd.name.empty(), "channel has no name");
	CHECK(!cd.videos.items.empty(), "channel has no videos");
	if (!cd.videos.continuation.empty()) {
		yt::VideoPage c2 = it.channelMore(cd.videos.continuation);
		printPage("channel page 2", c2);
		CHECK(!c2.items.empty(), "channel continuation empty");
	}

	printf("\n%s (%d failures)\n", failures ? "FAILED" : "ALL OK", failures);
	return failures ? 1 : 0;
}
