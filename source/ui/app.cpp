#include "ui/app.hpp"

#include <ctime>

#include "gfx_util.hpp"
#include "library.hpp"
#include "tasks.hpp"
#include "thumbnails.hpp"
#include "ui/gfx.hpp"
#include "ui/pages.hpp"
#include "util.hpp"

namespace ui {

App* g_app = nullptr;

App::App() {
	g_app = this;
	top_ = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
	bottom_ = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
	tabs_[TabHome] = makeHomePage();
	tabs_[TabSearch] = makeSearchPage();
	tabs_[TabSubs] = makeSubsPage();
	tabs_[TabLibrary] = makeLibraryPage();
	tabs_[TabSettings] = makeSettingsPage();
	::aptHook(&hook_, aptHook, this);
	hidSetRepeatParameters(350, 90);
}

void App::aptHook(APT_HookType hook, void* param) {
	auto* app = static_cast<App*>(param);
	if (hook == APTHOOK_ONSUSPEND || hook == APTHOOK_ONSLEEP) {
		if (Page* p = app->current()) p->onSuspend();
		g_lib.saveIfDirty(true);
	}
}

Page* App::current() {
	if (!stack_.empty()) return stack_.back().get();
	return tabs_[tab_].get();
}

void App::push(std::shared_ptr<Page> p) {
	if (Page* cur = current()) cur->onHide();
	stack_.push_back(std::move(p));
	stack_.back()->onShow();
}

void App::pop() { pendingPops_++; }

void App::switchTab(int tab) {
	if (tab < 0 || tab >= TabCount) return;
	// Leaving pushed pages (e.g. a video) when switching tabs.
	while (!stack_.empty()) {
		stack_.back()->onHide();
		stack_.pop_back();
	}
	if (tab != tab_) tabs_[tab_]->onHide();
	tab_ = tab;
	tabs_[tab_]->onShow();
}

void App::openVideo(const yt::VideoItem& v) {
	// Only one player at a time: drop any watch pages already on the stack.
	// (Safe here: openVideo runs from row actions in tasks::pump, never from
	// inside a watch page's own update.)
	for (size_t i = 0; i < stack_.size();) {
		if (stack_[i]->isWatch()) stack_.erase(stack_.begin() + i);
		else i++;
	}
	push(makeWatchPage(v));
}

void App::openChannel(const std::string& id, const std::string& name) { push(makeChannelPage(id, name)); }

void App::startSearch() {
	switchTab(TabSearch);
	wantSearch_ = true;  // open the keyboard after this frame's input handling
}

void App::handleGlobalInput() {
	if (in.down & KEY_START) {
		quit = true;
		return;
	}
	if (!stack_.empty()) return;
	if (in.down & KEY_L) switchTab((tab_ + TabCount - 1) % TabCount);
	if (in.down & KEY_R) switchTab((tab_ + 1) % TabCount);
	if (in.down & KEY_X) startSearch();
	// Tab bar taps.
	if (in.tapped && in.ty >= kH - kTabBarH) {
		in.consumeTap();
		int t = in.tx * TabCount / kBotW;
		if (t == tab_ && t == TabSearch) startSearch();
		else switchTab(t);
	}
}

void App::autotest() {
	u64 now = util::nowMs();
	switch (autotestStep_) {
	case 0:
		if (now - autotestAt_ < 1500) return;
		util::logf("autotest: searching '%s'", autotestQuery_.c_str());
		switchTab(TabSearch);
		tabs_[TabSearch]->runQuery(autotestQuery_);
		autotestStep_ = 1;
		autotestAt_ = now;
		return;
	case 1:
		if (const yt::VideoItem* v = tabs_[TabSearch]->firstVideo()) {
			util::logf("autotest: opening %s '%s'", v->id.c_str(), v->title.c_str());
			yt::VideoItem copy = *v;
			autotestStep_ = 2;
			tasks::post([this, copy] { openVideo(copy); });
		} else if (now - autotestAt_ > 30000) {
			util::logf("autotest: no search results after 30s");
			autotestStep_ = -1;
		}
		return;
	default: return;
	}
}

void App::run() {
	{
		std::string text;
		if (util::readFile(util::dataDir() + "/autotest.txt", text)) {
			autotestQuery_ = util::trim(text.substr(0, text.find('\n')));
			if (!autotestQuery_.empty()) {
				autotestStep_ = 0;
				autotestAt_ = util::nowMs();
			}
		}
	}
	tabs_[tab_]->onShow();
	while (aptMainLoop() && !quit) {
		in.poll();
		if (autotestStep_ >= 0) autotest();
		tasks::pump();
		g_thumbs.update();

		// Touches that start on the tab bar belong to it, not to the list above.
		bool tabBarTouch = stack_.empty() && (in.touching || in.touchUp) && in.startY >= kH - kTabBarH;
		handleGlobalInput();
		if (quit) break;
		Page* page = current();
		Input pageIn = in;
		if (tabBarTouch) pageIn.touching = pageIn.touchDown = pageIn.touchUp = pageIn.tapped = false;
		page->update(pageIn);
		while (pendingPops_ > 0) {
			pendingPops_--;
			if (stack_.empty()) break;
			stack_.back()->onHide();
			stack_.pop_back();
			current()->onShow();
		}
		if (wantSearch_ && stack_.empty() && tab_ == TabSearch) {
			wantSearch_ = false;
			tabs_[TabSearch]->onSearchKey();
		}
		g_lib.saveIfDirty();

		page = current();
		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
		processDeferredFrees();
		ui::beginFrame();

		C2D_TargetClear(top_, col::bg);
		C2D_SceneBegin(top_);
		page->drawTop();

		C2D_TargetClear(bottom_, col::bg);
		C2D_SceneBegin(bottom_);
		page->drawBottom();
		if (page->showTabBar()) drawTabBar();
		drawToast();

		C3D_FrameEnd(0);
	}
	// Tear down pages (and their players) before the caller shuts services down.
	stack_.clear();
	for (auto& t : tabs_) t.reset();
	aptUnhook(&hook_);
}

void App::drawTabBar() {
	float y = kH - kTabBarH;
	rect(0, y, kBotW, kTabBarH, col::bg);
	rect(0, y, kBotW, 1, col::divider);
	static const char* labels[TabCount] = {"Home", "Search", "Subs", "Library", "Settings"};
	float tw = (float)kBotW / TabCount;
	for (int i = 0; i < TabCount; i++) {
		float cx = tw * i + tw / 2;
		bool active = i == tab_;
		u32 c = active ? col::text : col::text3;
		float iy = y + 14;
		switch (i) {
		case TabHome: iconHome(cx, iy, 15, c); break;
		case TabSearch: iconSearch(cx, iy, 15, c); break;
		case TabSubs: iconSubs(cx, iy, 15, c); break;
		case TabLibrary: iconLibrary(cx, iy, 14, c); break;
		case TabSettings: iconSettings(cx, iy, 15, c); break;
		}
		text(labels[i], cx, y + 24, 0.34f, c, Center);
		if (active) roundRect(cx - 10, y + 1, 20, 2, 1, col::accent);
	}
}

// ---------------------------------------------------------------- shared chrome

static int g_wifi = 0, g_battery = 5;
static bool g_charging = false;
static u64 g_statusPolled = 0;

void drawStatusBar(const std::string& title) {
	u64 now = util::nowMs();
	if (now - g_statusPolled > 2000) {
		g_statusPolled = now;
		g_wifi = osGetWifiStrength();
		u8 lvl = 5, chg = 0;
		if (R_SUCCEEDED(PTMU_GetBatteryLevel(&lvl))) g_battery = lvl;
		if (R_SUCCEEDED(PTMU_GetBatteryChargeState(&chg))) g_charging = chg != 0;
	}
	logo(10, 6, 14);
	text("Tube3DS", 34, 13 - lineHeight(0.5f) / 2, 0.5f, col::text);
	float tx = 34 + textWidth("Tube3DS", 0.5f) + 8;
	textFit(title, tx, 13 - lineHeight(0.44f) / 2, 0.44f, col::text2, 200);

	time_t t = time(nullptr);
	struct tm* lt = localtime(&t);
	char clock[16];
	snprintf(clock, sizeof clock, "%d:%02d", lt ? lt->tm_hour : 0, lt ? lt->tm_min : 0);
	iconBattery(kTopW - 32, 8, col::text2, g_battery, g_charging);
	iconWifi(kTopW - 50, 13, 12, col::text2, g_wifi);
	text(clock, kTopW - 62, 13 - lineHeight(0.44f) / 2, 0.44f, col::text2, Right);
}

void drawVideoPreview(const yt::VideoItem& v) {
	const float tx = 10, ty = 30, tw = 256, th = 144;
	const C2D_Image* img = g_thumbs.get(v.id);
	if (img) {
		image(*img, tx, ty, tw, th);
	} else {
		rect(tx, ty, tw, th, col::surface2);
		spinner(tx + tw / 2, ty + th / 2, 10, col::text3);
	}
	cornerMask(tx, ty, tw, th, 10, col::bg);
	if (!v.duration.empty()) {
		float sc = 0.45f;
		float bw = textWidth(v.duration, sc) + 10;
		float bx = tx + tw - bw - 6, by = ty + th - 22;
		roundRect(bx, by, bw, 16, 4, v.live ? col::accent : col::overlayStrong);
		text(v.duration, bx + bw / 2, by + 8 - lineHeight(sc) / 2, sc, col::white, Center);
	}
	double resume = g_lib.resumePosition(v.id);
	int dur = 0;
	{
		int total = 0, cur = 0;
		for (char c : v.duration) {
			if (c >= '0' && c <= '9') cur = cur * 10 + (c - '0');
			else if (c == ':') total = total * 60 + cur, cur = 0;
		}
		dur = total * 60 + cur;
	}
	if (resume > 0 && dur > 0) {
		rect(tx + 4, ty + th - 4, tw - 8, 3, 0x80FFFFFF);
		rect(tx + 4, ty + th - 4, (tw - 8) * (float)std::min(1.0, resume / dur), 3, col::accent);
	}

	// Side column.
	float cx = tx + tw + 12, cw = kTopW - cx - 10;
	avatar(v.channel, cx + 14, ty + 16, 14);
	float y = ty + 36;
	y += textBlock(v.channel, cx, y, 0.45f, col::text, cw, 2) + 6;
	if (!v.views.empty()) {
		textFit(v.views, cx, y, 0.42f, col::text2, cw);
		y += lineHeight(0.42f) + 2;
	}
	if (!v.published.empty()) {
		textFit(v.published, cx, y, 0.42f, col::text2, cw);
		y += lineHeight(0.42f) + 2;
	}
	if (resume > 0) {
		textFit("Resume " + util::formatTime(resume), cx, y + 4, 0.42f, col::accent, cw);
		y += lineHeight(0.42f) + 6;
	}
	if (g_lib.isSaved(v.id)) {
		iconBookmark(cx + 6, y + 10, 12, col::text2, true);
		text("Saved", cx + 16, y + 10 - lineHeight(0.4f) / 2, 0.4f, col::text2);
	}

	textBlock(v.title, tx, ty + th + 8, 0.56f, col::text, kTopW - 20, 2);
	text("A Watch   Y Channel   SELECT Save   X Search", kTopW / 2.0f, kH - 14, 0.36f, col::text3, Center);
}

void drawHero(const std::string& headline, const std::string& sub) {
	logo(kTopW / 2.0f - 28, 64, 40);
	text(headline, kTopW / 2.0f, 118, 0.75f, col::text, Center);
	std::vector<std::string> lines;
	int n = wrap(sub, 0.46f, 320, 2, lines);
	for (int i = 0; i < n; i++) text(lines[i], kTopW / 2.0f, 150 + i * lineHeight(0.46f), 0.46f, col::text2, Center);
	text("L/R Switch tabs   X Search   START Exit", kTopW / 2.0f, kH - 14, 0.36f, col::text3, Center);
}

void drawTopBar(const std::string& title) {
	rect(0, 0, kBotW, kTopBarH, col::bg);
	rect(0, kTopBarH - 1, kBotW, 1, col::divider);
	iconBack(18, kTopBarH / 2, 18, col::text);
	textFit(title, 36, kTopBarH / 2 - lineHeight(0.5f) / 2, 0.5f, col::text, kBotW - 48);
}

bool topBarBackTapped(Input& in) {
	if (in.tapped && in.ty < kTopBarH && in.tx < 120) {
		in.consumeTap();
		return true;
	}
	return false;
}

bool keyboard(const std::string& hint, const std::string& initial, std::string& out) {
	SwkbdState kb;
	swkbdInit(&kb, SWKBD_TYPE_NORMAL, 2, 200);
	swkbdSetHintText(&kb, hint.c_str());
	swkbdSetInitialText(&kb, initial.c_str());
	swkbdSetButton(&kb, SWKBD_BUTTON_LEFT, "Cancel", false);
	swkbdSetButton(&kb, SWKBD_BUTTON_RIGHT, "Search", true);
	swkbdSetValidation(&kb, SWKBD_NOTEMPTY_NOTBLANK, 0, 0);
	swkbdSetFeatures(&kb, SWKBD_DARKEN_TOP_SCREEN);
	char buf[256] = {0};
	SwkbdButton b = swkbdInputText(&kb, buf, sizeof buf);
	if (b != SWKBD_BUTTON_RIGHT) return false;
	out = buf;
	return true;
}

}  // namespace ui
