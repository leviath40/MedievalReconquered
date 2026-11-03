#include "proxy.h"

/* ------------------------------------------------------------------ sprite draw logging (IconLog=1, debug)
 *
 * Stage 1 of HD icons: find out which sprites the game draws where. Nearly all 2D sprites go through the
 * 16-bit RLE blitter at 0x73D47E: cdecl blit(sprite, x, y, frame, clipW, clipH), ~281 call sites. Its
 * entry is a Proteus hook site (jmp 0xF43389), so instead every `call 0x73D47E` in .text is redirected
 * to hk_blit, which logs and calls the original. The sprite struct is the loaded BIF: 256 x u16 palette,
 * then +0x200 width, +0x204 height, +0x20C/+0x210 frame counts. Each sprite is identified by an FNV-1a
 * hash of its first 0x214 bytes (palette + header), which tools can match against the files on disk.
 * icons.log gets one line per new (hash, x, y, frame, caller), capped at 20000 lines.
 */
#define BLIT_FN 0x73D47Eu
typedef int (__cdecl *PFN_Blit)(BYTE *, int, int, int, int, int);

static FILE *g_icLog;
static int g_icLines;
static DWORD g_icSeen[1 << 16];        /* open-addressing set of entry keys (0 = empty) */

static DWORD fnv(const BYTE *p, int n, DWORD h)
{
    for (int i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

static int __cdecl hk_blit(BYTE *spr, int x, int y, int frame, int cw, int ch)
{
    DWORD ret = (DWORD)__builtin_return_address(0) - 5;
    if (g_icLog && spr && g_icLines < 20000) {
        DWORD h = fnv(spr, 0x214, 2166136261u);
        DWORD k[5] = { h, (DWORD)x, (DWORD)y, (DWORD)frame, ret };
        DWORD key = fnv((const BYTE *)k, sizeof k, 2166136261u) | 1;
        DWORD i = key & 0xFFFF;
        while (g_icSeen[i] && g_icSeen[i] != key) i = (i + 1) & 0xFFFF;
        if (!g_icSeen[i]) {
            g_icSeen[i] = key;
            fprintf(g_icLog, "%08lx %4u %4u %3u %5d %5d %4d %5d %5d %08lx %dx%d\n", (unsigned long)h,
                    *(UINT *)(spr + 0x200), *(UINT *)(spr + 0x204), *(UINT *)(spr + 0x20C) * *(UINT *)(spr + 0x210),
                    x, y, frame, cw, ch, (unsigned long)ret, g_bbW, g_bbH);
            g_icLines++;
            fflush(g_icLog);                    /* the game is often killed, not closed */
        }
    }
    return ((PFN_Blit)BLIT_FN)(spr, x, y, frame, cw, ch);
}

void patch_icon_log(void)
{
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%sicons.log", g_dir);
    g_icLog = fopen(path, "w");
    if (!g_icLog) return;
    fprintf(g_icLog, "# hash w h frames x y frame clipW clipH caller backbuffer\n");
    BYTE *base = (BYTE *)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    int n = 0;
    for (int s = 0; s < nt->FileHeader.NumberOfSections; s++, sec++) {
        if (memcmp(sec->Name, ".text", 5)) continue;
        BYTE *p = base + sec->VirtualAddress, *end = p + sec->Misc.VirtualSize - 5;
        for (; p < end; p++) {
            if (*p != 0xE8 || (DWORD)(p + 5) + *(int *)(p + 1) != BLIT_FN) continue;
            int rel = (int)((DWORD)hk_blit - (DWORD)(p + 5));
            if (patch_mem(p + 1, &rel, 4)) n++;
        }
    }
    plog("icons: %d blitter calls redirected, logging to icons.log", n);
    fprintf(g_icLog, "# %d call sites redirected\n", n);
    fflush(g_icLog);
}
