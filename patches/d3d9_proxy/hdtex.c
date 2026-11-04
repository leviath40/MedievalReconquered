/* HD battle textures. HDTextures=1 swaps game textures for hd_textures/<hash>.jpg or .png, TexSurvey=1 (debug) reports them.
 *
 * Ground tiles, skies and model textures reach D3D9 one by one as 1-level A1R5G5B5 managed textures and are never
 * rewritten, so one content hash at the first bind identifies them. UVs are normalised: a bigger picture just works.
 * Pictures are managed textures, so they survive Reset but cost twice their size in this 32-bit process: past
 * HD_BUDGET the ones unused for a while are released and reloaded if the game needs them again.
 *
 * The device vtable lives on the heap and D3D9 puts its own SetTexture back in it after install, so hd_frame
 * restores the hook once per frame. The survey lives here because both need that hook: two modules re-hooking one
 * slot every frame would chain into each other.
 *
 * Survey: every bound texture gets a line in tex_survey.txt and its top level is saved once in tex_dump/. The list
 * is rewritten every 5 seconds because the game is usually killed rather than closed. */
#include "proxy.h"

#define SLOTS 16384
#define HD_MAX 4096
#define HD_BUDGET (1400u << 20)
#define HD_IDLE_FRAMES 600
#define MAX_DUMP_BYTES (1500u << 20)

typedef struct {
    IDirect3DTexture9 *tex;
    UINT w, h, levels;
    DWORD usage, hash, stages;
    int fmt, pool, mode, hd;                     /* hd: 0 = not looked yet, -1 = none, else index + 1 */
    unsigned binds, updates, rewrites;
} Tex;

typedef struct { DWORD hash; IDirect3DTexture9 *tex; unsigned bytes, last; } HdEntry;
typedef struct { int on; DWORD mip, min, aniso; } Sampler;

typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateTex)(IDirect3DDevice9 *, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL,
                                                   IDirect3DTexture9 **, HANDLE *);
typedef HRESULT (STDMETHODCALLTYPE *PFN_UpdateTex)(IDirect3DDevice9 *, IDirect3DBaseTexture9 *, IDirect3DBaseTexture9 *);
typedef HRESULT (STDMETHODCALLTYPE *PFN_SetTex)(IDirect3DDevice9 *, DWORD, IDirect3DBaseTexture9 *);

static Tex *g_tex;
static HdEntry g_hd[HD_MAX];
static Sampler g_smp[8];
static int g_hdCount, g_hdHits;
static unsigned g_hdBytes, g_hdFrame, g_hdPeak, g_hdFreed;
static PFN_CreateTex g_createTex;
static PFN_UpdateTex g_updateTex;
static PFN_SetTex g_setTex;
static IDirect3DDevice9 *g_dev;
static unsigned g_created, g_unknown, g_dumpBytes, g_rehooks;
static SIZE_T g_minFree = (SIZE_T)-1;

static Tex *slot(const void *tex, int add)
{
    UINT i = (UINT)(((UINT_PTR)tex >> 4) * 2654435761u) % SLOTS;
    for (int n = 0; n < SLOTS; n++, i = (i + 1) % SLOTS) {
        if (g_tex[i].tex == tex) return &g_tex[i];
        if (!g_tex[i].tex) return add ? &g_tex[i] : NULL;
    }
    return NULL;
}

static int bytes_per_pixel(int fmt)
{
    switch (fmt) {
    case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8: return 4;
    case D3DFMT_R5G6B5: case D3DFMT_X1R5G5B5: case D3DFMT_A1R5G5B5: case D3DFMT_A4R4G4B4: case D3DFMT_X4R4G4B4: return 2;
    case D3DFMT_A8: case D3DFMT_L8: case D3DFMT_P8: return 1;
    }
    return 0;
}

/* FNV-1a of the top level, 0 when it cannot be read. The survey saves it the first time a hash is met. */
static DWORD read_tex(const Tex *t)
{
    int bpp = bytes_per_pixel(t->fmt);
    D3DLOCKED_RECT lr;
    if (!bpp || t->pool == D3DPOOL_DEFAULT || (t->usage & D3DUSAGE_RENDERTARGET)) return 0;
    if (FAILED(IDirect3DTexture9_LockRect(t->tex, 0, &lr, NULL, D3DLOCK_READONLY))) return 0;
    DWORD hash = 2166136261u;
    for (UINT y = 0; y < t->h; y++) {
        const BYTE *p = (const BYTE *)lr.pBits + y * lr.Pitch;
        for (UINT x = 0; x < t->w * bpp; x++) hash = (hash ^ p[x]) * 16777619u;
    }
    if (hash && g_texSurvey) {
        char path[MAX_PATH];
        snprintf(path, sizeof path, "%stex_dump\\%08lx_%ux%u_f%d.raw", g_dir, (unsigned long)hash, t->w, t->h, t->fmt);
        if (g_dumpBytes < MAX_DUMP_BYTES && GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
            FILE *f = fopen(path, "wb");
            if (f) {
                for (UINT y = 0; y < t->h; y++) fwrite((const BYTE *)lr.pBits + y * lr.Pitch, t->w * bpp, 1, f);
                fclose(f);
                g_dumpBytes += t->w * t->h * bpp;
            }
        }
    }
    IDirect3DTexture9_UnlockRect(t->tex, 0);
    return hash;
}

