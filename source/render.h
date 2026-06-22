#pragma once
#include <citro2d.h>

// ---- screen geometry ----
#define TOP_W   400
#define BOT_W   320
#define SCR_H   240

// ---- theme palette (dark, grungy) ----
namespace Pal {
    const u32 bg0      = C2D_Color32(0x14,0x10,0x12,0xFF);
    const u32 bg1      = C2D_Color32(0x05,0x04,0x06,0xFF);
    const u32 panel    = C2D_Color32(0x20,0x1a,0x1c,0xFF);
    const u32 panelHi  = C2D_Color32(0x2c,0x24,0x26,0xFF);
    const u32 line     = C2D_Color32(0x3a,0x30,0x32,0xFF);
    const u32 text     = C2D_Color32(0xe8,0xe0,0xd8,0xFF);
    const u32 textDim  = C2D_Color32(0x9a,0x90,0x88,0xFF);
    const u32 textMute = C2D_Color32(0x60,0x58,0x54,0xFF);
    const u32 red      = C2D_Color32(0xc8,0x2c,0x2c,0xFF);
    const u32 redDim   = C2D_Color32(0x7a,0x22,0x22,0xFF);
    const u32 brass    = C2D_Color32(0xc8,0xa0,0x40,0xFF);
    const u32 steel    = C2D_Color32(0x9a,0x9a,0xa2,0xFF);
    const u32 green    = C2D_Color32(0x5a,0xc8,0x6a,0xFF);
    const u32 amber    = C2D_Color32(0xe0,0xa0,0x30,0xFF);
    const u32 black    = C2D_Color32(0x00,0x00,0x00,0xFF);
    const u32 white    = C2D_Color32(0xff,0xff,0xff,0xFF);
}

// convenience: solid rect at depth 0 (citro2d's C2D_DrawRectSolid needs a z arg)
static inline void RS(float x, float y, float w, float h, u32 c){
    C2D_DrawRectSolid(x, y, 0.0f, w, h, c);
}

void gfxTextInit();
void gfxTextExit();
void gfxTextFrame();                 // clear the per-frame text buffer

// text alignment helpers map onto C2D flags
enum { ALN_L = 0, ALN_C = 1, ALN_R = 2 };

void drawText(float x, float y, float scale, u32 color, int align, const char* fmt, ...);
float measureText(float scale, const char* fmt, ...);

// rounded-ish panel (citro2d has no native rounded rect; we fake corners)
void drawPanel(float x, float y, float w, float h, u32 fill, u32 border);
void drawBar(float x, float y, float w, float h, float frac, u32 fg, u32 bg);
void drawVignette(float w, float h, float strength);
void drawScanlines(float w, float h, float alpha);
void drawDim(float w, float h, float darkness);
