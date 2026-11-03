#include "proxy.h"

/* ------------------------------------------------------------------ menu upscaling (Upscale=1/2)
 *
 * Borderless only. When the game's back buffer is smaller than the image area on the monitor (the
 * 800x600 front end), Present is replaced by: copy the back buffer into a texture, draw it with a
 * pixel shader into our own swap chain at monitor size (wood or black bars outside g_dst), present that.
 *   1 = Sharp:  "sharp bilinear" (integer prescale, then a 1-texel-wide linear blend at texel edges)
 *   2 = Smooth: Catmull-Rom bicubic + light unsharp mask
 * Shaders are HLSL compiled at startup with the system d3dcompiler_47.dll. The game's render states
 * are captured/restored with a state block; render target and depth surface are saved and restored.
 * D3DPOOL_DEFAULT objects are released before Reset and recreated on demand.
 */
typedef HRESULT (WINAPI *PFN_D3DCompile)(const void *, SIZE_T, const char *, const void *, void *, const char *,
                                         const char *, UINT, UINT, void **, void **);
typedef struct { void *(STDMETHODCALLTYPE *qi)(void *); ULONG (STDMETHODCALLTYPE *addref)(void *);
                 ULONG (STDMETHODCALLTYPE *release)(void *); void *(STDMETHODCALLTYPE *ptr)(void *);
                 SIZE_T (STDMETHODCALLTYPE *size)(void *); } BlobVtbl;
typedef struct { BlobVtbl *vt; } Blob;

static const char g_usVs[] =
    "struct V { float4 p : POSITION; float2 t : TEXCOORD0; };\n"
    "V main(V v) { return v; }\n";

static const char g_usPsSharp[] =
    "sampler2D s0 : register(s0);\n"
    "float4 c0 : register(c0);  /* tex w, h, 1/w, 1/h */\n"
    "float4 c1 : register(c1);  /* out w, h, prescale, sharpen */\n"
    "float4 main(float2 uv : TEXCOORD0) : COLOR {\n"
    "    float2 texel = uv * c0.xy;\n"
    "    float2 base = floor(texel);\n"
    "    float2 d = frac(texel) - 0.5;\n"
    "    float r = 0.5 - 0.5 / c1.z;\n"
    "    float2 f = (d - clamp(d, -r, r)) * c1.z + 0.5;\n"
    "    return float4(tex2D(s0, (base + f) * c0.zw).rgb, 1);\n"
    "}\n";

static const char g_usPsSmooth[] =
    "sampler2D s0 : register(s0);\n"
    "float4 c0 : register(c0);\n"
    "float4 c1 : register(c1);\n"
    "float4 main(float2 uv : TEXCOORD0) : COLOR {\n"
    "    float2 sp = uv * c0.xy;\n"
    "    float2 t1 = floor(sp - 0.5) + 0.5;\n"
    "    float2 f = sp - t1;\n"
    "    float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));\n"
    "    float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);\n"
    "    float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));\n"
    "    float2 w3 = f * f * (-0.5 + 0.5 * f);\n"
    "    float2 w12 = w1 + w2;\n"
    "    float2 p0 = (t1 - 1) * c0.zw, p3 = (t1 + 2) * c0.zw, p12 = (t1 + w2 / w12) * c0.zw;\n"
    "    float3 c = tex2D(s0, float2(p0.x, p0.y)).rgb * w0.x * w0.y\n"
    "             + tex2D(s0, float2(p12.x, p0.y)).rgb * w12.x * w0.y\n"
    "             + tex2D(s0, float2(p3.x, p0.y)).rgb * w3.x * w0.y\n"
    "             + tex2D(s0, float2(p0.x, p12.y)).rgb * w0.x * w12.y\n"
    "             + tex2D(s0, float2(p12.x, p12.y)).rgb * w12.x * w12.y\n"
    "             + tex2D(s0, float2(p3.x, p12.y)).rgb * w3.x * w12.y\n"
    "             + tex2D(s0, float2(p0.x, p3.y)).rgb * w0.x * w3.y\n"
    "             + tex2D(s0, float2(p12.x, p3.y)).rgb * w12.x * w3.y\n"
    "             + tex2D(s0, float2(p3.x, p3.y)).rgb * w3.x * w3.y;\n"
    "    float3 b = tex2D(s0, uv).rgb;\n"
    "    return float4(saturate(c + c1.w * (c - b)), 1);\n"
    "}\n";

