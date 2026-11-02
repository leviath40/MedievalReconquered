/*
 * d3d9.dll proxy: borderless fullscreen for Medieval: Total War (Proteus build).
 *
 * The exe imports D3D9.dll!Direct3DCreate9, so this DLL (in the game folder) is loaded
 * instead of the system one. It forwards to the real System32 d3d9.dll and, when enabled
 * in MedievalReconquered.ini, changes what Proteus asks for:
 *   - IDirect3D9::CreateDevice / IDirect3DDevice9::Reset: Windowed = TRUE (no mode switch);
 *   - the game window is kept at the size of its monitor (it is already WS_POPUP);
 *   - IDirect3DDevice9::Present: the game-resolution back buffer is stretched to the monitor,
 *     with black bars if KeepAspect=1;
 *   - mouse: window messages, GetCursorPos, SetCursorPos and ClipCursor are converted between
 *     monitor pixels and game pixels.
 * Campaign/Battle Width/Height (set by the launcher) override the campaign and battle resolutions
 * chosen in the game's Video menu.
 * It also adds a "Borderless Fullscreen" on/off switch to the in-game Video options menu, which
 * writes Borderless= in the ini (applies at the next start).
 * Nothing in the exe file is changed: everything is patched in memory at load time.
 *
 * MedievalReconquered.ini (next to the exe):
 *   [Display]
 *   Borderless=1     ; 0 = original exclusive fullscreen (the proxy only forwards)
 *   KeepAspect=1     ; 0 = stretch to the whole monitor
 *   CampaignWidth=0  ; campaign map resolution, 0 = use the game's setting
 *   CampaignHeight=0
 *   BattleWidth=0    ; battle resolution, 0 = use the game's setting
 *   BattleHeight=0
 *   ShowFPS=0        ; 1 = framerate counter at the top right (works in both display modes)
 *   FPSLimit=0       ; frame rate cap, e.g. 60 (0 = off; works in both display modes)
 *   BatchUI=1        ; 1 = draw the battle UI panels' 2D parts under one back-buffer lock (faster)
 *   Profile=0        ; 1 = write profile.log: per-second breakdown of frame time (diagnostics)
 *   Log=0            ; 1 = write borderless.log
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define COBJMACROS
#include <d3d9.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <mmsystem.h>

typedef IDirect3D9 *(WINAPI *PFN_Create9)(UINT);
typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateDevice)(IDirect3D9 *, UINT, D3DDEVTYPE, HWND, DWORD,
                                                      D3DPRESENT_PARAMETERS *, IDirect3DDevice9 **);
typedef HRESULT (STDMETHODCALLTYPE *PFN_Reset)(IDirect3DDevice9 *, D3DPRESENT_PARAMETERS *);
typedef HRESULT (STDMETHODCALLTYPE *PFN_Present)(IDirect3DDevice9 *, const RECT *, const RECT *,
                                                 HWND, const RGNDATA *);
typedef BOOL (WINAPI *PFN_GetCursorPos)(LPPOINT);
typedef BOOL (WINAPI *PFN_SetCursorPos)(int, int);
typedef BOOL (WINAPI *PFN_ClipCursor)(const RECT *);

static HMODULE g_real;
static PFN_Create9 g_realCreate9;
static PFN_CreateDevice g_origCreateDevice;
static PFN_Reset g_origReset;
static PFN_Present g_origPresent;
static PFN_GetCursorPos g_realGetCursorPos;
static PFN_SetCursorPos g_realSetCursorPos;
static PFN_ClipCursor g_realClipCursor;

static int g_borderless = 1, g_keepAspect = 1, g_log = 0, g_showFps = 0, g_fpsLimit = 0, g_profile = 0, g_batchUI = 1;
static int g_campW, g_campH, g_batW, g_batH;   /* resolution overrides from the ini, 0 = off */
static HWND g_hwnd;
static WNDPROC g_origWndProc;
static RECT g_mon;                 /* monitor rect, screen coordinates */
static int g_bbW, g_bbH;           /* game resolution (back buffer) */
static RECT g_dst;                 /* where the image goes, client coordinates */
static int g_hooksInstalled;
static char g_dir[MAX_PATH];

static void logf(const char *fmt, ...)
{
    if (!g_log) return;
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%sborderless.log", g_dir);
    FILE *f = fopen(path, "a");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

static void load_config(void)
{
    char ini[MAX_PATH];
    GetModuleFileNameA(NULL, g_dir, MAX_PATH);
    char *s = strrchr(g_dir, '\\');
    if (s) s[1] = 0;
    snprintf(ini, sizeof ini, "%sMedievalReconquered.ini", g_dir);
    g_borderless = GetPrivateProfileIntA("Display", "Borderless", 1, ini);
    g_keepAspect = GetPrivateProfileIntA("Display", "KeepAspect", 1, ini);
    g_log = GetPrivateProfileIntA("Display", "Log", 0, ini);
    g_showFps = GetPrivateProfileIntA("Display", "ShowFPS", 0, ini);
    g_fpsLimit = GetPrivateProfileIntA("Display", "FPSLimit", 0, ini);
    g_profile = GetPrivateProfileIntA("Display", "Profile", 0, ini);
    g_batchUI = GetPrivateProfileIntA("Display", "BatchUI", 1, ini);
    if (g_fpsLimit < 0 || g_fpsLimit > 1000) g_fpsLimit = 0;
    g_campW = GetPrivateProfileIntA("Display", "CampaignWidth", 0, ini);
    g_campH = GetPrivateProfileIntA("Display", "CampaignHeight", 0, ini);
    g_batW = GetPrivateProfileIntA("Display", "BattleWidth", 0, ini);
    g_batH = GetPrivateProfileIntA("Display", "BattleHeight", 0, ini);
}

/* ------------------------------------------------------------------ geometry */

static void update_geometry(void)
{
    HMONITOR m = MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = { sizeof mi };
    GetMonitorInfoA(m, &mi);
    g_mon = mi.rcMonitor;
    int mw = g_mon.right - g_mon.left, mh = g_mon.bottom - g_mon.top;
    int dw = mw, dh = mh;
    if (g_keepAspect && g_bbW > 0 && g_bbH > 0) {
        if ((long long)mw * g_bbH > (long long)mh * g_bbW) dw = (int)((long long)mh * g_bbW / g_bbH);
        else dh = (int)((long long)mw * g_bbH / g_bbW);
    }
    g_dst.left = (mw - dw) / 2;
    g_dst.top = (mh - dh) / 2;
    g_dst.right = g_dst.left + dw;
    g_dst.bottom = g_dst.top + dh;
}

static void fit_window(void)
{
    if (!g_hwnd) return;
    update_geometry();
    SetWindowPos(g_hwnd, HWND_TOP, g_mon.left, g_mon.top, g_mon.right - g_mon.left,
                 g_mon.bottom - g_mon.top, SWP_NOACTIVATE | SWP_FRAMECHANGED);
    InvalidateRect(g_hwnd, NULL, TRUE);
}

/* client (monitor) pixels -> game pixels */
static void to_game(int *x, int *y)
{
    int dw = g_dst.right - g_dst.left, dh = g_dst.bottom - g_dst.top;
    if (dw <= 0 || dh <= 0 || g_bbW <= 0) return;
    int gx = (int)((long long)(*x - g_dst.left) * g_bbW / dw);
    int gy = (int)((long long)(*y - g_dst.top) * g_bbH / dh);
    *x = gx < 0 ? 0 : gx >= g_bbW ? g_bbW - 1 : gx;
    *y = gy < 0 ? 0 : gy >= g_bbH ? g_bbH - 1 : gy;
}

/* game pixels -> client (monitor) pixels */
static void to_client(int *x, int *y)
{
    int dw = g_dst.right - g_dst.left, dh = g_dst.bottom - g_dst.top;
    if (g_bbW <= 0 || g_bbH <= 0) return;
    *x = g_dst.left + (int)((long long)*x * dw / g_bbW);
    *y = g_dst.top + (int)((long long)*y * dh / g_bbH);
}

/* ------------------------------------------------------------------ window + mouse hooks */

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_WINDOWPOSCHANGING: {           /* keep the window at monitor size */
        WINDOWPOS *p = (WINDOWPOS *)lp;
        if (!(p->flags & SWP_NOSIZE) || !(p->flags & SWP_NOMOVE)) {
            p->x = g_mon.left; p->y = g_mon.top;
            p->cx = g_mon.right - g_mon.left; p->cy = g_mon.bottom - g_mon.top;
            p->flags &= ~(SWP_NOSIZE | SWP_NOMOVE);
        }
        break;
    }
    case WM_ERASEBKGND: {                  /* black bars */
        RECT r;
        GetClientRect(h, &r);
        FillRect((HDC)wp, &r, (HBRUSH)GetStockObject(BLACK_BRUSH));
        return 1;
    }
    case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
    case WM_XBUTTONDOWN: case WM_XBUTTONUP: case WM_XBUTTONDBLCLK: {
        int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
        to_game(&x, &y);
        lp = MAKELPARAM(x, y);
        break;
    }
    }
    return CallWindowProcA(g_origWndProc, h, msg, wp, lp);
}

