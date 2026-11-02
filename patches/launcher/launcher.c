/*
 * Medieval Reconquered launcher: pick the resolution and the display mode, then start the game.
 *
 * Writes [Display] CampaignWidth/Height, BattleWidth/Height and Borderless in
 * MedievalReconquered.ini (next to this exe) and starts MedievalReconquered.exe. The d3d9.dll
 * proxy (patches/borderless) reads them at startup: Borderless selects borderless or exclusive
 * fullscreen, the sizes override the campaign and battle resolutions of the game's Video menu
 * (0: keep the game's setting).
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define GAME_EXE "MedievalReconquered.exe"
#define ID_CAMP  100
#define ID_RES   101
#define ID_MODE  102
#define ID_PLAY  103
#define ID_QUIT  104

typedef struct { int w, h; } Mode;
static Mode g_modes[128];
static int g_nModes;
static char g_dir[MAX_PATH], g_ini[MAX_PATH];
static HWND g_camp, g_res, g_mode;    /* g_res: battle resolution */

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
}

static int save_and_play(HWND win)
{
    save_res(g_camp, "CampaignWidth", "CampaignHeight");
    save_res(g_res, "BattleWidth", "BattleHeight");
    WritePrivateProfileStringA("Display", "Borderless",
                               SendMessageA(g_mode, CB_GETCURSEL, 0, 0) == 1 ? "1" : "0", g_ini);

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
        add(h, "STATIC", "Campaign map:", 0, 16, 20, 90, 20, 0);
        g_camp = add(h, "COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 110, 16, 190, 300, ID_CAMP);
        add(h, "STATIC", "Battle:", 0, 16, 54, 90, 20, 0);
        g_res = add(h, "COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 110, 50, 190, 300, ID_RES);
        add(h, "STATIC", "Display mode:", 0, 16, 88, 90, 20, 0);
        g_mode = add(h, "COMBOBOX", "", CBS_DROPDOWNLIST | WS_TABSTOP, 110, 84, 190, 100, ID_MODE);
        add(h, "BUTTON", "Play", BS_DEFPUSHBUTTON | WS_TABSTOP, 110, 126, 90, 28, ID_PLAY);
        add(h, "BUTTON", "Quit", WS_TABSTOP, 210, 126, 90, 28, ID_QUIT);
        fill_controls();
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

    WNDCLASSA wc = { 0 };
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconA(inst, MAKEINTRESOURCEA(1));
    wc.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "MedievalReconqueredLauncher";
    RegisterClassA(&wc);

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT r = { 0, 0, 320, 170 };
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
