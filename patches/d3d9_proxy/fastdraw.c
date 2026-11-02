#include "proxy.h"

/* ------------------------------------------------------------------ fast indexed draws (FastDraw=1)
 *
 * Proteus draws most battle geometry with DrawIndexedPrimitiveUP (data passed from memory). On
 * current drivers each such call costs ~25 us even for ~70 vertices, about 15x a DrawPrimitiveUP of the
 * same size. We copy the vertices and indices into our own dynamic buffers (ring buffers: NOOVERWRITE
 * while there is room, DISCARD when wrapping, so the CPU never waits for the GPU) and issue a normal
 * DrawIndexedPrimitive. Afterwards stream 0 and the index buffer are set to NULL, which is the state
 * DrawIndexedPrimitiveUP leaves behind. The buffers are D3DPOOL_DEFAULT: released before Reset and
 * recreated on the next draw.
 */
#define FD_VB_SIZE (8u << 20)
#define FD_IB_SIZE (2u << 20)

typedef HRESULT (STDMETHODCALLTYPE *PFN_DIPUP_)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void *,
                                                D3DFORMAT, const void *, UINT);
static PFN_DIPUP_ g_fdOrigDIPUP;
static IDirect3DVertexBuffer9 *g_fdVb;
static IDirect3DIndexBuffer9 *g_fdIb[2];          /* 16-bit, 32-bit indices */
static UINT g_fdVbPos, g_fdIbPos[2];
static int g_fdFailed;                            /* buffer creation failed: stay on the original path */

void fd_release(void)
{
    if (g_fdVb) { IDirect3DVertexBuffer9_Release(g_fdVb); g_fdVb = NULL; }
    for (int i = 0; i < 2; i++)
        if (g_fdIb[i]) { IDirect3DIndexBuffer9_Release(g_fdIb[i]); g_fdIb[i] = NULL; }
    g_fdVbPos = g_fdIbPos[0] = g_fdIbPos[1] = 0;
}

static UINT fd_index_count(D3DPRIMITIVETYPE t, UINT prims)
{
    switch (t) {
    case D3DPT_TRIANGLELIST:  return prims * 3;
    case D3DPT_TRIANGLESTRIP:
    case D3DPT_TRIANGLEFAN:   return prims + 2;
    case D3DPT_LINELIST:      return prims * 2;
    case D3DPT_LINESTRIP:     return prims + 1;
    case D3DPT_POINTLIST:     return prims;
    default:                  return 0;
    }
}

static int fd_ensure(IDirect3DDevice9 *dev, int i32)
{
    if (g_fdFailed) return 0;
    DWORD usage = D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY;
    D3DDEVICE_CREATION_PARAMETERS cp;
    if (SUCCEEDED(IDirect3DDevice9_GetCreationParameters(dev, &cp)) &&
        (cp.BehaviorFlags & (D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_MIXED_VERTEXPROCESSING)))
        usage |= D3DUSAGE_SOFTWAREPROCESSING;
    if (!g_fdVb && FAILED(IDirect3DDevice9_CreateVertexBuffer(dev, FD_VB_SIZE, usage, 0, D3DPOOL_DEFAULT, &g_fdVb, NULL))) {
        g_fdVb = NULL;
        g_fdFailed = 1;
        plog("fastdraw: vertex buffer creation failed, using the original draws");
        return 0;
    }
    if (!g_fdIb[i32] && FAILED(IDirect3DDevice9_CreateIndexBuffer(dev, FD_IB_SIZE, usage,
                                                                  i32 ? D3DFMT_INDEX32 : D3DFMT_INDEX16,
                                                                  D3DPOOL_DEFAULT, &g_fdIb[i32], NULL))) {
        g_fdIb[i32] = NULL;
        g_fdFailed = 1;
        plog("fastdraw: index buffer creation failed, using the original draws");
        return 0;
    }
    return 1;
}

static HRESULT STDMETHODCALLTYPE fd_DIPUP(IDirect3DDevice9 *dev, D3DPRIMITIVETYPE type, UINT minIdx, UINT numV,
                                          UINT prims, const void *idata, D3DFORMAT ifmt, const void *vdata,
                                          UINT stride)
{
    int i32 = ifmt == D3DFMT_INDEX32;
    UINT isz = i32 ? 4 : 2, nidx = fd_index_count(type, prims);
    UINT vbytes = numV * stride, ibytes = nidx * isz;
    if (!idata || !vdata || !stride || !numV || !nidx || (ifmt != D3DFMT_INDEX16 && !i32) ||
        vbytes > FD_VB_SIZE / 4 || ibytes > FD_IB_SIZE / 4 || !fd_ensure(dev, i32))
        return g_fdOrigDIPUP(dev, type, minIdx, numV, prims, idata, ifmt, vdata, stride);

    void *p;
    UINT voff = (g_fdVbPos + stride - 1) / stride * stride;
    DWORD vflag = D3DLOCK_NOOVERWRITE;
    if (voff + vbytes > FD_VB_SIZE) { voff = 0; vflag = D3DLOCK_DISCARD; }
    if (FAILED(IDirect3DVertexBuffer9_Lock(g_fdVb, voff, vbytes, &p, vflag)))
        return g_fdOrigDIPUP(dev, type, minIdx, numV, prims, idata, ifmt, vdata, stride);
    memcpy(p, (const BYTE *)vdata + (size_t)minIdx * stride, vbytes);
    IDirect3DVertexBuffer9_Unlock(g_fdVb);
    g_fdVbPos = voff + vbytes;

    UINT ioff = (g_fdIbPos[i32] + isz - 1) / isz * isz;
    DWORD iflag = D3DLOCK_NOOVERWRITE;
    if (ioff + ibytes > FD_IB_SIZE) { ioff = 0; iflag = D3DLOCK_DISCARD; }
    if (FAILED(IDirect3DIndexBuffer9_Lock(g_fdIb[i32], ioff, ibytes, &p, iflag)))
        return g_fdOrigDIPUP(dev, type, minIdx, numV, prims, idata, ifmt, vdata, stride);
    memcpy(p, idata, ibytes);
    IDirect3DIndexBuffer9_Unlock(g_fdIb[i32]);
    g_fdIbPos[i32] = ioff + ibytes;

    IDirect3DDevice9_SetStreamSource(dev, 0, g_fdVb, 0, stride);
    IDirect3DDevice9_SetIndices(dev, g_fdIb[i32]);
    HRESULT hr = IDirect3DDevice9_DrawIndexedPrimitive(dev, type, (INT)(voff / stride) - (INT)minIdx, minIdx, numV,
                                                       ioff / isz, prims);
    IDirect3DDevice9_SetStreamSource(dev, 0, NULL, 0, 0);
    IDirect3DDevice9_SetIndices(dev, NULL);
    return hr;
}

void fd_install(IDirect3DDevice9 *dev)
{
    if (!g_fastDraw || g_fdOrigDIPUP) return;
    void **vt = *(void ***)dev;
    g_fdOrigDIPUP = (PFN_DIPUP_)vt[84];
    patch_ptr(&vt[84], (void *)fd_DIPUP);
    plog("fastdraw: DrawIndexedPrimitiveUP redirected");
}
