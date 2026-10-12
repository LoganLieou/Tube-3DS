#include "ui/list.hpp"

#include <cmath>

#include "ui/gfx.hpp"
#include "util.hpp"

namespace ui {

void Input::poll() {
	hidScanInput();
	down = hidKeysDown();
	held = hidKeysHeld();
	up = hidKeysUp();
	repeat = hidKeysDownRepeat();
	hidCircleRead(&cpad);

	touchDown = (down & KEY_TOUCH) != 0;
	touchUp = (up & KEY_TOUCH) != 0;
	touching = (held & KEY_TOUCH) != 0;
	tapped = false;
	if (touching) {
		touchPosition tp;
		hidTouchRead(&tp);
		tx = tp.px;
		ty = tp.py;
	}
	if (touchDown) {
		startX = tx;
		startY = ty;
		touchStartMs = util::nowMs();
		dragged = false;
	}
	if (touching && (abs(tx - startX) > 7 || abs(ty - startY) > 7)) dragged = true;
	if (touchUp && !dragged && util::nowMs() - touchStartMs < 600) tapped = true;
}

void ScrollList::clear() {
	rows.clear();
	selected = 0;
	scroll_ = 0;
	velocity_ = 0;
	target_ = -1;
	nearEndFired_ = false;
	rowsAtFire_ = 0;
}

float ScrollList::contentHeight() const {
	float total = 0;
	for (const auto& r : rows) total += r->height();
	return total;
}

float ScrollList::rowTop(int idx) const {
	float t = 0;
	for (int i = 0; i < idx && i < (int)rows.size(); i++) t += rows[i]->height();
	return t;
}

void ScrollList::clamp() {
	float maxScroll = contentHeight() - h;
	if (maxScroll < 0) maxScroll = 0;
	if (scroll_ < 0) scroll_ = 0;
	if (scroll_ > maxScroll) scroll_ = maxScroll;
}

void ScrollList::ensureVisible(int idx) {
	if (idx < 0 || idx >= (int)rows.size()) return;
	float top = rowTop(idx), bottom = top + rows[idx]->height();
	float margin = 12;
	float tgt = scroll_;
	if (top - margin < scroll_) tgt = top - margin;
	else if (bottom + margin > scroll_ + h) tgt = bottom + margin - h;
	float maxScroll = std::max(0.0f, contentHeight() - h);
	target_ = std::min(std::max(tgt, 0.0f), maxScroll);
}

int ScrollList::moveSelection(int from, int dir) const {
	int i = from;
	while (true) {
		i += dir;
		if (i < 0 || i >= (int)rows.size()) return from;
		if (rows[i]->selectable()) return i;
	}
}

void ScrollList::select(int idx, bool scrollTo) {
	if (idx < 0 || idx >= (int)rows.size()) return;
	selected = idx;
	if (scrollTo) ensureVisible(idx);
}

Row* ScrollList::selectedRow() {
	if (selected < 0 || selected >= (int)rows.size()) return nullptr;
	return rows[selected].get();
}

const yt::VideoItem* ScrollList::selectedVideo() {
	Row* r = selectedRow();
	return r ? r->video() : nullptr;
}

void ScrollList::update(Input& in, bool dpad) {
	// Keep the selection on a selectable row.
	if (!rows.empty() && (selected < 0 || selected >= (int)rows.size() || !rows[selected]->selectable())) {
		int s = moveSelection(-1, 1);
		selected = (s >= 0 && s < (int)rows.size() && rows[s]->selectable()) ? s : 0;
	}

	if (dpad) {
		u32 k = in.repeat;
		int dir = (k & KEY_DOWN) ? 1 : (k & KEY_UP) ? -1 : 0;
		if (dir) {
			if (!showSelection) {
				showSelection = true;
			} else {
				int n = moveSelection(selected, dir);
				if (n != selected) selected = n;
			}
			ensureVisible(selected);
			lastInteraction_ = util::nowMs();
		}
	}

	// Circle pad scrolls freely.
	if (abs(in.cpad.dy) > 20) {
		target_ = -1;
		velocity_ = 0;
		scroll_ -= in.cpad.dy / 14.0f;
		clamp();
	}

	bool inside = in.startX >= x && in.startX < x + w && in.startY >= y && in.startY < y + h;
	if (in.touchDown && inside) {
		dragging_ = true;
		dragStartScroll_ = scroll_;
		lastTouchY_ = in.ty;
		velocity_ = 0;
		target_ = -1;
	}
	if (dragging_ && in.touching) {
		if (in.dragged) {
			float newScroll = dragStartScroll_ - (in.ty - in.startY);
			velocity_ = velocity_ * 0.5f + (lastTouchY_ - in.ty) * 0.5f;
			scroll_ = newScroll;
			// Rubber-band past the ends.
			float maxScroll = std::max(0.0f, contentHeight() - h);
			if (scroll_ < 0) scroll_ *= 0.4f;
			if (scroll_ > maxScroll) scroll_ = maxScroll + (scroll_ - maxScroll) * 0.4f;
		}
		lastTouchY_ = in.ty;
	}
	if (dragging_ && in.touchUp) {
		dragging_ = false;
		if (in.tapped && inside) {
			// Find the row under the finger.
			float ly = in.ty - y + scroll_;
			float top = 0;
			for (int i = 0; i < (int)rows.size(); i++) {
				float rh = rows[i]->height();
				if (ly >= top && ly < top + rh) {
					if (rows[i]->selectable()) selected = i;
					in.consumeTap();
					rows[i]->tap(in.tx - x, ly - top);
					break;
				}
				top += rh;
			}
			velocity_ = 0;
		}
	}

	if (!dragging_) {
		if (target_ >= 0) {
			scroll_ += (target_ - scroll_) * 0.3f;
			if (fabsf(target_ - scroll_) < 0.5f) {
				scroll_ = target_;
				target_ = -1;
			}
		} else if (fabsf(velocity_) > 0.2f) {
			scroll_ += velocity_;
			velocity_ *= 0.94f;
		} else {
			velocity_ = 0;
		}
		// Settle back inside the bounds.
		float maxScroll = std::max(0.0f, contentHeight() - h);
		if (scroll_ < 0) {
			scroll_ *= 0.6f;
			velocity_ = 0;
			if (scroll_ > -0.5f) scroll_ = 0;
		} else if (scroll_ > maxScroll) {
			scroll_ = maxScroll + (scroll_ - maxScroll) * 0.6f;
			velocity_ = 0;
			if (scroll_ - maxScroll < 0.5f) scroll_ = maxScroll;
		}
	}

	// Ask for more content when the end comes into view.
	if (onNearEnd) {
		if (rows.size() != rowsAtFire_) nearEndFired_ = false;
		float remaining = contentHeight() - (scroll_ + h);
		bool nearEnd = remaining < h || (selected >= (int)rows.size() - 4);
		if (nearEnd && !nearEndFired_ && !rows.empty()) {
			nearEndFired_ = true;
			rowsAtFire_ = rows.size();
			onNearEnd();
		}
	}
}

void ScrollList::draw() {
	float top = y - scroll_;
	for (int i = 0; i < (int)rows.size(); i++) {
		float rh = rows[i]->height();
		if (top + rh >= y && top < y + h) rows[i]->draw(x, top, w, showSelection && i == selected);
		top += rh;
		if (top > y + h) break;
	}
	// Scroll indicator.
	float total = contentHeight();
	if (total > h + 1) {
		float barH = std::max(16.0f, h * h / total);
		float barY = y + (h - barH) * (scroll_ / (total - h));
		if (barY < y) barY = y;
		if (barY + barH > y + h) barY = y + h - barH;
		roundRect(x + w - 4, barY, 3, barH, 1.5f, (col::text2 & 0x00FFFFFF) | 0x60000000);
	}
}

}  // namespace ui
