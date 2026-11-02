/*
 * Medieval Reconquered launcher: pick the resolution and the display mode, then start the game.
 *
 * Writes [Display] CampaignWidth/Height, BattleWidth/Height, Borderless, ShowFPS and FPSLimit in
 * MedievalReconquered.ini (next to this exe) and starts MedievalReconquered.exe. The d3d9.dll
 * proxy (patches/borderless) reads them at startup: Borderless selects borderless or exclusive
 * fullscreen, the sizes override the campaign and battle resolutions of the game's Video menu
 * (0: keep the game's setting).
 *
 * Background: the game's main-menu parchment (frontend_files/default_parchment_wide.tga), read
 * from the game folder at run time, so no game art is part of the launcher. Without it the
 * window falls back to the plain system look.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define GAME_EXE "MedievalReconquered.exe"
#define BG_FILE  "frontend_files/default_parchment_wide.tga"
#define CLIENT_W 520                /* 800x600 texture at 0.65: parchment x 75-437, y 16-365, banner removed */
#define CLIENT_H 390
#define ID_CAMP  100
#define ID_RES   101
#define ID_MODE  102
#define ID_PLAY  103
#define ID_QUIT  104
#define ID_FPS   105
#define ID_LIMIT 106

typedef struct { int w, h; } Mode;
static Mode g_modes[128];
static int g_nModes;
static char g_dir[MAX_PATH], g_ini[MAX_PATH];
static HWND g_camp, g_res, g_mode, g_fps, g_limit;    /* g_res: battle resolution */
static BYTE *g_bgBits;                /* 24-bit bottom-up pixels, DIB layout */
static BITMAPINFO g_bgInfo;
static HFONT g_labelFont;
static HICON g_emblem;
static HDC g_logoDC;                  /* title logo, premultiplied 32-bit, for AlphaBlend */
static int g_logoW, g_logoH;

/* Resource 2: logo.bmp, 32-bit top-down BGRA with straight alpha (see build.py). */
static void load_logo(HINSTANCE inst)
{
    HRSRC r = FindResourceA(inst, MAKEINTRESOURCEA(2), (LPCSTR)RT_RCDATA);
    BYTE *bmp = r ? LockResource(LoadResource(inst, r)) : NULL;
    if (!bmp || SizeofResource(inst, r) < 54 || bmp[0] != 'B' || bmp[1] != 'M') return;
    BITMAPINFOHEADER *bih = (BITMAPINFOHEADER *)(bmp + 14);
    if (bih->biBitCount != 32 || bih->biCompression != BI_RGB) return;
    int w = bih->biWidth, h = abs(bih->biHeight);
    BYTE *src = bmp + *(DWORD *)(bmp + 10);
    BITMAPINFO bi = { .bmiHeader = *bih };
    void *bits;
    HBITMAP dib = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!dib) return;
    BYTE *d = bits;
    for (int i = 0; i < w * h; i++, d += 4, src += 4) {
        d[0] = (BYTE)(src[0] * src[3] / 255);
        d[1] = (BYTE)(src[1] * src[3] / 255);
        d[2] = (BYTE)(src[2] * src[3] / 255);
        d[3] = src[3];
    }
    g_logoDC = CreateCompatibleDC(NULL);
    SelectObject(g_logoDC, dib);
    g_logoW = w;
    g_logoH = h;
}

/* Paint over the scroll banner at the top of the parchment (800x600 image coordinates): copy the
 * parchment from SHIFT rows below, scaled per row and channel by the edge darkening measured beside
 * the banner, feathered over 12 px at the sides. */
