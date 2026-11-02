#include "proxy.h"

/* ------------------------------------------------------------------ FPS counter
 *
 * Drawn on the back buffer just before Present with ColorFill (one call per horizontal run of
 * font pixels). ColorFill ignores render states, viewport, scissor and the current render target,
 * so the game's state is never touched (also safe on a pure device).
 */
static const BYTE g_font[][7] = {   /* 5x7, bit 4 = leftmost column */
    {14,17,19,21,25,17,14}, {4,12,4,4,4,4,14}, {14,17,1,2,4,8,31}, {31,2,4,2,1,17,14},
    {2,6,10,18,31,2,2}, {31,16,30,1,1,17,14}, {6,8,16,30,17,17,14}, {31,1,2,4,8,8,8},
    {14,17,17,14,17,17,14}, {14,17,17,15,1,2,12},
    {31,16,16,30,16,16,16} /* F */, {30,17,17,30,16,16,16} /* P */, {15,16,16,14,1,1,30} /* S */,
    {0,0,0,0,0,0,0} /* space */,
};

static int glyph(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    return c == 'F' ? 10 : c == 'P' ? 11 : c == 'S' ? 12 : 13;
}

void draw_fps(IDirect3DDevice9 *dev)
{
    static LARGE_INTEGER freq, last;
    static int frames, fps = -1;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!freq.QuadPart) { QueryPerformanceFrequency(&freq); last = now; }
    frames++;
    double dt = (double)(now.QuadPart - last.QuadPart) / (double)freq.QuadPart;
    if (dt >= 0.5) { fps = (int)(frames / dt + 0.5); frames = 0; last = now; }
    if (fps < 0) return;

    IDirect3DSurface9 *bb = NULL;
    if (FAILED(IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return;
    D3DSURFACE_DESC d;
    if (SUCCEEDED(IDirect3DSurface9_GetDesc(bb, &d))) {
        char text[16];
        int n = snprintf(text, sizeof text, "%d FPS", fps > 9999 ? 9999 : fps);
        int px = ((int)d.Height + 245) / 490;              /* font pixel size: 1 at 600p, 2 at 1080p */
        if (px < 1) px = 1;
        int pad = 2 * px, w = n * 6 * px - px + 2 * pad, h = 7 * px + 2 * pad;
        int x0 = (int)d.Width - w - 4 * px, y0 = 4 * px;
        if (x0 >= 0) {
            RECT r = { x0, y0, x0 + w, y0 + h };
            IDirect3DDevice9_ColorFill(dev, bb, &r, D3DCOLOR_XRGB(16, 16, 16));
            for (int i = 0; i < n; i++) {
                const BYTE *g = g_font[glyph(text[i])];
                int gx = x0 + pad + i * 6 * px;
                for (int row = 0; row < 7; row++)
                    for (int col = 0; col < 5;) {
                        if (!(g[row] & (16 >> col))) { col++; continue; }
                        int c1 = col;
                        while (c1 < 5 && (g[row] & (16 >> c1))) c1++;
                        RECT p = { gx + col * px, y0 + pad + row * px, gx + c1 * px, y0 + pad + (row + 1) * px };
                        IDirect3DDevice9_ColorFill(dev, bb, &p, D3DCOLOR_XRGB(255, 255, 255));
                        col = c1;
                    }
            }
        }
    }
    IDirect3DSurface9_Release(bb);
}

/* Frame rate cap: wait until 1/limit s after the previous frame's deadline. Sleep(1) with a 1 ms
 * timer resolution for most of the wait, then spin for the last ~2 ms for an even frame pace. */
void limit_fps(void)
{
    static LARGE_INTEGER freq, next;
    LARGE_INTEGER now;
    if (!freq.QuadPart) {
        QueryPerformanceFrequency(&freq);
        timeBeginPeriod(1);
    }
    LONGLONG period = freq.QuadPart / g_fpsLimit;
    QueryPerformanceCounter(&now);
    if (!next.QuadPart || now.QuadPart - next.QuadPart > period) {   /* first frame or far behind */
        next.QuadPart = now.QuadPart + period;
        return;
    }
    while (now.QuadPart < next.QuadPart) {
        LONGLONG ms = (next.QuadPart - now.QuadPart) * 1000 / freq.QuadPart;
        if (ms > 2) Sleep((DWORD)(ms - 2)); else YieldProcessor();
        QueryPerformanceCounter(&now);
    }
    next.QuadPart += period;
}
