/*
 * 2D 赛车 自动游戏（供 faces.c 表盘 18 调用）
 *
 * 规则：
 *   - 俯视视角的垂直公路，玩家赛车从画面底部向前行驶（道路向后滚动）；
 *   - 前方车辆（红/蓝/绿/紫）不断从画面顶部驶来，玩家自动左右变道超越它们；
 *   - 玩家与车辆相撞 → 被撞车旋转着飞进绿化带，再旋转飞出画面之外，
 *     玩家短暂闪烁无敌，游戏不重置；
 *   - 车辆生成时自动避让、同车道自动跟车，避免互相重叠；
 *   - 顶部状态栏黑底白字显示 HH:MM 时间（得分位置）。
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "display.h"
#include "faces.h"
#include "net_time.h"

#define RC_TOP        8       /* 顶部状态栏高度 */
#define RC_ROAD_L     14      /* 路面左边界 x */
#define RC_ROAD_R     50      /* 路面右边界 x（不含） */
#define RC_LANE_L     22      /* 左车道中心 */
#define RC_LANE_R     42      /* 右车道中心 */
#define RC_PLAYER_Y   56      /* 玩家车中心 y */
#define RC_MAX_CARS   5       /* 最多同屏车辆 */
#define RC_MAX_PART   32      /* 最大粒子数 */

/* 车辆状态 */
#define RCS_NORMAL 0          /* 正常行驶 */
#define RCS_KNOB   1          /* 被撞翻滚飞出（先飞过绿化带，再飞出画面） */

typedef struct {
    float x;
    float y;
    float vy;
    float vx;        /* 横向速度（翻滚时用） */
    float rot;       /* 翻滚角度 */
    int state;
    int color;
    int alive;
} rc_car_t;

typedef struct {
    float x;
    float y;
    float vx;
    float vy;
    int life;
    int alive;
    int kind;        /* 0=火花 1=烟 */
} rc_part_t;

static rc_car_t s_cars[RC_MAX_CARS];
static rc_part_t s_parts[RC_MAX_PART];
static float s_px;        /* 玩家车 x */
static float s_scroll;    /* 道路滚动偏移 */
static int s_inv;         /* 玩家无敌帧 */
static int s_tick;
static bool s_inited;

static const color_t RC_CAR_COLORS[4] = {
    {255, 80, 80},    /* 红 */
    {80, 160, 255},   /* 蓝 */
    {120, 220, 100},  /* 绿 */
    {230, 120, 255},  /* 紫 */
};

static void rc_reset(void)
{
    memset(s_cars, 0, sizeof(s_cars));
    memset(s_parts, 0, sizeof(s_parts));
    s_px = (float)RC_LANE_L;
    s_scroll = 0.0f;
    s_inv = 0;
    s_tick = 0;
}

static void rc_spawn_particle(float x, float y, int kind)
{
    for (int i = 0; i < RC_MAX_PART; i++) {
        if (s_parts[i].alive) {
            continue;
        }
        float ang = (float)(rand() % 360) * 0.01745f;
        float spd = (kind == 1) ? (0.3f + (float)(rand() % 5) * 0.08f)
                                : (1.0f + (float)(rand() % 10) * 0.15f);
        s_parts[i].x = x;
        s_parts[i].y = y;
        s_parts[i].vx = cosf(ang) * spd;
        s_parts[i].vy = sinf(ang) * spd - ((kind == 1) ? 0.3f : 0.0f);
        s_parts[i].life = (kind == 1) ? (14 + rand() % 10) : (8 + rand() % 8);
        s_parts[i].kind = kind;
        s_parts[i].alive = 1;
        return;
    }
}

static void rc_explode(float x, float y)
{
    for (int i = 0; i < 8; i++) {
        rc_spawn_particle(x, y, 0);
    }
}

