#include "ui/pages.hpp"

#include <algorithm>
#include <unordered_set>

#include "library.hpp"
#include "net/http.hpp"
#include "tasks.hpp"
#include "ui/gfx.hpp"
#include "ui/rows.hpp"
#include "util.hpp"

namespace ui {

// ================================================================ ListPage

ListPage::ListPage() {
	list_.x = 0;
	list_.y = 0;
	list_.w = kBotW;
	list_.h = kH - kTabBarH;
}

void ListPage::update(Input& in) {
	list_.update(in);
	const yt::VideoItem* v = list_.selectedVideo();
	if (!v) return;
	if (in.down & KEY_Y) {
		if (!v->channelId.empty()) g_app->openChannel(v->channelId, v->channel);
	}
	if (in.down & KEY_SELECT) {
		yt::VideoItem copy = *v;
		g_lib.toggleSaved(copy);
		toast(g_lib.isSaved(copy.id) ? "Saved to Watch later" : "Removed from Watch later");
	}
}

void ListPage::drawTop() {
	drawStatusBar(title());
	const yt::VideoItem* v = list_.selectedVideo();
	if (v) drawVideoPreview(*v);
	else drawHero(heroTitle_, heroSub_);
}

void ListPage::drawBottom() { list_.draw(); }

const yt::VideoItem* ListPage::firstVideo() {
	for (const auto& r : list_.rows)
		if (const yt::VideoItem* v = r->video()) return v;
	return nullptr;
}

std::string ListPage::keepSelection() {
	const yt::VideoItem* v = list_.selectedVideo();
	std::string id = v ? v->id : "";
	list_.rows.clear();
	return id;
}

void ListPage::restoreSelection(const std::string& id) {
	int firstVideo = -1;
	for (int i = 0; i < (int)list_.rows.size(); i++) {
		const yt::VideoItem* v = list_.rows[i]->video();
		if (!v) continue;
		if (firstVideo < 0) firstVideo = i;
		if (!id.empty() && v->id == id) {
			list_.selected = i;
			return;
		}
	}
	if (list_.selected >= (int)list_.rows.size()) list_.selected = (int)list_.rows.size() - 1;
	if (list_.selected < 0) list_.selected = 0;
	// With no previous video, prefer the first video over a header or button so
	// the top screen has something to preview.
	if (id.empty() && firstVideo >= 0 && !list_.selectedVideo()) list_.selected = firstVideo;
}

void ListPage::addVideos(const std::vector<yt::VideoItem>& items) {
	for (const auto& v : items) {
		yt::VideoItem copy = v;
		list_.emplace<VideoRow>(v, [copy] { g_app->openVideo(copy); });
	}
}

// A key on a list page activates the selected row (open video, press button...).
static void activateOnA(ScrollList& list, Input& in) {
	if ((in.down & KEY_A) && list.showSelection) {
		if (Row* r = list.selectedRow()) r->activate();
	}
}

// ================================================================ Home (subscription feed)

class HomePage : public ListPage {
public:
	HomePage() { heroSub_ = "YouTube for your 3DS"; }
	const char* title() const override { return "Home"; }

	void onShow() override {
		std::string key = subsKey();
		bool stale = util::nowMs() - loadedAt_ > 15 * 60 * 1000;
		if (key != subsKey_ || (stale && pending_ == 0)) refresh();
		else rebuild();
	}

	void update(Input& in) override {
		ListPage::update(in);
		activateOnA(list_, in);
	}

private:
	std::vector<yt::VideoItem> feed_;
	std::string subsKey_ = "-";
	u64 loadedAt_ = 0;
	int pending_ = 0;
	int failed_ = 0;
	int gen_ = 0;

	static std::string subsKey() {
		std::string k;
		for (const auto& c : g_lib.subscriptions) k += c.id + ",";
		return k;
	}