static BOOL WINAPI hk_GetCursorPos(LPPOINT p)
{
    if (!g_realGetCursorPos(p)) return FALSE;
    int x = p->x - g_mon.left, y = p->y - g_mon.top;   /* window sits at the monitor origin */
    to_game(&x, &y);
    p->x = x; p->y = y;
    return TRUE;
}

static BOOL WINAPI hk_SetCursorPos(int x, int y)
{
    to_client(&x, &y);
    return g_realSetCursorPos(x + g_mon.left, y + g_mon.top);
}

static BOOL WINAPI hk_ClipCursor(const RECT *r)
{
    if (!r) return g_realClipCursor(NULL);
    RECT c = *r;
    int l = c.left, t = c.top, rr = c.right, b = c.bottom;
    to_client(&l, &t);
    to_client(&rr, &b);
    c.left = l + g_mon.left; c.top = t + g_mon.top;
    c.right = rr + g_mon.left; c.bottom = b + g_mon.top;
    return g_realClipCursor(&c);
}

/* Write n bytes at dst (code, data or a vtable), whatever the page protection. */
static int patch_mem(void *dst, const void *src, SIZE_T n)
{
    DWORD old;
    if (!VirtualProtect(dst, n, PAGE_EXECUTE_READWRITE, &old)) return 0;
    memcpy(dst, src, n);
    VirtualProtect(dst, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), dst, n);
    return 1;
}

static int patch_ptr(void **slot, void *val)
{
    return patch_mem(slot, &val, sizeof val);
}

/* Replace every pointer equal to `from` in [start, start+len) with `to`. */
static int patch_range(BYTE *start, SIZE_T len, void *from, void *to)
{
    int n = 0;
    for (SIZE_T i = 0; i + sizeof(void *) <= len; i += sizeof(void *))
        if (*(void **)(start + i) == from) n += patch_ptr((void **)(start + i), to);
    return n;
}

/* Exe import table (IAT directory) and Proteus's private import table (0xF3C000-0xF3C0A0). */
static int patch_imports(void *from, void *to)
{
    BYTE *base = (BYTE *)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY *iat = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IAT];
    int n = 0;
    if (iat->VirtualAddress) n += patch_range(base + iat->VirtualAddress, iat->Size, from, to);
    if (nt->OptionalHeader.SizeOfImage >= 0xB3C0A0)
        n += patch_range(base + 0xB3C000, 0xA0, from, to);
    return n;
}

static void install_hooks(void)
{
    if (g_hooksInstalled) return;
    g_hooksInstalled = 1;
    HMODULE u = GetModuleHandleA("user32.dll");
    g_realGetCursorPos = (PFN_GetCursorPos)GetProcAddress(u, "GetCursorPos");
    g_realSetCursorPos = (PFN_SetCursorPos)GetProcAddress(u, "SetCursorPos");
    g_realClipCursor = (PFN_ClipCursor)GetProcAddress(u, "ClipCursor");
    int a = patch_imports((void *)g_realGetCursorPos, (void *)hk_GetCursorPos);
    int b = patch_imports((void *)g_realSetCursorPos, (void *)hk_SetCursorPos);
    int c = patch_imports((void *)g_realClipCursor, (void *)hk_ClipCursor);
    g_origWndProc = (WNDPROC)SetWindowLongPtrA(g_hwnd, GWLP_WNDPROC, (LONG_PTR)wndproc);
    logf("hooks: GetCursorPos x%d, SetCursorPos x%d, ClipCursor x%d, wndproc %p", a, b, c,
         (void *)g_origWndProc);
}

/* ------------------------------------------------------------------ D3D9 hooks */

static void make_windowed(D3DPRESENT_PARAMETERS *pp)
{
    logf("requested: %ux%u fmt %d windowed %d swap %d refresh %u flags %#x",
         pp->BackBufferWidth, pp->BackBufferHeight, pp->BackBufferFormat, pp->Windowed,
         pp->SwapEffect, pp->FullScreen_RefreshRateInHz, pp->Flags);
    g_bbW = pp->BackBufferWidth;
    g_bbH = pp->BackBufferHeight;
    pp->Windowed = TRUE;
    pp->FullScreen_RefreshRateInHz = 0;
    if (pp->hDeviceWindow) g_hwnd = pp->hDeviceWindow;
    fit_window();
}