static IDirect3DPixelShader9 *g_usPs[3];          /* [0] logo, [1] sharp, [2] smooth */

static const char g_usPsLogo[] =
    "sampler2D frame : register(s0);  /* the game's 800x600 frame, point */\n"
    "sampler2D spr : register(s1);    /* exact low-res logo sprite, point, A 0 = transparent */\n"
    "sampler2D hd : register(s2);     /* high-res logo, premultiplied, trilinear */\n"
    "float4 c0 : register(c0);        /* frame w, h, 1/w, 1/h */\n"
    "float4 c2 : register(c2);        /* sprite x, y, w, h in the frame */\n"
    "float4 c3 : register(c3);        /* parchment tone behind the logo */\n"
    "float4 main(float2 uv : TEXCOORD0) : COLOR {\n"
    "    float2 ti = floor(uv * c2.zw);\n"
    "    float opaque = 0, matched = 0;\n"
    "    for (int dy = -1; dy <= 1; dy++)\n"
    "        for (int dx = -1; dx <= 1; dx++) {\n"
    "            float2 t = ti + float2(dx, dy) + 0.5;\n"
    "            float4 s = tex2Dlod(spr, float4(t / c2.zw, 0, 0));\n"
    "            float3 f = tex2Dlod(frame, float4((c2.xy + t) * c0.zw, 0, 0)).rgb;\n"
    "            float3 d = abs(s.rgb - f);\n"
    "            if (s.a > 0.5) { opaque += 1; if (max(d.r, max(d.g, d.b)) < 0.02) matched += 1; }\n"
    "        }\n"
    "    if (opaque < 0.5 || matched < opaque) return float4(0, 0, 0, 0);  /* not the logo here: keep */\n"
    "    float4 h = tex2D(hd, uv);\n"
    "    float centre = tex2Dlod(spr, float4((ti + 0.5) / c2.zw, 0, 0)).a;\n"
    "    if (centre > 0.5) return float4(h.rgb + c3.rgb * (1 - h.a), 1);   /* replace the low-res logo pixel */\n"
    "    return h;                                                          /* over the parchment */\n"
    "}\n";

/* high-res main-menu logo (frontend_files/Main_Menu/MTW_LOGO_HD.raw, written by patches/logo/build_logo.py) */
static IDirect3DTexture9 *g_lgSpr, *g_lgHd;
static float g_lgRect[4], g_lgTone[4];
static int g_lgState;                  /* 0 = not loaded yet, 1 = ready, -1 = unavailable */

static IDirect3DTexture9 *lg_texture(IDirect3DDevice9 *dev, UINT w, UINT h, const BYTE *bgra, int mips)
{
    IDirect3DTexture9 *t = NULL;
    D3DLOCKED_RECT lr;
    if (FAILED(IDirect3DDevice9_CreateTexture(dev, w, h, mips ? 0 : 1, mips ? D3DUSAGE_AUTOGENMIPMAP : 0,
                                              D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t, NULL)) &&
        (!mips || FAILED(IDirect3DDevice9_CreateTexture(dev, w, h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t, NULL))))
        return NULL;
    if (FAILED(IDirect3DTexture9_LockRect(t, 0, &lr, NULL, 0))) { IDirect3DTexture9_Release(t); return NULL; }
    for (UINT y = 0; y < h; y++) memcpy((BYTE *)lr.pBits + y * lr.Pitch, bgra + (size_t)y * w * 4, (size_t)w * 4);
    IDirect3DTexture9_UnlockRect(t, 0);
    if (mips) IDirect3DTexture9_GenerateMipSubLevels(t);
    return t;
}