	void refresh() {
		subsKey_ = subsKey();
		loadedAt_ = util::nowMs();
		feed_.clear();
		failed_ = 0;
		int gen = ++gen_;
		const size_t maxChannels = 30;
		pending_ = (int)std::min(g_lib.subscriptions.size(), maxChannels);
		for (size_t i = 0; i < g_lib.subscriptions.size() && i < maxChannels; i++) {
			std::string id = g_lib.subscriptions[i].id;
			std::string name = g_lib.subscriptions[i].name;
			tasks::run([this, gen, id, name, tok = alive_](yt::InnerTube& it) {
				if (!*tok) return;
				yt::ChannelData cd = it.channel(id);
				tasks::post(guard([this, gen, cd = std::move(cd), name]() mutable {
					if (gen != gen_) return;
					pending_--;
					if (!cd.error.empty()) failed_++;
					size_t n = 0;
					for (auto& v : cd.videos.items) {
						if (v.live || n++ >= 8) continue;
						if (v.channel.empty()) v.channel = name;
						feed_.push_back(v);
					}
					std::stable_sort(feed_.begin(), feed_.end(), [](const yt::VideoItem& a, const yt::VideoItem& b) {
						return util::parseRelativeAge(a.published) < util::parseRelativeAge(b.published);
					});
					if (feed_.size() > 150) feed_.resize(150);
					rebuild();
				}));
			});
		}
		rebuild();
	}

	void rebuild() {
		std::string sel = keepSelection();
		if (g_lib.subscriptions.empty()) {
			heroTitle_ = "Welcome to Tube3DS";
			heroSub_ = "Search for videos, then subscribe to channels to build your feed.";
			list_.emplace<MessageRow>("Your feed is empty",
			                          "Subscribe to channels and their newest videos will show up here.", nullptr, 84);
			list_.emplace<ButtonRow>("Search YouTube", [] { g_app->startSearch(); }, true);
			if (!g_lib.history.empty()) {
				list_.emplace<HeaderRow>("Recently watched");
				std::vector<yt::VideoItem> recent(g_lib.history.begin(),
				                                  g_lib.history.begin() + std::min<size_t>(g_lib.history.size(), 12));
				addVideos(recent);
			}
		} else {
			heroTitle_ = "Tube3DS";
			heroSub_ = "Latest videos from your subscriptions";
			list_.emplace<HeaderRow>("From your subscriptions", "Refresh", guard([this] { refresh(); }));
			if (feed_.empty() && pending_ > 0) {
				list_.emplace<LoadingRow>(60);
			} else if (feed_.empty()) {
				list_.emplace<MessageRow>("Couldn't load your feed", "Check your connection, then tap to retry.",
				                          guard([this] { refresh(); }));
			}
			addVideos(feed_);
			if (!feed_.empty() && pending_ > 0) list_.emplace<LoadingRow>(36);
		}
		restoreSelection(sel);
	}
};

// ================================================================ Search

class SearchPage : public ListPage {
public:
	SearchPage() {
		heroTitle_ = "Search";
		heroSub_ = "Tap the search bar or press X";
		list_.onNearEnd = [this] { loadMore(); };
	}
	const char* title() const override { return "Search"; }
	void onShow() override { rebuild(); }
	void onSearchKey() override { openKeyboard(); }
	void runQuery(const std::string& q) override { search(q); }

	void update(Input& in) override {
		ListPage::update(in);
		activateOnA(list_, in);
	}

private:
	std::string query_;
	yt::VideoPage results_;
	bool loading_ = false, loadingMore_ = false;
	std::string error_;
	int gen_ = 0;

	void openKeyboard() {
		std::string q;
		if (keyboard("Search YouTube", query_, q)) {
			q = util::trim(q);
			if (!q.empty()) search(q);
		}
	}

	void search(const std::string& q) {
		query_ = q;
		g_lib.addSearch(q);
		results_ = yt::VideoPage();
		error_.clear();
		loading_ = true;
		loadingMore_ = false;
		int gen = ++gen_;
		list_.scrollToTop();
		rebuild();
		tasks::run([this, q, gen, tok = alive_](yt::InnerTube& it) {
			if (!*tok) return;
			yt::VideoPage p = it.search(q);
			tasks::post(guard([this, gen, p = std::move(p)]() mutable {
				if (gen != gen_) return;
				loading_ = false;
				error_ = p.error;
				results_ = std::move(p);
				rebuild();
				list_.selected = 1;  // first result
			}));
		});
	}

