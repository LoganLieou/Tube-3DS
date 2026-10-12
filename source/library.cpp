#include "library.hpp"

#include <algorithm>
#include <cctype>

#include "util.hpp"

Library g_lib;

static constexpr size_t kMaxHistory = 200;
static constexpr size_t kMaxSaved = 500;
static constexpr size_t kMaxSearches = 15;
static constexpr size_t kMaxResume = 100;

int Settings::effectiveMaxHeight() const {
	if (quality > 0) return quality;
	// 144p decodes comfortably on Old 3DS; New 3DS has headroom for 240p (the
	// top screen is 400x240, so higher mostly costs CPU).
	return util::isNew3DS() ? 240 : 144;
}

static std::string path(const char* name) { return util::dataDir() + "/" + name; }

static const char* kConfigTemplate = R"({
  "_readme": "Overrides for the YouTube client identity. Only needed if playback or search stops working after a YouTube change. Remove the leading underscore from a block to enable it.",
  "_player_client": {
    "clientName": "VISIONOS",
    "clientVersion": "1.02",
    "clientNameId": 101,
    "userAgent": "Mozilla/5.0 (Macintosh; Intel Mac OS X 15_7_3) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/26.0 Safari/605.1.15",
    "deviceMake": "Apple",
    "deviceModel": "RealityDevice17,1",
    "osName": "visionOS",
    "osVersion": "26.5.23O471"
  },
  "_web_client": {
    "clientName": "WEB",
    "clientVersion": "2.20260708.00.00",
    "clientNameId": 1
  }
}
)";

void Library::load() {
	util::mkdirs(util::dataDir());
	std::string text;
	json::Value v;

	if (util::readFile(path("settings.json"), text) && json::parse(text, v)) {
		settings.quality = (int)v["quality"].integer(0);
		settings.fastDecode = v["fastDecode"].boolean(true);
		settings.autoplay = v["autoplay"].boolean(true);
		settings.verifyTls = v["verifyTls"].boolean(true);
		settings.audioOnly = v["audioOnly"].boolean(false);
		if (v["region"].isString() && v["region"].str().size() == 2) settings.region = v["region"].str();
		settings.volume = (float)v["volume"].num(1.0);
	}

	if (util::readFile(path("library.json"), text) && json::parse(text, v)) {
		for (const auto& e : v["history"].items()) history.push_back(yt::VideoItem::fromJson(e));
		for (const auto& e : v["saved"].items()) saved.push_back(yt::VideoItem::fromJson(e));
		for (const auto& e : v["subscriptions"].items()) {
			Channel c{e["id"].str(), e["name"].str()};
			if (!c.id.empty()) subscriptions.push_back(c);
		}
		for (const auto& e : v["searches"].items())
			if (e.isString()) searches.push_back(e.str());
		for (const auto& e : v["resume"].items()) {
			std::string id = e["id"].str();
			if (id.empty()) continue;
			resume_[id] = e["t"].num();
			resumeOrder_.push_back(id);
		}
	}

	if (util::readFile(path("config.json"), text)) {
		if (!json::parse(text, config)) util::logf("config.json is not valid JSON; ignoring");
	} else {
		util::writeFile(path("config.json"), kConfigTemplate);
	}
}