static int lg_load(IDirect3DDevice9 *dev)
{
    if (g_lgState) return g_lgState > 0;
    g_lgState = -1;
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%sfrontend_files\\Main_Menu\\MTW_LOGO_HD.raw", g_dir);
    FILE *f = fopen(path, "rb");
    if (!f) { plog("logo: %s not found, no high-res logo", path); return 0; }
    UINT hdr[10];
    BYTE *data = NULL;
    if (fread(hdr, 4, 10, f) == 10 && !memcmp(hdr, "MRHD", 4) && hdr[1] == 1 && hdr[2] <= 1024 && hdr[3] <= 1024 &&
        hdr[6] <= 4096 && hdr[7] <= 4096) {
        size_t ns = (size_t)hdr[2] * hdr[3] * 4, nh = (size_t)hdr[6] * hdr[7] * 4;
        data = malloc(ns + nh);
        if (data && fread(data, 1, ns + nh, f) == ns + nh) {
            g_lgSpr = lg_texture(dev, hdr[2], hdr[3], data, 0);
            g_lgHd = lg_texture(dev, hdr[6], hdr[7], data + ns, 1);
            g_lgRect[0] = (float)hdr[4]; g_lgRect[1] = (float)hdr[5];
            g_lgRect[2] = (float)hdr[2]; g_lgRect[3] = (float)hdr[3];
            g_lgTone[0] = ((hdr[8] >> 16) & 255) / 255.0f;
            g_lgTone[1] = ((hdr[8] >> 8) & 255) / 255.0f;
            g_lgTone[2] = (hdr[8] & 255) / 255.0f;
            if (g_lgSpr && g_lgHd) g_lgState = 1;
        }
    }
    free(data);
    fclose(f);
    plog("logo: high-res logo %s", g_lgState > 0 ? "loaded" : "unusable");
    return g_lgState > 0;
}

/* second pass, inside the upscaler's scene: the high-res logo over the sprite's area */
static void lg_draw(IDirect3DDevice9 *dev, const D3DSURFACE_DESC *bd, int dw, int dh)
{
    if (bd->Width != 800 || bd->Height != 600 || !g_usPs[0] || !lg_load(dev)) return;
    float fw = (float)bd->Width, fh = (float)bd->Height;
    float x0 = g_lgRect[0] / fw * 2 - 1, x1 = (g_lgRect[0] + g_lgRect[2]) / fw * 2 - 1;
    float y0 = 1 - g_lgRect[1] / fh * 2, y1 = 1 - (g_lgRect[1] + g_lgRect[3]) / fh * 2;
    float hx = 1.0f / dw, hy = 1.0f / dh;
    float q[4][6] = {
        { x0 - hx, y0 + hy, 0, 1, 0, 0 }, { x1 - hx, y0 + hy, 0, 1, 1, 0 },
        { x0 - hx, y1 + hy, 0, 1, 0, 1 }, { x1 - hx, y1 + hy, 0, 1, 1, 1 },
    };
    IDirect3DDevice9_SetPixelShader(dev, g_usPs[0]);
    IDirect3DDevice9_SetPixelShaderConstantF(dev, 2, g_lgRect, 1);
    IDirect3DDevice9_SetPixelShaderConstantF(dev, 3, g_lgTone, 1);
    IDirect3DDevice9_SetTexture(dev, 1, (IDirect3DBaseTexture9 *)g_lgSpr);
    IDirect3DDevice9_SetTexture(dev, 2, (IDirect3DBaseTexture9 *)g_lgHd);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    for (int s = 1; s < 3; s++) {
        DWORD filt = s == 1 ? D3DTEXF_POINT : D3DTEXF_LINEAR;
        IDirect3DDevice9_SetSamplerState(dev, s, D3DSAMP_MINFILTER, filt);
        IDirect3DDevice9_SetSamplerState(dev, s, D3DSAMP_MAGFILTER, filt);
        IDirect3DDevice9_SetSamplerState(dev, s, D3DSAMP_MIPFILTER, s == 2 ? D3DTEXF_LINEAR : D3DTEXF_NONE);
        IDirect3DDevice9_SetSamplerState(dev, s, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        IDirect3DDevice9_SetSamplerState(dev, s, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        IDirect3DDevice9_SetSamplerState(dev, s, D3DSAMP_SRGBTEXTURE, FALSE);
        IDirect3DDevice9_SetSamplerState(dev, s, D3DSAMP_MAXMIPLEVEL, 0);
    }
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHABLENDENABLE, TRUE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_SRCBLEND, D3DBLEND_ONE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_BLENDOP, D3DBLENDOP_ADD);
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, q, sizeof q[0]);
    IDirect3DDevice9_SetTexture(dev, 1, NULL);
    IDirect3DDevice9_SetTexture(dev, 2, NULL);
}