	void loadMore() {
		if (loading_ || loadingMore_ || results_.continuation.empty() || !error_.empty()) return;
		loadingMore_ = true;
		std::string token = results_.continuation;
		int gen = gen_;
		rebuild();
		tasks::run([this, token, gen, tok = alive_](yt::InnerTube& it) {
			if (!*tok) return;
			yt::VideoPage p = it.search("", token);
			tasks::post(guard([this, gen, p = std::move(p)]() mutable {
				if (gen != gen_) return;
				loadingMore_ = false;
				if (!p.error.empty()) {
					toast("Couldn't load more results");
					results_.continuation.clear();
				} else {
					std::unordered_set<std::string> seen;
					for (const auto& v : results_.items) seen.insert(v.id);
					for (auto& v : p.items)
						if (seen.insert(v.id).second) results_.items.push_back(std::move(v));
					results_.continuation = p.continuation;
				}
				rebuild();
			}));
		});
	}

	void rebuild() {
		std::string sel = keepSelection();
		list_.emplace<SearchBarRow>([this] { return query_; }, guard([this] { openKeyboard(); }));
		if (loading_) {
			list_.emplace<LoadingRow>(80);
		} else if (!error_.empty()) {
			std::string q = query_;
			list_.emplace<MessageRow>("Search failed", error_ + " \xC2\xB7 Tap to retry", guard([this, q] { search(q); }));
		} else if (!query_.empty() && results_.items.empty()) {
			list_.emplace<MessageRow>("No results", "Try different keywords.");
		} else if (query_.empty()) {
			if (!g_lib.searches.empty()) {
				list_.emplace<HeaderRow>("Recent searches", "Clear", guard([this] {
					g_lib.searches.clear();
					g_lib.markDirty();
					rebuild();
				}));
				for (const auto& s : g_lib.searches)
					list_.emplace<RecentSearchRow>(s, guard([this, s] { search(s); }));
			} else {
				list_.emplace<MessageRow>("Find something to watch", "Press X anywhere to search.", nullptr, 90);
			}
		} else {
			addVideos(results_.items);
			if (loadingMore_) list_.emplace<LoadingRow>(44);
		}
		restoreSelection(sel);
	}
};

// ================================================================ Subscriptions

class SubsPage : public ListPage {
public:
	SubsPage() {
		heroTitle_ = "Subscriptions";
	}
	const char* title() const override { return "Subscriptions"; }
	void onShow() override { rebuild(); }

	void update(Input& in) override {
		ListPage::update(in);
		activateOnA(list_, in);
	}

private:
	void rebuild() {
		std::string sel = keepSelection();
		size_t n = g_lib.subscriptions.size();
		heroSub_ = n == 0 ? "You haven't subscribed to anyone yet"
		                  : std::to_string(n) + (n == 1 ? " channel" : " channels");
		if (n == 0) {
			list_.emplace<MessageRow>("No subscriptions yet",
			                          "Open a channel (Y on any video) and tap Subscribe.", nullptr, 90);
			list_.emplace<ButtonRow>("Search YouTube", [] { g_app->startSearch(); }, true);
		} else {
			list_.emplace<HeaderRow>("Channels");
			for (const auto& c : g_lib.subscriptions) {
				std::string id = c.id, name = c.name;
				list_.emplace<ChannelRow>(id, name, [id, name] { g_app->openChannel(id, name); });
			}
		}
		restoreSelection(sel);
	}
};

// ================================================================ Library

class LibraryPage : public ListPage {
public:
	LibraryPage() { heroTitle_ = "Library"; }
	const char* title() const override { return "Library"; }
	void onShow() override { rebuild(); }

	void update(Input& in) override {
		ListPage::update(in);
		activateOnA(list_, in);
		if (in.down & KEY_SELECT) rebuildLater();  // Watch later may have changed
	}

private:
	int seg_ = 0;  // 0 = history, 1 = watch later

	void rebuildLater() { tasks::post(guard([this] { rebuild(); })); }