static HRESULT STDMETHODCALLTYPE hk_Reset(IDirect3DDevice9 *dev, D3DPRESENT_PARAMETERS *pp)
{
    if (!g_borderless) return g_origReset(dev, pp);
    D3DPRESENT_PARAMETERS mine = *pp;
    make_windowed(&mine);
    HRESULT hr = g_origReset(dev, &mine);
    if (FAILED(hr) && mine.BackBufferFormat != D3DFMT_UNKNOWN) {
        mine.BackBufferFormat = D3DFMT_UNKNOWN;    /* let D3D use the desktop format */
        hr = g_origReset(dev, &mine);
    }
    logf("Reset -> %#lx", hr);
    pp->BackBufferWidth = mine.BackBufferWidth;
    pp->BackBufferHeight = mine.BackBufferHeight;
    return hr;
}

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

static void draw_fps(IDirect3DDevice9 *dev)
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
static void limit_fps(void)
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

/* ------------------------------------------------------------------ profiler (Profile=1)
 *
 * Times the Direct3D calls where a frame can lose time (Present, EndScene, draws, buffer/texture/
 * surface locks, copies, resource creation) and writes one line per second to profile.log, in ms
 * per frame. "game" = frame time minus everything measured = the game's own code.
 * Resource vtables are patched the first time an object of that class is seen.
 */
typedef struct { LONGLONG t; unsigned n; } Acc;
enum { A_PRESENT, A_LIMIT, A_ENDSCENE, A_DRAW, A_BUF, A_TEX, A_SURF, A_COPY, A_CREATE, A_COUNT };
static const char *const g_accName[A_COUNT] = { "present", "limit", "endscene", "draw", "buflock", "texlock",
                                                "surflock", "copy", "create" };
static Acc g_acc[A_COUNT];
/* buffer lock classes: dynamic+DISCARD, dynamic+NOOVERWRITE, dynamic plain, static DEFAULT, MANAGED, other */
static unsigned g_bufClass[6];
static LARGE_INTEGER g_pFreq;
static LONGLONG g_pFrameStart, g_pSecStart, g_pMaxFrame;
static unsigned g_pFrames, g_pSlow;