static void remove_banner(BYTE *bits, int w, int ht, size_t stride, int topdown)
{
    enum { X0 = 225, X1 = 590, Y0 = 20, Y1 = 78, SHIFT = 60 };
    if (w != 800 || ht != 600) return;
#define PX(x, y) (bits + stride * (size_t)(topdown ? (y) : ht - 1 - (y)) + (size_t)(x) * 3)
    for (int y = Y0; y < Y1; y++) {
        double sum[2][3] = { { 0 } };
        for (int x = 120; x < 680; x++) {
            if (x == 215) x = 600;                          /* reference columns: 120-214, 600-679 */
            for (int c = 0; c < 3; c++) { sum[0][c] += PX(x, y)[c]; sum[1][c] += PX(x, y + SHIFT)[c]; }
        }
        for (int x = X0; x < X1; x++) {
            double t = (x - X0) / 12.0, t2 = (X1 - 1 - x) / 12.0;
            if (t2 < t) t = t2;
            if (t > 1) t = 1;
            BYTE *d = PX(x, y), *src = PX(x, y + SHIFT);
            for (int c = 0; c < 3; c++) {
                double v = d[c] * (1 - t) + src[c] * (sum[1][c] > 0 ? sum[0][c] / sum[1][c] : 1) * t;
                d[c] = (BYTE)(v > 255 ? 255 : v + 0.5);
            }
        }
    }
#undef PX
}

/* Uncompressed 24-bit TGA (type 2), the layout the game's frontend files use. */
static void load_background(void)
{
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s%s", g_dir, BG_FILE);
    FILE *f = fopen(path, "rb");
    if (!f) return;
    BYTE h[18];
    if (fread(h, 1, 18, f) == 18 && h[1] == 0 && h[2] == 2 && h[16] == 24) {
        int w = h[12] | h[13] << 8, ht = h[14] | h[15] << 8;
        size_t stride = ((size_t)w * 3 + 3) & ~(size_t)3, row = (size_t)w * 3;
        BYTE *bits = calloc(stride, ht);
        fseek(f, 18 + h[0], SEEK_SET);
        int ok = bits != NULL;
        for (int y = 0; ok && y < ht; y++)
            ok = fread(bits + stride * y, 1, row, f) == row;
        if (ok) {
            remove_banner(bits, w, ht, stride, (h[17] & 0x20) != 0);
            g_bgBits = bits;
            g_bgInfo.bmiHeader = (BITMAPINFOHEADER){ .biSize = sizeof(BITMAPINFOHEADER), .biWidth = w,
                .biHeight = (h[17] & 0x20) ? -ht : ht, .biPlanes = 1, .biBitCount = 24 };
        } else {
            free(bits);
        }
    }
    fclose(f);
}

static void text_at(HDC dc, const char *s, int x, int y, int w, int h, UINT fmt)
{
    RECT r = { x, y, x + w, y + h };
    DrawTextA(dc, s, -1, &r, fmt | DT_SINGLELINE | DT_VCENTER);
}

static void paint(HWND win)
{
    PAINTSTRUCT ps;
    HDC wdc = BeginPaint(win, &ps);
    HDC dc = CreateCompatibleDC(wdc);
    HBITMAP bmp = CreateCompatibleBitmap(wdc, CLIENT_W, CLIENT_H), oldBmp = SelectObject(dc, bmp);
    if (g_bgBits) {
        SetStretchBltMode(dc, HALFTONE);
        SetBrushOrgEx(dc, 0, 0, NULL);
        StretchDIBits(dc, 0, 0, CLIENT_W, CLIENT_H, 0, 0, g_bgInfo.bmiHeader.biWidth,
                      abs(g_bgInfo.bmiHeader.biHeight), g_bgBits, &g_bgInfo, DIB_RGB_COLORS, SRCCOPY);
    } else {
        RECT r = { 0, 0, CLIENT_W, CLIENT_H };
        FillRect(dc, &r, GetSysColorBrush(COLOR_BTNFACE));
    }
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, g_bgBits ? RGB(70, 40, 20) : GetSysColor(COLOR_BTNTEXT));
    if (g_emblem) DrawIconEx(dc, (CLIENT_W - 72) / 2, 28, g_emblem, 72, 72, 0, NULL, DI_NORMAL);
    HFONT oldFont = SelectObject(dc, g_labelFont);
    if (g_logoDC) {
        BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        AlphaBlend(dc, (CLIENT_W - g_logoW) / 2, 104, g_logoW, g_logoH, g_logoDC, 0, 0, g_logoW, g_logoH, bf);
    } else {
        text_at(dc, "Medieval Reconquered", 0, 104, CLIENT_W, 36, DT_CENTER);
    }
    text_at(dc, "Campaign map:", 100, 180, 130, 24, DT_LEFT);
    text_at(dc, "Battle:", 100, 207, 130, 24, DT_LEFT);
    text_at(dc, "Display mode:", 100, 234, 130, 24, DT_LEFT);
    text_at(dc, "FPS counter:", 100, 261, 130, 24, DT_LEFT);
    text_at(dc, "Limit to 60 FPS:", 100, 288, 140, 24, DT_LEFT);
    SelectObject(dc, oldFont);
    BitBlt(wdc, 0, 0, CLIENT_W, CLIENT_H, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
    EndPaint(win, &ps);
}