	void rebuild() {
		std::string sel = keepSelection();
		list_.emplace<SegmentRow>(std::vector<std::string>{"History", "Watch later"}, [this] { return seg_; },
		                          [this, tok = alive_](int i) {
			                          if (!*tok) return;
			                          seg_ = i;
			                          list_.scrollToTop();
			                          rebuild();
		                          });
		const auto& items = seg_ == 0 ? g_lib.history : g_lib.saved;
		heroSub_ = seg_ == 0 ? "Videos you've watched" : "Press SELECT on any video to save it";
		if (items.empty()) {
			list_.emplace<MessageRow>(seg_ == 0 ? "No watch history" : "Nothing saved yet",
			                          seg_ == 0 ? "Videos you watch will appear here."
			                                    : "Press SELECT on a video to add it to Watch later.",
			                          nullptr, 90);
		} else {
			addVideos(items);
		}
		restoreSelection(sel);
	}
};

// ================================================================ Settings

static const char* kRegions[] = {"US", "GB", "CA", "AU", "IN", "DE", "FR", "ES", "IT", "JP", "BR", "MX", "NL", "SE"};

class SettingsPage : public ListPage {
public:
	SettingsPage() {
		heroTitle_ = "Settings";
		heroSub_ = util::isNew3DS() ? "Running on a New 3DS" : "Running on an Old 3DS";
	}
	const char* title() const override { return "Settings"; }
	void onShow() override { rebuild(); }

	void update(Input& in) override {
		ListPage::update(in);
		activateOnA(list_, in);
	}

private:
	int confirm_ = -1;  // index of an action awaiting a second tap

	static std::string onOff(bool b) { return b ? "On" : "Off"; }

	void changed() {
		g_lib.markDirty();
		net::setVerifyTls(g_lib.settings.verifyTls);
		yt::configure(g_lib.config, "", g_lib.settings.region);
	}

	void destructive(int id, const char* label, const char* confirmLabel, std::function<void()> fn) {
		list_.emplace<SettingRow>(confirm_ == id ? confirmLabel : label, nullptr, guard([this, id, fn] {
			if (confirm_ == id) {
				fn();
				g_lib.markDirty();
				confirm_ = -1;
				toast("Done");
			} else {
				confirm_ = id;
			}
			rebuild();
		}));
	}

	void rebuild() {
		int sel = list_.selected;
		list_.rows.clear();
		Settings& s = g_lib.settings;

		list_.emplace<HeaderRow>("Playback");
		list_.emplace<SettingRow>(
			"Video quality",
			[&s] {
				if (s.quality == 0) return std::string("Auto (") + std::to_string(s.effectiveMaxHeight()) + "p)";
				return std::to_string(s.quality) + "p";
			},
			guard([this, &s] {
				static const int q[] = {0, 144, 240, 360, 480};
				int i = 0;
				while (i < 5 && q[i] != s.quality) i++;
				s.quality = q[(i + 1) % 5];
				changed();
			}),
			"Higher than 240p needs a New 3DS");
		list_.emplace<SettingRow>("Fast decoding", [&s] { return onOff(s.fastDecode); },
		                          guard([this, &s] {
			                          s.fastDecode = !s.fastDecode;
			                          changed();
		                          }),
		                          "Skips deblocking: smoother, slightly blockier");
		list_.emplace<SettingRow>("Audio only", [&s] { return onOff(s.audioOnly); },
		                          guard([this, &s] {
			                          s.audioOnly = !s.audioOnly;
			                          changed();
		                          }),
		                          "Saves data and battery");
		list_.emplace<SettingRow>("Autoplay next video", [&s] { return onOff(s.autoplay); },
		                          guard([this, &s] {
			                          s.autoplay = !s.autoplay;
			                          changed();
		                          }));

		list_.emplace<HeaderRow>("Content");
		list_.emplace<SettingRow>("Region", [&s] { return s.region; },
		                          guard([this, &s] {
			                          int n = sizeof(kRegions) / sizeof(kRegions[0]);
			                          int i = 0;
			                          while (i < n && s.region != kRegions[i]) i++;
			                          s.region = kRegions[(i + 1) % n];
			                          changed();
		                          }),
		                          "Affects search results and feeds");

		list_.emplace<HeaderRow>("Network");
		list_.emplace<SettingRow>("Verify HTTPS certificates", [&s] { return onOff(s.verifyTls); },
		                          guard([this, &s] {
			                          s.verifyTls = !s.verifyTls;
			                          changed();
		                          }),
		                          "Turn off only if connections keep failing");

		list_.emplace<HeaderRow>("Your data");
		destructive(1, "Clear watch history", "Tap again to clear history", [] { g_lib.history.clear(); });
		destructive(2, "Clear search history", "Tap again to clear searches", [] { g_lib.searches.clear(); });
		destructive(3, "Clear Watch later", "Tap again to clear Watch later", [] { g_lib.saved.clear(); });
		destructive(4, "Remove all subscriptions", "Tap again to unsubscribe from all",
		            [] { g_lib.subscriptions.clear(); });

		list_.emplace<HeaderRow>("About");
		const yt::ClientConfig& pc = yt::playerClient();
		list_.emplace<SettingRow>("Tube3DS 1.0", nullptr, nullptr, "An unofficial YouTube client for 3DS");
		list_.emplace<SettingRow>("Player client", nullptr, nullptr, pc.name + " " + pc.version);
		list_.emplace<SettingRow>("Config & logs", nullptr, nullptr, util::dataDir());

		list_.selected = sel;
		restoreSelection("");
	}
};

// ================================================================ Channel

class ChannelHeaderRow : public Row {
public:
	ChannelHeaderRow(std::function<const yt::ChannelData&()> data, Action onSubscribe)
		: data_(std::move(data)), onSubscribe_(std::move(onSubscribe)) {}
	float height() const override { return 84; }