/* side bars (MenuSides=1): the game's own wood texture (textures/campmap/Wood2.tga) instead of black around
 * the 4:3 image, darker towards the screen edges, with a soft shadow next to the image */
static const char g_usPsSides[] =
    "sampler2D wood : register(s0);  /* tiled, linear, wrap */\n"
    "float4 c4 : register(c4);       /* monitor w, h, tile size in pixels, shadow width in pixels */\n"
    "float4 c5 : register(c5);       /* image rect l, t, r, b */\n"
    "float4 main(float2 uv : TEXCOORD0) : COLOR {\n"
    "    float2 p = uv * c4.xy;\n"
    "    float dx = max(max(c5.x - p.x, p.x - c5.z), 0), dy = max(max(c5.y - p.y, p.y - c5.w), 0);\n"
    "    float edge = saturate(max(dx / max(c5.x, 1), dy / max(c5.y, 1)));  /* 0 at the image, 1 at the screen edge */\n"
    "    float shadow = lerp(0.5, 1.0, saturate(max(dx, dy) / c4.w));\n"
    "    float3 c = tex2D(wood, p / c4.z).rgb * (1 - 0.45 * edge) * shadow;\n"
    "    return float4(c, 1);\n"
    "}\n";
static IDirect3DPixelShader9 *g_usPsSidesObj;
static IDirect3DTexture9 *g_sdWood;
static int g_sdState;                  /* 0 = not loaded yet, 1 = ready, -1 = unavailable */

/* 24-bit or 32-bit uncompressed TGA -> BGRA texture */
static int sd_load(IDirect3DDevice9 *dev)
{
    if (g_sdState) return g_sdState > 0;
    g_sdState = -1;
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%stextures\\campmap\\Wood2.tga", g_dir);
    FILE *f = fopen(path, "rb");
    if (!f) { plog("sides: %s not found, black bars", path); return 0; }
    BYTE h[18];
    BYTE *src = NULL, *bgra = NULL;
    if (fread(h, 1, 18, f) == 18 && h[2] == 2 && (h[16] == 24 || h[16] == 32) && !fseek(f, 18 + h[0], SEEK_SET)) {
        UINT w = h[12] | h[13] << 8, ht = h[14] | h[15] << 8, bpp = h[16] / 8;
        if (w && ht && w <= 2048 && ht <= 2048) {
            src = malloc((size_t)w * ht * bpp);
            bgra = malloc((size_t)w * ht * 4);
            if (src && bgra && fread(src, bpp, (size_t)w * ht, f) == (size_t)w * ht) {
                for (UINT y = 0; y < ht; y++) {
                    const BYTE *s = src + (size_t)((h[17] & 0x20) ? y : ht - 1 - y) * w * bpp;
                    for (UINT x = 0; x < w; x++) {
                        BYTE *d = bgra + ((size_t)y * w + x) * 4;
                        d[0] = s[x * bpp]; d[1] = s[x * bpp + 1]; d[2] = s[x * bpp + 2]; d[3] = 255;
                    }
                }
                g_sdWood = lg_texture(dev, w, ht, bgra, 1);
                if (g_sdWood) g_sdState = 1;
            }
        }
    }
    free(src);
    free(bgra);
    fclose(f);
    plog("sides: wood texture %s", g_sdState > 0 ? "loaded" : "unusable");
    return g_sdState > 0;
}

