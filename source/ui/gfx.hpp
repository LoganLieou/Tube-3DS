#pragma once

#include <3ds.h>
#include <citro2d.h>

#include <string>
#include <vector>

// Drawing helpers on top of citro2d: theme colors, shapes, text layout, icons.
namespace ui {

constexpr int kTopW = 400, kBotW = 320, kH = 240;

namespace col {
extern const u32 bg, surface, surface2, surface3, divider, text, text2, text3, accent, accentDim, blue,
	overlay, overlayStrong, white, black, green;
}

void init();
void exit();
void beginFrame();  // resets the per-frame text buffer

enum Align { Left, Center, Right };

float lineHeight(float scale);
float textWidth(const std::string& s, float scale);
void text(const std::string& s, float x, float y, float scale, u32 color, Align a = Left);
// Single line, shortened with "…" to fit maxW.
void textFit(const std::string& s, float x, float y, float scale, u32 color, float maxW, Align a = Left);
std::string ellipsize(const std::string& s, float scale, float maxW);
// Greedy word wrap; the last allowed line is ellipsized. Returns line count.
int wrap(const std::string& s, float scale, float maxW, int maxLines, std::vector<std::string>& out);
// Draws wrapped text; returns the height used.
float textBlock(const std::string& s, float x, float y, float scale, u32 color, float maxW, int maxLines);

void rect(float x, float y, float w, float h, u32 c);
void roundRect(float x, float y, float w, float h, float r, u32 c);
void roundRectOutline(float x, float y, float w, float h, float r, float t, u32 c);
void circle(float cx, float cy, float r, u32 c);
void ring(float cx, float cy, float r, float t, u32 c);
void tri(float x0, float y0, float x1, float y1, float x2, float y2, u32 c);
void line(float x0, float y0, float x1, float y1, float t, u32 c);
void gradientV(float x, float y, float w, float h, u32 top, u32 bottom);

// Draws img scaled into (x, y, w, h).
void image(const C2D_Image& img, float x, float y, float w, float h);
// Paints the area outside rounded corners with bg (for rounding images).
void cornerMask(float x, float y, float w, float h, float r, u32 bg);
void spinner(float cx, float cy, float r, u32 c);

// Icons, centered on (cx, cy) with nominal size s.
void iconPlay(float cx, float cy, float s, u32 c);
void iconPause(float cx, float cy, float s, u32 c);
void iconSeek(float cx, float cy, float s, u32 c, bool forward);
void iconHome(float cx, float cy, float s, u32 c);
void iconSearch(float cx, float cy, float s, u32 c);
void iconSubs(float cx, float cy, float s, u32 c);
void iconLibrary(float cx, float cy, float s, u32 c);
void iconSettings(float cx, float cy, float s, u32 c);
void iconClock(float cx, float cy, float s, u32 c);
void iconBookmark(float cx, float cy, float s, u32 c, bool filled);
void iconBack(float cx, float cy, float s, u32 c);
void iconWifi(float cx, float cy, float s, u32 c, int bars);
void iconBattery(float x, float y, u32 c, int level, bool charging);
void logo(float x, float y, float h);

// Avatar placeholder: colored circle with the first letter of name.
void avatar(const std::string& name, float cx, float cy, float r);

// Transient message shown at the bottom of the bottom screen.
void toast(const std::string& msg, u32 ms = 2200);
void drawToast();

}  // namespace ui