	void draw(float x, float y, float w, bool sel) override {
		const yt::ChannelData& cd = data_();
		avatar(cd.name, x + 38, y + 34, 26);
		textFit(cd.name, x + 74, y + 12, 0.6f, col::text, w - 84);
		std::string meta = cd.handle;
		if (!cd.subscribers.empty()) meta += (meta.empty() ? "" : " \xC2\xB7 ") + cd.subscribers;
		textFit(meta, x + 74, y + 32, 0.4f, col::text2, w - 84);
		textFit(cd.videoCount, x + 74, y + 45, 0.4f, col::text2, w - 84);

		bool subbed = g_lib.isSubscribed(cd.id);
		const char* label = subbed ? "Subscribed" : "Subscribe";
		float bw = textWidth(label, 0.45f) + 28;
		float bx = x + w - bw - 12, by = y + 52;
		roundRect(bx, by, bw, 26, 13, subbed ? col::surface2 : col::text);
		if (sel) roundRectOutline(bx - 2, by - 2, bw + 4, 30, 15, 2, col::blue);
		text(label, bx + bw / 2, by + 13 - lineHeight(0.45f) / 2, 0.45f, subbed ? col::text : col::black, Center);
	}
	void activate() override { tasks::post(onSubscribe_); }

private:
	std::function<const yt::ChannelData&()> data_;
	Action onSubscribe_;
};

class ChannelPage : public Page {
public:
	ChannelPage(const std::string& id, const std::string& name) {
		data_.id = id;
		data_.name = name;
		list_.x = 0;
		list_.y = kTopBarH;
		list_.w = kBotW;
		list_.h = kH - kTopBarH;
		list_.onNearEnd = [this] { loadMore(); };
		load();
	}
	const char* title() const override { return "Channel"; }
	bool showTabBar() const override { return false; }

	void update(Input& in) override {
		if (topBarBackTapped(in) || (in.down & KEY_B)) {
			g_app->pop();
			return;
		}
		list_.update(in);
		activateOnA(list_, in);
		const yt::VideoItem* v = list_.selectedVideo();
		if (v && (in.down & KEY_SELECT)) {
			yt::VideoItem copy = *v;
			g_lib.toggleSaved(copy);
			toast(g_lib.isSaved(copy.id) ? "Saved to Watch later" : "Removed from Watch later");
		}
		if (in.down & KEY_X) toggleSub();
	}

	void drawTop() override {
		drawStatusBar(data_.name.empty() ? "Channel" : data_.name);
		const yt::VideoItem* v = list_.selectedVideo();
		if (v) drawVideoPreview(*v);
		else drawHero(data_.name.empty() ? "Channel" : data_.name, data_.subscribers.empty() ? "Loading channel..." : data_.subscribers);
	}

