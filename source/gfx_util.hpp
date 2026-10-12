#pragma once

#include <3ds.h>
#include <citro2d.h>

#include <vector>

// Helpers for writing pixels straight into PICA200 tiled textures.
//
// 3DS textures are stored as 8x8 tiles in Z-order, tile rows top to bottom
// (as drawn by citro2d with a top = 1.0 subtexture). TiledLayout precomputes per-column and
// per-row offsets so converters can write pixel (x, y) at xoff[x] + yoff[y].
struct TiledLayout {
	int texW = 0, texH = 0;
	std::vector<u32> xoff;
	std::vector<u32> yoff;

	void init(int w, int h);
	u32 offset(int x, int y) const { return xoff[x] + yoff[y]; }
};

u32 nextPow2(u32 v);

// Converts a YUV 4:2:0 image (BT.601, limited range) into an RGB565 tiled texture.
void yuv420ToTiled565(const u8* Y, int ys, const u8* U, int us, const u8* V, int vs, int w, int h,
                      u16* dst, const TiledLayout& layout);

// Converts packed RGB888 into an RGB565 tiled texture, resampling from (sw, sh)
// to (dw, dh) with a bilinear filter.
void rgbToTiled565(const u8* rgb, int sw, int sh, int dw, int dh, u16* dst, const TiledLayout& layout);

// Subtexture covering the top-left (w, h) of a (texW, texH) texture.
Tex3DS_SubTexture makeSubTexture(int w, int h, int texW, int texH);

// Textures can't be freed while the GPU may still be drawing the previous frame.
// deferTexFree queues a texture; processDeferredFrees (called right after
// C3D_FrameBegin, when the previous frame's GPU work has finished) frees them.
void deferTexFree(const C3D_Tex& tex);
void processDeferredFrees();

// libctru's linear-heap allocator (used for textures and DSP buffers) has no
// locking. Every linearAlloc/linearFree made after startup, including the ones
// inside C3D_TexInit/C3D_TexDelete, must go through these helpers.
void lockLinearHeap();
void unlockLinearHeap();
bool texInitLocked(C3D_Tex* tex, u16 w, u16 h, GPU_TEXCOLOR fmt);
void texDeleteLocked(C3D_Tex* tex);
