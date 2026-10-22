/*
 * 贪吃蛇自动游戏（供 faces.c 表盘 15 调用）
 *
 * 规则：
 *   - 32x28 格（每格 2x2 像素），游戏区在顶部时间行下方；
 *   - 屏幕边缘即边界（无边框），蛇头出界或撞到自己即死亡重开；
 *   - 苹果红色，蛇青色起步，每吃一次苹果换一种颜色（不变红色）；
 *   - 顶部一行显示 HH:MM 时间（白色），属于边界的一部分；
 *   - AI 用 BFS 寻路吃苹果，无路可走时选择安全方向。
 */
#include <stdlib.h>
#include <string.h>

#include "display.h"
#include "faces.h"
#include "net_time.h"

#define SN_COLS     32       /* 网格列数（32 x 2px = 64） */
#define SN_ROWS     28       /* 网格行数（28 x 2px = 56，位于时间行下方） */
#define SN_CELL     2        /* 每格像素 */
#define SN_TOP      8        /* 游戏区顶部像素（时间行高度） */
#define SN_MAX       (SN_COLS * SN_ROWS)
#define SN_STEP_FRAMES 4     /* 每 4 帧移动一格 */

/* 蛇身颜色（不含红色） */
static const color_t SN_COLORS[] = {
    {0, 220, 160},    /* 青绿（初始） */
    {80, 200, 255},   /* 天蓝 */
    {255, 220, 60},   /* 黄 */
    {180, 100, 255},  /* 紫 */
    {110, 255, 120},  /* 绿 */
    {255, 170, 60},   /* 橙 */
    {100, 255, 220},  /* 青 */
};
#define SN_COLOR_N ((int)(sizeof(SN_COLORS) / sizeof(SN_COLORS[0])))

static int8_t s_sx[SN_MAX];
static int8_t s_sy[SN_MAX];
static int s_len;
static int s_apple_x, s_apple_y;
static int s_tick;
static int s_color_idx;
static bool s_inited;

/* 占用标记（BFS 用） */
static uint8_t s_occ[SN_ROWS][SN_COLS];
static int8_t s_prev[SN_ROWS][SN_COLS];
static int16_t s_queue[SN_MAX];

static int8_t s_dx = 1, s_dy = 0; /* 当前方向 */

static bool sn_blocked(int x, int y)
{
    if (x < 0 || x >= SN_COLS || y < 0 || y >= SN_ROWS) {
        return true;
    }
    return s_occ[y][x] != 0;
}

static void sn_place_apple(void)
{
    for (int tries = 0; tries < 200; tries++) {
        int x = rand() % SN_COLS;
        int y = rand() % SN_ROWS;
        bool on_snake = false;
        for (int i = 0; i < s_len; i++) {
            if (s_sx[i] == x && s_sy[i] == y) {
                on_snake = true;
                break;
            }
        }
        if (!on_snake) {
            s_apple_x = x;
            s_apple_y = y;
            return;
        }
    }
    s_apple_x = 0;
    s_apple_y = SN_ROWS - 1;
}

static void sn_reset(void)
{
    s_len = 3;
    s_sx[0] = SN_COLS / 2;
    s_sy[0] = SN_ROWS / 2;
    s_sx[1] = s_sx[0] - 1;
    s_sy[1] = s_sy[0];
    s_sx[2] = s_sx[0] - 2;
    s_sy[2] = s_sy[0];
    s_dx = 1;
    s_dy = 0;
    s_color_idx = 0;
    s_tick = 0;
    sn_place_apple();
}

/* BFS 寻路：返回下一步方向 */
static bool sn_find_step(int *out_dx, int *out_dy)
{
    memset(s_occ, 0, sizeof(s_occ));
    for (int i = 0; i < s_len; i++) {
        s_occ[s_sy[i]][s_sx[i]] = 1;
    }
    /* 苹果格不视为障碍 */
    s_occ[s_apple_y][s_apple_x] = 0;

    memset(s_prev, -1, sizeof(s_prev));

    int hx = s_sx[0];
    int hy = s_sy[0];
    int head = 0;
    int tail = 0;

    s_queue[tail++] = (int16_t)(hy * SN_COLS + hx);
    s_prev[hy][hx] = 0; /* 访问标记 */

    static const int DX[4] = {1, -1, 0, 0};
    static const int DY[4] = {0, 0, 1, -1};

    bool found = false;
    while (head < tail) {
        int cur = s_queue[head++];
        int cx = cur % SN_COLS;
        int cy = cur / SN_COLS;

        if (cx == s_apple_x && cy == s_apple_y) {
            found = true;
            break;
        }

        for (int d = 0; d < 4; d++) {
            int nx = cx + DX[d];
            int ny = cy + DY[d];
            if (sn_blocked(nx, ny)) {
                continue;
            }
            if (s_prev[ny][nx] != -1) {
                continue;
            }
            s_prev[ny][nx] = (int8_t)d;
            s_queue[tail++] = (int16_t)(ny * SN_COLS + nx);
        }
    }

    if (!found) {
        return false;
    }

    /* 从苹果回溯到蛇头的第一步 */
    int px = s_apple_x;
    int py = s_apple_y;
    while (!(px == hx && py == hy)) {
        int d = s_prev[py][px];
        if (d < 0) {
            return false;
        }
        int prev_x = px - DX[d];
        int prev_y = py - DY[d];
        if (prev_x == hx && prev_y == hy) {
            *out_dx = DX[d];
            *out_dy = DY[d];
            return true;
        }
        px = prev_x;
        py = prev_y;
    }
    return false;
}

