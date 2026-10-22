/*
 * Flappy Bird 自动游戏（供 faces.c 表盘 16 调用）
 *
 * 规则：
 *   - 小鸟固定在左侧，靠扇翅维持高度，穿过右侧下水管道的间隙；
 *   - 自动游玩：根据前方管道间隙中心决定何时扇翅；
 *   - 撞管道 / 地面 / 天花板 → 立即重开；
 *   - 小鸟有翅膀拍打动画；
 *   - 右上角显示 HH:MM 时间（得分位置）。
 */
#include <stdlib.h>
#include <string.h>

#include "display.h"
#include "faces.h"
#include "net_time.h"

#define FL_PIPE_NUM    3
#define FL_PIPE_W      8
#define FL_GAP_H       24
#define FL_BIRD_X      14
#define FL_GRAVITY     0.30f
#define FL_FLAP       -2.0f
#define FL_PIPE_SPEED  1.0f
#define FL_GROUND_Y    60

static struct {
    float x;
    int gap_y;
} s_pipes[FL_PIPE_NUM];

static float s_bird_y;
static float s_vy;
static int s_wing_phase;
static int s_tick;
static bool s_inited;

static void fl_reset(void)
{
    s_bird_y = 30.0f;
    s_vy = 0.0f;
    s_wing_phase = 0;
    s_tick = 0;

    for (int i = 0; i < FL_PIPE_NUM; i++) {
        s_pipes[i].x = 64.0f + (float)(i * 30) + 20.0f;
        s_pipes[i].gap_y = 12 + rand() % 20;
    }
}

static void fl_draw_bird(int x, int y)
{
    /* 身体（黄色） */
    disp_fill_circle(x, y, 3, C_RGB(255, 220, 60));
    disp_fill_circle(x + 1, y, 2, C_RGB(255, 235, 110));

    /* 翅膀拍打：上下摆动 */
    int w = (s_wing_phase % 12 < 6) ? -1 : 1;
    disp_line(x - 2, y + w, x + 1, y + w * 2, C_RGB(255, 170, 30));
    disp_line(x - 2, y + w + 1, x + 1, y + w * 2 + 1, C_RGB(255, 170, 30));

    /* 眼睛 */
    disp_pixel(x + 2, y - 1, C_RGB(255, 255, 255));
    disp_pixel(x + 2, y, C_RGB(0, 0, 0));

    /* 嘴巴（橙色三角） */
    disp_fill_rect(x + 3, y - 1, 2, 1, C_RGB(255, 140, 40));
    disp_pixel(x + 3, y, C_RGB(255, 140, 40));
}

static void fl_update(void)
{
    s_tick++;
    s_wing_phase++;

    /* 管道移动 */
    for (int i = 0; i < FL_PIPE_NUM; i++) {
        s_pipes[i].x -= FL_PIPE_SPEED;
        if (s_pipes[i].x < -(float)FL_PIPE_W - 4.0f) {
            float max_x = 0.0f;
            for (int j = 0; j < FL_PIPE_NUM; j++) {
                if (s_pipes[j].x > max_x) {
                    max_x = s_pipes[j].x;
                }
            }
            s_pipes[i].x = max_x + 30.0f + (float)(rand() % 10);
            /* 与上一根管道间隙高度相近（±8px），让鸟能平稳跟随 */
            int new_gap = s_pipes[i].gap_y + (rand() % 13) - 6;
            if (new_gap < 12) {
                new_gap = 12;
            }
            if (new_gap > 32) {
                new_gap = 32;
            }
            s_pipes[i].gap_y = new_gap;
        }
    }

    /* AI：找到前方最近的管道间隙中心 */
    float target = (float)(FL_GROUND_Y / 2);
    float nearest_x = 1e9f;
    for (int i = 0; i < FL_PIPE_NUM; i++) {
        if (s_pipes[i].x + (float)FL_PIPE_W > (float)FL_BIRD_X - 6.0f &&
            s_pipes[i].x < nearest_x) {
            nearest_x = s_pipes[i].x;
            target = (float)s_pipes[i].gap_y + (float)FL_GAP_H / 2.0f;
        }
    }

    /*
     * 在下坠且落到目标间隙中心下方 1px 时扇翅：
     * 扇翅一次上升约 6.7px，鸟中心在 [target-5.7, target+1] 内振荡，
     * 加上鸟半径 3px 仍完全处于 22px 的间隙内。
     */
    if (s_vy > 0.0f && s_bird_y > target + 1.0f) {
        s_vy = FL_FLAP;
        s_wing_phase = 0;
    }

    /* 物理 */
    s_vy += FL_GRAVITY;
    s_bird_y += s_vy;

    int by = (int)s_bird_y;

    /* 撞地面 / 天花板 */
    if (by > FL_GROUND_Y - 2 || by < 3) {
        fl_reset();
        return;
    }

    /* 撞管道 */
    for (int i = 0; i < FL_PIPE_NUM; i++) {
        if ((float)(FL_BIRD_X + 3) > s_pipes[i].x &&
            (float)(FL_BIRD_X - 3) < s_pipes[i].x + (float)FL_PIPE_W) {
            if (by - 3 < s_pipes[i].gap_y ||
                by + 3 > s_pipes[i].gap_y + FL_GAP_H) {
                fl_reset();
                return;
            }
        }
    }
}

static void fl_render(void)
{
    /* 黑色背景 */
    disp_clear(C_RGB(0, 0, 0));

    /* 管道（绿色） */
    for (int i = 0; i < FL_PIPE_NUM; i++) {
        int px = (int)s_pipes[i].x;
        if (px < 64 && px > -FL_PIPE_W) {
            color_t green = C_RGB(70, 200, 70);
            color_t dark = C_RGB(40, 150, 40);

            /* 上管道 */
            disp_fill_rect(px, 0, FL_PIPE_W, s_pipes[i].gap_y, green);
            /* 下管道 */
            disp_fill_rect(px, s_pipes[i].gap_y + FL_GAP_H,
                           FL_PIPE_W, FL_GROUND_Y - (s_pipes[i].gap_y + FL_GAP_H), green);

            /* 管道口深色边（上下各 2px） */
            disp_fill_rect(px, s_pipes[i].gap_y - 2, FL_PIPE_W, 2, dark);
            disp_fill_rect(px, s_pipes[i].gap_y + FL_GAP_H, FL_PIPE_W, 2, dark);
        }
    }

    /* 地面 */
    disp_hline(0, 63, FL_GROUND_Y, C_RGB(90, 70, 35));

    /* 小鸟 */
    fl_draw_bird(FL_BIRD_X, (int)s_bird_y);

    /* 右上角时间（得分位置） */
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
        /* 品红色：绿色的互补色，穿过绿色管道时依然醒目 */
        disp_text5x7(64 - tw - 2, 2, tbuf, C_RGB(255, 0, 255));
    }

    disp_present();
}

void face_flappy(void)
{
    if (!s_inited) {
        s_inited = true;
        fl_reset();
    }

    fl_update();
    fl_render();
}
