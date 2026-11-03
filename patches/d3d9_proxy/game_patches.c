#include "proxy.h"

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
        plog("menu: Borderless set to %d (applies at next start)", g_switchState);
    }
    return 0;
}

void patch_video_menu(void)
{
    g_switchState = (BYTE)(g_borderless != 0);
    static const BYTE ctor_orig[7] = { 0x6A, 0x11, 0x68, 0xE0, 0x25, 0x80, 0x00 };
    BYTE *exe = (BYTE *)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(exe + ((IMAGE_DOS_HEADER *)exe)->e_lfanew);
    if (nt->OptionalHeader.ImageBase != 0x400000 || nt->OptionalHeader.AddressOfEntryPoint != 0xB3E1B0 ||
        *(DWORD *)COUNT_INIT != MENU_COUNT || *(DWORD *)TABLE_SLOT != MENU_TABLE ||
        memcmp((void *)CTOR_PUSHES, ctor_orig, sizeof ctor_orig)) {
        plog("menu: unexpected exe layout, Video menu switch not added");
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
        plog("menu: Borderless switch added to the Video menu");
    }
}


/* ------------------------------------------------------------------ resolution override */
/*
 * Medieval.Cfg stores the campaign and battle resolutions as indices (0xE77B04 / 0xE77B08) into
 * per-device mode lists the game builds at startup. At 0x6DE4BE (mov edx,[0xE77B04]) the lists
 * exist and the game is about to clamp the indices and turn them into width/height
 * (0x7A2788 campaign, 0x7A2780 battle; confirmed in play 2026-11-03). The hook writes the indices of
 * the ini resolutions first.
 * Device records: 0xE7A1E0 + 0x60 * [0x895F8C]; +0x34 mode records (32 bytes: +4 width,
 * +8 height); +0x24/+0x54 campaign count/index array; +0x20/+0x44 battle count/index array.
 */
#define RES_HOOK   0x6DE4BEu
#define CFG_CAMP   0xE77B04u
#define CFG_BATTLE 0xE77B08u
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
    int camp = find_mode(dev, 0x24, 0x54, g_campW, g_campH);
    int battle = find_mode(dev, 0x20, 0x44, g_batW, g_batH);
    if (camp >= 0) *(int *)CFG_CAMP = camp;
    if (battle >= 0) *(int *)CFG_BATTLE = battle;
    plog("resolution: campaign %dx%d -> index %d, battle %dx%d -> index %d (-1 = game setting kept)",
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

void patch_ui_batch(void)
{
    static const BYTE tail_orig[23] = { 0x8B, 0x16, 0x8B, 0xCE, 0xFF, 0x52, 0x10, 0x84, 0xC0, 0x74, 0x0C, 0xE8,
                                        0x39, 0x37, 0xFA, 0xFF, 0x8B, 0x06, 0x8B, 0xCE, 0xFF, 0x50, 0x14 };
    static const BYTE group_orig[16] = { 0x8B, 0x44, 0x24, 0x04, 0x56, 0x57, 0x83, 0xF8, 0x04, 0x8B, 0xF9, 0x75,
                                         0x26, 0xE8, 0x4E, 0xAB };
    if (memcmp((void *)UI_TAIL, tail_orig, sizeof tail_orig) || memcmp((void *)UI_GROUP_DRAW, group_orig, sizeof group_orig)) {
        plog("ui batch: code differs, not patched");
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
    plog("ui batch: %s", ok ? "patched" : "patch failed");
}


void patch_resolution(void)
{
    static const BYTE orig[6] = { 0x8B, 0x15, 0x04, 0x7B, 0xE7, 0x00 };
    if ((g_campW <= 0 || g_campH <= 0) && (g_batW <= 0 || g_batH <= 0)) return;
    if (memcmp((void *)RES_HOOK, orig, sizeof orig)) {
        plog("resolution: unexpected exe layout, override not applied");
        return;
    }
    BYTE code[6] = { 0xE8, 0, 0, 0, 0, 0x90 };
    DWORD rel = (DWORD)res_thunk - (RES_HOOK + 5);
    memcpy(code + 1, &rel, 4);
    if (patch_mem((void *)RES_HOOK, code, sizeof code))
        plog("resolution: override hooked");
}
