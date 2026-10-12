#pragma once

#include <functional>
#include <string>

#include "ui/list.hpp"
#include "yt/innertube.hpp"

namespace ui {

// Row actions run on the next frame (via tasks::post) because they often
// rebuild the very list the row lives in.
using Action = std::function<void()>;

int parseDuration(const std::string& s);  // "1:02:03" -> 3723

class VideoRow : public Row {
public:
	VideoRow(const yt::VideoItem& v, Action onOpen) : v_(v), onOpen_(std::move(onOpen)) {}
	float height() const override { return 64; }
	void draw(float x, float y, float w, bool sel) override;
	void activate() override;
	const yt::VideoItem* video() const override { return &v_; }

private:
	yt::VideoItem v_;
	Action onOpen_;
};

class HeaderRow : public Row {
public:
	HeaderRow(std::string title, std::string action = "", Action onAction = nullptr)
		: title_(std::move(title)), action_(std::move(action)), onAction_(std::move(onAction)) {}
	float height() const override { return 30; }
	void draw(float x, float y, float w, bool sel) override;
	bool selectable() const override { return (bool)onAction_; }
	void activate() override;

private:
	std::string title_, action_;
	Action onAction_;
};

class MessageRow : public Row {
public:
	MessageRow(std::string title, std::string detail = "", Action onTap = nullptr, float h = 0)
		: title_(std::move(title)), detail_(std::move(detail)), onTap_(std::move(onTap)), h_(h) {}
	float height() const override;
	void draw(float x, float y, float w, bool sel) override;
	bool selectable() const override { return (bool)onTap_; }
	void activate() override;

private:
	std::string title_, detail_;
	Action onTap_;
	float h_;
};

class LoadingRow : public Row {
public:
	explicit LoadingRow(float h = 44) : h_(h) {}
	float height() const override { return h_; }
	void draw(float x, float y, float w, bool sel) override;
	bool selectable() const override { return false; }

private:
	float h_;
};

class ButtonRow : public Row {
public:
	ButtonRow(std::string label, Action a, bool primary = false)
		: label_(std::move(label)), a_(std::move(a)), primary_(primary) {}
	float height() const override { return 44; }
	void draw(float x, float y, float w, bool sel) override;
	void activate() override;

private:
	std::string label_;
	Action a_;
	bool primary_;
};

class ChannelRow : public Row {
public:
	ChannelRow(std::string id, std::string name, Action onOpen)
		: id_(std::move(id)), name_(std::move(name)), onOpen_(std::move(onOpen)) {}
	float height() const override { return 50; }
	void draw(float x, float y, float w, bool sel) override;
	void activate() override;

private:
	std::string id_, name_;
	Action onOpen_;
};

// "Label .......... Value" with tap-to-change.
class SettingRow : public Row {
public:
	SettingRow(std::string label, std::function<std::string()> value, Action onTap, std::string hint = "")
		: label_(std::move(label)), value_(std::move(value)), onTap_(std::move(onTap)), hint_(std::move(hint)) {}
	float height() const override { return hint_.empty() ? 40 : 52; }
	void draw(float x, float y, float w, bool sel) override;
	bool selectable() const override { return (bool)onTap_; }
	void activate() override;

private:
	std::string label_;
	std::function<std::string()> value_;
	Action onTap_;
	std::string hint_;
};

class SearchBarRow : public Row {
public:
	SearchBarRow(std::function<std::string()> query, Action onTap) : query_(std::move(query)), onTap_(std::move(onTap)) {}
	float height() const override { return 46; }
	void draw(float x, float y, float w, bool sel) override;
	void activate() override;

private:
	std::function<std::string()> query_;
	Action onTap_;
};

class RecentSearchRow : public Row {
public:
	RecentSearchRow(std::string q, Action onTap) : q_(std::move(q)), onTap_(std::move(onTap)) {}
	float height() const override { return 36; }
	void draw(float x, float y, float w, bool sel) override;
	void activate() override;

private:
	std::string q_;
	Action onTap_;
};

// Two or more tabs in a pill ("History | Watch later").
class SegmentRow : public Row {
public:
	SegmentRow(std::vector<std::string> labels, std::function<int()> current, std::function<void(int)> onPick)
		: labels_(std::move(labels)), current_(std::move(current)), onPick_(std::move(onPick)) {}
	float height() const override { return 40; }
	void draw(float x, float y, float w, bool sel) override;
	void activate() override;  // cycles
	void tap(float lx, float ly) override;

private:
	std::vector<std::string> labels_;
	std::function<int()> current_;
	std::function<void(int)> onPick_;
	float lastW_ = 320;
};

// Draws the shared selection highlight.
void rowHighlight(float x, float y, float w, float h, bool sel);

}  // namespace ui
