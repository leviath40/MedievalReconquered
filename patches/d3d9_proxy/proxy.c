/*
 * d3d9.dll proxy for Medieval: Total War (Proteus build): borderless fullscreen, resolutions,
 * FPS counter/cap, battle performance fixes, menu upscaling, profiler.
 *
 * Files:
 *   proxy.c         this overview, config, borderless window/mouse, D3D9 hooks, exports, DllMain
 *   fps.c           FPS counter (ShowFPS) and frame rate cap (FPSLimit)
 *   profile.c       frame-time profiler (Profile) -> profile.log
 *   fastdraw.c      DrawIndexedPrimitiveUP through our own dynamic buffers (FastDraw)
 *   upscale.c       shader upscaling of low-res screens, full-resolution menu logo, wood side bars,
 *                   HD sprites (Upscale, MenuSides, HDIcons)
 *   card_shadow.c   smooth ground shadows on the HD unit cards
 *   game_patches.c  in-memory game code patches: Video menu switch, resolution override, BatchUI
 *   icons.c         sprite blitter hook: per-frame blit list for HDIcons, IconLog (debug)
 *   proxy.h         shared declarations
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
 *   Upscale=2        ; borderless: upscale low-res screens (menus) with a shader: 0 = plain stretch,
 *                    ;   1 = sharp, 2 = smooth
 *   FastDraw=1       ; 1 = replay DrawIndexedPrimitiveUP through our own dynamic buffers (faster)
 *   CardIcons=1      ; 1 = bigger soldier icon on the battle unit cards
 *   BatchUI=1        ; 1 = draw the battle UI panels' 2D parts under one back-buffer lock (faster)
 *   Profile=1        ; 1 = write profile.log: per-second breakdown of frame time (on by default,
 *                    ;     new file past 4 MB; 0 = off)
 *   MenuSides=1      ; with Upscale: fill the bars beside 4:3 screens with the game's wood texture (0 = black)
 *   IconLog=0        ; debug: 1 = log sprite draws to icons.log (icons.c)
 *   HDIcons=0        ; with Upscale: draw HD copies of sprites from hd_icons/ (patches/hd_icons)
 *   DumpFrame=0      ; debug: 1 = F11 saves frame_<n>.raw/.txt and final_<n>.raw
 *   Log=0            ; 1 = write borderless.log
 */
#include "proxy.h"

static HMODULE g_real;
static PFN_Create9 g_realCreate9;
static PFN_CreateDevice g_origCreateDevice;
static PFN_Reset g_origReset;
static PFN_Present g_origPresent;
static PFN_GetCursorPos g_realGetCursorPos;
static PFN_SetCursorPos g_realSetCursorPos;
static PFN_ClipCursor g_realClipCursor;

int g_cardIcons = 1;
IDirect3DDevice9 *g_dev9;
int g_borderless = 1, g_keepAspect = 1, g_log = 0, g_showFps = 0, g_fpsLimit = 0, g_profile = 1, g_batchUI = 1, g_fastDraw = 1, g_upscale = 2, g_dumpFrame = 0, g_menuSides = 1, g_iconLog = 0, g_hdIcons = 0;
int g_campW, g_campH, g_batW, g_batH;   /* resolution overrides from the ini, 0 = off */
HWND g_hwnd;
static WNDPROC g_origWndProc;
RECT g_mon;                 /* monitor rect, screen coordinates */
int g_bbW, g_bbH;           /* game resolution (back buffer) */
RECT g_dst;                 /* where the image goes, client coordinates */
static int g_hooksInstalled;
char g_dir[MAX_PATH];

void plog(const char *fmt, ...)
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
    g_profile = GetPrivateProfileIntA("Display", "Profile", 1, ini);
    g_batchUI = GetPrivateProfileIntA("Display", "BatchUI", 1, ini);
    g_cardIcons = GetPrivateProfileIntA("Display", "CardIcons", 1, ini);
    g_fastDraw = GetPrivateProfileIntA("Display", "FastDraw", 1, ini);
    g_upscale = GetPrivateProfileIntA("Display", "Upscale", 2, ini);
    g_dumpFrame = GetPrivateProfileIntA("Display", "DumpFrame", 0, ini);
    g_menuSides = GetPrivateProfileIntA("Display", "MenuSides", 1, ini);
    g_iconLog = GetPrivateProfileIntA("Display", "IconLog", 0, ini);
    g_hdIcons = GetPrivateProfileIntA("Display", "HDIcons", 0, ini);
    if (g_upscale < 0 || g_upscale > 2) g_upscale = 2;
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
    MONITORINFO mi = { .cbSize = sizeof mi };
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
int patch_mem(void *dst, const void *src, SIZE_T n)
{
    DWORD old;
    if (!VirtualProtect(dst, n, PAGE_EXECUTE_READWRITE, &old)) return 0;
    memcpy(dst, src, n);
    VirtualProtect(dst, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), dst, n);
    return 1;
}

