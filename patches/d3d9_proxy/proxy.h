/* Shared declarations of the d3d9 proxy (see proxy.c for the overview). */
#ifndef PROXY_H
#define PROXY_H
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define COBJMACROS
#include <d3d9.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <mmsystem.h>

typedef IDirect3D9 *(WINAPI *PFN_Create9)(UINT);
typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateDevice)(IDirect3D9 *, UINT, D3DDEVTYPE, HWND, DWORD,
                                                      D3DPRESENT_PARAMETERS *, IDirect3DDevice9 **);
typedef HRESULT (STDMETHODCALLTYPE *PFN_Reset)(IDirect3DDevice9 *, D3DPRESENT_PARAMETERS *);
typedef HRESULT (STDMETHODCALLTYPE *PFN_Present)(IDirect3DDevice9 *, const RECT *, const RECT *,
                                                 HWND, const RGNDATA *);
typedef BOOL (WINAPI *PFN_GetCursorPos)(LPPOINT);
typedef BOOL (WINAPI *PFN_SetCursorPos)(int, int);
typedef BOOL (WINAPI *PFN_ClipCursor)(const RECT *);

/* config (MedievalReconquered.ini [Display]) */
extern int g_borderless, g_keepAspect, g_log, g_showFps, g_fpsLimit, g_profile, g_batchUI, g_fastDraw, g_upscale, g_dumpFrame, g_menuSides, g_iconLog, g_hdIcons;
extern int g_campW, g_campH, g_batW, g_batH;

/* window and image geometry (proxy.c) */
extern char g_dir[MAX_PATH];
extern HWND g_hwnd;
extern RECT g_mon, g_dst;
extern int g_bbW, g_bbH;
extern UINT g_presentInterval;

void plog(const char *fmt, ...);
int patch_mem(void *dst, const void *src, SIZE_T n);
int patch_ptr(void **slot, void *val);
void patch_icons(void);
/* one sprite blit (icons.c): spr is only valid during its frame (the game may free it), hash identifies it */
typedef struct { BYTE *spr; DWORD hash, last; int x, y, frame, cw, ch, w, h; } IcDraw;
#define IC_MAX 2048
extern IcDraw g_icDraws[IC_MAX];
extern int g_icN;
DWORD fnv(const BYTE *p, int n, DWORD h);

/* fps.c */
void draw_fps(IDirect3DDevice9 *dev);
void limit_fps(void);

/* profile.c */
typedef struct { LONGLONG t; unsigned n; } Acc;
enum { A_PRESENT, A_LIMIT, A_ENDSCENE, A_DRAW, A_BUF, A_TEX, A_SURF, A_COPY, A_CREATE, A_COUNT };
extern Acc g_acc[A_COUNT];
LONGLONG qpc(void);
#define TIMED(acc, call) do { LONGLONG t0_ = qpc(); hr = (call); g_acc[acc].t += qpc() - t0_; g_acc[acc].n++; } while (0)
void prof_install(IDirect3DDevice9 *dev);
void prof_frame(LONGLONG now);

/* fastdraw.c */
void fd_install(IDirect3DDevice9 *dev);
void fd_release(void);

/* upscale.c */
int us_present(IDirect3DDevice9 *dev);
void us_release(void);

/* game_patches.c */
void patch_video_menu(void);
void patch_resolution(void);
void patch_ui_batch(void);

#endif
