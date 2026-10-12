#include <algorithm>
#include <cmath>

#include "library.hpp"
#include "media/player.hpp"
#include "tasks.hpp"
#include "thumbnails.hpp"
#include "ui/gfx.hpp"
#include "ui/pages.hpp"
#include "ui/rows.hpp"
#include "util.hpp"

namespace ui {

namespace {

constexpr float kPanelH = 88;  // bottom-screen controls panel
constexpr float kSeekX = 50, kSeekW = 220, kSeekY = 44;

// Title + views/date under the controls.
class TitleRow : public Row {
public:
	TitleRow(std::string title, std::string meta) : title_(std::move(title)), meta_(std::move(meta)) {
		lines_ = wrap(title_, 0.52f, kBotW - 24, 3, wrapped_);
	}
	float height() const override { return 14 + lines_ * lineHeight(0.52f) + (meta_.empty() ? 0 : 16); }
	bool selectable() const override { return false; }
	void draw(float x, float y, float, bool) override {
		float lh = lineHeight(0.52f);
		for (int i = 0; i < lines_; i++) text(wrapped_[i], x + 12, y + 8 + i * lh, 0.52f, col::text);
		if (!meta_.empty()) text(meta_, x + 12, y + 10 + lines_ * lh, 0.4f, col::text2);
	}

private:
	std::string title_, meta_;
	std::vector<std::string> wrapped_;
	int lines_ = 1;
};

class ChannelInfoRow : public Row {
public:
	ChannelInfoRow(std::string id, std::string name, std::string subs)
		: id_(std::move(id)), name_(std::move(name)), subs_(std::move(subs)) {}
	float height() const override { return 52; }
	void draw(float x, float y, float w, bool sel) override {
		rowHighlight(x, y, w, height(), sel);
		avatar(name_, x + 28, y + 26, 16);
		bool subbed = g_lib.isSubscribed(id_);
		const char* label = subbed ? "Subscribed" : "Subscribe";
		bw_ = textWidth(label, 0.43f) + 24;
		float bx = x + w - bw_ - 12;
		textFit(name_, x + 52, y + 10, 0.48f, col::text, bx - x - 60);
		textFit(subs_, x + 52, y + 28, 0.38f, col::text2, bx - x - 60);
		roundRect(bx, y + 13, bw_, 26, 13, subbed ? col::surface3 : col::text);
		text(label, bx + bw_ / 2, y + 26 - lineHeight(0.43f) / 2, 0.43f, subbed ? col::text : col::black, Center);
	}
	void tap(float lx, float) override {
		if (lx > kBotW - bw_ - 16) toggle();
		else activate();
	}
	void activate() override {
		std::string id = id_, name = name_;
		tasks::post([id, name] { g_app->openChannel(id, name); });
	}
	void toggle() {
		g_lib.toggleSubscription(id_, name_);
		toast(g_lib.isSubscribed(id_) ? "Subscribed to " + name_ : "Unsubscribed");
	}

private:
	std::string id_, name_, subs_;
	float bw_ = 90;
};

class DescriptionRow : public Row {
public:
	explicit DescriptionRow(const std::string& desc) : desc_(desc) { relayout(); }
	float height() const override { return 16 + lines_.size() * lineHeight(0.4f) + 14; }
	void draw(float x, float y, float w, bool sel) override {
		roundRect(x + 8, y + 4, w - 16, height() - 8, 8, sel ? col::surface3 : col::surface2);
		float lh = lineHeight(0.4f);
		for (size_t i = 0; i < lines_.size(); i++) text(lines_[i], x + 16, y + 10 + i * lh, 0.4f, col::text);
		text(expanded_ ? "Show less" : "...more", x + 16, y + 10 + lines_.size() * lh, 0.38f, col::text2);
	}
	void activate() override {
		expanded_ = !expanded_;
		relayout();
	}

private:
	std::string desc_;
	bool expanded_ = false;
	std::vector<std::string> lines_;
	void relayout() { wrap(desc_, 0.4f, kBotW - 36, expanded_ ? 80 : 3, lines_); }
};

}  // namespace

class WatchPage : public Page {
public:
	explicit WatchPage(const yt::VideoItem& v) {
		list_.x = 0;
		list_.y = kPanelH;
		list_.w = kBotW;
		list_.h = kH - kPanelH;
		list_.showSelection = false;
		list_.onNearEnd = [this] { loadMoreRelated(); };
		load(v);
	}
	~WatchPage() override {
		saveResume();
		player_.close();
	}

