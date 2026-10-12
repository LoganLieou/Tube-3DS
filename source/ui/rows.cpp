#include "ui/rows.hpp"

#include "library.hpp"
#include "tasks.hpp"
#include "thumbnails.hpp"
#include "ui/gfx.hpp"

namespace ui {

static void later(const Action& a) {
	if (a) tasks::post(a);
}

int parseDuration(const std::string& s) {
	int total = 0, cur = 0;
	bool any = false;
	for (char c : s) {
		if (c >= '0' && c <= '9') {
			cur = cur * 10 + (c - '0');
			any = true;
		} else if (c == ':') {
			total = total * 60 + cur;
			cur = 0;
		} else {
			return 0;
		}
	}
	return any ? total * 60 + cur : 0;
}

void rowHighlight(float x, float y, float w, float h, bool sel) {
	if (sel) roundRect(x + 4, y + 1, w - 8, h - 2, 8, col::surface2);
}

// ---------------------------------------------------------------- VideoRow

void VideoRow::draw(float x, float y, float w, bool sel) {
	rowHighlight(x, y, w, height(), sel);
	const float tx = x + 10, ty = y + 5, tw = 96, th = 54;
	const C2D_Image* img = g_thumbs.get(v_.id);
	if (img) image(*img, tx, ty, tw, th);
	else rect(tx, ty, tw, th, col::surface3);

	// Watch progress bar for partially watched videos.
	double resume = g_lib.resumePosition(v_.id);
	int dur = parseDuration(v_.duration);
	if (resume > 0 && dur > 0) {
		rect(tx, ty + th - 3, tw, 3, (col::text3 & 0x00FFFFFF) | 0xC0000000);
		rect(tx, ty + th - 3, tw * (float)std::min(1.0, resume / dur), 3, col::accent);
	}
	cornerMask(tx, ty, tw, th, 6, sel ? col::surface2 : col::bg);

	if (!v_.duration.empty()) {
		const float sc = 0.36f;
		float bw = textWidth(v_.duration, sc) + 7;
		float bx = tx + tw - bw - 3, by = ty + th - 15;
		roundRect(bx, by, bw, 12, 3, v_.live ? col::accent : col::overlayStrong);
		text(v_.duration, bx + bw / 2, by + 6 - lineHeight(sc) / 2, sc, col::white, Center);
	}

	float textX = tx + tw + 8, textW = x + w - textX - 10;
	textBlock(v_.title, textX, y + 3, 0.46f, col::text, textW, 2);
	textFit(v_.channel, textX, y + 34, 0.38f, col::text2, textW);
	std::string meta = v_.views;
	if (!v_.published.empty()) meta += (meta.empty() ? "" : " \xC2\xB7 ") + v_.published;
	textFit(meta, textX, y + 46, 0.38f, col::text2, textW);
}

void VideoRow::activate() { later(onOpen_); }

// ---------------------------------------------------------------- HeaderRow

void HeaderRow::draw(float x, float y, float w, bool sel) {
	if (sel) roundRect(x + w - textWidth(action_, 0.42f) - 26, y + 4, textWidth(action_, 0.42f) + 18, 22, 11,
	                   col::surface2);
	text(title_, x + 12, y + 15 - lineHeight(0.52f) / 2, 0.52f, col::text);
	if (!action_.empty())
		text(action_, x + w - 17, y + 15 - lineHeight(0.42f) / 2, 0.42f, col::blue, Right);
}

void HeaderRow::activate() { later(onAction_); }

// ---------------------------------------------------------------- MessageRow

float MessageRow::height() const {
	if (h_ > 0) return h_;
	return detail_.empty() ? 48 : 70;
}

void MessageRow::draw(float x, float y, float w, bool sel) {
	rowHighlight(x, y, w, height(), sel);
	float h = height();
	float blockH = lineHeight(0.5f) + (detail_.empty() ? 0 : lineHeight(0.4f) * 2 + 4);
	float cy = y + (h - blockH) / 2;
	textFit(title_, x + w / 2, cy, 0.5f, col::text, w - 24, Center);
	if (!detail_.empty()) {
		std::vector<std::string> lines;
		int n = wrap(detail_, 0.4f, w - 32, 2, lines);
		for (int i = 0; i < n; i++)
			text(lines[i], x + w / 2, cy + lineHeight(0.5f) + 4 + i * lineHeight(0.4f), 0.4f, col::text2, Center);
	}
}

void MessageRow::activate() { later(onTap_); }

// ---------------------------------------------------------------- LoadingRow

void LoadingRow::draw(float x, float y, float w, bool) { spinner(x + w / 2, y + h_ / 2, 9, col::text2); }

// ---------------------------------------------------------------- ButtonRow

void ButtonRow::draw(float x, float y, float w, bool sel) {
	float bw = std::min(w - 40, textWidth(label_, 0.46f) + 40);
	float bx = x + (w - bw) / 2, by = y + 6;
	u32 bg = primary_ ? col::accent : (sel ? col::surface3 : col::surface2);
	roundRect(bx, by, bw, 32, 16, bg);
	if (sel && primary_) roundRectOutline(bx - 2, by - 2, bw + 4, 36, 18, 2, col::white);
	text(label_, x + w / 2, by + 16 - lineHeight(0.46f) / 2, 0.46f, col::white, Center);
}

void ButtonRow::activate() { later(a_); }

// ---------------------------------------------------------------- ChannelRow

void ChannelRow::draw(float x, float y, float w, bool sel) {
	rowHighlight(x, y, w, height(), sel);
	avatar(name_, x + 30, y + 25, 17);
	textFit(name_, x + 56, y + 25 - lineHeight(0.5f) / 2, 0.5f, col::text, w - 90);
	// Chevron
	line(x + w - 24, y + 19, x + w - 19, y + 25, 1.6f, col::text3);
	line(x + w - 19, y + 25, x + w - 24, y + 31, 1.6f, col::text3);
}

void ChannelRow::activate() { later(onOpen_); }

// ---------------------------------------------------------------- SettingRow

void SettingRow::draw(float x, float y, float w, bool sel) {
	rowHighlight(x, y, w, height(), sel);
	float sc = 0.47f;
	std::string v = value_ ? value_() : "";
	float vw = v.empty() ? 0 : textWidth(v, 0.44f);
	float labelY = hint_.empty() ? y + 20 - lineHeight(sc) / 2 : y + 8;
	textFit(label_, x + 14, labelY, sc, onTap_ ? col::text : col::text2, w - vw - 44);
	if (!hint_.empty()) textFit(hint_, x + 14, y + 8 + lineHeight(sc), 0.38f, col::text3, w - vw - 44);
	if (!v.empty()) {
		bool on = v == "On";
		u32 c = on ? col::blue : col::text2;
		text(v, x + w - 16, y + height() / 2 - lineHeight(0.44f) / 2, 0.44f, c, Right);
	}
}

void SettingRow::activate() { later(onTap_); }

// ---------------------------------------------------------------- SearchBarRow

void SearchBarRow::draw(float x, float y, float w, bool sel) {
	float bx = x + 10, by = y + 7, bw = w - 20, bh = 32;
	roundRect(bx, by, bw, bh, 16, sel ? col::surface3 : col::surface2);
	if (sel) roundRectOutline(bx, by, bw, bh, 16, 1.5f, col::blue);
	iconSearch(bx + 20, by + bh / 2, 15, col::text2);
	std::string q = query_ ? query_() : "";
	if (q.empty()) text("Search YouTube", bx + 36, by + bh / 2 - lineHeight(0.47f) / 2, 0.47f, col::text3);
	else textFit(q, bx + 36, by + bh / 2 - lineHeight(0.47f) / 2, 0.47f, col::text, bw - 48);
}

void SearchBarRow::activate() { later(onTap_); }

// ---------------------------------------------------------------- RecentSearchRow

void RecentSearchRow::draw(float x, float y, float w, bool sel) {
	rowHighlight(x, y, w, height(), sel);
	iconClock(x + 24, y + 18, 15, col::text2);
	textFit(q_, x + 44, y + 18 - lineHeight(0.47f) / 2, 0.47f, col::text, w - 64);
}

void RecentSearchRow::activate() { later(onTap_); }

// ---------------------------------------------------------------- SegmentRow

void SegmentRow::draw(float x, float y, float w, bool sel) {
	lastW_ = w;
	int n = (int)labels_.size();
	int cur = current_ ? current_() : 0;
	float bx = x + 10, by = y + 5, bw = w - 20, bh = 30;
	roundRect(bx, by, bw, bh, 15, col::surface2);
	if (sel) roundRectOutline(bx, by, bw, bh, 15, 1.5f, col::blue);
	float sw = bw / n;
	for (int i = 0; i < n; i++) {
		float sx = bx + i * sw;
		if (i == cur) roundRect(sx + 3, by + 3, sw - 6, bh - 6, 12, col::text);
		text(labels_[i], sx + sw / 2, by + bh / 2 - lineHeight(0.45f) / 2, 0.45f, i == cur ? col::black : col::text,
		     Center);
	}
}

void SegmentRow::activate() {
	int n = (int)labels_.size();
	int next = ((current_ ? current_() : 0) + 1) % n;
	auto pick = onPick_;
	tasks::post([pick, next] { pick(next); });
}

void SegmentRow::tap(float lx, float) {
	int n = (int)labels_.size();
	float sw = (lastW_ - 20) / n;
	int i = (int)((lx - 10) / sw);
	if (i < 0) i = 0;
	if (i >= n) i = n - 1;
	auto pick = onPick_;
	tasks::post([pick, i] { pick(i); });
}

}  // namespace ui