void Library::save() {
	json::Value s = json::Value::object();
	s.set("quality", json::Value((double)settings.quality));
	s.set("fastDecode", json::Value(settings.fastDecode));
	s.set("autoplay", json::Value(settings.autoplay));
	s.set("verifyTls", json::Value(settings.verifyTls));
	s.set("audioOnly", json::Value(settings.audioOnly));
	s.set("region", json::Value(settings.region));
	s.set("volume", json::Value((double)settings.volume));
	util::writeFile(path("settings.json"), s.dump());

	json::Value l = json::Value::object();
	json::Value& h = l.set("history", json::Value::array());
	for (const auto& v : history) h.push(v.toJson());
	json::Value& sv = l.set("saved", json::Value::array());
	for (const auto& v : saved) sv.push(v.toJson());
	json::Value& subs = l.set("subscriptions", json::Value::array());
	for (const auto& c : subscriptions) {
		json::Value o = json::Value::object();
		o.set("id", json::Value(c.id));
		o.set("name", json::Value(c.name));
		subs.push(std::move(o));
	}
	json::Value& q = l.set("searches", json::Value::array());
	for (const auto& s2 : searches) q.push(json::Value(s2));
	json::Value& r = l.set("resume", json::Value::array());
	for (const auto& id : resumeOrder_) {
		auto it = resume_.find(id);
		if (it == resume_.end()) continue;
		json::Value o = json::Value::object();
		o.set("id", json::Value(id));
		o.set("t", json::Value(it->second));
		r.push(std::move(o));
	}
	if (!util::writeFile(path("library.json"), l.dump())) util::logf("failed to save library.json");
	lastSave_ = util::nowMs();
	dirty_ = false;
}

void Library::saveIfDirty(bool force) {
	if (!dirty_) return;
	if (force || util::nowMs() - lastSave_ > 3000) save();
}

template <typename T, typename Pred>
static void eraseIf(std::vector<T>& v, Pred p) {
	v.erase(std::remove_if(v.begin(), v.end(), p), v.end());
}

void Library::addHistory(const yt::VideoItem& v) {
	if (v.id.empty()) return;
	eraseIf(history, [&](const yt::VideoItem& o) { return o.id == v.id; });
	history.insert(history.begin(), v);
	if (history.size() > kMaxHistory) history.resize(kMaxHistory);
	markDirty();
}

bool Library::isSaved(const std::string& id) const {
	for (const auto& v : saved)
		if (v.id == id) return true;
	return false;
}

void Library::toggleSaved(const yt::VideoItem& v) {
	if (isSaved(v.id)) {
		eraseIf(saved, [&](const yt::VideoItem& o) { return o.id == v.id; });
	} else {
		saved.insert(saved.begin(), v);
		if (saved.size() > kMaxSaved) saved.resize(kMaxSaved);
	}
	markDirty();
}

bool Library::isSubscribed(const std::string& channelId) const {
	for (const auto& c : subscriptions)
		if (c.id == channelId) return true;
	return false;
}

void Library::toggleSubscription(const std::string& id, const std::string& name) {
	if (id.empty()) return;
	if (isSubscribed(id)) {
		eraseIf(subscriptions, [&](const Channel& c) { return c.id == id; });
	} else {
		subscriptions.push_back({id, name});
		std::sort(subscriptions.begin(), subscriptions.end(), [](const Channel& a, const Channel& b) {
			return util::toLower(a.name) < util::toLower(b.name);
		});
	}
	markDirty();
}

void Library::addSearch(const std::string& q) {
	std::string t = util::trim(q);
	if (t.empty()) return;
	std::string lower = util::toLower(t);
	eraseIf(searches, [&](const std::string& s) { return util::toLower(s) == lower; });
	searches.insert(searches.begin(), t);
	if (searches.size() > kMaxSearches) searches.resize(kMaxSearches);
	markDirty();
}

double Library::resumePosition(const std::string& id) const {
	auto it = resume_.find(id);
	return it == resume_.end() ? 0 : it->second;
}

void Library::setResumePosition(const std::string& id, double pos, double duration) {
	if (id.empty()) return;
	// Forget the position near the start/end of a video (it's "finished").
	bool keep = pos > 15 && (duration <= 0 || pos < duration - 20);
	auto it = resume_.find(id);
	if (!keep) {
		if (it != resume_.end()) {
			resume_.erase(it);
			eraseIf(resumeOrder_, [&](const std::string& s) { return s == id; });
			markDirty();
		}
		return;
	}
	if (it == resume_.end()) {
		resumeOrder_.push_back(id);
		if (resumeOrder_.size() > kMaxResume) {
			resume_.erase(resumeOrder_.front());
			resumeOrder_.erase(resumeOrder_.begin());
		}
	}
	resume_[id] = pos;
	markDirty();
}