/* 生成一辆前方车辆：避开现有车辆（横向/纵向都不重叠） */
static void rc_spawn_car(void)
{
    const float spawn_y = (float)(RC_TOP + 2);

    for (int i = 0; i < RC_MAX_CARS; i++) {
        if (s_cars[i].alive) {
            continue;
        }

        /* 选一个与现有车辆横向错开的车道位置 */
        float lanes[2] = {(float)RC_LANE_L, (float)RC_LANE_R};
        int order[2] = {0, 1};
        if (rand() & 1) {
            order[0] = 1;
            order[1] = 0;
        }

        float nx = -1.0f;
        for (int k = 0; k < 2; k++) {
            float lx = lanes[order[k]] + (float)(rand() % 5) - 2.0f;
            bool bad = false;

            for (int j = 0; j < RC_MAX_CARS; j++) {
                if (j == i || !s_cars[j].alive) {
                    continue;
                }
                /* 生成点附近有车：纵向太近则换位/放弃 */
                if (fabsf(s_cars[j].y - spawn_y) < 14.0f &&
                    fabsf(s_cars[j].x - lx) < 12.0f) {
                    bad = true;
                    break;
                }
                /* 同车道且纵向太近 */
                if (fabsf(s_cars[j].y - spawn_y) < 18.0f &&
                    fabsf(s_cars[j].x - lx) < 8.0f) {
                    bad = true;
                    break;
                }
            }

            if (!bad) {
                nx = lx;
                break;
            }
        }

        if (nx < 0.0f) {
            return;   /* 当前位置都冲突，本次不生成 */
        }

        s_cars[i].x = nx;
        s_cars[i].y = spawn_y;
        s_cars[i].vy = 0.7f + (float)(rand() % 10) * 0.1f;
        s_cars[i].vx = 0.0f;
        s_cars[i].rot = 0.0f;
        s_cars[i].state = RCS_NORMAL;
        s_cars[i].color = rand() % 4;
        s_cars[i].alive = 1;
        return;
    }
}

/* 被撞：翻滚抛向绿化带 */
static void rc_knock_car(rc_car_t *car)
{
    float dir = (car->x >= s_px) ? 1.0f : -1.0f;

    car->state = RCS_KNOB;
    car->vx = dir * (3.0f + (float)(rand() % 5) * 0.3f);  /* 横向弹飞 */
    car->vy = 0.5f;               /* 向下惯性，保证滑出画面 */
    car->rot = 0.0f;

    rc_explode(car->x, car->y);
}

