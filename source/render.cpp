#include "render.h"
#include <cstdio>
#include <cstdarg>
#include <cmath>

static C2D_TextBuf s_buf;

void gfxTextInit(){ s_buf = C2D_TextBufNew(4096); }
void gfxTextExit(){ C2D_TextBufDelete(s_buf); }
void gfxTextFrame(){ C2D_TextBufClear(s_buf); }

static void formatv(char* out, size_t n, const char* fmt, va_list ap){
    vsnprintf(out, n, fmt, ap);
}

void drawText(float x, float y, float scale, u32 color, int align, const char* fmt, ...){
    char tmp[256];
    va_list ap; va_start(ap, fmt); formatv(tmp, sizeof(tmp), fmt, ap); va_end(ap);
    C2D_Text t;
    C2D_TextParse(&t, s_buf, tmp);
    C2D_TextOptimize(&t);
    u32 flags = C2D_WithColor;
    if (align == ALN_C) flags |= C2D_AlignCenter;
    else if (align == ALN_R) flags |= C2D_AlignRight;
    C2D_DrawText(&t, flags, x, y, 0.5f, scale, scale, color);
}

float measureText(float scale, const char* fmt, ...){
    char tmp[256];
    va_list ap; va_start(ap, fmt); formatv(tmp, sizeof(tmp), fmt, ap); va_end(ap);
    C2D_Text t;
    C2D_TextParse(&t, s_buf, tmp);
    C2D_TextOptimize(&t);
    float w=0,h=0;
    C2D_TextGetDimensions(&t, scale, scale, &w, &h);
    return w;
}

void drawPanel(float x, float y, float w, float h, u32 fill, u32 border){
    const float c = 3.0f; // corner cut
    // body
    RS(x+c, y, w-2*c, h, fill);
    RS(x, y+c, w, h-2*c, fill);
    // corner blocks (small) to soften
    RS(x+1, y+1, c, c, fill);
    RS(x+w-c-1, y+1, c, c, fill);
    RS(x+1, y+h-c-1, c, c, fill);
    RS(x+w-c-1, y+h-c-1, c, c, fill);
    // border lines
    RS(x+c, y, w-2*c, 1, border);
    RS(x+c, y+h-1, w-2*c, 1, border);
    RS(x, y+c, 1, h-2*c, border);
    RS(x+w-1, y+c, 1, h-2*c, border);
}

void drawBar(float x, float y, float w, float h, float frac, u32 fg, u32 bg){
    if (frac < 0) frac = 0; if (frac > 1) frac = 1;
    RS(x, y, w, h, bg);
    RS(x, y, w*frac, h, fg);
}

void drawVignette(float w, float h, float strength){
    // layered translucent black frames -> soft darkening toward edges
    int layers = 7;
    for (int i=0;i<layers;i++){
        float t = (float)i/layers;
        u32 a = (u32)(strength * 26 * (1.0f - t));
        u32 col = C2D_Color32(0,0,0,a);
        float inset = i*3.0f;
        // top & bottom
        RS(0, inset, w, 3, col);
        RS(0, h-inset-3, w, 3, col);
        RS(inset, 0, 3, h, col);
        RS(w-inset-3, 0, 3, h, col);
    }
}

void drawScanlines(float w, float h, float alpha){
    u32 col = C2D_Color32(0,0,0,(u32)(alpha*255));
    for (float y=0; y<h; y+=2.0f)
        RS(0, y, w, 1, col);
}

void drawDim(float w, float h, float darkness){
    if (darkness <= 0.001f) return;
    RS(0,0,w,h, C2D_Color32(0,0,0,(u32)(darkness*255)));
}
