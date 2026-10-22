/*
 * 俄罗斯方块自动游戏（供 faces.c 表盘 14 调用）
 *
 * 实现要点：
 *   - 10x20 游戏区，格子 6x3 像素；
 *   - 7 种方块 × 4 旋转的位图表；
 *   - 简单 AI：枚举所有 (旋转, 列) 组合，用 El-Tetris 评分
 *     （消行奖励、高度/空洞/凹凸惩罚）选择最优落点；
 *   - 自动执行旋转、平移、下落、消行；
 *   - 游戏结束（新块顶到顶部）自动重开；
 *   - 右上角显示 HH:MM 时间（非得分）。
 */
#include <stdlib.h>
#include <string.h>

#include "display.h"
#include "faces.h"
#include "net_time.h"

#define TB_COLS      10
#define TB_ROWS      20
#define TB_CELL_W    6
#define TB_CELL_H    3
#define TB_ORIGIN_X  2
#define TB_ORIGIN_Y  2

/* 7 种方块，每种 4 个旋转，每个 4x4=16 格位图（行优先） */
static const uint8_t TB_SHAPES[7][4][16] = {
    /* I */
    {
        {0,0,0,0, 1,1,1,1, 0,0,0,0, 0,0,0,0},
        {0,0,1,0, 0,0,1,0, 0,0,1,0, 0,0,1,0},
        {0,0,0,0, 0,0,0,0, 1,1,1,1, 0,0,0,0},
        {0,1,0,0, 0,1,0,0, 0,1,0,0, 0,1,0,0},
    },
    /* J */
    {
        {1,0,0,0, 1,1,1,0, 0,0,0,0, 0,0,0,0},
        {0,1,1,0, 0,1,0,0, 0,1,0,0, 0,0,0,0},
        {0,0,0,0, 1,1,1,0, 0,0,1,0, 0,0,0,0},
        {0,1,0,0, 0,1,0,0, 1,1,0,0, 0,0,0,0},
    },
    /* L */
    {
        {0,0,1,0, 1,1,1,0, 0,0,0,0, 0,0,0,0},
        {0,1,0,0, 0,1,0,0, 0,1,1,0, 0,0,0,0},
        {0,0,0,0, 1,1,1,0, 1,0,0,0, 0,0,0,0},
        {1,1,0,0, 0,1,0,0, 0,1,0,0, 0,0,0,0},
    },
    /* O */
    {
        {0,1,1,0, 0,1,1,0, 0,0,0,0, 0,0,0,0},
        {0,1,1,0, 0,1,1,0, 0,0,0,0, 0,0,0,0},
        {0,1,1,0, 0,1,1,0, 0,0,0,0, 0,0,0,0},
        {0,1,1,0, 0,1,1,0, 0,0,0,0, 0,0,0,0},
    },
    /* S */
    {
        {0,1,1,0, 1,1,0,0, 0,0,0,0, 0,0,0,0},
        {0,1,0,0, 0,1,1,0, 0,0,1,0, 0,0,0,0},
        {0,0,0,0, 0,1,1,0, 1,1,0,0, 0,0,0,0},
        {1,0,0,0, 1,1,0,0, 0,1,0,0, 0,0,0,0},
    },
    /* T */
    {
        {0,1,0,0, 1,1,1,0, 0,0,0,0, 0,0,0,0},
        {0,1,0,0, 0,1,1,0, 0,1,0,0, 0,0,0,0},
        {0,0,0,0, 1,1,1,0, 0,1,0,0, 0,0,0,0},
        {0,1,0,0, 1,1,0,0, 0,1,0,0, 0,0,0,0},
    },
    /* Z */
    {
        {1,1,0,0, 0,1,1,0, 0,0,0,0, 0,0,0,0},
        {0,0,1,0, 0,1,1,0, 0,1,0,0, 0,0,0,0},
        {0,0,0,0, 1,1,0,0, 0,1,1,0, 0,0,0,0},
        {0,1,0,0, 1,1,0,0, 1,0,0,0, 0,0,0,0},
    },
};

static const color_t TB_COLORS[7] = {
    {0, 240, 240},   /* I 青 */
    {0, 110, 240},   /* J 蓝 */
    {255, 150, 50},  /* L 橙 */
    {240, 240, 0},   /* O 黄 */
    {0, 220, 80},    /* S 绿 */
    {180, 90, 240},  /* T 紫 */
    {240, 60, 60},   /* Z 红 */
};

