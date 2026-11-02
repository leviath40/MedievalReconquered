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
 *   Log=0            ; 1 = write borderless.log
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <stdarg.h>

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

static int g_borderless = 1, g_keepAspect = 1, g_log = 0;
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

static HRESULT STDMETHODCALLTYPE hk_Present(IDirect3DDevice9 *dev, const RECT *src, const RECT *dst,
                                            HWND wnd, const RGNDATA *dirty)
{
    if (!dst) dst = &g_dst;
    return g_origPresent(dev, src, dst, wnd, dirty);
}

static HRESULT STDMETHODCALLTYPE hk_CreateDevice(IDirect3D9 *d3d, UINT adapter, D3DDEVTYPE type,
                                                 HWND focus, DWORD flags, D3DPRESENT_PARAMETERS *pp,
                                                 IDirect3DDevice9 **out)
{
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
    if (d3d && g_borderless && !g_origCreateDevice) {
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
        logf("--- d3d9 proxy loaded: Borderless=%d KeepAspect=%d", g_borderless, g_keepAspect);
        g_switchState = (BYTE)(g_borderless != 0);
        patch_video_menu();
        patch_resolution();
    }
    return TRUE;
}
