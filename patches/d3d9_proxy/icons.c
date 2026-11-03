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
 * With HDIcons=1 every blit of the frame is also recorded (g_icDraws) for the upscaler, which draws HD
 * copies over them (upscale.c); the list is cleared after each Present.
 */
#define BLIT_FN 0x73D47Eu
typedef int (__cdecl *PFN_Blit)(BYTE *, int, int, int, int, int);

IcDraw g_icDraws[IC_MAX];
int g_icN;
static FILE *g_icLog;
static int g_icLines;
static DWORD g_icSeen[1 << 16];        /* open-addressing set of entry keys (0 = empty) */

DWORD fnv(const BYTE *p, int n, DWORD h)
{
    for (int i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

static int __cdecl hk_blit(BYTE *spr, int x, int y, int frame, int cw, int ch)
{
    DWORD ret = (DWORD)__builtin_return_address(0) - 5;
    if (g_hdIcons && spr && g_icN < IC_MAX) {
        UINT w = *(UINT *)(spr + 0x200), h = *(UINT *)(spr + 0x204), n = *(UINT *)(spr + 0x20C) * *(UINT *)(spr + 0x210);
        if (w && h && w <= 2048 && h <= 2048 && n && n <= 4096) {
            IcDraw *d = &g_icDraws[g_icN++];
            d->spr = spr; d->hash = fnv(spr, 0x214, 2166136261u); d->last = 0;
            d->x = x; d->y = y; d->frame = frame < 0 ? 0 : (int)((UINT)frame % n); d->cw = cw; d->ch = ch;
            d->w = (int)w; d->h = (int)h;
        }
    }
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

/* IconLog=2 (debug survey): the other 2D routines next to the blitter (0x73C000-0x73EB00, found by a call
 * scan). Each external `call` to them goes through a stub that logs the caller, ecx and 8 stack slots, then
 * jumps on with every register and the stack untouched, so the calling convention doesn't matter.
 * Up to 12 lines per (routine, caller). */
static const DWORD g_svTargets[] = {
    0x73C00A, 0x73C07C, 0x73C134, 0x73C1A0, 0x73C1D8, 0x73C2B7, 0x73C371, 0x73C42C, 0x73C4ED, 0x73C9AE,
    0x73CA39, 0x73CE65, 0x73CED1, 0x73D1B9, 0x73D57D, 0x73D9EB, 0x73DB81, 0x73DD0C, 0x73E232, 0x73E510, 0x73EA2D,
};
#define SV_N (int)(sizeof g_svTargets / sizeof g_svTargets[0])

static void __cdecl survey_log(const DWORD *sp, int k)
{
    static DWORD keys[4096], counts[4096];
    DWORD key = (sp[0] * 31 + (DWORD)k) | 1, i = key & 4095;
    while (keys[i] && keys[i] != key) i = (i + 1) & 4095;
    keys[i] = key;
    if (++counts[i] > 12 || g_icLines >= 20000) return;
    g_icLines++;
    fprintf(g_icLog, "S %06lx %06lx ecx=%08lx %ld %ld %ld %ld %ld %ld %ld %ld %dx%d\n", (unsigned long)g_svTargets[k],
            (unsigned long)(sp[0] - 5), (unsigned long)sp[-2], (long)sp[1], (long)sp[2], (long)sp[3], (long)sp[4],
            (long)sp[5], (long)sp[6], (long)sp[7], (long)sp[8], g_bbW, g_bbH);
    fflush(g_icLog);
}

static void survey_install(BYTE *base, IMAGE_NT_HEADERS *nt)
{
    BYTE *stubs = VirtualAlloc(NULL, SV_N * 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!stubs) return;
    for (int k = 0; k < SV_N; k++) {
        BYTE *s = stubs + k * 32;
        DWORD fn = (DWORD)survey_log, t = g_svTargets[k];
        BYTE code[] = { 0x60, 0x68, 0, 0, 0, 0, 0x8D, 0x44, 0x24, 0x24, 0x50, 0xB8, 0, 0, 0, 0, 0xFF, 0xD0,
                        0x83, 0xC4, 0x08, 0x61, 0x68, 0, 0, 0, 0, 0xC3 };
        memcpy(code + 2, &k, 4); memcpy(code + 12, &fn, 4); memcpy(code + 23, &t, 4);
        memcpy(s, code, sizeof code);
    }
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    int n = 0;
    for (int s = 0; s < nt->FileHeader.NumberOfSections; s++, sec++) {
        if (memcmp(sec->Name, ".text", 5)) continue;
        BYTE *p = base + sec->VirtualAddress, *end = p + sec->Misc.VirtualSize - 5;
        for (; p < end; p++) {
            if (*p != 0xE8 || ((DWORD)p >= 0x73C000 && (DWORD)p < 0x73EB00)) continue;
            DWORD t = (DWORD)(p + 5) + *(int *)(p + 1);
            for (int k = 0; k < SV_N; k++)
                if (t == g_svTargets[k]) {
                    int rel = (int)((DWORD)(stubs + k * 32) - (DWORD)(p + 5));
                    if (patch_mem(p + 1, &rel, 4)) n++;
                }
        }
    }
    fprintf(g_icLog, "# survey: %d calls to %d routines redirected\n", n, SV_N);
    fflush(g_icLog);
}

void patch_icons(void)
{
    char path[MAX_PATH];
    if (g_iconLog) {
        snprintf(path, sizeof path, "%sicons.log", g_dir);
        g_icLog = fopen(path, "w");
        if (g_icLog) fprintf(g_icLog, "# hash w h frames x y frame clipW clipH caller backbuffer\n");
    }
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
    plog("icons: %d blitter calls redirected (IconLog=%d HDIcons=%d)", n, g_iconLog, g_hdIcons);
    if (g_icLog) { fprintf(g_icLog, "# %d call sites redirected\n", n); fflush(g_icLog); }
    if (g_iconLog == 2 && g_icLog) survey_install(base, nt);
}