	const char* title() const override { return "Watch"; }
	bool showTabBar() const override { return false; }
	bool isWatch() const override { return true; }
	void onSuspend() override { player_.setPaused(true); }
	void onHide() override { player_.setPaused(true); }

	void update(Input& in) override;
	void drawTop() override;
	void drawBottom() override;

private:
	yt::VideoItem item_;
	Player player_;
	yt::PlayerData pd_;
	bool havePd_ = false;
	std::string loadError_;
	yt::WatchData wd_;
	bool haveWd_ = false, loadingMore_ = false;
	ScrollList list_;
	int gen_ = 0;
	int qualityOverride_ = 0;

	u64 osdUntil_ = 0;
	bool scrubbing_ = false;
	float scrubPos_ = 0;
	u64 endedAt_ = 0;
	bool autoplayCancelled_ = false;
	u64 lastResumeSave_ = 0;
	u64 lastStatsLog_ = 0;
	bool historyAdded_ = false;

	void load(const yt::VideoItem& v);
	void startPlayer(double at);
	void rebuildList();
	void loadMoreRelated();
	const yt::VideoItem* nextVideo();
	void showOsd(u64 ms = 3000) { osdUntil_ = util::nowMs() + ms; }
	void saveResume();
	void cycleQuality();
	void togglePause();
	void seekBy(double d);
	void drawControls();
};

void WatchPage::load(const yt::VideoItem& v) {
	if (!item_.id.empty()) saveResume();
	player_.close();
	item_ = v;
	pd_ = yt::PlayerData();
	wd_ = yt::WatchData();
	havePd_ = haveWd_ = loadingMore_ = false;
	loadError_.clear();
	endedAt_ = 0;
	autoplayCancelled_ = false;
	historyAdded_ = false;
	qualityOverride_ = 0;
	list_.clear();
	list_.showSelection = false;
	showOsd(4000);
	int gen = ++gen_;
	std::string id = v.id;

	tasks::run([this, id, gen, tok = alive_](yt::InnerTube& it) {
		if (!*tok) return;
		yt::PlayerData pd = it.player(id);
		tasks::post(guard([this, gen, pd = std::move(pd)]() mutable {
			if (gen != gen_) return;
			pd_ = std::move(pd);
			havePd_ = true;
			if (!pd_.error.empty()) {
				loadError_ = pd_.error;
				return;
			}
			if (item_.title.empty()) item_.title = pd_.title;
			if (item_.channel.empty()) item_.channel = pd_.author;
			if (item_.channelId.empty()) item_.channelId = pd_.channelId;
			if (item_.duration.empty() && pd_.lengthSec > 0) item_.duration = util::formatTime(pd_.lengthSec);
			double resume = g_lib.resumePosition(item_.id);
			startPlayer(resume);
			if (resume > 0) toast("Resuming at " + util::formatTime(resume));
			if (haveWd_) rebuildList();
		}));
	});
	tasks::run([this, id, gen, tok = alive_](yt::InnerTube& it) {
		if (!*tok) return;
		yt::WatchData wd = it.next(id);
		tasks::post(guard([this, gen, wd = std::move(wd)]() mutable {
			if (gen != gen_) return;
			wd_ = std::move(wd);
			haveWd_ = true;
			if (item_.title.empty()) item_.title = wd_.title;
			if (item_.channel.empty()) item_.channel = wd_.channel;
			if (item_.channelId.empty()) item_.channelId = wd_.channelId;
			if (item_.views.empty()) item_.views = wd_.views;
			if (item_.published.empty()) item_.published = wd_.date;
			rebuildList();
		}));
	});
	rebuildList();
}

void WatchPage::startPlayer(double at) {
	Player::Options opt;
	opt.maxHeight = qualityOverride_ ? qualityOverride_ : g_lib.settings.effectiveMaxHeight();
	opt.fastDecode = g_lib.settings.fastDecode;
	opt.audioOnly = g_lib.settings.audioOnly;
	if (!player_.open(pd_, opt, at)) {
		loadError_ = player_.error();
		return;
	}
	player_.setVolume(g_lib.settings.volume);
}

void WatchPage::saveResume() {
	if (item_.id.empty() || !player_.isOpen()) return;
	Player::State st = player_.state();
	if (st == Player::State::Loading || st == Player::State::Error) return;
	double pos = st == Player::State::Ended ? player_.duration() : player_.position();
	g_lib.setResumePosition(item_.id, pos, player_.duration());
}

const yt::VideoItem* WatchPage::nextVideo() {
	for (const auto& v : wd_.related.items)
		if (!v.live && v.id != item_.id) return &v;
	return nullptr;
}

void WatchPage::rebuildList() {
	int sel = list_.selected;
	list_.rows.clear();
	std::string title = item_.title.empty() ? "Loading..." : item_.title;
	std::string meta = haveWd_ ? wd_.views : item_.views;
	std::string date = haveWd_ ? wd_.date : item_.published;
	if (!date.empty()) meta += (meta.empty() ? "" : " \xC2\xB7 ") + date;
	list_.emplace<TitleRow>(title, meta);

	std::string chId = !wd_.channelId.empty() ? wd_.channelId : item_.channelId;
	std::string chName = !wd_.channel.empty() ? wd_.channel : item_.channel;
	if (!chId.empty()) list_.emplace<ChannelInfoRow>(chId, chName, wd_.subscribers);

	std::string desc = !wd_.description.empty() ? wd_.description : pd_.description;
	if (!desc.empty()) list_.emplace<DescriptionRow>(desc);

	list_.emplace<HeaderRow>("Up next");
	if (!haveWd_) {
		list_.emplace<LoadingRow>(60);
	} else if (wd_.related.items.empty()) {
		list_.emplace<MessageRow>(wd_.error.empty() ? "No related videos" : "Couldn't load related videos",
		                          wd_.error);
	} else {
		for (const auto& v : wd_.related.items) {
			yt::VideoItem copy = v;
			list_.emplace<VideoRow>(v, guard([this, copy] { load(copy); }));
		}
		if (loadingMore_) list_.emplace<LoadingRow>(44);
	}
	list_.selected = std::min(sel, (int)list_.rows.size() - 1);
}

void WatchPage::loadMoreRelated() {
	if (!haveWd_ || loadingMore_ || wd_.related.continuation.empty()) return;
	loadingMore_ = true;
	std::string token = wd_.related.continuation;
	int gen = gen_;
	rebuildList();
	tasks::run([this, token, gen, tok = alive_](yt::InnerTube& it) {
		if (!*tok) return;
		yt::VideoPage p = it.nextMore(token);
		tasks::post(guard([this, gen, p = std::move(p)]() mutable {
			if (gen != gen_) return;
			loadingMore_ = false;
			for (auto& v : p.items) wd_.related.items.push_back(std::move(v));
			wd_.related.continuation = p.error.empty() ? p.continuation : "";
			rebuildList();
		}));
	});
}

void WatchPage::togglePause() {
	Player::State st = player_.state();
	if (st == Player::State::Ended) {
		player_.seek(0);
		player_.setPaused(false);
		endedAt_ = 0;
	} else {
		player_.setPaused(!player_.paused());
	}
	showOsd();
}

void WatchPage::seekBy(double d) {
	if (!player_.isOpen()) return;
	player_.seek(player_.position() + d);
	showOsd();
}

void WatchPage::cycleQuality() {
	if (!havePd_ || !pd_.error.empty()) return;
	std::vector<int> heights;
	for (const auto& s : pd_.streams)
		if (!s.audio && s.codecs.rfind("avc1", 0) == 0 && s.height > 0 && s.height <= 480 &&
		    std::find(heights.begin(), heights.end(), s.height) == heights.end())
			heights.push_back(s.height);
	std::sort(heights.begin(), heights.end());
	if (heights.size() < 2) {
		toast("No other qualities available");
		return;
	}
	int cur = player_.hasVideo() ? atoi(player_.qualityLabel().c_str()) : 0;
	int next = heights[0];
	for (int h : heights)
		if (h > cur) {
			next = h;
			break;
		}
	double pos = player_.position();
	bool wasPaused = player_.paused();
	qualityOverride_ = next;
	startPlayer(pos);
	if (wasPaused) player_.setPaused(true);
	toast("Quality: " + std::to_string(next) + "p" + (next > 240 && !util::isNew3DS() ? " (may stutter)" : ""));
}

void WatchPage::update(Input& in) {
	Player::State st = player_.state();

	// Autoplay countdown after the video ends.
	if (st == Player::State::Ended && !endedAt_) {
		endedAt_ = util::nowMs();
		g_lib.setResumePosition(item_.id, player_.duration(), player_.duration());
	}
	bool countdown = endedAt_ && g_lib.settings.autoplay && !autoplayCancelled_ && nextVideo();
	if (countdown && util::nowMs() - endedAt_ > 6000) {
		yt::VideoItem next = *nextVideo();
		tasks::post(guard([this, next] { load(next); }));
		return;
	}

	if (!historyAdded_ && (st == Player::State::Playing || st == Player::State::Paused)) {
		historyAdded_ = true;
		g_lib.addHistory(item_);
	}
	if (player_.isOpen() && util::nowMs() - lastStatsLog_ > 5000) {
		lastStatsLog_ = util::nowMs();
		static const char* names[] = {"idle", "loading", "buffering", "playing", "paused", "ended", "error"};
		util::logf("watch %s: %s pos=%.1f/%.0f buffered=%.1f %s frames=%lu dropped=%lu", item_.id.c_str(),
		           names[(int)st], player_.position(), player_.duration(), player_.bufferedUntil(),
		           player_.qualityLabel().c_str(), (unsigned long)player_.framesPresented(),
		           (unsigned long)player_.framesDropped());
	}
	if ((st == Player::State::Playing) && util::nowMs() - lastResumeSave_ > 10000) {
		lastResumeSave_ = util::nowMs();
		saveResume();
	}

	// ---- buttons
	if (in.down & KEY_B) {
		if (countdown) {
			autoplayCancelled_ = true;
		} else if (list_.showSelection) {
			list_.showSelection = false;
		} else {
			g_app->pop();
			return;
		}
	}
	if (in.down & KEY_A) {
		if (countdown) {
			yt::VideoItem next = *nextVideo();
			tasks::post(guard([this, next] { load(next); }));
			return;
		}
		if (list_.showSelection) {
			if (Row* r = list_.selectedRow()) r->activate();
		} else {
			togglePause();
		}
	}
	if (in.down & KEY_L) seekBy(-10);
	if (in.down & KEY_R) seekBy(10);
	if (in.repeat & KEY_LEFT) seekBy(-5);
	if (in.repeat & KEY_RIGHT) seekBy(5);
	if (in.down & KEY_SELECT) {
		g_lib.toggleSaved(item_);
		toast(g_lib.isSaved(item_.id) ? "Saved to Watch later" : "Removed from Watch later");
	}
	if (in.down & KEY_Y) {
		std::string id = !wd_.channelId.empty() ? wd_.channelId : item_.channelId;
		std::string name = !wd_.channel.empty() ? wd_.channel : item_.channel;
		if (!id.empty()) g_app->openChannel(id, name);
	}
	if (in.down & KEY_X) cycleQuality();
	if (in.down & (KEY_UP | KEY_DOWN)) showOsd(1500);

	// ---- touch on the controls panel
	if (in.touchDown && in.startY < kPanelH) {
		if (in.startY >= kSeekY - 14 && in.startY <= kSeekY + 12 && in.startX >= kSeekX - 10 &&
		    in.startX <= kSeekX + kSeekW + 10 && player_.duration() > 0) {
			scrubbing_ = true;
		}
	}
	if (scrubbing_) {
		float f = (in.tx - kSeekX) / kSeekW;
		scrubPos_ = std::min(1.0f, std::max(0.0f, f));
		showOsd();
		if (in.touchUp || !in.touching) {
			scrubbing_ = false;
			player_.seek(scrubPos_ * player_.duration());
		}
	} else if (in.tapped && in.startY < kPanelH) {
		in.consumeTap();
		float tx = in.tx, ty = in.ty;
		if (tx < 38 && ty < 34) {
			g_app->pop();
			return;
		}
		if (tx > kBotW - 40 && ty < 34) {
			g_lib.toggleSaved(item_);
			toast(g_lib.isSaved(item_.id) ? "Saved to Watch later" : "Removed from Watch later");
		} else if (ty > 58) {
			if (fabsf(tx - 160) < 22) togglePause();
			else if (fabsf(tx - 110) < 22) seekBy(-10);
			else if (fabsf(tx - 210) < 22) seekBy(10);
			else if (tx > 244) cycleQuality();
		}
	}

	if (!scrubbing_) list_.update(in);
}

void WatchPage::drawTop() {
	rect(0, 0, kTopW, kH, col::black);
	C2D_Image img;
	int vw = 0, vh = 0;
	bool haveFrame = player_.frame(img, vw, vh);
	Player::State st = player_.state();

	if (haveFrame && vw > 0 && vh > 0) {
		float s = std::min((float)kTopW / vw, (float)kH / vh);
		float dw = vw * s, dh = vh * s;
		image(img, (kTopW - dw) / 2, (kH - dh) / 2, dw, dh);
	} else {
		// Thumbnail while loading (or for audio-only playback).
		const C2D_Image* th = g_thumbs.get(item_.id);
		if (th) image(*th, 0, (kH - 225) / 2.0f, kTopW, 225);
		if (st != Player::State::Playing && st != Player::State::Paused) rect(0, 0, kTopW, kH, col::overlay);
	}

	bool loading = !loadError_.empty() ? false : (!havePd_ || st == Player::State::Loading);
	if (!loadError_.empty() || st == Player::State::Error) {
		std::string err = !loadError_.empty() ? loadError_ : player_.error();
		rect(0, 0, kTopW, kH, col::overlayStrong);
		text("Can't play this video", kTopW / 2.0f, 92, 0.62f, col::text, Center);
		textBlock(err, 40, 120, 0.45f, col::text2, kTopW - 80, 3);
		text("B: Back", kTopW / 2.0f, 196, 0.42f, col::text3, Center);
		return;
	}
	if (loading || st == Player::State::Buffering) spinner(kTopW / 2.0f, kH / 2.0f, 16, col::white);

	if (endedAt_ && g_lib.settings.autoplay && !autoplayCancelled_ && nextVideo()) {
		const yt::VideoItem* nv = nextVideo();
		int left = 6 - (int)((util::nowMs() - endedAt_) / 1000);
		rect(0, 0, kTopW, kH, col::overlayStrong);
		text("Up next in " + std::to_string(std::max(left, 0)), kTopW / 2.0f, 34, 0.5f, col::text2, Center);
		const C2D_Image* nt = g_thumbs.get(nv->id);
		if (nt) image(*nt, 120, 58, 160, 90);
		else rect(120, 58, 160, 90, col::surface2);
		cornerMask(120, 58, 160, 90, 8, col::black);
		textBlock(nv->title, 40, 156, 0.5f, col::text, kTopW - 80, 2);
		text("A: Play now    B: Cancel", kTopW / 2.0f, 208, 0.42f, col::text3, Center);
		return;
	}

	bool osd = util::nowMs() < osdUntil_ || st == Player::State::Paused || st == Player::State::Ended || scrubbing_;
	if (!osd) return;
	gradientV(0, 0, kTopW, 34, 0xB0000000, 0x00000000);
	textFit(item_.title, 10, 7, 0.5f, col::white, kTopW - 80);
	if (player_.isOpen()) {
		std::string q = player_.qualityLabel();
		float qw = textWidth(q, 0.38f) + 10;
		roundRect(kTopW - qw - 8, 7, qw, 15, 4, 0x90000000);
		text(q, kTopW - qw / 2 - 8, 7.5f, 0.38f, col::white, Center);
	}

	gradientV(0, kH - 54, kTopW, 54, 0x00000000, 0xC0000000);
	double dur = player_.duration();
	double pos = scrubbing_ ? scrubPos_ * dur : player_.position();
	float barX = 12, barW = kTopW - 24, barY = kH - 22;
	rect(barX, barY, barW, 3, 0x50FFFFFF);
	if (dur > 0) {
		double buf = player_.bufferedUntil();
		rect(barX, barY, barW * (float)std::min(1.0, buf / dur), 3, 0x80FFFFFF);
		float px = barW * (float)std::min(1.0, pos / dur);
		rect(barX, barY, px, 3, col::accent);
		circle(barX + px, barY + 1.5f, 5, col::accent);
	}
	std::string t = util::formatTime(pos) + " / " + util::formatTime(dur);
	text(t, barX, barY - 16, 0.42f, col::white);
	if (st == Player::State::Paused) iconPause(kTopW / 2.0f, kH / 2.0f, 30, 0xC0FFFFFF);
	text("A Play/Pause   L/R \xC2\xB1" "10s   X Quality", kTopW - 12, barY - 16, 0.36f, 0xB0FFFFFF, Right);
}

void WatchPage::drawControls() {
	rect(0, 0, kBotW, kPanelH, col::surface);
	rect(0, kPanelH - 1, kBotW, 1, col::divider);

	iconBack(18, 17, 18, col::text);
	textFit(item_.title.empty() ? "Loading..." : item_.title, 38, 17 - lineHeight(0.45f) / 2, 0.45f, col::text,
	        kBotW - 84);
	iconBookmark(kBotW - 20, 17, 16, g_lib.isSaved(item_.id) ? col::text : col::text2, g_lib.isSaved(item_.id));

	double dur = player_.duration();
	double pos = scrubbing_ ? scrubPos_ * dur : player_.position();
	text(util::formatTime(pos), kSeekX - 8, kSeekY - lineHeight(0.38f) / 2, 0.38f, col::text2, Right);
	text(util::formatTime(dur), kSeekX + kSeekW + 8, kSeekY - lineHeight(0.38f) / 2, 0.38f, col::text2);
	roundRect(kSeekX, kSeekY - 2, kSeekW, 4, 2, col::surface3);
	if (dur > 0) {
		double buf = player_.bufferedUntil();
		roundRect(kSeekX, kSeekY - 2, kSeekW * (float)std::min(1.0, buf / dur), 4, 2, col::text3);
		float px = kSeekW * (float)std::min(1.0, std::max(0.0, pos / dur));
		roundRect(kSeekX, kSeekY - 2, px, 4, 2, col::accent);
		circle(kSeekX + px, kSeekY, scrubbing_ ? 8 : 6, col::accent);
	}

	float by = 72;
	Player::State st = player_.state();
	iconSeek(110, by, 20, col::text, false);
	iconSeek(210, by, 20, col::text, true);
	circle(160, by, 15, col::text);
	if (st == Player::State::Loading || st == Player::State::Buffering) spinner(160, by, 8, col::black);
	else if (st == Player::State::Playing) iconPause(160, by, 14, col::black);
	else iconPlay(161, by, 14, col::black);

	std::string q = player_.isOpen() ? player_.qualityLabel() : "--";
	float qw = textWidth(q, 0.4f) + 16;
	roundRect(kBotW - qw - 14, by - 10, qw, 20, 10, col::surface2);
	text(q, kBotW - qw / 2 - 14, by - lineHeight(0.4f) / 2, 0.4f, col::text, Center);
}

void WatchPage::drawBottom() {
	list_.draw();
	drawControls();
}

std::shared_ptr<Page> makeWatchPage(const yt::VideoItem& v) { return std::make_shared<WatchPage>(v); }

}  // namespace ui