/* first pass, before the image: wood over the whole monitor (the image is drawn over it) */
static void sd_draw(IDirect3DDevice9 *dev, UINT mw, UINT mh, int dh, UINT bh)
{
    if (!g_menuSides || !g_usPsSidesObj || !sd_load(dev)) return;
    D3DVIEWPORT9 vp = { 0, 0, mw, mh, 0.0f, 1.0f };
    IDirect3DDevice9_SetViewport(dev, &vp);
    float k = (float)dh / bh;                       /* monitor pixels per game pixel */
    float c[8] = { (float)mw, (float)mh, 512.0f * k, 6.0f * k,
                   (float)g_dst.left, (float)g_dst.top, (float)g_dst.right, (float)g_dst.bottom };
    IDirect3DDevice9_SetPixelShaderConstantF(dev, 4, c, 2);
    IDirect3DDevice9_SetPixelShader(dev, g_usPsSidesObj);
    IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)g_sdWood);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_SRGBTEXTURE, FALSE);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MAXMIPLEVEL, 0);
    float hx = 1.0f / mw, hy = 1.0f / mh;
    float q[4][6] = {
        { -1 - hx,  1 + hy, 0, 1, 0, 0 }, { 1 - hx,  1 + hy, 0, 1, 1, 0 },
        { -1 - hx, -1 + hy, 0, 1, 0, 1 }, { 1 - hx, -1 + hy, 0, 1, 1, 1 },
    };
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, q, sizeof q[0]);
}

static IDirect3DVertexShader9 *g_usVsObj;
static IDirect3DVertexDeclaration9 *g_usDecl;
static IDirect3DSwapChain9 *g_usChain;
static IDirect3DTexture9 *g_usTex;
static IDirect3DStateBlock9 *g_usState;
static UINT g_usTexW, g_usTexH;
static int g_usBroken;                            /* setup failed: plain Present from now on */

void us_release(void)
{
    if (g_usChain) { IDirect3DSwapChain9_Release(g_usChain); g_usChain = NULL; }
    if (g_usTex) { IDirect3DTexture9_Release(g_usTex); g_usTex = NULL; }
    if (g_usState) { IDirect3DStateBlock9_Release(g_usState); g_usState = NULL; }
}

static void *us_compile(const char *src, const char *target)
{
    static PFN_D3DCompile compile;
    if (!compile) {
        HMODULE m = LoadLibraryA("d3dcompiler_47.dll");
        compile = m ? (PFN_D3DCompile)GetProcAddress(m, "D3DCompile") : NULL;
        if (!compile) { plog("upscale: d3dcompiler_47.dll not available"); return NULL; }
    }
    Blob *code = NULL, *err = NULL;
    HRESULT hr = compile(src, strlen(src), "upscale", NULL, NULL, "main", target, 0, 0, (void **)&code, (void **)&err);
    if (FAILED(hr)) {
        plog("upscale: %s compile failed: %s", target, err ? (const char *)err->vt->ptr(err) : "?");
        if (err) err->vt->release(err);
        return NULL;
    }
    if (err) err->vt->release(err);
    return code;
}

