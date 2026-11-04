/* Smooth ground shadows for the HD unit cards (report section 14).
 *
 * In the 50x50 art a shadow is a grey blob with a pale fringe and a staircase outline, which the upscaler keeps.
 * This finds the blob in the low-res sprite and repaints it in the 200x200 picture as one flat colour with a
 * smooth outline. Pictures still contain the parchment here, the shader keys it out afterwards.
 * Thresholds were checked by eye against real cards: figures, grey horses and mail armour stay untouched. */
#include <stdlib.h>
#include <string.h>

#define N 50
#define Z 4
#define HD (N * Z)

typedef unsigned char u8;

static int s_op(const u8 *l, int x, int y) { return x >= 0 && y >= 0 && x < N && y < N && l[(y * N + x) * 4 + 3]; }
static int s_r(const u8 *l, int x, int y) { return l[(y * N + x) * 4 + 2]; }
static int s_g(const u8 *l, int x, int y) { return l[(y * N + x) * 4 + 1]; }
static int s_b(const u8 *l, int x, int y) { return l[(y * N + x) * 4]; }

static int s_grey(const u8 *l, int x, int y)
{
    if (!s_op(l, x, y)) return 0;
    int r = s_r(l, x, y), g = s_g(l, x, y), b = s_b(l, x, y);
    return abs(r - g) <= 16 && g - b >= -6 && g - b <= 34;
}

static const int s_dx[4] = { 1, -1, 0, 0 }, s_dy[4] = { 0, 0, 1, -1 };