static uint8_t s_grid[TB_ROWS][TB_COLS];
static int s_type, s_rot, s_x, s_y;
static int s_target_rot, s_target_x;
static int s_tick;
static bool s_inited;

static bool tb_occupied(int type, int rot, int x, int y)
{
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            if (!TB_SHAPES[type][rot][r * 4 + c]) {
                continue;
            }
            int gx = x + c;
            int gy = y + r;
            if (gx < 0 || gx >= TB_COLS || gy >= TB_ROWS) {
                return true;
            }
            if (gy >= 0 && s_grid[gy][gx]) {
                return true;
            }
        }
    }
    return false;
}

static bool tb_occupied_in(const uint8_t g[TB_ROWS][TB_COLS], int type, int rot, int x, int y)
{
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            if (!TB_SHAPES[type][rot][r * 4 + c]) {
                continue;
            }
            int gx = x + c;
            int gy = y + r;
            if (gx < 0 || gx >= TB_COLS || gy >= TB_ROWS) {
                return true;
            }
            if (gy >= 0 && g[gy][gx]) {
                return true;
            }
        }
    }
    return false;
}

/* 模拟放置并评分（El-Tetris 权重） */
static float tb_score_placement(int type, int rot, int x)
{
    uint8_t g[TB_ROWS][TB_COLS];
    memcpy(g, s_grid, sizeof(g));

    if (tb_occupied_in(g, type, rot, x, 0)) {
        return -1e9f; /* 位置无效（超出边界或与已有块冲突） */
    }

    /* 落到最低 */
    int y = 0;
    while (!tb_occupied_in(g, type, rot, x, y + 1)) {
        y++;
    }

    /* 放置 */
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            if (TB_SHAPES[type][rot][r * 4 + c]) {
                int gy = y + r;
                if (gy >= 0) {
                    g[gy][x + c] = 1;
                }
            }
        }
    }

    /* 消行 */
    int lines = 0;
    for (int row = TB_ROWS - 1; row >= 0; row--) {
        bool full = true;
        for (int c = 0; c < TB_COLS; c++) {
            if (!g[row][c]) {
                full = false;
                break;
            }
        }
        if (full) {
            lines++;
            for (int r2 = row; r2 > 0; r2--) {
                memcpy(g[r2], g[r2 - 1], TB_COLS);
            }
            memset(g[0], 0, TB_COLS);
            row++;
        }
    }

    /* 高度 / 空洞 / 凹凸 */
    int heights[TB_COLS];
    int agg_h = 0;
    int holes = 0;
    for (int c = 0; c < TB_COLS; c++) {
        int h = 0;
        bool seen = false;
        for (int r = 0; r < TB_ROWS; r++) {
            if (g[r][c]) {
                if (!seen) {
                    h = TB_ROWS - r;
                    seen = true;
                }
            } else if (seen) {
                holes++;
            }
        }
        heights[c] = h;
        agg_h += h;
    }
    int bump = 0;
    for (int c = 0; c < TB_COLS - 1; c++) {
        int d = heights[c] - heights[c + 1];
        bump += (d < 0) ? -d : d;
    }

    return 0.76f * (float)lines - 0.51f * (float)agg_h
           - 0.36f * (float)holes - 0.18f * (float)bump;
}

/* AI 选择最优 (旋转, 列) */
static void tb_ai_choose(void)
{
    float best = -1e9f;
    int br = 0;
    int bx = 0;

    for (int rot = 0; rot < 4; rot++) {
        for (int x = -4; x < TB_COLS + 1; x++) {
            float sc = tb_score_placement(s_type, rot, x);
            if (sc > best) {
                best = sc;
                br = rot;
                bx = x;
            }
        }
    }
    s_target_rot = br;
    s_target_x = bx;
}

