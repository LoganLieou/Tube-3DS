#pragma once

#include <3ds.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "yt/innertube.hpp"

namespace ui {

struct Input {
	u32 down = 0, held = 0, up = 0, repeat = 0;
	bool touching = false, touchDown = false, touchUp = false;
	int tx = 0, ty = 0;          // current (or last) touch position
	int startX = 0, startY = 0;  // where the current touch began
	u64 touchStartMs = 0;
	bool dragged = false;        // moved far enough to count as a drag
	bool tapped = false;         // released without dragging
	circlePosition cpad{0, 0};

	void poll();
	bool tapIn(float x, float y, float w, float h) const {
		return tapped && tx >= x && tx < x + w && ty >= y && ty < y + h;
	}
	bool touchIn(float x, float y, float w, float h) const {
		return touching && tx >= x && tx < x + w && ty >= y && ty < y + h;
	}
	// Consumes the current tap so nothing else reacts to it.
	void consumeTap() { tapped = false; }
};

class Row {
public:
	virtual ~Row() {}
	virtual float height() const = 0;
	virtual void draw(float x, float y, float w, bool selected) = 0;
	virtual bool selectable() const { return true; }
	virtual void activate() {}
	// Tap at local coordinates; defaults to activate().
	virtual void tap(float lx, float ly) { activate(); }
	virtual const yt::VideoItem* video() const { return nullptr; }
};

class ScrollList {
public:
	float x = 0, y = 0, w = 320, h = 240;
	std::vector<std::unique_ptr<Row>> rows;
	int selected = 0;
	bool showSelection = true;  // false = selection hidden until the D-pad is used
	std::function<void()> onNearEnd;

	void clear();
	void add(std::unique_ptr<Row> r) { rows.push_back(std::move(r)); }
	template <typename T, typename... Args>
	T* emplace(Args&&... args) {
		auto r = std::make_unique<T>(std::forward<Args>(args)...);
		T* p = r.get();
		rows.push_back(std::move(r));
		return p;
	}

	// Handles touch, D-pad and circle pad. Returns true if it used the input.
	void update(Input& in, bool dpad = true);
	void draw();

	Row* selectedRow();
	const yt::VideoItem* selectedVideo();
	void select(int idx, bool scrollTo = true);
	float contentHeight() const;
	void scrollToTop() { scroll_ = 0, velocity_ = 0; }
	float scroll() const { return scroll_; }

private:
	float scroll_ = 0, velocity_ = 0;
	float target_ = -1;  // animated scroll target (from D-pad), -1 = none
	bool dragging_ = false;
	float dragStartScroll_ = 0;
	int lastTouchY_ = 0;
	u64 lastInteraction_ = 0;
	bool nearEndFired_ = false;
	size_t rowsAtFire_ = 0;

	float rowTop(int idx) const;
	void clamp();
	void ensureVisible(int idx);
	int moveSelection(int from, int dir) const;
};

}  // namespace ui
