#pragma once

#include <3ds.h>

#include <string>
#include <unordered_map>
#include <vector>

#include "yt/innertube.hpp"

struct Settings {
	int quality = 0;          // 0 = auto, else max video height (144/240/360/480)
	bool fastDecode = true;   // skip H.264 deblocking
	bool autoplay = true;     // play the next related video when one ends
	bool verifyTls = true;
	bool audioOnly = false;
	std::string region = "US";
	float volume = 1.0f;

	int effectiveMaxHeight() const;
};

struct Channel {
	std::string id;
	std::string name;
};

// Everything we persist to the SD card (sdmc:/3ds/Tube3DS/).
class Library {
public:
	Settings settings;
	std::vector<yt::VideoItem> history;  // newest first
	std::vector<yt::VideoItem> saved;    // "Watch later", newest first
	std::vector<Channel> subscriptions;  // alphabetical
	std::vector<std::string> searches;   // newest first

	void load();
	void markDirty() { dirty_ = true; }
	void saveIfDirty(bool force = false);  // throttled to once every few seconds

	void addHistory(const yt::VideoItem& v);
	bool isSaved(const std::string& id) const;
	void toggleSaved(const yt::VideoItem& v);
	bool isSubscribed(const std::string& channelId) const;
	void toggleSubscription(const std::string& id, const std::string& name);
	void addSearch(const std::string& q);

	// Resume positions for partially watched videos.
	double resumePosition(const std::string& id) const;
	void setResumePosition(const std::string& id, double pos, double duration);

	// User-editable overrides for the InnerTube client identity (config.json).
	json::Value config;

private:
	std::unordered_map<std::string, double> resume_;
	std::vector<std::string> resumeOrder_;
	bool dirty_ = false;
	u64 lastSave_ = 0;
	void save();
};

extern Library g_lib;
