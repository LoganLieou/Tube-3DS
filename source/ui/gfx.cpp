#include "ui/gfx.hpp"

#include <cmath>
#include <cstring>
#include <unordered_map>

#include "util.hpp"

namespace ui {

// citro2d colors are ABGR.
#define RGBA(r, g, b, a) ((u32)(r) | ((u32)(g) << 8) | ((u32)(b) << 16) | ((u32)(a) << 24))

namespace col {
const u32 bg = RGBA(15, 15, 15, 255);
const u32 surface = RGBA(28, 28, 28, 255);
const u32 surface2 = RGBA(39, 39, 39, 255);
const u32 surface3 = RGBA(56, 56, 56, 255);
const u32 divider = RGBA(48, 48, 48, 255);
const u32 text = RGBA(241, 241, 241, 255);
const u32 text2 = RGBA(170, 170, 170, 255);
const u32 text3 = RGBA(113, 113, 113, 255);
const u32 accent = RGBA(255, 0, 51, 255);
const u32 accentDim = RGBA(255, 0, 51, 70);
const u32 blue = RGBA(62, 166, 255, 255);
const u32 overlay = RGBA(0, 0, 0, 150);
const u32 overlayStrong = RGBA(0, 0, 0, 205);
const u32 white = RGBA(255, 255, 255, 255);
const u32 black = RGBA(0, 0, 0, 255);
const u32 green = RGBA(43, 166, 64, 255);
}  // namespace col

static C2D_TextBuf g_buf = nullptr;
static constexpr float kDepth = 0.5f;
static float g_lineFeed = 30.0f;
static u8 g_asciiW[128];
static std::unordered_map<u32, u8> g_wideW;

static constexpr int kArcSeg = 6;  // segments per quarter circle
static float g_cos[kArcSeg * 4 + 1], g_sin[kArcSeg * 4 + 1];

static float charWidth(u32 cp) {
	if (cp < 128) return g_asciiW[cp];
	auto it = g_wideW.find(cp);
	if (it != g_wideW.end()) return it->second;
	CFNT_s* font = fontGetSystemFont();
	charWidthInfo_s* w = fontGetCharWidthInfo(font, fontGlyphIndexFromCodePoint(font, cp));
	u8 v = w ? w->charWidth : 0;
	if (g_wideW.size() < 4096) g_wideW[cp] = v;
	return v;
}

void init() {
	g_buf = C2D_TextBufNew(8192);
	CFNT_s* font = fontGetSystemFont();
	FINF_s* finf = fontGetInfo(font);
	if (finf && finf->lineFeed) g_lineFeed = finf->lineFeed;
	for (u32 c = 0; c < 128; c++) {
		charWidthInfo_s* w = fontGetCharWidthInfo(font, fontGlyphIndexFromCodePoint(font, c));
		g_asciiW[c] = w ? w->charWidth : 0;
	}
	for (int i = 0; i <= kArcSeg * 4; i++) {
		float a = (float)i / (kArcSeg * 4) * 2.0f * (float)M_PI;
		g_cos[i] = cosf(a);
		g_sin[i] = sinf(a);
	}
}

void exit() {
	if (g_buf) C2D_TextBufDelete(g_buf);
	g_buf = nullptr;
}

void beginFrame() { C2D_TextBufClear(g_buf); }

float lineHeight(float scale) { return g_lineFeed * scale; }

float textWidth(const std::string& s, float scale) {
	float w = 0;
	const char* p = s.data();
	const char* end = p + s.size();
	while (p < end) w += charWidth(util::utf8Next(p, end));
	return w * scale;
}

static u32 alignFlag(Align a) {
	return a == Center ? C2D_AlignCenter : a == Right ? C2D_AlignRight : C2D_AlignLeft;
}

void text(const std::string& s, float x, float y, float scale, u32 color, Align a) {
	if (s.empty()) return;
	C2D_Text t;
	C2D_TextFontParse(&t, nullptr, g_buf, s.c_str());
	C2D_DrawText(&t, C2D_WithColor | alignFlag(a), x, y, kDepth, scale, scale, color);
}

std::string ellipsize(const std::string& s, float scale, float maxW) {
	if (textWidth(s, scale) <= maxW) return s;
	static const std::string dots = "\xE2\x80\xA6";  // U+2026
	float limit = maxW - textWidth(dots, scale);
	float w = 0;
	const char* begin = s.data();
	const char* p = begin;
	const char* end = p + s.size();
	const char* cut = begin;
	while (p < end) {
		const char* q = p;
		float cw = charWidth(util::utf8Next(q, end)) * scale;
		if (w + cw > limit) break;
		w += cw;
		p = q;
		cut = p;
	}
	std::string out(begin, cut);
	while (!out.empty() && out.back() == ' ') out.pop_back();
	return out + dots;
}

void textFit(const std::string& s, float x, float y, float scale, u32 color, float maxW, Align a) {
	text(ellipsize(s, scale, maxW), x, y, scale, color, a);
}

int wrap(const std::string& src, float scale, float maxW, int maxLines, std::vector<std::string>& out) {
	out.clear();
	if (src.empty() || maxLines <= 0) return 0;
	const char* begin = src.data();
	const char* end = begin + src.size();
	const char* lineStart = begin;
	const char* lastSpace = nullptr;
	const char* p = begin;
	float w = 0;

	auto finishRest = [&](const char* from) {
		std::string rest(from, end);
		for (auto& ch : rest)
			if (ch == '\n') ch = ' ';
		out.push_back(ellipsize(util::trim(rest), scale, maxW));
	};

	while (p < end) {
		if ((int)out.size() == maxLines - 1) {
			finishRest(lineStart);
			return (int)out.size();
		}
		if (*p == '\n') {
			out.emplace_back(lineStart, p);
			p++;
			lineStart = p;
			lastSpace = nullptr;
			w = 0;
			continue;
		}
		const char* q = p;
		u32 cp = util::utf8Next(q, end);
		float cw = charWidth(cp) * scale;
		if (w + cw > maxW && p > lineStart) {
			if (lastSpace && lastSpace > lineStart) {
				out.emplace_back(lineStart, lastSpace);
				lineStart = lastSpace + 1;
			} else {
				out.emplace_back(lineStart, p);
				lineStart = p;
			}
			lastSpace = nullptr;
			w = textWidth(std::string(lineStart, p), scale);
			continue;  // re-evaluate this character on the new line
		}
		if (cp == ' ') lastSpace = p;
		w += cw;
		p = q;
	}
	if (lineStart < end) out.emplace_back(lineStart, end);
	return (int)out.size();
}

float textBlock(const std::string& s, float x, float y, float scale, u32 color, float maxW, int maxLines) {
	std::vector<std::string> lines;
	int n = wrap(s, scale, maxW, maxLines, lines);
	float lh = lineHeight(scale);
	for (int i = 0; i < n; i++) text(lines[i], x, y + i * lh, scale, color);
	return n * lh;
}

// ---------------------------------------------------------------- shapes

void rect(float x, float y, float w, float h, u32 c) {
	if (w <= 0 || h <= 0) return;
	C2D_DrawRectSolid(x, y, kDepth, w, h, c);
}

void tri(float x0, float y0, float x1, float y1, float x2, float y2, u32 c) {
	C2D_DrawTriangle(x0, y0, c, x1, y1, c, x2, y2, c, kDepth);
}

static void quarterFan(float cx, float cy, float r, int quadrant, u32 c) {
	int base = quadrant * kArcSeg;
	for (int i = 0; i < kArcSeg; i++) {
		int a = base + i, b = base + i + 1;
		tri(cx, cy, cx + g_cos[a] * r, cy + g_sin[a] * r, cx + g_cos[b] * r, cy + g_sin[b] * r, c);
	}
}

void roundRect(float x, float y, float w, float h, float r, u32 c) {
	if (r > w / 2) r = w / 2;
	if (r > h / 2) r = h / 2;
	if (r < 1) {
		rect(x, y, w, h, c);
		return;
	}
	rect(x + r, y, w - 2 * r, h, c);
	rect(x, y + r, r, h - 2 * r, c);
	rect(x + w - r, y + r, r, h - 2 * r, c);
	// Quadrants in screen space (y down): 0 = bottom-right, 1 = bottom-left, 2 = top-left, 3 = top-right.
	quarterFan(x + w - r, y + h - r, r, 0, c);
	quarterFan(x + r, y + h - r, r, 1, c);
	quarterFan(x + r, y + r, r, 2, c);
	quarterFan(x + w - r, y + r, r, 3, c);
}

static void arcBand(float cx, float cy, float r, float t, int from, int to, u32 c) {
	float ri = r - t;
	for (int i = from; i < to; i++) {
		int a = i % (kArcSeg * 4), b = (i + 1) % (kArcSeg * 4);
		float ax = g_cos[a], ay = g_sin[a], bx = g_cos[b], by = g_sin[b];
		tri(cx + ax * r, cy + ay * r, cx + bx * r, cy + by * r, cx + ax * ri, cy + ay * ri, c);
		tri(cx + bx * r, cy + by * r, cx + bx * ri, cy + by * ri, cx + ax * ri, cy + ay * ri, c);
	}
}

void roundRectOutline(float x, float y, float w, float h, float r, float t, u32 c) {
	if (r > w / 2) r = w / 2;
	if (r > h / 2) r = h / 2;
	rect(x + r, y, w - 2 * r, t, c);
	rect(x + r, y + h - t, w - 2 * r, t, c);
	rect(x, y + r, t, h - 2 * r, c);
	rect(x + w - t, y + r, t, h - 2 * r, c);
	arcBand(x + w - r, y + h - r, r, t, 0, kArcSeg, c);
	arcBand(x + r, y + h - r, r, t, kArcSeg, 2 * kArcSeg, c);
	arcBand(x + r, y + r, r, t, 2 * kArcSeg, 3 * kArcSeg, c);
	arcBand(x + w - r, y + r, r, t, 3 * kArcSeg, 4 * kArcSeg, c);
}

void circle(float cx, float cy, float r, u32 c) { C2D_DrawCircleSolid(cx, cy, kDepth, r, c); }

void ring(float cx, float cy, float r, float t, u32 c) { arcBand(cx, cy, r, t, 0, kArcSeg * 4, c); }

void line(float x0, float y0, float x1, float y1, float t, u32 c) {
	C2D_DrawLine(x0, y0, c, x1, y1, c, t, kDepth);
}

void gradientV(float x, float y, float w, float h, u32 top, u32 bottom) {
	C2D_DrawRectangle(x, y, kDepth, w, h, top, top, bottom, bottom);
}

void image(const C2D_Image& img, float x, float y, float w, float h) {
	if (!img.tex || !img.subtex || img.subtex->width == 0) return;
	C2D_DrawImageAt(img, x, y, kDepth, nullptr, w / img.subtex->width, h / img.subtex->height);
}

void cornerMask(float x, float y, float w, float h, float r, u32 bg) {
	struct Corner {
		float px, py, cx, cy;
		int quadrant;
	} corners[4] = {
		{x + w, y + h, x + w - r, y + h - r, 0},
		{x, y + h, x + r, y + h - r, 1},
		{x, y, x + r, y + r, 2},
		{x + w, y, x + w - r, y + r, 3},
	};
	for (const auto& k : corners) {
		int base = k.quadrant * kArcSeg;
		for (int i = 0; i < kArcSeg; i++) {
			int a = base + i, b = base + i + 1;
			tri(k.px, k.py, k.cx + g_cos[a] * r, k.cy + g_sin[a] * r, k.cx + g_cos[b] * r, k.cy + g_sin[b] * r, bg);
		}
	}
}

void spinner(float cx, float cy, float r, u32 c) {
	u64 t = util::nowMs();
	int head = (int)((t / 80) % 12);
	for (int i = 0; i < 12; i++) {
		float a = (float)i / 12 * 2 * (float)M_PI;
		int age = (head - i + 12) % 12;
		u32 alpha = (u32)(255 - age * 19);
		u32 cc = (c & 0x00FFFFFF) | (alpha << 24);
		circle(cx + cosf(a) * r, cy + sinf(a) * r, r * 0.16f + 0.6f, cc);
	}
}

// ---------------------------------------------------------------- icons

void iconPlay(float cx, float cy, float s, u32 c) {
	tri(cx - s * 0.32f, cy - s * 0.42f, cx - s * 0.32f, cy + s * 0.42f, cx + s * 0.42f, cy, c);
}

void iconPause(float cx, float cy, float s, u32 c) {
	rect(cx - s * 0.34f, cy - s * 0.4f, s * 0.24f, s * 0.8f, c);
	rect(cx + s * 0.10f, cy - s * 0.4f, s * 0.24f, s * 0.8f, c);
}

void iconSeek(float cx, float cy, float s, u32 c, bool fwd) {
	float d = fwd ? 1 : -1;
	for (int k = 0; k < 2; k++) {
		float ox = cx + d * (k == 0 ? -s * 0.22f : s * 0.22f);
		tri(ox - d * s * 0.24f, cy - s * 0.3f, ox - d * s * 0.24f, cy + s * 0.3f, ox + d * s * 0.24f, cy, c);
	}
}

void iconHome(float cx, float cy, float s, u32 c) {
	tri(cx - s * 0.5f, cy - s * 0.02f, cx, cy - s * 0.5f, cx + s * 0.5f, cy - s * 0.02f, c);
	rect(cx - s * 0.34f, cy - s * 0.04f, s * 0.68f, s * 0.48f, c);
}

void iconSearch(float cx, float cy, float s, u32 c) {
	ring(cx - s * 0.1f, cy - s * 0.1f, s * 0.32f, s * 0.11f, c);
	line(cx + s * 0.12f, cy + s * 0.12f, cx + s * 0.42f, cy + s * 0.42f, s * 0.13f, c);
}

void iconSubs(float cx, float cy, float s, u32 c) {
	rect(cx - s * 0.32f, cy - s * 0.48f, s * 0.64f, s * 0.08f, c);
	roundRectOutline(cx - s * 0.46f, cy - s * 0.32f, s * 0.92f, s * 0.72f, s * 0.1f, s * 0.09f, c);
	tri(cx - s * 0.1f, cy - s * 0.12f, cx - s * 0.1f, cy + s * 0.2f, cx + s * 0.16f, cy + s * 0.04f, c);
}

void iconLibrary(float cx, float cy, float s, u32 c) {
	for (int i = 0; i < 3; i++) {
		float y = cy - s * 0.32f + i * s * 0.3f;
		rect(cx - s * 0.45f, y, s * 0.14f, s * 0.12f, c);
		rect(cx - s * 0.22f, y, s * 0.66f, s * 0.12f, c);
	}
}

void iconSettings(float cx, float cy, float s, u32 c) {
	for (int i = 0; i < 8; i++) {
		float a = i * (float)M_PI / 4;
		line(cx + cosf(a) * s * 0.22f, cy + sinf(a) * s * 0.22f, cx + cosf(a) * s * 0.48f, cy + sinf(a) * s * 0.48f,
		     s * 0.17f, c);
	}
	ring(cx, cy, s * 0.36f, s * 0.2f, c);
}

void iconClock(float cx, float cy, float s, u32 c) {
	ring(cx, cy, s * 0.45f, s * 0.09f, c);
	line(cx, cy, cx, cy - s * 0.26f, s * 0.08f, c);
	line(cx, cy, cx + s * 0.18f, cy + s * 0.1f, s * 0.08f, c);
}

void iconBookmark(float cx, float cy, float s, u32 c, bool filled) {
	float l = cx - s * 0.3f, r = cx + s * 0.3f, t = cy - s * 0.45f, b = cy + s * 0.45f;
	if (filled) {
		rect(l, t, r - l, b - t - s * 0.02f, c);
		tri(l, b, cx, b - s * 0.28f, cx, b, col::bg);  // notch drawn in background color
		tri(cx, b, cx, b - s * 0.28f, r, b, col::bg);
	} else {
		float th = s * 0.09f;
		rect(l, t, r - l, th, c);
		rect(l, t, th, b - t, c);
		rect(r - th, t, th, b - t, c);
		line(l, b, cx, b - s * 0.28f, th, c);
		line(cx, b - s * 0.28f, r, b, th, c);
	}
}

void iconBack(float cx, float cy, float s, u32 c) {
	line(cx + s * 0.15f, cy - s * 0.35f, cx - s * 0.2f, cy, s * 0.12f, c);
	line(cx - s * 0.2f, cy, cx + s * 0.15f, cy + s * 0.35f, s * 0.12f, c);
}

void iconWifi(float cx, float cy, float s, u32 c, int bars) {
	for (int i = 0; i < 3; i++) {
		float h = s * (0.3f + i * 0.3f);
		u32 cc = i < bars ? c : ((c & 0x00FFFFFF) | 0x50000000);
		rect(cx - s * 0.45f + i * s * 0.34f, cy + s * 0.45f - h, s * 0.22f, h, cc);
	}
}

void iconBattery(float x, float y, u32 c, int level, bool charging) {
	roundRectOutline(x, y, 20, 10, 2, 1.2f, c);
	rect(x + 20, y + 3, 2, 4, c);
	u32 fill = charging ? col::green : (level <= 1 ? col::accent : c);
	float w = 16.0f * (level < 0 ? 0 : level > 5 ? 5 : level) / 5.0f;
	rect(x + 2, y + 2, w, 6, fill);
}

void logo(float x, float y, float h) {
	float w = h * 1.42f;
	roundRect(x, y, w, h, h * 0.28f, col::accent);
	iconPlay(x + w / 2 + h * 0.04f, y + h / 2, h * 0.55f, col::white);
}

void avatar(const std::string& name, float cx, float cy, float r) {
	static const u32 palette[] = {
		RGBA(229, 57, 53, 255), RGBA(142, 36, 170, 255), RGBA(57, 73, 171, 255), RGBA(3, 155, 229, 255),
		RGBA(0, 137, 123, 255), RGBA(124, 179, 66, 255), RGBA(251, 140, 0, 255), RGBA(109, 76, 65, 255),
	};
	u32 h = 2166136261u;
	for (unsigned char ch : name) h = (h ^ ch) * 16777619u;
	circle(cx, cy, r, palette[h % 8]);
	std::string first;
	if (!name.empty()) {
		const char* p = name.data();
		const char* end = p + name.size();
		if (*p == '@' && name.size() > 1) p++;
		const char* q = p;
		util::utf8Next(q, end);
		first.assign(p, q);
		if (first.size() == 1) first[0] = (char)toupper((unsigned char)first[0]);
	}
	float sc = r / 22.0f;
	text(first, cx, cy - lineHeight(sc) / 2, sc, col::white, Center);
}

// ---------------------------------------------------------------- toast

static std::string g_toast;
static u64 g_toastUntil = 0;

void toast(const std::string& msg, u32 ms) {
	g_toast = msg;
	g_toastUntil = util::nowMs() + ms;
}

void drawToast() {
	if (g_toast.empty() || util::nowMs() > g_toastUntil) return;
	float sc = 0.45f;
	float w = textWidth(g_toast, sc) + 24;
	if (w > 300) w = 300;
	float x = (kBotW - w) / 2, y = 176;
	roundRect(x, y, w, 24, 12, RGBA(50, 50, 50, 240));
	textFit(g_toast, kBotW / 2.0f, y + 12 - lineHeight(sc) / 2, sc, col::white, w - 16, Center);
}

}  // namespace ui