static LONGLONG qpc(void) { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
#define TIMED(acc, call) do { LONGLONG t0_ = qpc(); hr = (call); g_acc[acc].t += qpc() - t0_; g_acc[acc].n++; } while (0)

typedef struct { void **vt; void *orig; } VtHook;
static VtHook g_hVb[8], g_hIb[8], g_hTex[8], g_hSurf[8], g_hLevel[8];

static void *orig_of(VtHook *h, void *obj)
{
    void **vt = *(void ***)obj;
    for (int i = 0; i < 8; i++) if (h[i].vt == vt) return h[i].orig;
    return NULL;
}

static void hook_vt(VtHook *h, void *obj, int idx, void *fn)
{
    if (!obj) return;
    void **vt = *(void ***)obj;
    for (int i = 0; i < 8; i++) {
        if (h[i].vt == vt) return;
        if (!h[i].vt) {
            if (vt[idx] == fn) return;             /* shared with an already hooked class */
            h[i].vt = vt;
            h[i].orig = vt[idx];
            patch_ptr(&vt[idx], fn);
            logf("profile: hooked vtable %p slot %d", (void *)vt, idx);
            return;
        }
    }
}

typedef HRESULT (STDMETHODCALLTYPE *PFN_BufLock)(void *, UINT, UINT, void **, DWORD);
typedef HRESULT (STDMETHODCALLTYPE *PFN_GetDesc)(void *, void *);
typedef HRESULT (STDMETHODCALLTYPE *PFN_TexLock)(void *, UINT, D3DLOCKED_RECT *, const RECT *, DWORD);
typedef HRESULT (STDMETHODCALLTYPE *PFN_SurfLock)(void *, D3DLOCKED_RECT *, const RECT *, DWORD);
typedef HRESULT (STDMETHODCALLTYPE *PFN_GetLevel)(void *, UINT, IDirect3DSurface9 **);

static void classify_buf(void *buf, DWORD flags)
{
    DWORD desc[8] = { 0 };                     /* VB/IB desc: Format, Type, Usage, Pool, ... */
    ((PFN_GetDesc)(*(void ***)buf)[13])(buf, desc);
    DWORD usage = desc[2], pool = desc[3];
    int c = 5;
    if (pool == D3DPOOL_MANAGED) c = 4;
    else if (pool == D3DPOOL_DEFAULT && (usage & D3DUSAGE_DYNAMIC))
        c = (flags & D3DLOCK_DISCARD) ? 0 : (flags & D3DLOCK_NOOVERWRITE) ? 1 : 2;
    else if (pool == D3DPOOL_DEFAULT) c = 3;
    g_bufClass[c]++;
}

static HRESULT STDMETHODCALLTYPE pk_VbLock(void *b, UINT off, UINT size, void **data, DWORD flags)
{
    HRESULT hr;
    classify_buf(b, flags);
    TIMED(A_BUF, ((PFN_BufLock)orig_of(g_hVb, b))(b, off, size, data, flags));
    return hr;
}

static HRESULT STDMETHODCALLTYPE pk_IbLock(void *b, UINT off, UINT size, void **data, DWORD flags)
{
    HRESULT hr;
    classify_buf(b, flags);
    TIMED(A_BUF, ((PFN_BufLock)orig_of(g_hIb, b))(b, off, size, data, flags));
    return hr;
}

static HRESULT STDMETHODCALLTYPE pk_TexLock(void *t, UINT lvl, D3DLOCKED_RECT *lr, const RECT *r, DWORD flags)
{
    HRESULT hr;
    TIMED(A_TEX, ((PFN_TexLock)orig_of(g_hTex, t))(t, lvl, lr, r, flags));
    return hr;
}

/* surface locks by call site + surface kind, dumped every 10 s (top entries by time) */
typedef struct { void *caller, *g[5]; D3DSURFACE_DESC d; DWORD flags; LONG rw, rh; LONGLONG t; unsigned n;
                 unsigned drawsBefore; } SiteStat;
static unsigned g_drawsSinceLock;
static SiteStat g_sites[64];
static unsigned g_drawKind[4];
static LONGLONG g_drawKindT[4];

/* Is a a plausible return address in the game's original code (preceded by a call instruction)? */
static int game_ret(DWORD a)
{
    if (a < 0x401010 || a >= 0x78358E) return 0;
    const BYTE *b = (const BYTE *)a;
    return b[-5] == 0xE8 || (b[-2] == 0xFF && (b[-1] & 0x38) == 0x10) || (b[-3] == 0xFF && (b[-2] & 0x38) == 0x10) ||
           (b[-6] == 0xFF && (b[-5] & 0x38) == 0x10) || (b[-7] == 0xFF && (b[-6] & 0x38) == 0x10);
}

static void site_add(void *caller, const D3DSURFACE_DESC *d, DWORD flags, const RECT *r, LONGLONG t)
{
    void *g[5] = { 0 };
    int ng = 0;
    if (d->Usage & D3DUSAGE_RENDERTARGET) {
        DWORD *sp = (DWORD *)__builtin_frame_address(0);
        DWORD *top = (DWORD *)((NT_TIB *)NtCurrentTeb())->StackBase;  /* stay inside this thread's stack */
        for (int i = 0; i < 1500 && ng < 5 && sp + i < top; i++)
            if (game_ret(sp[i]) && (!ng || g[ng - 1] != (void *)sp[i])) g[ng++] = (void *)sp[i];
    }
    LONG rw = r ? r->right - r->left : -1, rh = r ? r->bottom - r->top : -1;
    for (int i = 0; i < 64; i++) {
        SiteStat *e = &g_sites[i];
        if (e->n && (e->caller != caller || memcmp(e->g, g, sizeof g) || e->flags != flags || e->rw != rw || e->rh != rh ||
                     e->d.Format != d->Format || e->d.Usage != d->Usage || e->d.Pool != d->Pool ||
                     e->d.Width != d->Width || e->d.Height != d->Height))
            continue;
        if (!e->n) { e->caller = caller; memcpy(e->g, g, sizeof g); e->d = *d; e->flags = flags; e->rw = rw; e->rh = rh; }
        e->n++;
        e->drawsBefore += g_drawsSinceLock;
        e->t += t;
        return;
    }
}

static void sites_dump(FILE *f)
{
    double ms = 1000.0 / (double)g_pFreq.QuadPart;
    fprintf(f, "  surface locks, last 10 s (caller [game ret1 ret2] fmt usage pool WxH flags rect: calls, total ms, "
               "ms/call, draws before):\n");
    for (int k = 0; k < 16; k++) {
        int best = -1;
        for (int i = 0; i < 64; i++)
            if (g_sites[i].n && (best < 0 || g_sites[i].t > g_sites[best].t)) best = i;
        if (best < 0) break;
        SiteStat *e = &g_sites[best];
        fprintf(f, "    %p [%p %p %p %p %p] fmt %d usage %#lx pool %d %ux%u flags %#lx rect %ldx%ld: %u, %.1f, %.3f, %.1f\n",
                e->caller, e->g[0], e->g[1], e->g[2], e->g[3], e->g[4], (int)e->d.Format, e->d.Usage, (int)e->d.Pool, e->d.Width, e->d.Height,
                e->flags, e->rw, e->rh, e->n, (double)e->t * ms, (double)e->t * ms / e->n,
                (double)e->drawsBefore / e->n);
        e->n = 0;
        e->t = 0;
    }
    memset(g_sites, 0, sizeof g_sites);
    fprintf(f, "  draws, last 10 s (DP/DIP/DPUP/DIPUP: calls, total ms): %u %.0f / %u %.0f / %u %.0f / %u %.0f\n",
            g_drawKind[0], (double)g_drawKindT[0] * ms, g_drawKind[1], (double)g_drawKindT[1] * ms,
            g_drawKind[2], (double)g_drawKindT[2] * ms, g_drawKind[3], (double)g_drawKindT[3] * ms);
    memset(g_drawKind, 0, sizeof g_drawKind);
    memset(g_drawKindT, 0, sizeof g_drawKindT);
}

typedef HRESULT (STDMETHODCALLTYPE *PFN_SurfDesc)(void *, D3DSURFACE_DESC *);
static HRESULT STDMETHODCALLTYPE pk_SurfLock(void *s, D3DLOCKED_RECT *lr, const RECT *r, DWORD flags)
{
    HRESULT hr;
    D3DSURFACE_DESC d = { 0 };
    ((PFN_SurfDesc)(*(void ***)s)[12])(s, &d);
    LONGLONG t0 = qpc();
    TIMED(A_SURF, ((PFN_SurfLock)orig_of(g_hSurf, s))(s, lr, r, flags));
    site_add(__builtin_return_address(0), &d, flags, r, qpc() - t0);
    if (d.Usage & D3DUSAGE_RENDERTARGET) g_drawsSinceLock = 0;
    return hr;
}

static HRESULT STDMETHODCALLTYPE pk_GetLevel(void *t, UINT lvl, IDirect3DSurface9 **out)
{
    HRESULT hr = ((PFN_GetLevel)orig_of(g_hLevel, t))(t, lvl, out);
    if (SUCCEEDED(hr) && out && *out) hook_vt(g_hSurf, *out, 13, (void *)pk_SurfLock);
    return hr;
}

/* device methods (vtable index in the comments) */
static void *g_pOrig[120];
#define ORIG(i, T) ((T)g_pOrig[i])

typedef HRESULT (STDMETHODCALLTYPE *PFN_D0)(IDirect3DDevice9 *);
static HRESULT STDMETHODCALLTYPE pk_EndScene(IDirect3DDevice9 *d)                                   /* 42 */
{ HRESULT hr; TIMED(A_ENDSCENE, ORIG(42, PFN_D0)(d)); return hr; }

typedef HRESULT (STDMETHODCALLTYPE *PFN_DP)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, UINT);
static HRESULT STDMETHODCALLTYPE pk_DP(IDirect3DDevice9 *d, D3DPRIMITIVETYPE t, UINT a, UINT b)    /* 81 */
{ HRESULT hr; LONGLONG t0 = qpc(); TIMED(A_DRAW, ORIG(81, PFN_DP)(d, t, a, b)); g_drawKind[0]++; g_drawsSinceLock++; g_drawKindT[0] += qpc() - t0; return hr; }

typedef HRESULT (STDMETHODCALLTYPE *PFN_DIP)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
static HRESULT STDMETHODCALLTYPE pk_DIP(IDirect3DDevice9 *d, D3DPRIMITIVETYPE t, INT a, UINT b, UINT c, UINT e,
                                        UINT f)                                                 /* 82 */
{ HRESULT hr; LONGLONG t0 = qpc(); TIMED(A_DRAW, ORIG(82, PFN_DIP)(d, t, a, b, c, e, f)); g_drawKind[1]++; g_drawsSinceLock++; g_drawKindT[1] += qpc() - t0; return hr; }

typedef HRESULT (STDMETHODCALLTYPE *PFN_DPUP)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, const void *, UINT);
static HRESULT STDMETHODCALLTYPE pk_DPUP(IDirect3DDevice9 *d, D3DPRIMITIVETYPE t, UINT a, const void *p, UINT s)
{ HRESULT hr; LONGLONG t0 = qpc(); TIMED(A_DRAW, ORIG(83, PFN_DPUP)(d, t, a, p, s)); g_drawKind[2]++; g_drawsSinceLock++; g_drawKindT[2] += qpc() - t0; return hr; }                  /* 83 */

typedef HRESULT (STDMETHODCALLTYPE *PFN_DIPUP)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void *,
                                               D3DFORMAT, const void *, UINT);