static int cmp_mode(const void *a, const void *b)
{
    const Mode *x = a, *y = b;
    return x->w != y->w ? x->w - y->w : x->h - y->h;
}

/* Monitor modes, at least 800x600 (the game's own minimum for maps), without duplicates. */
static void list_modes(void)
{
    DEVMODEA dm = { .dmSize = sizeof dm };
    for (DWORD i = 0; EnumDisplaySettingsA(NULL, i, &dm); i++) {
        if (dm.dmPelsWidth < 800 || dm.dmPelsHeight < 600) continue;
        int dup = 0;
        for (int k = 0; k < g_nModes; k++)
            if (g_modes[k].w == (int)dm.dmPelsWidth && g_modes[k].h == (int)dm.dmPelsHeight) dup = 1;
        if (!dup && g_nModes < (int)(sizeof g_modes / sizeof *g_modes))
            g_modes[g_nModes++] = (Mode){ dm.dmPelsWidth, dm.dmPelsHeight };
    }
    qsort(g_modes, g_nModes, sizeof *g_modes, cmp_mode);
}

static HWND add(HWND parent, const char *cls, const char *text, DWORD style, int x, int y, int w, int h, int id)
{
    HWND c = CreateWindowExA(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, parent,
                             (HMENU)(INT_PTR)id, GetModuleHandleA(NULL), NULL);
    SendMessageA(c, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    return c;
}

static void fill_res(HWND box, const char *keyW, const char *keyH)
{
    int w = GetPrivateProfileIntA("Display", keyW, 0, g_ini);
    int h = GetPrivateProfileIntA("Display", keyH, 0, g_ini);
    int sel = 0;
    SendMessageA(box, CB_ADDSTRING, 0, (LPARAM)"Game setting (Video menu)");
    for (int i = 0; i < g_nModes; i++) {
        char s[32];
        snprintf(s, sizeof s, "%d x %d", g_modes[i].w, g_modes[i].h);
        SendMessageA(box, CB_ADDSTRING, 0, (LPARAM)s);
        if (g_modes[i].w == w && g_modes[i].h == h) sel = i + 1;
    }
    SendMessageA(box, CB_SETCURSEL, sel, 0);
}

static void save_res(HWND box, const char *keyW, const char *keyH)
{
    int sel = (int)SendMessageA(box, CB_GETCURSEL, 0, 0);
    char s[16];
    snprintf(s, sizeof s, "%d", sel > 0 ? g_modes[sel - 1].w : 0);
    WritePrivateProfileStringA("Display", keyW, s, g_ini);
    snprintf(s, sizeof s, "%d", sel > 0 ? g_modes[sel - 1].h : 0);
    WritePrivateProfileStringA("Display", keyH, s, g_ini);
}

static void fill_controls(void)
{
    fill_res(g_camp, "CampaignWidth", "CampaignHeight");
    fill_res(g_res, "BattleWidth", "BattleHeight");

    SendMessageA(g_mode, CB_ADDSTRING, 0, (LPARAM)"Fullscreen");
    SendMessageA(g_mode, CB_ADDSTRING, 0, (LPARAM)"Borderless Fullscreen");
    SendMessageA(g_mode, CB_SETCURSEL, GetPrivateProfileIntA("Display", "Borderless", 1, g_ini) ? 1 : 0, 0);
    SendMessageA(g_fps, BM_SETCHECK, GetPrivateProfileIntA("Display", "ShowFPS", 0, g_ini) ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageA(g_limit, BM_SETCHECK, GetPrivateProfileIntA("Display", "FPSLimit", 0, g_ini) ? BST_CHECKED : BST_UNCHECKED, 0);
}

static int save_and_play(HWND win)
{
    save_res(g_camp, "CampaignWidth", "CampaignHeight");
    save_res(g_res, "BattleWidth", "BattleHeight");
    WritePrivateProfileStringA("Display", "Borderless",
                               SendMessageA(g_mode, CB_GETCURSEL, 0, 0) == 1 ? "1" : "0", g_ini);
    WritePrivateProfileStringA("Display", "ShowFPS",
                               SendMessageA(g_fps, BM_GETCHECK, 0, 0) == BST_CHECKED ? "1" : "0", g_ini);
    WritePrivateProfileStringA("Display", "FPSLimit",
                               SendMessageA(g_limit, BM_GETCHECK, 0, 0) == BST_CHECKED ? "60" : "0", g_ini);

    char exe[MAX_PATH];
    snprintf(exe, sizeof exe, "%s%s", g_dir, GAME_EXE);
    STARTUPINFOA si = { .cb = sizeof si };
    PROCESS_INFORMATION pi;
    if (!CreateProcessA(exe, NULL, NULL, NULL, FALSE, 0, NULL, g_dir, &si, &pi)) {
        char msg[MAX_PATH + 64];
        snprintf(msg, sizeof msg, "Could not start %s (error %lu).", exe, GetLastError());
        MessageBoxA(win, msg, "Medieval Reconquered", MB_ICONERROR);
        return 0;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 1;
}

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        g_camp = add(h, "COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 240, 181, 180, 300, ID_CAMP);
        g_res = add(h, "COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 240, 208, 180, 300, ID_RES);
        g_mode = add(h, "COMBOBOX", "", CBS_DROPDOWNLIST | WS_TABSTOP, 240, 235, 180, 100, ID_MODE);
        g_fps = add(h, "BUTTON", "", BS_AUTOCHECKBOX | WS_TABSTOP, 240, 265, 16, 16, ID_FPS);
        g_limit = add(h, "BUTTON", "", BS_AUTOCHECKBOX | WS_TABSTOP, 240, 292, 16, 16, ID_LIMIT);
        add(h, "BUTTON", "Play", BS_DEFPUSHBUTTON | WS_TABSTOP, 150, 318, 100, 30, ID_PLAY);
        add(h, "BUTTON", "Quit", WS_TABSTOP, 270, 318, 100, 30, ID_QUIT);
        fill_controls();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_CTLCOLORSTATIC:     /* the checkbox: no grey box on the parchment */
        if ((HWND)lp == g_fps || (HWND)lp == g_limit) {
            SetBkMode((HDC)wp, TRANSPARENT);
            return (LRESULT)GetStockObject(NULL_BRUSH);
        }
        break;
    case WM_PAINT:
        paint(h);
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == ID_PLAY || LOWORD(wp) == IDOK) {
            if (save_and_play(h)) DestroyWindow(h);
        } else if (LOWORD(wp) == ID_QUIT || LOWORD(wp) == IDCANCEL) {
            DestroyWindow(h);
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(h, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    (void)prev; (void)cmd;
    GetModuleFileNameA(NULL, g_dir, MAX_PATH);
    char *s = strrchr(g_dir, '\\');
    if (s) s[1] = 0;
    snprintf(g_ini, sizeof g_ini, "%sMedievalReconquered.ini", g_dir);
    list_modes();
    load_background();
    g_emblem = LoadImageA(inst, MAKEINTRESOURCEA(1), IMAGE_ICON, 72, 72, 0);
    load_logo(inst);
    g_labelFont = CreateFontA(17, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY,
                              0, "Georgia");

    WNDCLASSA wc = { 0 };
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconA(inst, MAKEINTRESOURCEA(1));
    wc.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "MedievalReconqueredLauncher";
    RegisterClassA(&wc);

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    RECT r = { 0, 0, CLIENT_W, CLIENT_H };
    AdjustWindowRect(&r, style, FALSE);
    int w = r.right - r.left, h = r.bottom - r.top;
    HWND win = CreateWindowExA(0, wc.lpszClassName, "Medieval Reconquered", style,
                               (GetSystemMetrics(SM_CXSCREEN) - w) / 2, (GetSystemMetrics(SM_CYSCREEN) - h) / 2,
                               w, h, NULL, NULL, inst, NULL);
    ShowWindow(win, show);

    MSG m;
    while (GetMessageA(&m, NULL, 0, 0) > 0) {
        if (IsDialogMessageA(win, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageA(&m);
    }
    return 0;
}