	void drawBottom() override {
		list_.draw();
		drawTopBar(data_.name);
	}

private:
	yt::ChannelData data_;
	ScrollList list_;
	bool loading_ = true, loadingMore_ = false;
	std::string error_;

	void toggleSub() {
		if (data_.id.empty()) return;
		g_lib.toggleSubscription(data_.id, data_.name);
		toast(g_lib.isSubscribed(data_.id) ? "Subscribed to " + data_.name : "Unsubscribed");
	}

	void load() {
		loading_ = true;
		error_.clear();
		rebuild();
		std::string id = data_.id;
		tasks::run([this, id, tok = alive_](yt::InnerTube& it) {
			if (!*tok) return;
			yt::ChannelData cd = it.channel(id);
			tasks::post(guard([this, cd = std::move(cd)]() mutable {
				loading_ = false;
				error_ = cd.error;
				std::string name = data_.name;
				data_ = std::move(cd);
				if (data_.name.empty()) data_.name = name;
				// Keep a subscription's stored name fresh.
				for (auto& c : g_lib.subscriptions)
					if (c.id == data_.id && c.name != data_.name && !data_.name.empty()) {
						c.name = data_.name;
						g_lib.markDirty();
					}
				rebuild();
			}));
		});
	}

	void loadMore() {
		if (loading_ || loadingMore_ || data_.videos.continuation.empty()) return;
		loadingMore_ = true;
		std::string token = data_.videos.continuation;
		rebuild();
		tasks::run([this, token, tok = alive_](yt::InnerTube& it) {
			if (!*tok) return;
			yt::VideoPage p = it.channelMore(token);
			tasks::post(guard([this, p = std::move(p)]() mutable {
				loadingMore_ = false;
				for (auto& v : p.items) {
					if (v.channelId.empty()) v.channelId = data_.id;
					if (v.channel.empty() || v.channel.find(" and ") != std::string::npos) v.channel = data_.name;
					data_.videos.items.push_back(std::move(v));
				}
				data_.videos.continuation = p.error.empty() ? p.continuation : "";
				rebuild();
			}));
		});
	}

	void rebuild() {
		const yt::VideoItem* v = list_.selectedVideo();
		std::string sel = v ? v->id : "";
		int selIdx = list_.selected;
		list_.rows.clear();
		list_.emplace<ChannelHeaderRow>([this]() -> const yt::ChannelData& { return data_; },
		                                guard([this] { toggleSub(); }));
		if (loading_) {
			list_.emplace<LoadingRow>(60);
		} else if (!error_.empty()) {
			list_.emplace<MessageRow>("Couldn't load channel", error_ + " \xC2\xB7 Tap to retry", guard([this] { load(); }));
		} else {
			if (data_.videos.items.empty()) list_.emplace<MessageRow>("No videos", "This channel has no uploads.");
			for (const auto& item : data_.videos.items) {
				yt::VideoItem copy = item;
				list_.emplace<VideoRow>(item, [copy] { g_app->openVideo(copy); });
			}
			if (loadingMore_) list_.emplace<LoadingRow>(44);
		}
		list_.selected = selIdx;
		for (int i = 0; i < (int)list_.rows.size(); i++) {
			const yt::VideoItem* rv = list_.rows[i]->video();
			if (!sel.empty() && rv && rv->id == sel) list_.selected = i;
		}
		if (list_.selected >= (int)list_.rows.size()) list_.selected = 0;
	}
};

// ================================================================ factories

std::shared_ptr<ListPage> makeHomePage() { return std::make_shared<HomePage>(); }
std::shared_ptr<ListPage> makeSearchPage() { return std::make_shared<SearchPage>(); }
std::shared_ptr<ListPage> makeSubsPage() { return std::make_shared<SubsPage>(); }
std::shared_ptr<ListPage> makeLibraryPage() { return std::make_shared<LibraryPage>(); }
std::shared_ptr<ListPage> makeSettingsPage() { return std::make_shared<SettingsPage>(); }
std::shared_ptr<Page> makeChannelPage(const std::string& id, const std::string& name) {
	return std::make_shared<ChannelPage>(id, name);
}

}  // namespace ui
