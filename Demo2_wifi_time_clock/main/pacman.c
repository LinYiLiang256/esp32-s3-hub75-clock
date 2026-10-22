/*
 * 吃豆人自动游戏（FC 画风，供 faces.c 表盘 18 调用）
 *
 * 画面还原 FC《Pac-Man》：
 *   - 24×24 格大地图（每格 4px），相机跟随吃豆人，只显示 16×14 格；
 *   - 蓝色墙块（管道式亮边细节）、白色豆子；
 *   - 黄色吃豆人自动吃豆，4 只彩色幽灵追击；
 *   - 幽灵碰到吃豆人时幽灵回巢（吃豆人不重置，避免频繁重来）；
 *   - 顶部 8px 状态栏显示 HH:MM 时间（黑底白字，清晰醒目）。
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "display.h"
#include "faces.h"
#include "net_time.h"

#define PM_MAP      24
#define PM_CELL     4
#define PM_VIEW_W   16
#define PM_VIEW_H   14
#define PM_TOP      8
#define PM_GHOSTS   4

/* 24x24 迷宫（# 墙，. 通道），DFS 生成 + 随机环路，已验证全连通 */
static const char PM_MAZE[PM_MAP][PM_MAP + 1] = {
    "########################",
    "#.#...............#...##",
    "#.######..#.##.##.#.####",
    "#.........#.....#.#...##",
    "#####..####.###.#.#.#.##",
    "#...............#.#.#.##",
    "###.###.##..##.##.###.##",
    "#...#.....#.#.....#....#",
    "#.###..##.###..#.##.#..#",
    "#.#...#.#...#.#.....#.##",
    "#.#.###.###...#..####.##",
    "#.#.....#.#...#.#.#....#",
    "#..#.##.#.#####...#.####",
    "#.....#.....#.....#.#.##",
    "#..########.#.###.#.#..#",
    "#.............#...#...##",
    "#..##.#.#.#.#.#.#####..#",
    "#.#...#...#...#.#.....##",
    "#.#.#.#.#.#####.#..##..#",
    "#.#.#.#.#.#.......#...##",
    "#.#.#.#.#.#.#######.#..#",
    "#...#...#...........#..#",
    "##.####.##.####..#######",
    "########################",
};

typedef struct {
    int gx;
    int gy;
    int tx;
    int ty;
    float prog;
    int dir;
} pm_actor_t;

static uint8_t s_dot[PM_MAP][PM_MAP];
static pm_actor_t s_pac;
static pm_actor_t s_ghost[PM_GHOSTS];
static int s_cam_x;
static int s_cam_y;
static int s_tick;
static bool s_inited;

static const color_t PM_GHOST_COLORS[PM_GHOSTS] = {
    {255, 60, 60},    /* 红 */
    {255, 150, 180},  /* 粉 */
    {80, 220, 220},   /* 青 */
    {255, 160, 60},   /* 橙 */
};

static const int PM_DX[4] = {0, 1, 0, -1};
static const int PM_DY[4] = {-1, 0, 1, 0};

/* 幽灵出生点（通道格） */
static const int PM_GHOST_SPAWN[PM_GHOSTS][2] = {
    {3, 1}, {5, 1}, {7, 1}, {9, 1},
};

static bool pm_is_wall(int gx, int gy)
{
    if (gx < 0 || gx >= PM_MAP || gy < 0 || gy >= PM_MAP) {
        return true;
    }
    return PM_MAZE[gy][gx] == '#';
}

static void pm_init_dots(void)
{
    for (int r = 0; r < PM_MAP; r++) {
        for (int c = 0; c < PM_MAP; c++) {
            s_dot[r][c] = pm_is_wall(c, r) ? 0 : 1;
        }
    }
}

static void pm_reset_actor(pm_actor_t *a, int gx, int gy, int dir)
{
    a->gx = gx;
    a->gy = gy;
    a->tx = gx;
    a->ty = gy;
    a->prog = 0.0f;
    a->dir = dir;
}

static void pm_init(void)
{
    pm_init_dots();
    pm_reset_actor(&s_pac, 1, 1, 1);

    for (int i = 0; i < PM_GHOSTS; i++) {
        pm_reset_actor(&s_ghost[i], PM_GHOST_SPAWN[i][0], PM_GHOST_SPAWN[i][1], 1);
    }

    s_cam_x = 0;
    s_cam_y = 0;
    s_tick = 0;
}

