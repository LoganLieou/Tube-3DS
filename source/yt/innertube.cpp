#include "yt/innertube.hpp"

#include <cstring>
#include <unordered_set>

#include "util.hpp"

#ifdef TUBE_HOST_TEST
extern std::string g_lastBody;  // lets the host test dump raw responses
#endif

namespace yt {

using json::Value;

// ---------------------------------------------------------------- client config

static ClientConfig g_web = {
	"WEB", "2.20260708.00.00", 1,
	"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/138.0.0.0 Safari/537.36",
	"", "", "Windows", "10.0",
};

// The visionOS client returns plain (unciphered) googlevideo URLs without a PO
// token, including H.264 "avc1" formats, which is exactly what the 3DS needs.
static ClientConfig g_player = {
	"VISIONOS", "1.02", 101,
	"Mozilla/5.0 (Macintosh; Intel Mac OS X 15_7_3) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/26.0 Safari/605.1.15",
	"Apple", "RealityDevice17,1", "visionOS", "26.5.23O471",
};

static std::string g_hl = "en";
static std::string g_gl = "US";
// configure() runs on the main thread while workers build requests.
static Mutex g_cfgLock;

static void applyOverride(ClientConfig& c, const Value& v) {
	if (!v.isObject()) return;
	if (v["clientName"].isString()) c.name = v["clientName"].str();
	if (v["clientVersion"].isString()) c.version = v["clientVersion"].str();
	if (v["clientNameId"].isNumber()) c.nameId = (int)v["clientNameId"].integer();
	if (v["userAgent"].isString()) c.userAgent = v["userAgent"].str();
	if (v["deviceMake"].isString()) c.deviceMake = v["deviceMake"].str();
	if (v["deviceModel"].isString()) c.deviceModel = v["deviceModel"].str();
	if (v["osName"].isString()) c.osName = v["osName"].str();
	if (v["osVersion"].isString()) c.osVersion = v["osVersion"].str();
}

void configure(const Value& config, const std::string& hl, const std::string& gl) {
	LockGuard g(g_cfgLock);
	applyOverride(g_web, config["web_client"]);
	applyOverride(g_player, config["player_client"]);
	if (!hl.empty()) g_hl = hl;
	if (!gl.empty()) g_gl = gl;
}

const ClientConfig& webClient() { return g_web; }
const ClientConfig& playerClient() { return g_player; }

// ---------------------------------------------------------------- serialization

Value VideoItem::toJson() const {
	Value v = Value::object();
	v.set("id", Value(id));
	v.set("title", Value(title));
	v.set("channel", Value(channel));
	v.set("channelId", Value(channelId));
	v.set("views", Value(views));
	v.set("published", Value(published));
	v.set("duration", Value(duration));
	if (live) v.set("live", Value(true));
	return v;
}

VideoItem VideoItem::fromJson(const Value& v) {
	VideoItem it;
	it.id = v["id"].str();
	it.title = v["title"].str();
	it.channel = v["channel"].str();
	it.channelId = v["channelId"].str();
	it.views = v["views"].str();
	it.published = v["published"].str();
	it.duration = v["duration"].str();
	it.live = v["live"].boolean();
	return it;
}

// ---------------------------------------------------------------- parsing helpers

static const Value* findKey(const Value& v, const char* key, int depth = 0) {
	if (depth > 40) return nullptr;
	if (v.isObject()) {
		for (const auto& kv : v.members()) {
			if (kv.first == key) return &kv.second;
			if (const Value* r = findKey(kv.second, key, depth + 1)) return r;
		}
	} else if (v.isArray()) {
		for (const auto& e : v.items())
			if (const Value* r = findKey(e, key, depth + 1)) return r;
	}
	return nullptr;
}

static std::string browseIdIn(const Value& v) {
	const Value* be = findKey(v, "browseEndpoint");
	if (be) {
		const std::string& id = (*be)["browseId"].str();
		if (id.rfind("UC", 0) == 0) return id;
	}
	return "";
}

static bool looksLikeDuration(const std::string& s) {
	if (s.empty()) return false;
	for (char c : s)
		if (!(c == ':' || (c >= '0' && c <= '9'))) return false;
	return s.find(':') != std::string::npos;
}

static bool parseVideoRenderer(const Value& v, VideoItem& it) {
	it.id = v["videoId"].str();
	if (it.id.empty()) return false;
	it.title = json::text(v["title"]);
	if (it.title.empty()) it.title = json::text(v["headline"]);

	const Value* owner = &v["ownerText"];
	if (owner->isNull()) owner = &v["longBylineText"];
	if (owner->isNull()) owner = &v["shortBylineText"];
	it.channel = json::text(*owner);
	it.channelId = (*owner)["runs"][0]["navigationEndpoint"]["browseEndpoint"]["browseId"].str();

	it.duration = json::text(v["lengthText"]);
	if (it.duration.empty()) {
		for (const auto& o : v["thumbnailOverlays"].items()) {
			std::string t = json::text(o["thumbnailOverlayTimeStatusRenderer"]["text"]);
			if (!t.empty()) {
				it.duration = t;
				break;
			}
		}
	}
	it.views = json::text(v["shortViewCountText"]);
	if (it.views.empty()) it.views = json::text(v["viewCountText"]);
	it.published = json::text(v["publishedTimeText"]);

	for (const auto& b : v["badges"].items()) {
		const std::string& style = b["metadataBadgeRenderer"]["style"].str();
		if (style.find("LIVE") != std::string::npos) it.live = true;
	}
	if (it.duration == "LIVE" || it.views.find("watching") != std::string::npos) it.live = true;
	if (it.live) it.duration = "LIVE";
	return !it.title.empty();
}

static bool parseLockup(const Value& v, VideoItem& it) {
	const std::string& type = v["contentType"].str();
	if (!type.empty() && type != "LOCKUP_CONTENT_TYPE_VIDEO") return false;
	it.id = v["contentId"].str();
	if (it.id.empty()) return false;

	const Value& meta = v["metadata"]["lockupMetadataViewModel"];
	it.title = json::text(meta["title"]);
	it.channelId = browseIdIn(meta["image"]);
	if (it.channelId.empty()) it.channelId = browseIdIn(meta["metadata"]);

	const Value& rows = meta["metadata"]["contentMetadataViewModel"]["metadataRows"];
	for (size_t r = 0; r < rows.size(); r++) {
		for (const auto& part : rows[r]["metadataParts"].items()) {
			std::string t = json::text(part["text"]);
			const std::string& label = part["accessibilityLabel"].str();
			if (t.empty()) continue;
			if (t.find("view") != std::string::npos || label.find("view") != std::string::npos) {
				it.views = t.find("view") != std::string::npos ? t : t + " views";
			} else if (t.find("watching") != std::string::npos) {
				it.views = t;
				it.live = true;
			} else if (t.find("ago") != std::string::npos || t.rfind("Streamed", 0) == 0) {
				it.published = t;
			} else if (r == 0 && it.channel.empty()) {
				it.channel = t;
			}
		}
	}

	// Duration lives in a thumbnail badge.
	std::vector<const Value*> stack{&v["contentImage"]};
	while (!stack.empty() && it.duration.empty()) {
		const Value* cur = stack.back();
		stack.pop_back();
		if (cur->isObject()) {
			const Value& badge = (*cur)["thumbnailBadgeViewModel"];
			if (badge.isObject()) {
				std::string t = badge["text"].str();
				if (looksLikeDuration(t)) it.duration = t;
				else if (t == "LIVE") it.live = true;
			}
			for (const auto& kv : cur->members()) stack.push_back(&kv.second);
		} else if (cur->isArray()) {
			for (const auto& e : cur->items()) stack.push_back(&e);
		}
	}
	if (it.live) it.duration = "LIVE";
	return !it.title.empty();
}

// Walks a response tree collecting videos in document order plus the first
// "load more" continuation token. Skips subtrees that only contain noise.
static void collect(const Value& v, VideoPage& page, std::unordered_set<std::string>& seen, int depth) {
	if (depth > 60) return;
	if (v.isArray()) {
		for (const auto& e : v.items()) collect(e, page, seen, depth + 1);
		return;
	}
	if (!v.isObject()) return;
	for (const auto& kv : v.members()) {
		const std::string& k = kv.first;
		if (k == "videoRenderer" || k == "compactVideoRenderer" || k == "gridVideoRenderer") {
			VideoItem it;
			if (parseVideoRenderer(kv.second, it) && seen.insert(it.id).second) page.items.push_back(std::move(it));
		} else if (k == "lockupViewModel") {
			VideoItem it;
			if (parseLockup(kv.second, it) && seen.insert(it.id).second) page.items.push_back(std::move(it));
		} else if (k == "continuationItemRenderer") {
			if (page.continuation.empty()) {
				const Value& ep = kv.second["continuationEndpoint"];
				std::string tok = ep["continuationCommand"]["token"].str();
				if (tok.empty()) tok = kv.second["button"]["buttonRenderer"]["command"]["continuationCommand"]["token"].str();
				page.continuation = tok;
			}
		} else if (k == "header" || k == "playerOverlays" || k == "frameworkUpdates" || k == "topbar" ||
		           k == "responseContext" || k == "engagementPanels" || k == "reelShelfRenderer" ||
		           k == "shortsLockupViewModel" || k == "menu" || k == "chipBarViewModel" ||
		           k == "searchPyvRenderer" || k == "adSlotRenderer" || k == "promotedSparklesWebRenderer") {
			continue;
		} else if (kv.second.isObject() || kv.second.isArray()) {
			collect(kv.second, page, seen, depth + 1);
		}
	}
}

static VideoPage collectPage(const Value& root) {
	VideoPage page;
	std::unordered_set<std::string> seen;
	collect(root, page, seen, 0);
	return page;
}

// ---------------------------------------------------------------- requests

static Mutex g_visitorLock;
static std::string g_visitor;

struct Locale {
	std::string hl, gl;
};

static Locale locale() {
	LockGuard g(g_cfgLock);
	return Locale{g_hl, g_gl};
}

static ClientConfig snapshot(const ClientConfig& c) {
	LockGuard g(g_cfgLock);
	return c;
}

static std::string contextJson(const ClientConfig& c, const std::string& visitor) {
	Locale loc = locale();
	std::string s = "\"context\":{\"client\":{";
	s += "\"clientName\":\"" + util::jsonEscape(c.name) + "\"";
	s += ",\"clientVersion\":\"" + util::jsonEscape(c.version) + "\"";
	if (!c.deviceMake.empty()) s += ",\"deviceMake\":\"" + util::jsonEscape(c.deviceMake) + "\"";
	if (!c.deviceModel.empty()) s += ",\"deviceModel\":\"" + util::jsonEscape(c.deviceModel) + "\"";
	if (!c.osName.empty()) s += ",\"osName\":\"" + util::jsonEscape(c.osName) + "\"";
	if (!c.osVersion.empty()) s += ",\"osVersion\":\"" + util::jsonEscape(c.osVersion) + "\"";
	if (!c.userAgent.empty() && c.name != "WEB") s += ",\"userAgent\":\"" + util::jsonEscape(c.userAgent) + "\"";
	s += ",\"hl\":\"" + util::jsonEscape(loc.hl) + "\",\"gl\":\"" + util::jsonEscape(loc.gl) + "\"";
	if (!visitor.empty()) s += ",\"visitorData\":\"" + util::jsonEscape(visitor) + "\"";
	s += "}}";
	return s;
}

std::string InnerTube::visitorData(bool refresh) {
	{
		LockGuard g(g_visitorLock);
		if (!g_visitor.empty() && !refresh) return g_visitor;
	}
	ClientConfig web = snapshot(g_web);
	std::string body = "{" + contextJson(web, "") + "}";
	net::Response r = http_.post("https://www.youtube.com/youtubei/v1/visitor_id?prettyPrint=false", body,
	                             {"Content-Type: application/json", "User-Agent: " + web.userAgent,
	                              "X-YouTube-Client-Name: 1", "X-YouTube-Client-Version: " + web.version});
	Value v;
	std::string vd;
	if (r.ok() && json::parse(r.body, v)) vd = v["responseContext"]["visitorData"].str();
	if (vd.empty()) util::logf("visitor_id failed: %s", r.error.c_str());
	LockGuard g(g_visitorLock);
	if (!vd.empty()) g_visitor = vd;
	return g_visitor;
}

bool InnerTube::call(const char* endpoint, const ClientConfig& shared, const std::string& fields,
                     Value& out, std::string& error) {
	const ClientConfig client = snapshot(shared);
	std::string visitor = visitorData();
	std::string body = "{" + contextJson(client, visitor);
	if (!fields.empty()) body += "," + fields;
	body += "}";

	std::vector<std::string> headers = {
		"Content-Type: application/json",
		"User-Agent: " + client.userAgent,
		"X-YouTube-Client-Name: " + std::to_string(client.nameId),
		"X-YouTube-Client-Version: " + client.version,
		"Origin: https://www.youtube.com",
	};
	if (!visitor.empty()) headers.push_back("X-Goog-Visitor-Id: " + visitor);

	std::string url = std::string("https://www.youtube.com/youtubei/v1/") + endpoint + "?prettyPrint=false";
	net::Response r = http_.post(url, body, headers);
#ifdef TUBE_HOST_TEST
	g_lastBody = r.body;
#endif
	if (!r.ok()) {
		error = r.error.empty() ? "Request failed" : r.error;
		// Our certificate/clock explanations are self-contained; others get a prefix.
		bool explained = error.find("3DS") != std::string::npos || error.find("Wi-Fi") != std::string::npos;
		if (r.status == 0 && !explained) error = "Network error: " + error;
		util::logf("%s failed: %s", endpoint, error.c_str());
		return false;
	}
	if (!json::parse(r.body, out)) {
		error = "Bad response from YouTube";
		return false;
	}
	return true;
}

VideoPage InnerTube::search(const std::string& query, const std::string& continuation) {
	Value root;
	std::string err;
	std::string fields = continuation.empty()
		? "\"query\":\"" + util::jsonEscape(query) + "\",\"params\":\"EgIQAQ==\""  // filter: videos
		: "\"continuation\":\"" + util::jsonEscape(continuation) + "\"";
	if (!call("search", g_web, fields, root, err)) {
		VideoPage p;
		p.error = err;
		return p;
	}
	return collectPage(root);
}

static void splitMime(const std::string& full, std::string& mime, std::string& codecs) {
	size_t semi = full.find(';');
	mime = full.substr(0, semi);
	size_t c = full.find("codecs=\"");
	if (c != std::string::npos) {
		c += 8;
		size_t e = full.find('"', c);
		codecs = full.substr(c, e == std::string::npos ? std::string::npos : e - c);
	}
}

static void parseStreams(const Value& list, std::vector<Stream>& out) {
	for (const auto& f : list.items()) {
		if (!f["url"].isString()) continue;  // ciphered formats are unusable without the JS player
		Stream s;
		s.url = f["url"].str();
		s.itag = (int)f["itag"].integer();
		splitMime(f["mimeType"].str(), s.mime, s.codecs);
		s.audio = s.mime.rfind("audio/", 0) == 0;
		s.width = (int)f["width"].integer();
		s.height = (int)f["height"].integer();
		s.fps = (int)f["fps"].integer();
		s.bitrate = (long)f["bitrate"].integer();
		s.size = (u64)f["contentLength"].integer();
		s.drc = f["isDrc"].boolean() || f["xtags"].str() == "CggKA2RyYxIBMQ";
		const Value& track = f["audioTrack"];
		if (track.isObject() && track.has("audioIsDefault") && !track["audioIsDefault"].boolean())
			continue;  // alternate-language dub
		out.push_back(std::move(s));
	}
}

PlayerData InnerTube::player(const std::string& videoId, bool freshVisitor) {
	PlayerData pd;
	pd.videoId = videoId;
	pd.userAgent = snapshot(g_player).userAgent;
	if (freshVisitor) visitorData(true);
	std::string fields = "\"videoId\":\"" + util::jsonEscape(videoId) +
	                     "\",\"contentCheckOk\":true,\"racyCheckOk\":true";

	for (int attempt = 0; attempt < 2; attempt++) {
		Value root;
		std::string err;
		if (!call("player", g_player, fields, root, err)) {
			pd.error = err;
			return pd;
		}
		const Value& ps = root["playabilityStatus"];
		const std::string& status = ps["status"].str();
		const Value& details = root["videoDetails"];
		pd.title = details["title"].str();
		pd.author = details["author"].str();
		pd.channelId = details["channelId"].str();
		pd.description = details["shortDescription"].str();
		pd.lengthSec = (int)details["lengthSeconds"].integer();
		pd.live = details["isLive"].boolean() || (details["isLiveContent"].boolean() && pd.lengthSec == 0);

		pd.streams.clear();
		parseStreams(root["streamingData"]["adaptiveFormats"], pd.streams);
		parseStreams(root["streamingData"]["formats"], pd.streams);

		if (status == "OK" && !pd.streams.empty()) {
			pd.error.clear();
			return pd;
		}
		std::string reason = ps["reason"].str();
		if (reason.empty()) reason = json::text(ps["errorScreen"]["playerErrorMessageRenderer"]["reason"]);
		if (status == "OK") reason = pd.live ? "Live streams aren't supported yet" : "No playable streams found";
		pd.error = reason.empty() ? ("Video unavailable (" + status + ")") : reason;
		util::logf("player %s: %s / %s", videoId.c_str(), status.c_str(), pd.error.c_str());

		// Bot checks are usually tied to the visitor ID; try once with a fresh one.
		if (status == "LOGIN_REQUIRED" && attempt == 0) {
			visitorData(true);
			continue;
		}
		break;
	}
	return pd;
}

WatchData InnerTube::next(const std::string& videoId) {
	WatchData wd;
	Value root;
	if (!call("next", g_web, "\"videoId\":\"" + util::jsonEscape(videoId) + "\"", root, wd.error)) return wd;

	const Value& results = root["contents"]["twoColumnWatchNextResults"];
	for (const auto& c : results["results"]["results"]["contents"].items()) {
		const Value& pri = c["videoPrimaryInfoRenderer"];
		if (pri.isObject()) {
			wd.title = json::text(pri["title"]);
			const Value& vc = pri["viewCount"]["videoViewCountRenderer"];
			wd.views = json::text(vc["shortViewCount"]);
			if (wd.views.empty()) wd.views = json::text(vc["viewCount"]);
			wd.date = json::text(pri["relativeDateText"]);
			if (wd.date.empty()) wd.date = json::text(pri["dateText"]);
		}
		const Value& sec = c["videoSecondaryInfoRenderer"];
		if (sec.isObject()) {
			const Value& owner = sec["owner"]["videoOwnerRenderer"];
			wd.channel = json::text(owner["title"]);
			wd.channelId = owner["navigationEndpoint"]["browseEndpoint"]["browseId"].str();
			wd.subscribers = json::text(owner["subscriberCountText"]);
			wd.description = sec["attributedDescription"]["content"].str();
			if (wd.description.empty()) wd.description = json::text(sec["description"]);
		}
	}
	wd.related = collectPage(results["secondaryResults"]);
	return wd;
}

VideoPage InnerTube::nextMore(const std::string& token) {
	Value root;
	VideoPage p;
	if (!call("next", g_web, "\"continuation\":\"" + util::jsonEscape(token) + "\"", root, p.error)) return p;
	return collectPage(root["onResponseReceivedEndpoints"]);
}

ChannelData InnerTube::channel(const std::string& channelId) {
	ChannelData cd;
	cd.id = channelId;
	Value root;
	// params selects the "Videos" tab.
	std::string fields = "\"browseId\":\"" + util::jsonEscape(channelId) + "\",\"params\":\"EgZ2aWRlb3PyBgQKAjoA\"";
	if (!call("browse", g_web, fields, root, cd.error)) return cd;

	const Value& md = root["metadata"]["channelMetadataRenderer"];
	cd.name = md["title"].str();
	if (md["externalId"].isString()) cd.id = md["externalId"].str();

	const Value* ph = findKey(root["header"], "pageHeaderViewModel");
	if (ph) {
		if (cd.name.empty()) cd.name = json::text((*ph)["title"]["dynamicTextViewModel"]["text"]);
		const Value& rows = (*ph)["metadata"]["contentMetadataViewModel"]["metadataRows"];
		for (const auto& row : rows.items()) {
			for (const auto& part : row["metadataParts"].items()) {
				std::string t = json::text(part["text"]);
				if (t.rfind("@", 0) == 0) cd.handle = t;
				else if (t.find("subscriber") != std::string::npos) cd.subscribers = t;
				else if (t.find("video") != std::string::npos) cd.videoCount = t;
			}
		}
	}
	cd.videos = collectPage(root["contents"]);
	for (auto& v : cd.videos.items) {
		if (v.channelId.empty()) v.channelId = cd.id;
		// Collab uploads read "YouTube and 3 more"; show the channel's own name.
		if (v.channel.empty() || v.channel.find(" and ") != std::string::npos) v.channel = cd.name;
	}
	if (cd.name.empty() && cd.videos.items.empty()) cd.error = "Channel not found";
	return cd;
}

VideoPage InnerTube::channelMore(const std::string& token) {
	Value root;
	VideoPage p;
	if (!call("browse", g_web, "\"continuation\":\"" + util::jsonEscape(token) + "\"", root, p.error)) return p;
	return collectPage(root["onResponseReceivedActions"]);
}

}  // namespace yt
