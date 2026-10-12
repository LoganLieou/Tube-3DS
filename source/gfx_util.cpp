#include "gfx_util.hpp"

#include <cstring>

#include "util.hpp"

u32 nextPow2(u32 v) {
	u32 p = 8;
	while (p < v) p <<= 1;
	return p;
}

static inline u32 mortonX(u32 b) { return (b & 1) | ((b & 2) << 1) | ((b & 4) << 2); }
static inline u32 mortonY(u32 b) { return ((b & 1) << 1) | ((b & 2) << 2) | ((b & 4) << 3); }

void TiledLayout::init(int w, int h) {
	texW = w;
	texH = h;
	xoff.resize(w);
	yoff.resize(h);
	for (int x = 0; x < w; x++) xoff[x] = (u32)(x >> 3) * 64 + mortonX(x & 7);
	// Rows are stored top-down: with citro2d's subtexture convention (top = 1.0)
	// the first row in memory is the top of the image.
	for (int y = 0; y < h; y++) yoff[y] = (u32)((y >> 3) * (w >> 3)) * 64 + mortonY(y & 7);
}

Tex3DS_SubTexture makeSubTexture(int w, int h, int texW, int texH) {
	Tex3DS_SubTexture s;
	s.width = (u16)w;
	s.height = (u16)h;
	s.left = 0.0f;
	s.top = 1.0f;
	s.right = (float)w / texW;
	s.bottom = 1.0f - (float)h / texH;
	return s;
}

// ---------------------------------------------------------------- YUV -> RGB565

namespace {
struct YuvTables {
	int y[256];
	int rv[256], gu[256], gv[256], bu[256];
	// Clamp tables indexed by (value >> 8) + 384; already shifted into 565 position.
	u16 r[1024], g[1024], b[1024];

	YuvTables() {
		for (int i = 0; i < 256; i++) {
			y[i] = 298 * (i - 16) + 128;
			rv[i] = 409 * (i - 128);
			gu[i] = -100 * (i - 128);
			gv[i] = -208 * (i - 128);
			bu[i] = 516 * (i - 128);
		}
		for (int i = 0; i < 1024; i++) {
			int v = i - 384;
			if (v < 0) v = 0;
			if (v > 255) v = 255;
			r[i] = (u16)((v >> 3) << 11);
			g[i] = (u16)((v >> 2) << 5);
			b[i] = (u16)(v >> 3);
		}
	}
};
const YuvTables& tables() {
	static YuvTables t;
	return t;
}
}  // namespace

static inline u16 px(const YuvTables& t, int yy, int r, int g, int b) {
	return t.r[((yy + r) >> 8) + 384] | t.g[((yy + g) >> 8) + 384] | t.b[((yy + b) >> 8) + 384];
}

void yuv420ToTiled565(const u8* Y, int ys, const u8* U, int us, const u8* V, int vs, int w, int h,
                      u16* dst, const TiledLayout& L) {
	const YuvTables& t = tables();
	if (w > L.texW) w = L.texW;
	if (h > L.texH) h = L.texH;
	const u32* xo = L.xoff.data();
	for (int y = 0; y + 1 < h; y += 2) {
		const u8* y0 = Y + y * ys;
		const u8* y1 = y0 + ys;
		const u8* u = U + (y >> 1) * us;
		const u8* v = V + (y >> 1) * vs;
		u16* d0 = dst + L.yoff[y];
		u16* d1 = dst + L.yoff[y + 1];
		for (int x = 0; x + 1 < w; x += 2) {
			int cu = u[x >> 1], cv = v[x >> 1];
			int r = t.rv[cv], g = t.gu[cu] + t.gv[cv], b = t.bu[cu];
			u32 o0 = xo[x], o1 = xo[x + 1];
			d0[o0] = px(t, t.y[y0[x]], r, g, b);
			d0[o1] = px(t, t.y[y0[x + 1]], r, g, b);
			d1[o0] = px(t, t.y[y1[x]], r, g, b);
			d1[o1] = px(t, t.y[y1[x + 1]], r, g, b);
		}
		if (w & 1) {
			int x = w - 1;
			int cu = u[x >> 1], cv = v[x >> 1];
			int r = t.rv[cv], g = t.gu[cu] + t.gv[cv], b = t.bu[cu];
			d0[xo[x]] = px(t, t.y[y0[x]], r, g, b);
			d1[xo[x]] = px(t, t.y[y1[x]], r, g, b);
		}
	}
	if (h & 1) {
		int y = h - 1;
		const u8* y0 = Y + y * ys;
		const u8* u = U + (y >> 1) * us;
		const u8* v = V + (y >> 1) * vs;
		u16* d0 = dst + L.yoff[y];
		for (int x = 0; x < w; x++) {
			int cu = u[x >> 1], cv = v[x >> 1];
			d0[xo[x]] = px(t, t.y[y0[x]], t.rv[cv], t.gu[cu] + t.gv[cv], t.bu[cu]);
		}
	}
	// Replicate the last column/row into the padding so bilinear filtering
	// doesn't pull in black at the edges.
	if (w < L.texW)
		for (int y = 0; y < h; y++) dst[L.yoff[y] + xo[w]] = dst[L.yoff[y] + xo[w - 1]];
	if (h < L.texH)
		for (int x = 0; x < w; x++) dst[L.yoff[h] + xo[x]] = dst[L.yoff[h - 1] + xo[x]];
}