static void rc_update(void)
{
    s_tick++;
    if (s_inv > 0) {
        s_inv--;
    }

    /* 道路滚动（速度感） */
    s_scroll += 2.2f;
    if (s_scroll > 1000.0f) {
        s_scroll -= 1000.0f;
    }

    /* 定时生成前方车辆 */
    if (s_tick % 42 == 0 ||
        (s_tick % 42 == 21 && (rand() & 1))) {
        rc_spawn_car();
    }

    /* 同车道跟车：防止快车追尾重叠 */
    for (int i = 0; i < RC_MAX_CARS; i++) {
        if (!s_cars[i].alive || s_cars[i].state != RCS_NORMAL) {
            continue;
        }
        for (int j = 0; j < RC_MAX_CARS; j++) {
            if (i == j || !s_cars[j].alive || s_cars[j].state != RCS_NORMAL) {
                continue;
            }
            if (fabsf(s_cars[i].x - s_cars[j].x) >= 9.0f) {
                continue;
            }
            /* i 在 j 后面（屏幕更靠下）且将要追尾 → 降到 j 的速度 */
            if (s_cars[i].y > s_cars[j].y && s_cars[i].y - s_cars[j].y < 15.0f &&
                s_cars[i].vy > s_cars[j].vy) {
                s_cars[i].vy = s_cars[j].vy;
            }
        }
    }

    /* 车辆移动 */
    for (int i = 0; i < RC_MAX_CARS; i++) {
        rc_car_t *car = &s_cars[i];
        if (!car->alive) {
            continue;
        }

        if (car->state == RCS_NORMAL) {
            car->y += car->vy;
            if (car->y > 70.0f) {
                car->alive = 0;
            }
        } else {
            /* RCS_KNOB：旋转着飞进绿化带，再旋出画面之外 */
            car->x += car->vx;
            car->y += car->vy;
            car->vx *= 0.90f;
            car->vy *= 0.98f;
            car->rot += 0.32f;
            if ((s_tick & 1) == 0) {
                rc_spawn_particle(car->x, car->y, 0);   /* 拖尾火花 */
            }

            /* 完全飞出画面（左/右/下）→ 消失 */
            if (car->x < -6.0f || car->x > 70.0f || car->y > 72.0f) {
                car->alive = 0;
            }
        }
    }

    /* 玩家 AI：找前方最近车辆，必要时变道躲避 */
    float threat_x = -999.0f;
    float threat_d = 1e9f;
    bool has_threat = false;

    for (int i = 0; i < RC_MAX_CARS; i++) {
        if (!s_cars[i].alive || s_cars[i].state != RCS_NORMAL) {
            continue;
        }
        float dy = (float)RC_PLAYER_Y - s_cars[i].y;   /* >0 表示在前方 */
        if (dy > -10.0f && dy < 46.0f && dy < threat_d) {
            threat_d = dy;
            threat_x = s_cars[i].x;
            has_threat = true;
        }
    }

    float target_x = (s_px < 32.0f) ? (float)RC_LANE_L : (float)RC_LANE_R;
    if (has_threat && fabsf(threat_x - target_x) < 9.0f) {
        /* 目标车道被占 → 换到另一条车道 */
        target_x = (target_x < 32.0f) ? (float)RC_LANE_R : (float)RC_LANE_L;
    }

    /* 平滑变道 */
    s_px += (target_x - s_px) * 0.14f;
    if (s_px < (float)RC_ROAD_L + 6.0f) {
        s_px = (float)RC_ROAD_L + 6.0f;
    }
    if (s_px > (float)RC_ROAD_R - 6.0f) {
        s_px = (float)RC_ROAD_R - 6.0f;
    }

    /* 碰撞：玩家撞车 → 对方被弹飞翻滚，玩家闪烁无敌（不重置游戏） */
    if (s_inv == 0) {
        for (int i = 0; i < RC_MAX_CARS; i++) {
            rc_car_t *car = &s_cars[i];
            if (!car->alive || car->state != RCS_NORMAL) {
                continue;
            }
            if (fabsf(car->x - s_px) < 6.0f &&
                fabsf(car->y - (float)RC_PLAYER_Y) < 9.0f) {
                rc_knock_car(car);
                s_inv = 30;
            }
        }
    }

    /* 粒子 */
    for (int i = 0; i < RC_MAX_PART; i++) {
        if (!s_parts[i].alive) {
            continue;
        }
        s_parts[i].x += s_parts[i].vx;
        s_parts[i].y += s_parts[i].vy;
        s_parts[i].life--;
        if (s_parts[i].life <= 0) {
            s_parts[i].alive = 0;
        }
    }
}

/* 绘制车辆（9x12，中心在 (x, y)） */
static void rc_draw_car(int x, int y, color_t c, bool player)
{
    color_t head = player ? C_RGB(255, 255, 255) : disp_lerp(c, C_RGB(255, 255, 255), 0.45f);

    /* 车身 */
    disp_fill_rect(x - 3, y - 5, 6, 10, c);
    /* 车头（上方）亮色 */
    disp_fill_rect(x - 2, y - 6, 4, 2, head);
    /* 车窗 */
    disp_fill_rect(x - 2, y - 2, 4, 3, C_RGB(30, 60, 120));
    disp_pixel(x - 2, y - 2, C_RGB(120, 180, 255));
    /* 车尾灯（下方） */
    disp_pixel(x - 3, y + 5, C_RGB(255, 60, 60));
    disp_pixel(x + 3, y + 5, C_RGB(255, 60, 60));
}

/* 绘制翻滚中的车辆：按 rot 角度旋转车身（7x11 像素） */
static void rc_draw_car_rot(int cx, int cy, color_t c, float rot)
{
    float s = sinf(rot);
    float co = cosf(rot);
    color_t head = disp_lerp(c, C_RGB(255, 255, 255), 0.45f);

    /* 循环画旋转后的车身（局部坐标 lx∈[-3,3], ly∈[-5,5]） */
    for (int ly = -5; ly <= 5; ly++) {
        for (int lx = -3; lx <= 3; lx++) {
            /* 车身轮廓：四角裁圆 */
            if ((abs(lx) == 3) && ((ly == -5) || (ly == 5))) {
                continue;
            }
            int sx = cx + (int)lroundf((float)lx * co - (float)ly * s);
            int sy = cy + (int)lroundf((float)lx * s + (float)ly * co);

            color_t px;
            if (ly == -5 || ly == -4) {
                px = head;                       /* 车头亮色 */
            } else if (ly >= -2 && ly <= 1) {
                px = C_RGB(30, 60, 120);         /* 车窗深色 */
            } else {
                px = c;                          /* 车身 */
            }
            disp_pixel(sx, sy, px);
        }
    }
}