static int us_setup(IDirect3DDevice9 *dev, const D3DSURFACE_DESC *bd)
{
    if (g_usBroken) return 0;
    if (!g_usVsObj) {
        Blob *b = us_compile(g_usVs, "vs_3_0");
        if (!b || FAILED(IDirect3DDevice9_CreateVertexShader(dev, b->vt->ptr(b), &g_usVsObj))) goto fail;
        b->vt->release(b);
        const char *src[3] = { g_usPsLogo, g_usPsSharp, g_usPsSmooth };
        for (int i = 0; i < 3; i++) {
            b = us_compile(src[i], "ps_3_0");
            if (!b || FAILED(IDirect3DDevice9_CreatePixelShader(dev, b->vt->ptr(b), &g_usPs[i]))) goto fail;
            b->vt->release(b);
        }
        b = us_compile(g_usPsSides, "ps_3_0");      /* optional: black bars without it */
        if (b && FAILED(IDirect3DDevice9_CreatePixelShader(dev, b->vt->ptr(b), &g_usPsSidesObj))) g_usPsSidesObj = NULL;
        if (b) b->vt->release(b);
        static const D3DVERTEXELEMENT9 el[] = {
            { 0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
            { 0, 16, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
            D3DDECL_END()
        };
        if (FAILED(IDirect3DDevice9_CreateVertexDeclaration(dev, el, &g_usDecl))) goto fail;
    }
    UINT mw = (UINT)(g_mon.right - g_mon.left), mh = (UINT)(g_mon.bottom - g_mon.top);
    if (!g_usChain) {
        D3DPRESENT_PARAMETERS pp = { 0 };
        pp.BackBufferWidth = mw;
        pp.BackBufferHeight = mh;
        pp.BackBufferFormat = D3DFMT_UNKNOWN;
        pp.BackBufferCount = 1;
        pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
        pp.hDeviceWindow = g_hwnd;
        pp.Windowed = TRUE;
        pp.PresentationInterval = g_presentInterval;
        if (FAILED(IDirect3DDevice9_CreateAdditionalSwapChain(dev, &pp, &g_usChain))) { g_usChain = NULL; goto fail; }
    }
    if (g_usTex && (g_usTexW != bd->Width || g_usTexH != bd->Height)) {
        IDirect3DTexture9_Release(g_usTex);
        g_usTex = NULL;
    }
    if (!g_usTex) {
        if (FAILED(IDirect3DDevice9_CreateTexture(dev, bd->Width, bd->Height, 1, D3DUSAGE_RENDERTARGET, bd->Format,
                                                  D3DPOOL_DEFAULT, &g_usTex, NULL))) { g_usTex = NULL; goto fail; }
        g_usTexW = bd->Width;
        g_usTexH = bd->Height;
    }
    if (!g_usState && FAILED(IDirect3DDevice9_CreateStateBlock(dev, D3DSBT_ALL, &g_usState))) { g_usState = NULL; goto fail; }
    return 1;
fail:
    plog("upscale: setup failed, using the plain stretch");
    g_usBroken = 1;
    us_release();
    return 0;
}

/* debug (DumpFrame=1): write the 300th back buffer to frame.raw (u32 w, h, format, bytes/pixel, rows) */
static void us_dump(IDirect3DDevice9 *dev, IDirect3DSurface9 *bb, const D3DSURFACE_DESC *bd)
{
    static int n;
    if (!g_dumpFrame || ++n != 300) return;
    UINT bpp = bd->Format == D3DFMT_R5G6B5 ? 2 : 4;
    plog("dump: %ux%u format %d", bd->Width, bd->Height, (int)bd->Format);
    IDirect3DSurface9 *sys = NULL;
    D3DLOCKED_RECT lr;
    if (FAILED(IDirect3DDevice9_CreateOffscreenPlainSurface(dev, bd->Width, bd->Height, bd->Format, D3DPOOL_SYSTEMMEM,
                                                            &sys, NULL))) { plog("dump: no surface"); return; }
    if (SUCCEEDED(IDirect3DDevice9_GetRenderTargetData(dev, bb, sys)) &&
        SUCCEEDED(IDirect3DSurface9_LockRect(sys, &lr, NULL, D3DLOCK_READONLY))) {
        char path[MAX_PATH];
        snprintf(path, sizeof path, "%sframe.raw", g_dir);
        FILE *f = fopen(path, "wb");
        if (f) {
            UINT wh[4] = { bd->Width, bd->Height, (UINT)bd->Format, bpp };
            fwrite(wh, 4, 4, f);
            for (UINT y = 0; y < bd->Height; y++) fwrite((BYTE *)lr.pBits + y * lr.Pitch, bpp, bd->Width, f);
            fclose(f);
        }
        IDirect3DSurface9_UnlockRect(sys);
    }
    IDirect3DSurface9_Release(sys);
}

/* returns 1 if it presented the frame */
int us_present(IDirect3DDevice9 *dev)
{
    IDirect3DSurface9 *bb = NULL, *texSurf = NULL, *out = NULL, *oldRt = NULL, *oldDs = NULL;
    D3DSURFACE_DESC bd;
    int done = 0;
    if (FAILED(IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return 0;
    IDirect3DSurface9_GetDesc(bb, &bd);
    us_dump(dev, bb, &bd);
    int dw = g_dst.right - g_dst.left, dh = g_dst.bottom - g_dst.top;
    if ((int)bd.Width >= dw || (int)bd.Height >= dh || !us_setup(dev, &bd)) goto out;

    IDirect3DStateBlock9_Capture(g_usState);
    IDirect3DDevice9_GetRenderTarget(dev, 0, &oldRt);
    IDirect3DDevice9_GetDepthStencilSurface(dev, &oldDs);
    IDirect3DTexture9_GetSurfaceLevel(g_usTex, 0, &texSurf);
    IDirect3DSwapChain9_GetBackBuffer(g_usChain, 0, D3DBACKBUFFER_TYPE_MONO, &out);
    if (!texSurf || !out || FAILED(IDirect3DDevice9_StretchRect(dev, bb, NULL, texSurf, NULL, D3DTEXF_POINT)))
        goto restore;

    IDirect3DDevice9_SetRenderTarget(dev, 0, out);
    IDirect3DDevice9_SetDepthStencilSurface(dev, NULL);
    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);
    IDirect3DDevice9_SetVertexDeclaration(dev, g_usDecl);
    IDirect3DDevice9_SetVertexShader(dev, g_usVsObj);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ZWRITEENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHABLENDENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHATESTENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_SCISSORTESTENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_STENCILENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_FOGENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_SRGBWRITEENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_COLORWRITEENABLE, 0xF);
    float prescale = (float)(dh / (int)bd.Height);
    if (prescale < 1) prescale = 1;
    float c[8] = { (float)bd.Width, (float)bd.Height, 1.0f / bd.Width, 1.0f / bd.Height,
                   (float)dw, (float)dh, prescale, 0.35f };
    /* full-viewport quad, shifted half a pixel (D3D9 pixel centres) */
    float hx = 1.0f / dw, hy = 1.0f / dh;
    float q[4][6] = {
        { -1 - hx,  1 + hy, 0, 1, 0, 0 }, { 1 - hx,  1 + hy, 0, 1, 1, 0 },
        { -1 - hx, -1 + hy, 0, 1, 0, 1 }, { 1 - hx, -1 + hy, 0, 1, 1, 1 },
    };
    if (SUCCEEDED(IDirect3DDevice9_BeginScene(dev))) {
        sd_draw(dev, (UINT)(g_mon.right - g_mon.left), (UINT)(g_mon.bottom - g_mon.top), dh, bd.Height);
        D3DVIEWPORT9 vp = { (DWORD)g_dst.left, (DWORD)g_dst.top, (DWORD)dw, (DWORD)dh, 0.0f, 1.0f };
        IDirect3DDevice9_SetViewport(dev, &vp);
        IDirect3DDevice9_SetPixelShaderConstantF(dev, 0, c, 2);
        IDirect3DDevice9_SetPixelShader(dev, g_usPs[g_upscale]);
        IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)g_usTex);
        IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_SRGBTEXTURE, FALSE);
        IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, q, sizeof q[0]);
        lg_draw(dev, &bd, dw, dh);
        IDirect3DDevice9_EndScene(dev);
        done = SUCCEEDED(IDirect3DSwapChain9_Present(g_usChain, NULL, NULL, NULL, NULL, 0));
    }
restore:
    IDirect3DDevice9_SetTexture(dev, 0, NULL);
    if (oldRt) IDirect3DDevice9_SetRenderTarget(dev, 0, oldRt);
    IDirect3DDevice9_SetDepthStencilSurface(dev, oldDs);
    IDirect3DStateBlock9_Apply(g_usState);
out:
    if (texSurf) IDirect3DSurface9_Release(texSurf);
    if (out) IDirect3DSurface9_Release(out);
    if (oldRt) IDirect3DSurface9_Release(oldRt);
    if (oldDs) IDirect3DSurface9_Release(oldDs);
    IDirect3DSurface9_Release(bb);
    return done;
}
