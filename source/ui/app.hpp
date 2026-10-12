#pragma once

#include <3ds.h>
#include <citro2d.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "ui/list.hpp"
#include "yt/innertube.hpp"

namespace ui {

class Page {
public:
	virtual ~Page() { *alive_ = false; }
	virtual const char* title() const = 0;
	virtual void onShow() {}
	virtual void onHide() {}
	virtual void update(Input& in) = 0;
	virtual void drawTop() = 0;
	virtual void drawBottom() = 0;
	virtual bool showTabBar() const { return true; }
	virtual bool isWatch() const { return false; }
	virtual void onSuspend() {}  // HOME menu / lid closed

	// Wraps a main-thread callback so it's dropped if this page has been closed.
	template <typename F>
	std::function<void()> guard(F f) {
		std::shared_ptr<bool> tok = alive_;
		return [tok, f]() mutable {
			if (*tok) f();
		};
	}

protected:
	std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

class ListPage;

enum Tab { TabHome, TabSearch, TabSubs, TabLibrary, TabSettings, TabCount };

class App {
public:
	App();
	void run();

	void push(std::shared_ptr<Page> p);
	void pop();
	void switchTab(int tab);
	Page* current();

	void openVideo(const yt::VideoItem& v);
	void openChannel(const std::string& id, const std::string& name);
	void startSearch();  // jump to Search and open the keyboard

	Input in;
	bool quit = false;

private:
	int tab_ = TabHome;
	std::shared_ptr<ListPage> tabs_[TabCount];
	std::vector<std::shared_ptr<Page>> stack_;
	int pendingPops_ = 0;
	bool wantSearch_ = false;
	C3D_RenderTarget* top_ = nullptr;
	C3D_RenderTarget* bottom_ = nullptr;
	aptHookCookie hook_;

	// Developer hook: if sdmc:/3ds/Tube3DS/autotest.txt exists, search for its
	// first line and play the first result, so playback can be exercised in an
	// emulator without input. Does nothing otherwise.
	std::string autotestQuery_;
	int autotestStep_ = -1;
	u64 autotestAt_ = 0;
	void autotest();

	void drawTabBar();
	void handleGlobalInput();
	static void aptHook(APT_HookType hook, void* param);
};

extern App* g_app;

// Shared chrome.
constexpr float kTabBarH = 38;
void drawStatusBar(const std::string& title);
// Big thumbnail + details of `v` on the top screen (below the status bar).
void drawVideoPreview(const yt::VideoItem& v);
void drawHero(const std::string& headline, const std::string& sub);
// Back button + title strip for pushed pages; returns true if back was tapped.
void drawTopBar(const std::string& title);
bool topBarBackTapped(Input& in);
constexpr float kTopBarH = 34;

// Opens the system keyboard. Returns false if cancelled.
bool keyboard(const std::string& hint, const std::string& initial, std::string& out);

}  // namespace ui
