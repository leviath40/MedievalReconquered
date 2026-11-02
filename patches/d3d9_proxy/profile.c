#include "proxy.h"

/* ------------------------------------------------------------------ profiler (Profile=1)
 *
 * Times the Direct3D calls where a frame can lose time (Present, EndScene, draws, buffer/texture/
 * surface locks, copies, resource creation) and writes one line per second to profile.log, in ms
 * per frame. "game" = frame time minus everything measured = the game's own code.
 * Resource vtables are patched the first time an object of that class is seen.
 */
static const char *const g_accName[A_COUNT] = { "present", "limit", "endscene", "draw", "buflock", "texlock",
                                                "surflock", "copy", "create" };
Acc g_acc[A_COUNT];
/* buffer lock classes: dynamic+DISCARD, dynamic+NOOVERWRITE, dynamic plain, static DEFAULT, MANAGED, other */
static unsigned g_bufClass[6];
static LARGE_INTEGER g_pFreq;
static LONGLONG g_pFrameStart, g_pSecStart, g_pMaxFrame;
static unsigned g_pFrames, g_pSlow;

LONGLONG qpc(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

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
            plog("profile: hooked vtable %p slot %d", (void *)vt, idx);
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
static double g_dipupVerts, g_dipupPrims, g_dipupBytes, g_dipupIdx; /* DIPUP sums over the dump period */
static unsigned g_dipupHist[8];   /* NumVertices buckets: <64 <256 <1k <4k <16k <64k more */
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
    if (g_drawKind[3])
        fprintf(f, "  DIPUP per call: verts %.0f prims %.0f bytes %.0f | NumVertices <64/<256/<1k/<4k/<16k/<64k/more: "
                   "%u %u %u %u %u %u %u\n", g_dipupVerts / g_drawKind[3], g_dipupPrims / g_drawKind[3],
                g_dipupBytes / g_drawKind[3], g_dipupHist[0], g_dipupHist[1], g_dipupHist[2], g_dipupHist[3],
                g_dipupHist[4], g_dipupHist[5], g_dipupHist[6] + g_dipupHist[7]);
    g_dipupVerts = g_dipupPrims = g_dipupBytes = g_dipupIdx = 0;
    memset(g_dipupHist, 0, sizeof g_dipupHist);
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
{ HRESULT hr; LONGLONG t0 = qpc();
  g_dipupVerts += b; g_dipupPrims += c; g_dipupBytes += (double)b * s; g_dipupIdx += i ? 1 : 0;
  { int k = 0; for (UINT x = b; x >= 64 && k < 7; x >>= 2) k++; g_dipupHist[k]++; }
  TIMED(A_DRAW, ORIG(84, PFN_DIPUP)(d, t, a, b, c, i, f, v, s)); g_drawKind[3]++; g_drawsSinceLock++; g_drawKindT[3] += qpc() - t0; return hr; }

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

void prof_install(IDirect3DDevice9 *dev)
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
    /* always on by default: start a fresh log once it passes 4 MB (about 4 hours of play) */
    WIN32_FILE_ATTRIBUTE_DATA fa;
    int fresh = GetFileAttributesExA(path, GetFileExInfoStandard, &fa) && (fa.nFileSizeHigh || fa.nFileSizeLow > (4u << 20));
    FILE *f = fopen(path, fresh ? "w" : "a");
    if (f) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(f, "--- profile start %04d-%02d-%02d %02d:%02d:%02d | Borderless=%d FPSLimit=%d BatchUI=%d FastDraw=%d\n"
                   "    ms per frame; n = calls per frame; game = frame - measured.\n"
                   "    buf = buffer locks per frame: dynDISCARD/dynNOOVERWRITE/dynPlain/staticDEFAULT/MANAGED/other\n",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, g_borderless, g_fpsLimit, g_batchUI,
                g_fastDraw);
        fclose(f);
    }
}

/* called once per frame, right after the real Present returned */
void prof_frame(LONGLONG now)
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