static HRESULT STDMETHODCALLTYPE pk_DIPUP(IDirect3DDevice9 *d, D3DPRIMITIVETYPE t, UINT a, UINT b, UINT c,
                                          const void *i, D3DFORMAT f, const void *v, UINT s)   /* 84 */
{ HRESULT hr; LONGLONG t0 = qpc(); TIMED(A_DRAW, ORIG(84, PFN_DIPUP)(d, t, a, b, c, i, f, v, s)); g_drawKind[3]++; g_drawsSinceLock++; g_drawKindT[3] += qpc() - t0; return hr; }

typedef HRESULT (STDMETHODCALLTYPE *PFN_UpdSurf)(IDirect3DDevice9 *, void *, const RECT *, void *, const POINT *);
static HRESULT STDMETHODCALLTYPE pk_UpdSurf(IDirect3DDevice9 *d, void *a, const RECT *r, void *b, const POINT *p)
{ HRESULT hr; TIMED(A_COPY, ORIG(30, PFN_UpdSurf)(d, a, r, b, p)); return hr; }               /* 30 */

typedef HRESULT (STDMETHODCALLTYPE *PFN_DPP)(IDirect3DDevice9 *, void *, void *);
static HRESULT STDMETHODCALLTYPE pk_UpdTex(IDirect3DDevice9 *d, void *a, void *b)            /* 31 */
{ HRESULT hr; TIMED(A_COPY, ORIG(31, PFN_DPP)(d, a, b)); return hr; }
static HRESULT STDMETHODCALLTYPE pk_GetRTData(IDirect3DDevice9 *d, void *a, void *b)         /* 32 */
{ HRESULT hr; TIMED(A_COPY, ORIG(32, PFN_DPP)(d, a, b)); return hr; }

typedef HRESULT (STDMETHODCALLTYPE *PFN_Stretch)(IDirect3DDevice9 *, void *, const RECT *, void *, const RECT *,
                                                 D3DTEXTUREFILTERTYPE);
static HRESULT STDMETHODCALLTYPE pk_Stretch(IDirect3DDevice9 *d, void *a, const RECT *r, void *b, const RECT *r2,
                                            D3DTEXTUREFILTERTYPE f)                            /* 34 */
{ HRESULT hr; TIMED(A_COPY, ORIG(34, PFN_Stretch)(d, a, r, b, r2, f)); return hr; }

typedef HRESULT (STDMETHODCALLTYPE *PFN_CTex)(IDirect3DDevice9 *, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL,
                                              IDirect3DTexture9 **, HANDLE *);
static HRESULT STDMETHODCALLTYPE pk_CTex(IDirect3DDevice9 *d, UINT w, UINT h, UINT l, DWORD u, D3DFORMAT f,
                                         D3DPOOL p, IDirect3DTexture9 **o, HANDLE *sh)         /* 23 */
{
    HRESULT hr;
    TIMED(A_CREATE, ORIG(23, PFN_CTex)(d, w, h, l, u, f, p, o, sh));
    if (SUCCEEDED(hr) && o && *o) {
        hook_vt(g_hTex, *o, 19, (void *)pk_TexLock);
        hook_vt(g_hLevel, *o, 18, (void *)pk_GetLevel);
    }
    return hr;
}

typedef HRESULT (STDMETHODCALLTYPE *PFN_CBuf)(IDirect3DDevice9 *, UINT, DWORD, DWORD, D3DPOOL, void **, HANDLE *);
static HRESULT STDMETHODCALLTYPE pk_CVb(IDirect3DDevice9 *d, UINT len, DWORD u, DWORD fvf, D3DPOOL p, void **o,
                                        HANDLE *sh)                                            /* 26 */
{
    HRESULT hr;
    TIMED(A_CREATE, ORIG(26, PFN_CBuf)(d, len, u, fvf, p, o, sh));
    if (SUCCEEDED(hr) && o && *o) hook_vt(g_hVb, *o, 11, (void *)pk_VbLock);
    return hr;
}

static HRESULT STDMETHODCALLTYPE pk_CIb(IDirect3DDevice9 *d, UINT len, DWORD u, DWORD fmt, D3DPOOL p, void **o,
                                        HANDLE *sh)                                            /* 27 */
{
    HRESULT hr;
    TIMED(A_CREATE, ORIG(27, PFN_CBuf)(d, len, u, fmt, p, o, sh));
    if (SUCCEEDED(hr) && o && *o) hook_vt(g_hIb, *o, 11, (void *)pk_IbLock);
    return hr;
}

typedef HRESULT (STDMETHODCALLTYPE *PFN_COff)(IDirect3DDevice9 *, UINT, UINT, D3DFORMAT, D3DPOOL, void **, HANDLE *);
static HRESULT STDMETHODCALLTYPE pk_COff(IDirect3DDevice9 *d, UINT w, UINT h, D3DFORMAT f, D3DPOOL p, void **o,
                                         HANDLE *sh)                                           /* 36 */
{
    HRESULT hr;
    TIMED(A_CREATE, ORIG(36, PFN_COff)(d, w, h, f, p, o, sh));
    if (SUCCEEDED(hr) && o && *o) hook_vt(g_hSurf, *o, 13, (void *)pk_SurfLock);
    return hr;
}

static void prof_hook_dev(IDirect3DDevice9 *dev, int idx, void *fn)
{
    void **vt = *(void ***)dev;
    if (vt[idx] == fn) return;
    g_pOrig[idx] = vt[idx];
    patch_ptr(&vt[idx], fn);
}