void rgbToTiled565(const u8* rgb, int sw, int sh, int dw, int dh, u16* dst, const TiledLayout& L) {
	if (dw > L.texW) dw = L.texW;
	if (dh > L.texH) dh = L.texH;
	// 16.16 fixed-point source step, sampling pixel centers.
	const u32 sx = ((u32)sw << 16) / dw;
	const u32 sy = ((u32)sh << 16) / dh;
	for (int y = 0; y < dh; y++) {
		s32 fy = (s32)(y * sy + (sy >> 1)) - 0x8000;
		if (fy < 0) fy = 0;
		int y0 = fy >> 16;
		int y1 = y0 + 1 < sh ? y0 + 1 : y0;
		u32 wy = (fy >> 8) & 0xFF;
		const u8* r0 = rgb + y0 * sw * 3;
		const u8* r1 = rgb + y1 * sw * 3;
		u16* d = dst + L.yoff[y];
		for (int x = 0; x < dw; x++) {
			s32 fx = (s32)(x * sx + (sx >> 1)) - 0x8000;
			if (fx < 0) fx = 0;
			int x0 = fx >> 16;
			int x1 = x0 + 1 < sw ? x0 + 1 : x0;
			u32 wx = (fx >> 8) & 0xFF;
			const u8* a = r0 + x0 * 3;
			const u8* b = r0 + x1 * 3;
			const u8* c = r1 + x0 * 3;
			const u8* e = r1 + x1 * 3;
			u32 out[3];
			for (int k = 0; k < 3; k++) {
				u32 top = a[k] * (256 - wx) + b[k] * wx;
				u32 bot = c[k] * (256 - wx) + e[k] * wx;
				out[k] = (top * (256 - wy) + bot * wy) >> 16;
			}
			d[L.xoff[x]] = (u16)(((out[0] >> 3) << 11) | ((out[1] >> 2) << 5) | (out[2] >> 3));
		}
	}
	if (dw < L.texW)
		for (int y = 0; y < dh; y++) dst[L.yoff[y] + L.xoff[dw]] = dst[L.yoff[y] + L.xoff[dw - 1]];
	if (dh < L.texH)
		for (int x = 0; x < dw; x++) dst[L.yoff[dh] + L.xoff[x]] = dst[L.yoff[dh - 1] + L.xoff[x]];
}

// ---------------------------------------------------------------- linear heap

static Mutex g_linearLock;

void lockLinearHeap() { g_linearLock.lock(); }
void unlockLinearHeap() { g_linearLock.unlock(); }

bool texInitLocked(C3D_Tex* tex, u16 w, u16 h, GPU_TEXCOLOR fmt) {
	LockGuard g(g_linearLock);
	return C3D_TexInit(tex, w, h, fmt);
}

void texDeleteLocked(C3D_Tex* tex) {
	LockGuard g(g_linearLock);
	C3D_TexDelete(tex);
}

// ---------------------------------------------------------------- deferred frees

static Mutex g_deferLock;
static std::vector<C3D_Tex> g_deferred;  // queued this frame
static std::vector<C3D_Tex> g_pending;   // queued last frame; safe to free after FrameBegin

void deferTexFree(const C3D_Tex& tex) {
	LockGuard g(g_deferLock);
	g_deferred.push_back(tex);
}

void processDeferredFrees() {
	LockGuard g(g_deferLock);
	for (auto& t : g_pending) texDeleteLocked(&t);
	g_pending.swap(g_deferred);
	g_deferred.clear();
}