static void rc_render(void)
{
    disp_clear(C_RGB(0, 0, 0));

    /* 两侧草地（全屏，从顶部开始） */
    disp_fill_rect(0, 0, RC_ROAD_L, 64, C_RGB(18, 72, 28));
    disp_fill_rect(RC_ROAD_R, 0, 64 - RC_ROAD_R, 64, C_RGB(18, 72, 28));

    /* 草丛纹理（随滚动下移） */
    for (int i = 0; i < 6; i++) {
        int gy = (i * 11 + (int)(s_scroll * 0.6f)) % 64;
        disp_fill_rect(3, gy, 3, 3, C_RGB(32, 116, 42));
        disp_fill_rect(57, (gy + 27) % 64, 3, 3, C_RGB(32, 116, 42));
    }

    /* 路面（全屏） */
    disp_fill_rect(RC_ROAD_L, 0, RC_ROAD_R - RC_ROAD_L, 64, C_RGB(42, 42, 52));

    /* 路缘白实线 */
    disp_vline(RC_ROAD_L - 1, 0, 63, C_RGB(230, 230, 230));
    disp_vline(RC_ROAD_R, 0, 63, C_RGB(230, 230, 230));

    /* 中央车道虚线（随滚动移动） */
    for (int y = 0; y < 64; y++) {
        int ph = ((y - (int)s_scroll) % 16 + 16) % 16;
        if (ph < 8) {
            disp_fill_rect(31, y, 2, 1, C_RGB(255, 255, 200));
        }
    }

    /* 前方车辆（正常 / 翻滚 / 报废） */
    for (int i = 0; i < RC_MAX_CARS; i++) {
        rc_car_t *car = &s_cars[i];
        if (!car->alive) {
            continue;
        }
        if (car->state == RCS_NORMAL) {
            rc_draw_car((int)car->x, (int)car->y, RC_CAR_COLORS[car->color], false);
        } else {
            rc_draw_car_rot((int)car->x, (int)car->y, RC_CAR_COLORS[car->color],
                            car->rot);
        }
    }

    /* 玩家赛车（无敌时闪烁） */
    if (s_inv == 0 || (s_inv & 4) < 2) {
        rc_draw_car((int)s_px, RC_PLAYER_Y, C_RGB(255, 200, 40), true);
    }

    /* 爆炸火花粒子 */
    for (int i = 0; i < RC_MAX_PART; i++) {
        if (!s_parts[i].alive) {
            continue;
        }
        color_t c;
        if (s_parts[i].kind == 1) {
            int g = 90 + s_parts[i].life * 10;
            if (g > 255) {
                g = 255;
            }
            c = C_RGB(g, g, g);
        } else {
            c = (s_parts[i].life > 8) ? C_RGB(255, 255, 180)
                : (s_parts[i].life > 4) ? C_RGB(255, 170, 40)
                                        : C_RGB(255, 70, 20);
        }
        disp_pixel((int)s_parts[i].x, (int)s_parts[i].y, c);
    }

    /* 时间悬浮显示：直接叠在画面上（无黑底条），黑描边 + 亮黄高对比字体 */
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
        int tx = (64 - tw) / 2;
        int ty = 1;

        /* 黑色描边（上下左右各 1px），任何背景上都清晰 */
        disp_text5x7(tx - 1, ty, tbuf, C_RGB(0, 0, 0));
        disp_text5x7(tx + 1, ty, tbuf, C_RGB(0, 0, 0));
        disp_text5x7(tx, ty - 1, tbuf, C_RGB(0, 0, 0));
        disp_text5x7(tx, ty + 1, tbuf, C_RGB(0, 0, 0));

        /* 主字：亮黄色（黑边 + 亮黄 = 最高对比） */
        disp_text5x7(tx, ty, tbuf, C_RGB(255, 255, 0));
    }

    disp_present();
}

void face_racer(void)
{
    if (!s_inited) {
        s_inited = true;
        rc_reset();
    }

    rc_update();
    rc_render();
}