/* 锁定当前块、消行、生成新块 */
static void tb_lock_and_spawn(void)
{
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            if (TB_SHAPES[s_type][s_rot][r * 4 + c]) {
                int gx = s_x + c;
                int gy = s_y + r;
                if (gx >= 0 && gx < TB_COLS && gy >= 0 && gy < TB_ROWS) {
                    s_grid[gy][gx] = 1;
                }
            }
        }
    }

    /* 消行 */
    for (int row = TB_ROWS - 1; row >= 0; row--) {
        bool full = true;
        for (int c = 0; c < TB_COLS; c++) {
            if (!s_grid[row][c]) {
                full = false;
                break;
            }
        }
        if (full) {
            for (int r2 = row; r2 > 0; r2--) {
                memcpy(s_grid[r2], s_grid[r2 - 1], TB_COLS);
            }
            memset(s_grid[0], 0, TB_COLS);
            row++;
        }
    }

    /* 生成新块（随机类型），若生成位置被占则重开 */
    s_type = rand() % 7;
    s_rot = 0;
    s_x = TB_COLS / 2 - 2;
    s_y = 0;

    if (tb_occupied(s_type, s_rot, s_x, s_y)) {
        /* 游戏结束：清空重开 */
        memset(s_grid, 0, sizeof(s_grid));
        s_type = rand() % 7;
        s_rot = 0;
        s_x = TB_COLS / 2 - 2;
        s_y = 0;
    }

    tb_ai_choose();
}

static void tb_reset(void)
{
    memset(s_grid, 0, sizeof(s_grid));
    s_tick = 0;

    s_type = rand() % 7;
    s_rot = 0;
    s_x = TB_COLS / 2 - 2;
    s_y = 0;
    tb_ai_choose();
}

static void tb_draw_cell(int col, int row, color_t c)
{
    int x = TB_ORIGIN_X + col * TB_CELL_W;
    int y = TB_ORIGIN_Y + row * TB_CELL_H;
    disp_fill_rect(x, y, TB_CELL_W - 1, TB_CELL_H - 1, c);
}

static void tb_render(void)
{
    /* 纯黑背景，不画游戏区边框：方块直接显示在整屏黑底上 */
    disp_clear(C_RGB(0, 0, 0));

    /* 已放置方块 */
    for (int row = 0; row < TB_ROWS; row++) {
        for (int col = 0; col < TB_COLS; col++) {
            if (s_grid[row][col]) {
                tb_draw_cell(col, row, C_RGB(150, 150, 170));
            }
        }
    }

    /* 当前方块 */
    color_t cc = TB_COLORS[s_type % 7];
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            if (TB_SHAPES[s_type][s_rot][r * 4 + c]) {
                int gx = s_x + c;
                int gy = s_y + r;
                if (gx >= 0 && gx < TB_COLS && gy >= 0 && gy < TB_ROWS) {
                    tb_draw_cell(gx, gy, cc);
                }
            }
        }
    }

    /* 右上角时间 HH:MM（白色，叠加在画面上） */
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
        disp_text5x7(64 - tw - 2, 2, tbuf, C_RGB(255, 255, 255));
    }

    disp_present();
}

void face_tetris(void)
{
    if (!s_inited) {
        s_inited = true;
        tb_reset();
    }

    s_tick++;

    /* 逐步旋转到目标角度 */
    if (s_rot != s_target_rot) {
        int nr = (s_rot + 1) % 4;
        if (!tb_occupied(s_type, nr, s_x, s_y)) {
            s_rot = nr;
        } else if ((s_rot + 2) % 4 == s_target_rot && !tb_occupied(s_type, (s_rot + 2) % 4, s_x, s_y)) {
            s_rot = (s_rot + 2) % 4;
        } else if (!tb_occupied(s_type, (s_rot + 3) % 4, s_x, s_y)) {
            s_rot = (s_rot + 3) % 4;
        } else {
            s_target_rot = s_rot; /* 无法旋转，放弃 */
        }
    } else if (s_x < s_target_x) {
        if (!tb_occupied(s_type, s_rot, s_x + 1, s_y)) {
            s_x++;
        } else {
            s_target_x = s_x;
        }
    } else if (s_x > s_target_x) {
        if (!tb_occupied(s_type, s_rot, s_x - 1, s_y)) {
            s_x--;
        } else {
            s_target_x = s_x;
        }
    }

    /* 下落：每 5 帧一格 */
    if (s_tick % 5 == 0) {
        if (!tb_occupied(s_type, s_rot, s_x, s_y + 1)) {
            s_y++;
        } else {
            tb_lock_and_spawn();
        }
    }

    tb_render();
}