/* 无路可走：选择安全方向（不撞墙/自己，不能直接反向，优先保持当前方向） */
static void sn_safe_step(int *out_dx, int *out_dy)
{
    static const int DX[4] = {1, -1, 0, 0};
    static const int DY[4] = {0, 0, 1, -1};

    int hx = s_sx[0];
    int hy = s_sy[0];

    int best_d = -1;
    int best_score = -1;

    for (int d = 0; d < 4; d++) {
        if (DX[d] == -s_dx && DY[d] == -s_dy) {
            continue; /* 不能直接反向 */
        }
        int nx = hx + DX[d];
        int ny = hy + DY[d];
        if (nx < 0 || nx >= SN_COLS || ny < 0 || ny >= SN_ROWS) {
            continue;
        }
        bool hit = false;
        for (int i = 0; i < s_len - 1; i++) {
            if (s_sx[i] == nx && s_sy[i] == ny) {
                hit = true;
                break;
            }
        }
        if (hit) {
            continue;
        }
        int score = (DX[d] == s_dx && DY[d] == s_dy) ? 2 : 1;
        if (score > best_score) {
            best_score = score;
            best_d = d;
        }
    }

    if (best_d >= 0) {
        *out_dx = DX[best_d];
        *out_dy = DY[best_d];
    } else {
        /* 死路：按原方向前进（下一帧会撞死重开） */
        *out_dx = s_dx;
        *out_dy = s_dy;
    }
}

static void sn_move(void)
{
    int dx = 0;
    int dy = 0;

    if (!sn_find_step(&dx, &dy)) {
        sn_safe_step(&dx, &dy);
    }

    int nx = s_sx[0] + dx;
    int ny = s_sy[0] + dy;

    /* 撞墙（屏幕边缘即边界） */
    if (nx < 0 || nx >= SN_COLS || ny < 0 || ny >= SN_ROWS) {
        sn_reset();
        return;
    }
    /* 撞自己（尾巴不算，因为它下一帧会移走） */
    for (int i = 0; i < s_len - 1; i++) {
        if (s_sx[i] == nx && s_sy[i] == ny) {
            sn_reset();
            return;
        }
    }

    bool eat = (nx == s_apple_x && ny == s_apple_y);
    int new_len = eat ? s_len + 1 : s_len;

    memmove(&s_sx[1], &s_sx[0], (size_t)(new_len - 1) * sizeof(int8_t));
    memmove(&s_sy[1], &s_sy[0], (size_t)(new_len - 1) * sizeof(int8_t));
    s_sx[0] = (int8_t)nx;
    s_sy[0] = (int8_t)ny;
    s_len = new_len;

    s_dx = (int8_t)dx;
    s_dy = (int8_t)dy;

    if (eat) {
        s_color_idx = (s_color_idx + 1) % SN_COLOR_N;
        sn_place_apple();
    }
}

static void sn_render(void)
{
    disp_clear(C_RGB(0, 0, 0));

    /* 顶部时间（白色，属于边界的一部分） */
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

    /* 苹果（红色） */
    disp_fill_rect(s_apple_x * SN_CELL, SN_TOP + s_apple_y * SN_CELL,
                   SN_CELL, SN_CELL, C_RGB(255, 40, 40));

    /* 蛇身（青绿起步，吃苹果变色） */
    color_t sc = SN_COLORS[s_color_idx];
    for (int i = 0; i < s_len; i++) {
        color_t c = (i == 0) ? disp_lerp(sc, C_RGB(255, 255, 255), 0.35f) : sc;
        disp_fill_rect(s_sx[i] * SN_CELL, SN_TOP + s_sy[i] * SN_CELL,
                       SN_CELL, SN_CELL, c);
    }

    disp_present();
}

void face_snake(void)
{
    if (!s_inited) {
        s_inited = true;
        sn_reset();
    }

    s_tick++;
    if (s_tick % SN_STEP_FRAMES == 0) {
        sn_move();
    }

    sn_render();
}
