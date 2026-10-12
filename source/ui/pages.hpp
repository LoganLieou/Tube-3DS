#pragma once

#include <memory>
#include <string>

#include "ui/app.hpp"

namespace ui {

// Base for pages that are a scrolling list with a video preview on the top screen.
class ListPage : public Page {
public:
	ListPage();
	void update(Input& in) override;
	void drawTop() override;
	void drawBottom() override;
	// Search tab opens the keyboard; others ignore it.
	virtual void onSearchKey() {}
	// Search tab runs the query directly (used by the autotest hook).
	virtual void runQuery(const std::string&) {}
	const yt::VideoItem* firstVideo();

protected:
	ScrollList list_;
	std::string heroTitle_ = "Tube3DS";
	std::string heroSub_;

	// Rebuild rows while keeping the scroll position and selected video.
	std::string keepSelection();
	void restoreSelection(const std::string& videoId);
	void addVideos(const std::vector<yt::VideoItem>& items);
};

std::shared_ptr<ListPage> makeHomePage();
std::shared_ptr<ListPage> makeSearchPage();
std::shared_ptr<ListPage> makeSubsPage();
std::shared_ptr<ListPage> makeLibraryPage();
std::shared_ptr<ListPage> makeSettingsPage();
std::shared_ptr<Page> makeChannelPage(const std::string& id, const std::string& name);
std::shared_ptr<Page> makeWatchPage(const yt::VideoItem& v);

}  // namespace ui