static IDirect3DTexture9 *hd_load(IDirect3DDevice9 *d, DWORD hash, unsigned *bytes)
{
    static const char *const ext[] = { "jpg", "png" };
    for (int i = 0; i < 2; i++) {
        char path[MAX_PATH];
        UINT w, h;
        snprintf(path, sizeof path, "%shd_textures\\%08lx.%s", g_dir, (unsigned long)hash, ext[i]);
        if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) continue;
        BYTE *px = hd_png(path, &w, &h);
        if (!px) return NULL;
        IDirect3DTexture9 *tex = lg_texture(d, w, h, px, 1);
        free(px);
        *bytes = w * h * 4 / 3 * 4 * 2;            /* with mips, video memory + system copy */
        return tex;
    }
    return NULL;
}

/* Index + 1 of the HD picture for this content, -1 if there is none: several game textures can share a picture. */
static int hd_lookup(IDirect3DDevice9 *d, DWORD hash)
{
    int i;
    for (i = 0; i < g_hdCount; i++) if (g_hd[i].hash == hash) return i + 1;
    if (g_hdCount >= HD_MAX) return -1;
    unsigned bytes = 0;
    IDirect3DTexture9 *tex = hd_load(d, hash, &bytes);
    if (!tex) return -1;
    g_hd[i].hash = hash; g_hd[i].tex = tex; g_hd[i].bytes = bytes;
    g_hdBytes += bytes;
    g_hdCount++;
    return i + 1;
}

/* Free the longest unused pictures. A picture still bound stays alive in D3D9 until the game binds something else. */
static void hd_trim(void)
{
    while (g_hdBytes > HD_BUDGET) {
        HdEntry *old = NULL;
        for (int i = 0; i < g_hdCount; i++)
            if (g_hd[i].tex && g_hd[i].last + HD_IDLE_FRAMES < g_hdFrame && (!old || g_hd[i].last < old->last)) old = &g_hd[i];
        if (!old) return;
        IDirect3DTexture9_Release(old->tex);
        old->tex = NULL;
        g_hdBytes -= old->bytes;
        g_hdFreed++;
    }
}

/* HD pictures have mips the game's textures never had, so the stage needs a mip filter while one is bound */
static void set_filter(IDirect3DDevice9 *d, DWORD stage, int hd)
{
    Sampler *s = &g_smp[stage & 7];
    if (hd == s->on) return;
    if (hd) {
        if (FAILED(IDirect3DDevice9_GetSamplerState(d, stage, D3DSAMP_MIPFILTER, &s->mip)) ||
            FAILED(IDirect3DDevice9_GetSamplerState(d, stage, D3DSAMP_MINFILTER, &s->min)) ||
            FAILED(IDirect3DDevice9_GetSamplerState(d, stage, D3DSAMP_MAXANISOTROPY, &s->aniso))) return;
        IDirect3DDevice9_SetSamplerState(d, stage, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
        IDirect3DDevice9_SetSamplerState(d, stage, D3DSAMP_MINFILTER, D3DTEXF_ANISOTROPIC);
        IDirect3DDevice9_SetSamplerState(d, stage, D3DSAMP_MAXANISOTROPY, 8);
    } else {
        IDirect3DDevice9_SetSamplerState(d, stage, D3DSAMP_MIPFILTER, s->mip);
        IDirect3DDevice9_SetSamplerState(d, stage, D3DSAMP_MINFILTER, s->min);
        IDirect3DDevice9_SetSamplerState(d, stage, D3DSAMP_MAXANISOTROPY, s->aniso);
    }
    s->on = hd;
}

static void write_list(void)
{
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%stex_survey.txt", g_dir);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "created %u, unknown binds %u, rehooks %u, hd %d, hd now %u MB peak %u MB freed %u, video memory free %u MB, "
               "lowest address space free %u MB\n", g_created, g_unknown, g_rehooks, g_hdHits, g_hdBytes >> 20, g_hdPeak >> 20,
            g_hdFreed, IDirect3DDevice9_GetAvailableTextureMem(g_dev) >> 20, (unsigned)(g_minFree >> 20));
    fprintf(f, "ptr w h levels usage fmt pool hash stages firstmode binds updates rewrites\n");
    for (int i = 0; i < SLOTS; i++) {
        const Tex *t = &g_tex[i];
        if (t->tex && (t->binds || t->updates))
            fprintf(f, "%p %u %u %u %#lx %d %d %08lx %#lx %d %u %u %u\n", (void *)t->tex, t->w, t->h, t->levels,
                    (unsigned long)t->usage, t->fmt, t->pool, (unsigned long)t->hash, (unsigned long)t->stages, t->mode,
                    t->binds, t->updates, t->rewrites);
    }
    fclose(f);
}