int patch_ptr(void **slot, void *val)
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
    plog("hooks: GetCursorPos x%d, SetCursorPos x%d, ClipCursor x%d, wndproc %p", a, b, c,
         (void *)g_origWndProc);
}

/* ------------------------------------------------------------------ D3D9 hooks */

UINT g_presentInterval;

static void make_windowed(D3DPRESENT_PARAMETERS *pp)
{
    plog("requested: %ux%u fmt %d windowed %d swap %d refresh %u flags %#x",
         pp->BackBufferWidth, pp->BackBufferHeight, pp->BackBufferFormat, pp->Windowed,
         pp->SwapEffect, pp->FullScreen_RefreshRateInHz, pp->Flags);
    g_bbW = pp->BackBufferWidth;
    g_bbH = pp->BackBufferHeight;
    g_presentInterval = pp->PresentationInterval;
    pp->Windowed = TRUE;
    pp->FullScreen_RefreshRateInHz = 0;
    if (pp->hDeviceWindow) g_hwnd = pp->hDeviceWindow;
    fit_window();
}

static HRESULT STDMETHODCALLTYPE hk_Reset(IDirect3DDevice9 *dev, D3DPRESENT_PARAMETERS *pp)
{
    fd_release();
    us_release();                 /* our D3DPOOL_DEFAULT buffers must go before Reset */
    if (!g_borderless) return g_origReset(dev, pp);
    D3DPRESENT_PARAMETERS mine = *pp;
    make_windowed(&mine);
    HRESULT hr = g_origReset(dev, &mine);
    if (FAILED(hr) && mine.BackBufferFormat != D3DFMT_UNKNOWN) {
        mine.BackBufferFormat = D3DFMT_UNKNOWN;    /* let D3D use the desktop format */
        hr = g_origReset(dev, &mine);
    }
    plog("Reset -> %#lx", hr);
    pp->BackBufferWidth = mine.BackBufferWidth;
    pp->BackBufferHeight = mine.BackBufferHeight;
    return hr;
}

static HRESULT present_frame(IDirect3DDevice9 *dev, const RECT *src, const RECT *dst, HWND wnd, const RGNDATA *dirty)
{
    HRESULT hr = D3D_OK;
    if (!(g_borderless && g_upscale && !src && !wnd && us_present(dev))) hr = g_origPresent(dev, src, dst, wnd, dirty);
    g_icN = 0;                                        /* next frame's sprite blits */
    return hr;
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
    if (!g_profile) return present_frame(dev, src, dst, wnd, dirty);
    TIMED(A_PRESENT, present_frame(dev, src, dst, wnd, dirty));
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
        if (SUCCEEDED(hr0) && *out) {
            g_dev9 = *out;
            fd_install(*out);
            prof_install(*out);
        }
        return hr0;
    }
    g_hwnd = focus;
    D3DPRESENT_PARAMETERS mine = *pp;
    make_windowed(&mine);
    HRESULT hr = g_origCreateDevice(d3d, adapter, type, focus, flags, &mine, out);
    if (FAILED(hr) && mine.BackBufferFormat != D3DFMT_UNKNOWN) {
        plog("CreateDevice failed (%#lx), retrying with desktop format", hr);
        mine.BackBufferFormat = D3DFMT_UNKNOWN;
        hr = g_origCreateDevice(d3d, adapter, type, focus, flags, &mine, out);
    }
    plog("CreateDevice -> %#lx, window %p, monitor %ldx%ld, image at %ld,%ld %ldx%ld", hr,
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
        g_dev9 = *out;
        fd_install(*out);
        prof_install(*out);
        install_hooks();
    }
    return hr;
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
    if (d3d && (g_borderless || g_showFps || g_fpsLimit || g_profile || g_fastDraw) && !g_origCreateDevice) {
        void **vt = *(void ***)d3d;
        g_origCreateDevice = (PFN_CreateDevice)vt[16];
        patch_ptr(&vt[16], (void *)hk_CreateDevice);
        plog("Direct3DCreate9(%u): CreateDevice hooked", sdk);
    }
    return d3d;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r)
{
    (void)r;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        load_config();
        plog("--- d3d9 proxy loaded: Borderless=%d KeepAspect=%d ShowFPS=%d FPSLimit=%d Profile=%d", g_borderless,
             g_keepAspect, g_showFps, g_fpsLimit, g_profile);
        patch_video_menu();
        patch_resolution();
        if (g_batchUI) patch_ui_batch();
        if (g_cardIcons) patch_card_icon();
        if (g_iconLog || g_hdIcons) patch_icons();
    }
    return TRUE;
}
