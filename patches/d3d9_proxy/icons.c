#include "proxy.h"

/* ------------------------------------------------------------------ sprite draw logging (IconLog=1, debug)
 *
 * Stage 1 of HD icons: find out which sprites the game draws where. Nearly all 2D sprites go through the
 * 16-bit RLE blitter at 0x73D47E: cdecl blit(sprite, x, y, frame, clipW, clipH), ~281 call sites. Its
 * entry is a Proteus hook site (jmp 0xF43389), so instead every `call 0x73D47E` in .text is redirected
 * to hk_blit, which logs and calls the original. The sprite struct is the loaded BIF: 256 x u16 palette,
 * then +0x200 width, +0x204 height, +0x20C/+0x210 frame counts. Each sprite is identified by an FNV-1a
 * hash (sprite_hash: palette, header, three pixel rows), which tools can match against the files on disk.
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

/* Sprite identity: palette + header + three pixel rows of frame 0. Many sprites share a palette and size, so
 * the first 0x214 bytes alone don't tell them apart. Same code in patches/hd_icons/build_icons.py. */
static DWORD sprite_hash(const BYTE *spr)
{
    DWORD h = fnv(spr, 0x214, 2166136261u);
    UINT rows = *(const UINT *)(spr + 0x204);
    if (!rows || rows > 2048) return h;
    const BYTE *p = spr + 0x214 + *(const UINT *)(spr + 0x214);
    for (UINT y = 0; y <= rows * 3 / 4; y++) {
        UINT len = *(const WORD *)p;
        if (len < 2 || len > 16384) break;
        if (y == rows / 4 || y == rows / 2 || y == rows * 3 / 4) h = fnv(p, (int)len, h);
        p += len;
    }
    return h;
}

/* Hidden buffers: list panels (the campaign army panel...) blit their cards into an off-screen buffer, only when
 * something changes, then 0x73BDB1 copies it to the screen every frame. Blits to a target that isn't screen sized
 * are remembered per buffer; each copy to the screen turns them into screen placements (report section 14). */
#define COPY_FN    0x73BDB1
#define TARGET     (*(BYTE **)0x8D0248)
#define TARGET_W   (*(int *)0x8D01B4)
#define TARGET_H   (*(int *)0x8D01B8)
#define BUF_MAX    16
#define BUF_RECS   128
typedef struct { BYTE *base; int n, next; IcDraw r[BUF_RECS]; } SprBuf;
static SprBuf g_bufs[BUF_MAX];
static int g_bufNext;

static void buf_add(const IcDraw *d)
{
    BYTE *base = TARGET;
    SprBuf *b = NULL;
    for (int i = 0; i < BUF_MAX && !b; i++) if (g_bufs[i].base == base) b = &g_bufs[i];
    if (!b) {
        b = &g_bufs[g_bufNext];
        g_bufNext = (g_bufNext + 1) % BUF_MAX;
        b->base = base; b->n = b->next = 0;
    }
    IcDraw *r = NULL;
    for (int i = 0; i < b->n && !r; i++) {               /* a redraw replaces what was at that spot */
        const IcDraw *o = &b->r[i];
        if (o->x == d->x && o->y == d->y && o->frame == d->frame && o->cw == d->cw && o->ch == d->ch) r = &b->r[i];
    }
    if (!r) {
        r = &b->r[b->next];
        b->next = (b->next + 1) % BUF_RECS;
        if (b->n < BUF_RECS) b->n++;
    }
    *r = *d;
}

static void __cdecl hk_copy(BYTE *src, int srcPitch, BYTE *dst, int dstPitch, int w, int h)
{
    ((void (__cdecl *)(BYTE *, int, BYTE *, int, int, int))COPY_FN)(src, srcPitch, dst, dstPitch, w, h);
    if (!g_hdIcons || TARGET_W != g_bbW || TARGET_H != g_bbH || srcPitch <= 0 || dstPitch <= 0) return;
    int dstOff = (int)(dst - TARGET);
    if (dstOff < 0 || dstOff >= dstPitch * g_bbH) return;
    SprBuf *b = NULL;
    for (int i = 0; i < BUF_MAX; i++)                    /* src can be inside the buffer: nearest base below it */
        if (g_bufs[i].base && g_bufs[i].base <= src && src < g_bufs[i].base + srcPitch * 2048 &&
            (!b || g_bufs[i].base > b->base)) b = &g_bufs[i];
    if (!b) return;
    int srcOff = (int)(src - b->base), sx = srcOff % srcPitch / 2, sy = srcOff / srcPitch;
    int dx = dstOff % dstPitch / 2, dy = dstOff / dstPitch;
    for (int i = 0; i < b->n && g_icN < IC_MAX; i++) {
        IcDraw *r = &b->r[i];
        int x = r->x - sx, y = r->y - sy;
        if (x < 0 || y < 0 || x >= w || y >= h) continue;
        int cw = r->cw < 0 || r->cw > r->w ? r->w : r->cw, ch = r->ch < 0 || r->ch > r->h ? r->h : r->ch;
        if (cw > w - x) cw = w - x;
        if (ch > h - y) ch = h - y;
        IcDraw *d = &g_icDraws[g_icN++];
        *d = *r;
        r->spr = NULL;                                       /* used once to load: it may go away later */
        d->x = dx + x; d->y = dy + y; d->cw = cw; d->ch = ch;
    }
}

static int __cdecl hk_blit(BYTE *spr, int x, int y, int frame, int cw, int ch)
{
    DWORD ret = (DWORD)__builtin_return_address(0) - 5;
    if (g_hdIcons && spr && g_icN < IC_MAX) {
        UINT w = *(UINT *)(spr + 0x200), h = *(UINT *)(spr + 0x204), n = *(UINT *)(spr + 0x20C) * *(UINT *)(spr + 0x210);
        if (w && h && w <= 2048 && h <= 2048 && n && n <= 4096) {
            IcDraw d = { spr, sprite_hash(spr), 0, x, y, frame < 0 ? 0 : (int)((UINT)frame % n), cw, ch, (int)w, (int)h };
            if (TARGET_W == g_bbW && TARGET_H == g_bbH) g_icDraws[g_icN++] = d;
            else if (cw && ch) buf_add(&d);              /* cards are blitted again with a 0x0 clip: nothing drawn */
        }
    }
    if (g_icLog && spr && g_icLines < 20000) {
        DWORD h = sprite_hash(spr);
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
            if (*p != 0xE8) continue;
            DWORD to = (DWORD)(p + 5) + *(int *)(p + 1);
            if (to != BLIT_FN && to != COPY_FN) continue;
            int rel = (int)((DWORD)(to == BLIT_FN ? (void *)hk_blit : (void *)hk_copy) - (DWORD)(p + 5));
            if (patch_mem(p + 1, &rel, 4)) n++;
        }
    }
    plog("icons: %d blit and copy calls redirected (IconLog=%d HDIcons=%d)", n, g_iconLog, g_hdIcons);
    if (g_icLog) { fprintf(g_icLog, "# %d call sites redirected\n", n); fflush(g_icLog); }
    if (g_iconLog == 2 && g_icLog) survey_install(base, nt);
}