static void prof_install(IDirect3DDevice9 *dev)
{
    static int done;
    if (!g_profile || done) return;
    done = 1;
    QueryPerformanceFrequency(&g_pFreq);
    prof_hook_dev(dev, 23, (void *)pk_CTex);
    prof_hook_dev(dev, 26, (void *)pk_CVb);
    prof_hook_dev(dev, 27, (void *)pk_CIb);
    prof_hook_dev(dev, 30, (void *)pk_UpdSurf);
    prof_hook_dev(dev, 31, (void *)pk_UpdTex);
    prof_hook_dev(dev, 32, (void *)pk_GetRTData);
    prof_hook_dev(dev, 34, (void *)pk_Stretch);
    prof_hook_dev(dev, 36, (void *)pk_COff);
    prof_hook_dev(dev, 42, (void *)pk_EndScene);
    prof_hook_dev(dev, 81, (void *)pk_DP);
    prof_hook_dev(dev, 82, (void *)pk_DIP);
    prof_hook_dev(dev, 83, (void *)pk_DPUP);
    prof_hook_dev(dev, 84, (void *)pk_DIPUP);
    IDirect3DSurface9 *bb = NULL;
    if (SUCCEEDED(IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
        hook_vt(g_hSurf, bb, 13, (void *)pk_SurfLock);
        IDirect3DSurface9_Release(bb);
    }
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%sprofile.log", g_dir);
    FILE *f = fopen(path, "a");
    if (f) {
        fprintf(f, "--- profile start. ms per frame; n = calls per frame; game = frame - measured.\n"
                   "    buf = buffer locks per frame: dynDISCARD/dynNOOVERWRITE/dynPlain/staticDEFAULT/MANAGED/other\n");
        fclose(f);
    }
}

/* called once per frame, right after the real Present returned */
static void prof_frame(LONGLONG now)
{
    if (!g_pSecStart) {
        g_pSecStart = g_pFrameStart = now;
        memset(g_acc, 0, sizeof g_acc);
        memset(g_bufClass, 0, sizeof g_bufClass);
        return;
    }
    LONGLONG ft = now - g_pFrameStart;
    g_pFrameStart = now;
    g_pFrames++;
    if (ft > g_pMaxFrame) g_pMaxFrame = ft;
    if (ft * 1000 > 50 * g_pFreq.QuadPart) g_pSlow++;
    if ((now - g_pSecStart) < g_pFreq.QuadPart) return;

    double ms = 1000.0 / (double)g_pFreq.QuadPart, fr = g_pFrames, total = (double)(now - g_pSecStart) * ms;
    char line[1024];
    int n = snprintf(line, sizeof line, "fps %5.1f | frame %5.1f max %5.1f slow>50ms %u", fr * 1000.0 / total,
                     total / fr, (double)g_pMaxFrame * ms, g_pSlow);
    double measured = 0;
    for (int i = 0; i < A_COUNT; i++) {
        double t = (double)g_acc[i].t * ms / fr;
        measured += t;
        if (g_acc[i].n)
            n += snprintf(line + n, sizeof line - n, " | %s %.2f n%.0f", g_accName[i], t, g_acc[i].n / fr);
    }
    snprintf(line + n, sizeof line - n, " | game %.2f | buf %.0f/%.0f/%.0f/%.0f/%.0f/%.0f", total / fr - measured,
             g_bufClass[0] / fr, g_bufClass[1] / fr, g_bufClass[2] / fr, g_bufClass[3] / fr,
             g_bufClass[4] / fr, g_bufClass[5] / fr);
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%sprofile.log", g_dir);
    FILE *f = fopen(path, "a");
    static int secs;
    if (f) {
        fprintf(f, "%s\n", line);
        if (++secs % 10 == 0) sites_dump(f);
        fclose(f);
    }
    memset(g_acc, 0, sizeof g_acc);
    memset(g_bufClass, 0, sizeof g_bufClass);
    g_pFrames = g_pSlow = 0;
    g_pMaxFrame = 0;
    g_pSecStart = now;
}

static HRESULT STDMETHODCALLTYPE hk_Present(IDirect3DDevice9 *dev, const RECT *src, const RECT *dst,
                                            HWND wnd, const RGNDATA *dirty)
{
    HRESULT hr;
    if (g_fpsLimit) {
        LONGLONG t0 = qpc();
        limit_fps();
        g_acc[A_LIMIT].t += qpc() - t0;
        g_acc[A_LIMIT].n++;
    }
    if (g_showFps) draw_fps(dev);
    if (g_borderless && !dst) dst = &g_dst;
    if (!g_profile) return g_origPresent(dev, src, dst, wnd, dirty);
    TIMED(A_PRESENT, g_origPresent(dev, src, dst, wnd, dirty));
    prof_frame(qpc());
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_CreateDevice(IDirect3D9 *d3d, UINT adapter, D3DDEVTYPE type,
                                                 HWND focus, DWORD flags, D3DPRESENT_PARAMETERS *pp,
                                                 IDirect3DDevice9 **out)
{
    if (!g_borderless) {      /* FPS counter/limit/profile only: hook Present, change nothing else */
        HRESULT hr0 = g_origCreateDevice(d3d, adapter, type, focus, flags, pp, out);
        if (SUCCEEDED(hr0) && *out && !g_origReset) {
            void **vt = *(void ***)*out;
            g_origReset = (PFN_Reset)vt[16];
            g_origPresent = (PFN_Present)vt[17];
            patch_ptr(&vt[16], (void *)hk_Reset);
            patch_ptr(&vt[17], (void *)hk_Present);
        }
        if (SUCCEEDED(hr0) && *out) prof_install(*out);
        return hr0;
    }
    g_hwnd = focus;
    D3DPRESENT_PARAMETERS mine = *pp;
    make_windowed(&mine);
    HRESULT hr = g_origCreateDevice(d3d, adapter, type, focus, flags, &mine, out);
    if (FAILED(hr) && mine.BackBufferFormat != D3DFMT_UNKNOWN) {
        logf("CreateDevice failed (%#lx), retrying with desktop format", hr);
        mine.BackBufferFormat = D3DFMT_UNKNOWN;
        hr = g_origCreateDevice(d3d, adapter, type, focus, flags, &mine, out);
    }
    logf("CreateDevice -> %#lx, window %p, monitor %ldx%ld, image at %ld,%ld %ldx%ld", hr,
         (void *)g_hwnd, g_mon.right - g_mon.left, g_mon.bottom - g_mon.top, g_dst.left, g_dst.top,
         g_dst.right - g_dst.left, g_dst.bottom - g_dst.top);
    if (SUCCEEDED(hr) && *out) {
        void **vt = *(void ***)*out;
        if (!g_origReset) {
            g_origReset = (PFN_Reset)vt[16];
            g_origPresent = (PFN_Present)vt[17];
            patch_ptr(&vt[16], (void *)hk_Reset);
            patch_ptr(&vt[17], (void *)hk_Present);
        }
        prof_install(*out);
        install_hooks();
    }
    return hr;
}

/* ------------------------------------------------------------------ Video menu switch
 *
 * The Video options menu ("MainMenuGraphicOptions") is a static table of 17 widget records
 * (0x134 bytes each) at 0x8025E0. It is used by:
 *   - the static constructor at 0x6A8790:  push 17 / push 0x8025E0 / push name / call 0x6EB8E0;
 *   - the Options dispatcher (0x709200):   table pointer at 0x8C1E70, count copied at startup
 *                                          from 0x791050 into 0xEFF7E0.
 * Before the exe's static initialisers run (this DllMain runs first), we copy the table with two
 * extra records appended (a label and an on/off switch, cloned from the Wildlife ones) and
 * point all three users at the copy. The label uses the "Graphics_Menu_Dustswitch_Title" text
 * (the Dust switch is not in this menu), which build.py renames in Loc/Eng/FRONTEND.TXT.
 * The switch callback mirrors the Smoke switch callback (0x70A490).
 */
#define MENU_TABLE      0x8025E0u
#define MENU_COUNT      17u
#define REC_SIZE        0x134u
#define REC_WILD_LABEL  0x80297Cu
#define REC_WILD_SWITCH 0x802AB0u
#define LOC_DUST        0x801938u      /* localised-text record "Graphics_Menu_Dustswitch_Title" */
#define COUNT_INIT      0x791050u      /* dword, copied to 0xEFF7E0 at startup */
#define TABLE_SLOT      0x8C1E70u      /* dword pointer used by the Options dispatcher */
#define CTOR_PUSHES     0x6A8790u      /* 6A 11 68 E0 25 80 00 */
#define SMOKE_SOUND     0x8B8F30u      /* "LMB on Graphics Options Menu Smoke toggle" */

typedef void (__cdecl *PFN_WidgetUpdate)(BYTE *);
typedef char (__thiscall *PFN_WidgetClicked)(BYTE *, int);
typedef void (__cdecl *PFN_UiEvent)(int, int, int, const char *);

static BYTE g_menuTable[(MENU_COUNT + 2) * REC_SIZE];
static BYTE g_switchState;

static int __cdecl borderless_switch_cb(BYTE *w)
{
    w[0x2a] = g_switchState;
    ((PFN_WidgetUpdate)0x6E6D20)(w);
    w[0x29] = 0;
    if (((PFN_WidgetClicked)0x6DF860)(w, 0) && (*(DWORD *)0x8DCCA8 & 0x800) && !(*(DWORD *)0x8DCCB4 & 0x800)) {
        ((PFN_UiEvent)0x6E9D50)(1, 0, 0, (const char *)SMOKE_SOUND);
        w[0x2a] ^= 1;
    }
    if (w[0x2a] != g_switchState) {
        char ini[MAX_PATH];
        g_switchState = w[0x2a];
        snprintf(ini, sizeof ini, "%sMedievalReconquered.ini", g_dir);
        WritePrivateProfileStringA("Display", "Borderless", g_switchState ? "1" : "0", ini);
        logf("menu: Borderless set to %d (applies at next start)", g_switchState);
    }
    return 0;
}

static void patch_video_menu(void)
{
    static const BYTE ctor_orig[7] = { 0x6A, 0x11, 0x68, 0xE0, 0x25, 0x80, 0x00 };
    BYTE *exe = (BYTE *)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(exe + ((IMAGE_DOS_HEADER *)exe)->e_lfanew);
    if (nt->OptionalHeader.ImageBase != 0x400000 || nt->OptionalHeader.AddressOfEntryPoint != 0xB3E1B0 ||
        *(DWORD *)COUNT_INIT != MENU_COUNT || *(DWORD *)TABLE_SLOT != MENU_TABLE ||
        memcmp((void *)CTOR_PUSHES, ctor_orig, sizeof ctor_orig)) {
        logf("menu: unexpected exe layout, Video menu switch not added");
        return;
    }
    memcpy(g_menuTable, (void *)MENU_TABLE, MENU_COUNT * REC_SIZE);
    BYTE *label = g_menuTable + MENU_COUNT * REC_SIZE;
    BYTE *sw = label + REC_SIZE;
    memcpy(label, (void *)REC_WILD_LABEL, REC_SIZE);
    memcpy(sw, (void *)REC_WILD_SWITCH, REC_SIZE);
    *(DWORD *)(label + 0x10) = 220;                 /* y: below Wildlife (170/190) */
    *(DWORD *)(label + 0x2c) = LOC_DUST;            /* text */
    *(DWORD *)(sw + 0x10) = 240;
    *(DWORD *)(sw + 0x24) = (DWORD)borderless_switch_cb;

    DWORD newCount = MENU_COUNT + 2, newTable = (DWORD)g_menuTable;
    BYTE ctor_new[7] = { 0x6A, (BYTE)newCount, 0x68 };
    memcpy(ctor_new + 3, &newTable, 4);
    if (patch_mem((void *)COUNT_INIT, &newCount, 4) && patch_mem((void *)TABLE_SLOT, &newTable, 4) &&
        patch_mem((void *)CTOR_PUSHES, ctor_new, sizeof ctor_new)) {
        logf("menu: Borderless switch added to the Video menu");
    }
}

/* ------------------------------------------------------------------ resolution override */
/*
 * Medieval.Cfg stores the campaign and battle resolutions as indices (0xE77B08 / 0xE77B04) into
 * per-device mode lists the game builds at startup. At 0x6DE4BE (mov edx,[0xE77B04]) the lists
 * exist and the game is about to clamp the indices and turn them into width/height
 * (0x7A2780 campaign, 0x7A2788 battle). The hook writes the indices of the ini resolutions first.
 * Device records: 0xE7A1E0 + 0x60 * [0x895F8C]; +0x34 mode records (32 bytes: +4 width,
 * +8 height); +0x20/+0x44 campaign count/index array; +0x24/+0x54 battle count/index array.
 */
#define RES_HOOK   0x6DE4BEu
#define CFG_BATTLE 0xE77B04u
#define CFG_CAMP   0xE77B08u
#define DEV_INDEX  0x895F8Cu
#define DEV_TABLE  0xE7A1E0u

static int find_mode(BYTE *dev, int countOff, int arrOff, int w, int h)
{
    if (w <= 0 || h <= 0) return -1;
    int n = *(int *)(dev + countOff);
    int *arr = *(int **)(dev + arrOff);
    BYTE *modes = *(BYTE **)(dev + 0x34);
    if (!arr || !modes) return -1;
    for (int i = 0; i < n; i++) {
        BYTE *m = modes + arr[i] * 32;
        if (*(int *)(m + 4) == w && *(int *)(m + 8) == h) return i;
    }
    return -1;
}

__attribute__((used)) void __cdecl apply_resolution(void)
{
    static int done;
    if (done) return;                                   /* startup only: in-game changes stay */
    done = 1;
    int d = *(int *)DEV_INDEX;
    if (d < 0) return;
    BYTE *dev = (BYTE *)DEV_TABLE + d * 0x60;
    int camp = find_mode(dev, 0x20, 0x44, g_campW, g_campH);
    int battle = find_mode(dev, 0x24, 0x54, g_batW, g_batH);
    if (camp >= 0) *(int *)CFG_CAMP = camp;
    if (battle >= 0) *(int *)CFG_BATTLE = battle;
    logf("resolution: campaign %dx%d -> index %d, battle %dx%d -> index %d (-1 = game setting kept)",
         g_campW, g_campH, camp, g_batW, g_batH, battle);
}

/* Replaces the 6-byte mov edx,[0xE77B04] with call + nop; redoes the mov after the C code. */
void res_thunk(void);
__asm__(".globl _res_thunk\n_res_thunk:\n"
        "  pushal\n  call _apply_resolution\n  popal\n"
        "  movl 0xE77B04, %edx\n  ret\n");

/* ------------------------------------------------------------------ battle UI lock batching (BatchUI=1)
 *
 * Each battle-screen panel (bmdialogbase.cpp, FUN_004c4270) draws its 3D part, then locks the back
 * buffer ("safe back screen", FUN_00467a90) and draws its 2D part by hand. Proteus turns each of those
 * locks after a 3D draw into a full render-target readback (GPU sync + a frame-sized copy both ways),
 * about 13 times per frame in battle. We queue the 2D parts while a panel group is drawn (group draw
 * method 0x4CEAC0, pass 4) and draw them all under one lock at the end of the group, in their
 * original order. The 3D parts are drawn as before.
 *   0x4C4347 (23 bytes)  2D tail: if (vt[0x10](p)) { FUN_00467a90(); vt[0x14](p); }
 *                        -> push esi / call ui_tail / jmp 0x4C435E
 *   0x4CEAC0             group draw(pass) -> jmp ui_group_draw (same thiscall, ret 4)
 */
#define UI_TAIL         0x4C4347u
#define UI_TAIL_END     0x4C435Eu
#define UI_GROUP_DRAW   0x4CEAC0u
#define FN_BACK_LOCK    0x467A90u      /* lock the back buffer for 2D drawing (no-op if locked) */
#define FN_GROUP_ACTIVE 0x4C9620u      /* thiscall, returns al */

typedef char (__attribute__((thiscall)) *PFN_ThisBool)(void *);
typedef void (__attribute__((thiscall)) *PFN_ThisVoid)(void *);
typedef void (__attribute__((thiscall)) *PFN_ThisInt)(void *, int);
typedef void (*PFN_Void)(void);

static void *g_uiQueue[256];
static int g_uiQueued, g_uiDepth;

static void ui_flush(void)
{
    if (!g_uiQueued) return;
    ((PFN_Void)FN_BACK_LOCK)();
    for (int i = 0; i < g_uiQueued; i++) {
        void *p = g_uiQueue[i];
        ((PFN_ThisVoid)(*(void ***)p)[0x14 / 4])(p);
    }
    g_uiQueued = 0;
}

static void __stdcall ui_tail(void *p)
{
    if (!((PFN_ThisBool)(*(void ***)p)[0x10 / 4])(p)) return;
    if (g_uiDepth > 0 && g_uiQueued < (int)(sizeof g_uiQueue / sizeof *g_uiQueue)) {
        g_uiQueue[g_uiQueued++] = p;
        return;
    }
    ((PFN_Void)FN_BACK_LOCK)();
    ((PFN_ThisVoid)(*(void ***)p)[0x14 / 4])(p);
}

/* replacement for the group draw method at 0x4CEAC0 (original: same loop, without the flush) */
static void __attribute__((thiscall)) ui_group_draw(BYTE *grp, int pass)
{
    if (pass != 4 || !((PFN_ThisBool)FN_GROUP_ACTIVE)(grp)) return;
    g_uiDepth++;
    for (void **it = *(void ***)(grp + 8); it != *(void ***)(grp + 0xC); it++) {
        void *panel = *it;
        ((PFN_ThisInt)(*(void ***)panel)[2])(panel, 0);
    }
    if (--g_uiDepth == 0) ui_flush();
}

static void patch_ui_batch(void)
{
    static const BYTE tail_orig[23] = { 0x8B, 0x16, 0x8B, 0xCE, 0xFF, 0x52, 0x10, 0x84, 0xC0, 0x74, 0x0C, 0xE8,
                                        0x39, 0x37, 0xFA, 0xFF, 0x8B, 0x06, 0x8B, 0xCE, 0xFF, 0x50, 0x14 };
    static const BYTE group_orig[16] = { 0x8B, 0x44, 0x24, 0x04, 0x56, 0x57, 0x83, 0xF8, 0x04, 0x8B, 0xF9, 0x75,
                                         0x26, 0xE8, 0x4E, 0xAB };
    if (memcmp((void *)UI_TAIL, tail_orig, sizeof tail_orig) || memcmp((void *)UI_GROUP_DRAW, group_orig, sizeof group_orig)) {
        logf("ui batch: code differs, not patched");
        return;
    }
    BYTE tail[23];
    memset(tail, 0x90, sizeof tail);
    tail[0] = 0x56;                                                  /* push esi */
    tail[1] = 0xE8;                                                  /* call ui_tail */
    *(DWORD *)(tail + 2) = (DWORD)(UINT_PTR)ui_tail - (UI_TAIL + 6);
    tail[6] = 0xE9;                                                  /* jmp 0x4C435E */
    *(DWORD *)(tail + 7) = UI_TAIL_END - (UI_TAIL + 11);
    BYTE jmp[5] = { 0xE9 };
    *(DWORD *)(jmp + 1) = (DWORD)(UINT_PTR)ui_group_draw - (UI_GROUP_DRAW + 5);
    int ok = patch_mem((void *)UI_TAIL, tail, sizeof tail) && patch_mem((void *)UI_GROUP_DRAW, jmp, sizeof jmp);
    logf("ui batch: %s", ok ? "patched" : "patch failed");
}

static void patch_resolution(void)
{
    static const BYTE orig[6] = { 0x8B, 0x15, 0x04, 0x7B, 0xE7, 0x00 };
    if ((g_campW <= 0 || g_campH <= 0) && (g_batW <= 0 || g_batH <= 0)) return;
    if (memcmp((void *)RES_HOOK, orig, sizeof orig)) {
        logf("resolution: unexpected exe layout, override not applied");
        return;
    }
    BYTE code[6] = { 0xE8, 0, 0, 0, 0, 0x90 };
    DWORD rel = (DWORD)res_thunk - (RES_HOOK + 5);
    memcpy(code + 1, &rel, 4);
    if (patch_mem((void *)RES_HOOK, code, sizeof code))
        logf("resolution: override hooked");
}

/* ------------------------------------------------------------------ exports */

static void load_real(void)
{
    if (g_real) return;
    char path[MAX_PATH];
    GetSystemDirectoryA(path, MAX_PATH);
    strcat(path, "\\d3d9.dll");
    g_real = LoadLibraryA(path);
    g_realCreate9 = g_real ? (PFN_Create9)GetProcAddress(g_real, "Direct3DCreate9") : NULL;
}

IDirect3D9 *WINAPI Direct3DCreate9(UINT sdk)
{
    load_real();
    if (!g_realCreate9) return NULL;
    IDirect3D9 *d3d = g_realCreate9(sdk);
    if (d3d && (g_borderless || g_showFps || g_fpsLimit || g_profile) && !g_origCreateDevice) {
        void **vt = *(void ***)d3d;
        g_origCreateDevice = (PFN_CreateDevice)vt[16];
        patch_ptr(&vt[16], (void *)hk_CreateDevice);
        logf("Direct3DCreate9(%u): CreateDevice hooked", sdk);
    }
    return d3d;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r)
{
    (void)r;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        load_config();
        logf("--- d3d9 proxy loaded: Borderless=%d KeepAspect=%d ShowFPS=%d FPSLimit=%d Profile=%d", g_borderless,
             g_keepAspect, g_showFps, g_fpsLimit, g_profile);
        g_switchState = (BYTE)(g_borderless != 0);
        patch_video_menu();
        patch_resolution();
        if (g_batchUI) patch_ui_batch();
    }
    return TRUE;
}
