#pragma once

#include <3ds.h>

#include <string>
#include <vector>

#include "net/http.hpp"
#include "yt/json.hpp"

namespace yt {

struct VideoItem {
	std::string id;
	std::string title;
	std::string channel;
	std::string channelId;
	std::string views;      // "74K views"
	std::string published;  // "3 months ago"
	std::string duration;   // "13:54"
	bool live = false;

	json::Value toJson() const;
	static VideoItem fromJson(const json::Value& v);
};

struct VideoPage {
	std::vector<VideoItem> items;
	std::string continuation;
	std::string error;
};

struct Stream {
	std::string url;
	std::string mime;    // "video/mp4"
	std::string codecs;  // "avc1.4D400C"
	int itag = 0;
	int width = 0, height = 0, fps = 0;
	long bitrate = 0;
	u64 size = 0;
	bool audio = false;
	bool drc = false;
};

struct PlayerData {
	std::string videoId;
	std::string error;
	std::string title, author, channelId, description;
	int lengthSec = 0;
	bool live = false;
	std::vector<Stream> streams;
	std::string userAgent;  // UA to use when fetching the streams
};

struct WatchData {
	std::string error;
	std::string title, channel, channelId, views, date, subscribers, description;
	VideoPage related;
};

struct ChannelData {
	std::string error;
	std::string id, name, handle, subscribers, videoCount;
	VideoPage videos;
};

// Identity sent to InnerTube. Defaults are compiled in; each can be overridden
// from sdmc:/3ds/Tube3DS/config.json when YouTube changes things.
struct ClientConfig {
	std::string name;        // clientName
	std::string version;     // clientVersion
	int nameId = 1;          // X-YouTube-Client-Name
	std::string userAgent;
	std::string deviceMake, deviceModel, osName, osVersion;
};

void configure(const json::Value& config, const std::string& hl, const std::string& gl);
const ClientConfig& webClient();
const ClientConfig& playerClient();

// Not thread-safe per instance: use one InnerTube (and net::Client) per thread.
class InnerTube {
public:
	explicit InnerTube(net::Client& http) : http_(http) {}

	VideoPage search(const std::string& query, const std::string& continuation = "");
	// freshVisitor: start a new visitor session first (used to recover from
	// streams that YouTube starts refusing mid-playback).
	PlayerData player(const std::string& videoId, bool freshVisitor = false);
	WatchData next(const std::string& videoId);
	VideoPage nextMore(const std::string& token);
	ChannelData channel(const std::string& channelId);
	VideoPage channelMore(const std::string& token);

private:
	net::Client& http_;

	bool call(const char* endpoint, const ClientConfig& client, const std::string& fields,
	          json::Value& out, std::string& error);
	std::string visitorData(bool refresh = false);
};

}  // namespace yt
