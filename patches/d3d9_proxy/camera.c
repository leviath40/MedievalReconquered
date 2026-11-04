/* Middle mouse button: rotate the battle camera, as in modern strategy games.
 *
 * The game moves the camera towards a wanted view each frame, with one speed limit shared by position, height and
 * angles (around 0x4D6020). So the current angles are set along with the wanted ones: the turn then has no backlog, stops
 * with the mouse, and leaves the speed limit to the rest of the camera. The game's own Mouselook option uses the
 * right button and moves in fixed steps. Runs at Present, on the game's thread. */
#include "proxy.h"

#define WANT_PITCH   (*(int *)0xB43664)     /* 0x4000 = full turn */
#define WANT_YAW     (*(int *)0xB43668)
#define CUR_PITCH    (*(int *)0x8D6770)
#define CUR_YAW      (*(int *)0x8D6774)
#define ANGLE_MASK   (*(int *)0x78B94C)
#define BATTLE       (*(void **)0xB4275C)   /* null outside a battle */

#define SPEED 6                             /* angle units per pixel: about 0.13 degrees */
#define TILT_MAX 0xF00                      /* about 84 degrees from level; exactly 90 is left out, the view has no up there */

int g_mouseRotate = 1, g_camDrag = 0;
static POINT g_grab;

void cam_frame(void)
{
    POINT p;
    int tilt;
    int held = g_mouseRotate && BATTLE && (GetAsyncKeyState(VK_MBUTTON) & 0x8000) &&
               GetForegroundWindow() == g_hwnd;
    if (!held || !GetCursorPos(&p)) {
        g_camDrag = 0;
        return;
    }
    if (!g_camDrag) {
        g_camDrag = 1;
        g_grab = p;
        return;
    }
    if (p.x == g_grab.x && p.y == g_grab.y) return;

    SetCursorPos(g_grab.x, g_grab.y);       /* the cursor stays put; wndproc hides the moves from the game */
    tilt = CUR_PITCH & ANGLE_MASK;
    if (tilt >= 0x2000) tilt -= 0x4000;
    tilt += (p.y - g_grab.y) * SPEED;
    if (tilt > TILT_MAX) tilt = TILT_MAX;
    if (tilt < -TILT_MAX) tilt = -TILT_MAX;
    CUR_PITCH = WANT_PITCH = tilt & ANGLE_MASK;
    CUR_YAW = WANT_YAW = (CUR_YAW + (p.x - g_grab.x) * SPEED) & ANGLE_MASK;
}

/* The game holds the tilt to 0x400 either way in 0x4D5600 and would pull a wider one back on the next key press. */
void patch_tilt_limit(void)
{
    static DWORD up[] = {0x4D5631, 0x4D5647, 0x4D5676, 0x4D5689}, down[] = {0x4D5651, 0x4D5660, 0x4D5695, 0x4D56A8};
    DWORD hi = TILT_MAX, lo = 0x4000 - TILT_MAX;
    int i, ok = 1;
    for (i = 0; i < 4; i++)
        ok &= patch_mem((void *)up[i], &hi, 4) && patch_mem((void *)down[i], &lo, 4);
    plog("camera: tilt limit %s", ok ? "widened" : "patch failed");
}