static HRESULT STDMETHODCALLTYPE ht_CreateTexture(IDirect3DDevice9 *d, UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT fmt,
                                                  D3DPOOL pool, IDirect3DTexture9 **out, HANDLE *share)
{
    HRESULT hr = g_createTex(d, w, h, levels, usage, fmt, pool, out, share);
    if (SUCCEEDED(hr) && out && *out) {
        Tex *t = slot(*out, 1);                    /* a recycled pointer starts again */
        g_created++;
        if (t) {
            memset(t, 0, sizeof *t);
            t->tex = *out; t->w = w; t->h = h; t->levels = levels; t->usage = usage; t->fmt = fmt; t->pool = pool;
        }
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE ht_UpdateTexture(IDirect3DDevice9 *d, IDirect3DBaseTexture9 *src, IDirect3DBaseTexture9 *dst)
{
    Tex *s = slot(src, 0), *t = slot(dst, 0);
    if (g_texSurvey) {
        if (s) s->hash = read_tex(s);
        if (s && t) t->hash = s->hash;
        if (t) t->updates++;
    }
    return g_updateTex(d, src, dst);
}

static HRESULT STDMETHODCALLTYPE ht_SetTexture(IDirect3DDevice9 *d, DWORD stage, IDirect3DBaseTexture9 *tex)
{
    IDirect3DTexture9 *hd = NULL;
    Tex *t = tex ? slot(tex, 0) : NULL;
    if (tex && !t) g_unknown++;
    if (t) {
        if (g_texSurvey) {
            if (!t->binds) t->mode = *(int *)0x8D671C;
            if (!(t->binds % 1000)) {              /* first bind, then look for textures the game rewrites */
                DWORD h = read_tex(t);
                if (h) {
                    if (t->hash && h != t->hash) t->rewrites++;
                    t->hash = h;
                }
            }
            t->binds++;
            t->stages |= 1u << (stage & 7);
        }
        if (g_hdTextures && t->fmt == D3DFMT_A1R5G5B5 && t->levels <= 1) {
            if (!t->hd) {
                t->hd = hd_lookup(d, t->hash ? t->hash : read_tex(t));
                if (t->hd > 0) g_hdHits++;
            }
            if (t->hd > 0) {
                HdEntry *e = &g_hd[t->hd - 1];
                if (!e->tex) {
                    e->tex = hd_load(d, e->hash, &e->bytes);
                    if (e->tex) g_hdBytes += e->bytes;
                }
                e->last = g_hdFrame;
                hd = e->tex;
            }
        }
    }
    HRESULT hr = g_setTex(d, stage, hd ? (IDirect3DBaseTexture9 *)hd : tex);
    if (g_hdTextures && stage < 8) set_filter(d, stage, hd != NULL);
    return hr;
}

void hd_frame(void)
{
    static DWORD last;
    if (!g_tex) return;
    g_hdFrame++;
    if (g_hdBytes > g_hdPeak) g_hdPeak = g_hdBytes;
    hd_trim();
    void **vt = *(void ***)g_dev;
    if (vt[65] != (void *)ht_SetTexture) {
        g_setTex = (PFN_SetTex)vt[65];
        patch_ptr(&vt[65], (void *)ht_SetTexture);
        g_rehooks++;
    }
    DWORD now = GetTickCount();
    if (g_texSurvey && now - last > 5000) {
        MEMORYSTATUSEX m = { sizeof m };
        if (GlobalMemoryStatusEx(&m) && (SIZE_T)m.ullAvailVirtual < g_minFree) g_minFree = (SIZE_T)m.ullAvailVirtual;
        last = now;
        write_list();
    }
}

void hd_install(IDirect3DDevice9 *dev)
{
    void **vt = *(void ***)dev;
    char path[MAX_PATH];
    if (g_tex) return;
    g_tex = (Tex *)calloc(SLOTS, sizeof(Tex));
    if (!g_tex) return;
    g_dev = dev;
    if (g_texSurvey) {
        snprintf(path, sizeof path, "%stex_dump", g_dir);
        CreateDirectoryA(path, NULL);
    }
    g_createTex = (PFN_CreateTex)vt[23];
    g_updateTex = (PFN_UpdateTex)vt[31];
    g_setTex = (PFN_SetTex)vt[65];
    patch_ptr(&vt[23], (void *)ht_CreateTexture);
    patch_ptr(&vt[31], (void *)ht_UpdateTexture);
    patch_ptr(&vt[65], (void *)ht_SetTexture);
}