static void pm_choose_dir(pm_actor_t *a, const pm_actor_t *target, bool pacman)
{
    int opts[4];
    int n = 0;

    for (int d = 0; d < 4; d++) {
        int nx = a->gx + PM_DX[d];
        int ny = a->gy + PM_DY[d];
        if (pm_is_wall(nx, ny)) {
            continue;
        }
        if (PM_DX[d] == -PM_DX[a->dir] && PM_DY[d] == -PM_DY[a->dir]) {
            continue;
        }
        opts[n++] = d;
    }

    if (n == 0) {
        for (int d = 0; d < 4; d++) {
            if (!pm_is_wall(a->gx + PM_DX[d], a->gy + PM_DY[d])) {
                opts[n++] = d;
            }
        }
        if (n == 0) {
            return;
        }
    }

    int pick = opts[rand() % n];

    if (pacman) {
        /* 优先朝有豆子的方向 */
        for (int k = 0; k < n; k++) {
            int d = opts[k];
            if (s_dot[a->gy + PM_DY[d]][a->gx + PM_DX[d]]) {
                pick = d;
                break;
            }
        }
    } else if (target) {
        int best = -1;
        int bd = 0x7fffffff;
        for (int k = 0; k < n; k++) {
            int d = opts[k];
            int nx = a->gx + PM_DX[d];
            int ny = a->gy + PM_DY[d];
            int dist = abs(nx - target->gx) + abs(ny - target->gy);
            if (dist < bd) {
                bd = dist;
                best = d;
            }
        }
        if (best >= 0) {
            pick = best;
        }
    }

    a->dir = pick;
    a->tx = a->gx + PM_DX[pick];
    a->ty = a->gy + PM_DY[pick];
}

/* 推进移动：吃豆人 10 帧/格，幽灵 13 帧/格（稍慢） */
static void pm_step(pm_actor_t *a, const pm_actor_t *target, bool pacman)
{
    a->prog += pacman ? 0.10f : 0.075f;
    if (a->prog >= 1.0f) {
        a->gx = a->tx;
        a->gy = a->ty;
        a->prog = 0.0f;
        pm_choose_dir(a, target, pacman);
    }
}

static void pm_update(void)
{
    s_tick++;

    pm_step(&s_pac, NULL, true);

    /* 吃豆子 */
    if (s_pac.prog < 0.35f && s_dot[s_pac.gy][s_pac.gx]) {
        s_dot[s_pac.gy][s_pac.gx] = 0;
    }

    /* 豆子吃完 → 重新生成 */
    bool all_eaten = true;
    for (int r = 0; r < PM_MAP && all_eaten; r++) {
        for (int c = 0; c < PM_MAP; c++) {
            if (s_dot[r][c]) {
                all_eaten = false;
                break;
            }
        }
    }
    if (all_eaten) {
        pm_init_dots();
    }

    /* 幽灵 */
    for (int i = 0; i < PM_GHOSTS; i++) {
        pm_step(&s_ghost[i], &s_pac, false);

        if (s_ghost[i].gx == s_pac.gx && s_ghost[i].gy == s_pac.gy) {
            /* 幽灵撞到吃豆人 → 幽灵回巢（吃豆人不受影响） */
            pm_reset_actor(&s_ghost[i], PM_GHOST_SPAWN[i][0], PM_GHOST_SPAWN[i][1], 1);
        }
    }

    /* 相机跟随吃豆人（带点前瞻偏移） */
    int cx = (int)s_pac.gx - PM_VIEW_W / 2;
    int cy = (int)s_pac.gy - PM_VIEW_H / 2;
    if (cx < 0) {
        cx = 0;
    }
    if (cx > PM_MAP - PM_VIEW_W) {
        cx = PM_MAP - PM_VIEW_W;
    }
    if (cy < 0) {
        cy = 0;
    }
    if (cy > PM_MAP - PM_VIEW_H) {
        cy = PM_MAP - PM_VIEW_H;
    }
    s_cam_x = cx;
    s_cam_y = cy;
}