/* l: low-res sprite, BGRA, alpha 0 = transparent. hd: picture on parchment, BGRA. Returns 1 if it changed hd. */
int card_shadow(const u8 *l, u8 *hd)
{
    static const float K[3] = { 207, 207, 199 };             /* parchment, r g b */
    u8 d[N][N], o[N][N], m[N][N], r[N][N], seen[N][N];
    short stack[N * N * 4][2], cell[N * N][2];
    memset(d, 0, sizeof d); memset(o, 0, sizeof o); memset(m, 0, sizeof m); memset(r, 0, sizeof r);
    memset(seen, 0, sizeof seen);
    for (int y = N * 31 / 50; y < N; y++)
        for (int x = 0; x < N; x++)
            d[y][x] = s_grey(l, x, y) && s_g(l, x, y) >= 60 && s_g(l, x, y) <= 175;
    for (int y = 0; y + 1 < N; y++)                          /* 2x2 opening: drops the lines beside spears */
        for (int x = 0; x + 1 < N; x++)
            if (d[y][x] && d[y][x + 1] && d[y + 1][x] && d[y + 1][x + 1])
                o[y][x] = o[y][x + 1] = o[y + 1][x] = o[y + 1][x + 1] = 1;
    int found = 0;
    for (int sy = 0; sy < N; sy++)
        for (int sx = 0; sx < N; sx++) {
            if (!o[sy][sx] || seen[sy][sx]) continue;
            u8 in[N][N];
            int nc = 0, sp = 0, x0 = N, x1 = -1, y0 = N, y1 = -1, sumy = 0;
            memset(in, 0, sizeof in);
            seen[sy][sx] = 1; stack[sp][0] = (short)sx; stack[sp++][1] = (short)sy;
            while (sp) {
                int x = stack[--sp][0], y = stack[sp][1];
                cell[nc][0] = (short)x; cell[nc++][1] = (short)y;
                in[y][x] = 1;
                sumy += y;
                if (x < x0) x0 = x;
                if (x > x1) x1 = x;
                if (y < y0) y0 = y;
                if (y > y1) y1 = y;
                for (int k = 0; k < 4; k++) {
                    int nx = x + s_dx[k], ny = y + s_dy[k];
                    if (nx < 0 || ny < 0 || nx >= N || ny >= N || !o[ny][nx] || seen[ny][nx]) continue;
                    if (abs(s_g(l, nx, ny) - s_g(l, x, y)) > 40) continue;
                    seen[ny][nx] = 1; stack[sp][0] = (short)nx; stack[sp++][1] = (short)ny;
                }
            }
            /* a blob reaching the top of the search area is the lower part of something taller (shield, horse) */
            int bw = x1 - x0 + 1, bh = y1 - y0 + 1;
            if (y0 <= N * 31 / 50 || nc < 10 || bw < 6 || bw * 10 < bh * 9 || sumy * 100 < nc * N * 55) continue;
            int per = 0, out = 0;
            for (int i = 0; i < nc; i++)
                for (int k = 0; k < 4; k++) {
                    int nx = cell[i][0] + s_dx[k], ny = cell[i][1] + s_dy[k];
                    if (nx >= 0 && ny >= 0 && nx < N && ny < N && in[ny][nx]) continue;
                    per++;
                    if (!s_op(l, nx, ny) || (s_grey(l, nx, ny) && s_g(l, nx, ny) >= 150)) out++;
                }
            if (out * 2 < per) continue;
            /* only the blob's own flat tone: grey legs joined to the shadow are textured and drop out */
            int hist[256], seenc = 0, med = 0;
            memset(hist, 0, sizeof hist);
            for (int i = 0; i < nc; i++) hist[s_g(l, cell[i][0], cell[i][1])]++;
            for (int v = 0; v < 256; v++) { seenc += hist[v]; if (seenc * 2 >= nc) { med = v; break; } }
            u8 flat[N][N];
            memset(flat, 0, sizeof flat);
            for (int i = 0; i < nc; i++)
                if (abs(s_g(l, cell[i][0], cell[i][1]) - med) <= 20) flat[cell[i][1]][cell[i][0]] = 1;
            for (int y = 0; y + 1 < N; y++)
                for (int x = 0; x + 1 < N; x++)
                    if (flat[y][x] && flat[y][x + 1] && flat[y + 1][x] && flat[y + 1][x + 1]) {
                        m[y][x] = m[y][x + 1] = m[y + 1][x] = m[y + 1][x + 1] = 1;
                        found = 1;
                    }
        }
    if (!found) return 0;

    for (int pass = 0; pass < 3; pass++) {                   /* fill the gaps: mottled horse shadows lose cells above */
        u8 add[N][N];
        memset(add, 0, sizeof add);
        for (int y = N * 31 / 50; y < N; y++)
            for (int x = 0; x < N; x++) {
                if (m[y][x] || !s_grey(l, x, y) || s_g(l, x, y) < 60 || s_g(l, x, y) > 175) continue;
                int near = 0;
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++) {
                        int nx = x + dx, ny = y + dy;
                        if (nx >= 0 && ny >= 0 && nx < N && ny < N && m[ny][nx]) near++;
                    }
                add[y][x] = near >= 4;
            }
        for (int y = 0; y < N; y++)
            for (int x = 0; x < N; x++) if (add[y][x]) m[y][x] = 1;
    }

    float s[3] = { 0, 0, 0 };
    int cnt = 0;
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) {
            if (!m[y][x]) continue;
            s[0] += (float)s_r(l, x, y); s[1] += (float)s_g(l, x, y); s[2] += (float)s_b(l, x, y);
            cnt++;
            for (int dy = -1; dy <= 1; dy++)                 /* fringe */
                for (int dx = -1; dx <= 1; dx++) {
                    int nx = x + dx, ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= N || ny >= N || m[ny][nx] || !s_grey(l, nx, ny) || s_g(l, nx, ny) < 140) continue;
                    for (int k = 0; k < 4; k++)      /* between the blob and the transparency, not armour */
                        if (!s_op(l, nx + s_dx[k], ny + s_dy[k])) r[ny][nx] = 1;
                }
        }
    for (int i = 0; i < 3; i++) s[i] /= (float)cnt;

    float g[N][N], t[N][N];
    for (int y = 0; y < N; y++) for (int x = 0; x < N; x++) g[y][x] = m[y][x];
    for (int pass = 0; pass < 2; pass++) {                   /* 1-2-1 blur, twice */
        for (int y = 0; y < N; y++)
            for (int x = 0; x < N; x++) {
                float sum = 0;
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++) {
                        int nx = x + dx, ny = y + dy;
                        if (nx >= 0 && ny >= 0 && nx < N && ny < N)
                            sum += g[ny][nx] * (float)((2 - abs(dx)) * (2 - abs(dy)));
                    }
                t[y][x] = sum / 16;
            }
        memcpy(g, t, sizeof g);
    }
    for (int py = 0; py < HD; py++)
        for (int px = 0; px < HD; px++) {
            float u = (px + 0.5f) / Z - 0.5f, v = (py + 0.5f) / Z - 0.5f;
            int cx = u < 0 ? -1 : (int)u, cy = v < 0 ? -1 : (int)v;
            float fx = u - (float)cx, fy = v - (float)cy;
            int x0 = cx < 0 ? 0 : cx, y0 = cy < 0 ? 0 : cy, x1 = cx + 1 >= N ? N - 1 : cx + 1, y1 = cy + 1 >= N ? N - 1 : cy + 1;
            float c = g[y0][x0] * (1 - fx) * (1 - fy) + g[y0][x1] * fx * (1 - fy) + g[y1][x0] * (1 - fx) * fy + g[y1][x1] * fx * fy;
            if (c <= 0.02f) continue;
            int lx = px / Z, ly = py / Z;
            if (s_op(l, lx, ly) && !m[ly][lx] && !r[ly][lx]) continue;   /* the figure: never touched */
            float a = (c - 0.3f) / 0.3f;
            a = a < 0 ? 0 : a > 1 ? 1 : a;
            a = a * a * (3 - 2 * a);
            u8 *q = hd + (py * HD + px) * 4;
            q[0] = (u8)(K[2] + (s[2] - K[2]) * a);
            q[1] = (u8)(K[1] + (s[1] - K[1]) * a);
            q[2] = (u8)(K[0] + (s[0] - K[0]) * a);
            q[3] = 255;
        }
    return 1;
}