static void pm_draw_wall(int px, int py)
{
    /* FC 管道式墙块：深蓝底 + 亮蓝边 */
    disp_fill_rect(px, py, PM_CELL, PM_CELL, C_RGB(35, 55, 200));
    disp_fill_rect(px + 1, py + 1, 2, 2, C_RGB(80, 120, 255));
}

static void pm_draw_actor(const pm_actor_t *a, color_t c, bool ghost)
{
    float fx = (float)a->gx + (float)(a->tx - a->gx) * a->prog;
    float fy = (float)a->gy + (float)(a->ty - a->gy) * a->prog;
    int px = (int)((fx - (float)s_cam_x) * (float)PM_CELL);
    int py = (int)((fy - (float)s_cam_y) * (float)PM_CELL) + PM_TOP;

    if (ghost) {
        /* 幽灵：圆头 + 眼睛 */
        disp_fill_rect(px, py + 1, PM_CELL, 3, c);
        disp_fill_rect(px + 1, py, 2, 1, c);
        disp_pixel(px + 1, py + 1, C_RGB(255, 255, 255));
        disp_pixel(px + 2, py + 1, C_RGB(255, 255, 255));
        disp_pixel(px + 1, py + 2, C_RGB(20, 20, 20));
        disp_pixel(px + 2, py + 2, C_RGB(20, 20, 20));
    } else {
        /* 吃豆人：黄色圆 + 嘴（朝向） */
        disp_fill_circle(px + 2, py + 2, 2, c);
        int mx = px + 2;
        int my = py + 2;
        switch (a->dir) {
        case 0:
            disp_pixel(mx, my - 2, C_RGB(0, 0, 0));
            disp_pixel(mx, my - 1, C_RGB(0, 0, 0));
            break;
        case 1:
            disp_pixel(mx + 2, my, C_RGB(0, 0, 0));
            disp_pixel(mx + 1, my, C_RGB(0, 0, 0));
            break;
        case 2:
            disp_pixel(mx, my + 2, C_RGB(0, 0, 0));
            disp_pixel(mx, my + 1, C_RGB(0, 0, 0));
            break;
        default:
            disp_pixel(mx - 2, my, C_RGB(0, 0, 0));
            disp_pixel(mx - 1, my, C_RGB(0, 0, 0));
            break;
        }
    }
}

static void pm_render(void)
{
    disp_clear(C_RGB(0, 0, 0));

    /* 视口内的地图（只画 16x14 格） */
    for (int r = s_cam_y; r < s_cam_y + PM_VIEW_H; r++) {
        for (int c = s_cam_x; c < s_cam_x + PM_VIEW_W; c++) {
            int px = (c - s_cam_x) * PM_CELL;
            int py = (r - s_cam_y) * PM_CELL + PM_TOP;

            if (pm_is_wall(c, r)) {
                pm_draw_wall(px, py);
            } else if (s_dot[r][c]) {
                disp_pixel(px + 2, py + 2, C_RGB(255, 255, 255));
            }
        }
    }

    /* 幽灵 */
    for (int i = 0; i < PM_GHOSTS; i++) {
        pm_draw_actor(&s_ghost[i], PM_GHOST_COLORS[i], true);
    }

    /* 吃豆人 */
    pm_draw_actor(&s_pac, C_RGB(255, 220, 50), false);

    /* 顶部状态栏：黑底白字时间（清晰醒目） */
    time_t now = net_time_now();
    if (now > 0) {
        struct tm tmv;
        localtime_r(&now, &tmv);

        char tbuf[6];
        tbuf[0] = (char)('0' + tmv.tm_hour / 10);
        tbuf[1] = (char)('0' + tmv.tm_hour % 10);
        tbuf[2] = ':';
        tbuf[3] = (char)('0' + tmv.tm_min / 10);
        tbuf[4] = (char)('0' + tmv.tm_min % 10);
        tbuf[5] = '\0';

        int tw = disp_text5x7_width(tbuf);
        disp_text5x7((64 - tw) / 2, 1, tbuf, C_RGB(255, 255, 255));
    }

    disp_present();
}

void face_pacman(void)
{
    if (!s_inited) {
        s_inited = true;
        pm_init();
    }

    pm_update();
    pm_render();
}
