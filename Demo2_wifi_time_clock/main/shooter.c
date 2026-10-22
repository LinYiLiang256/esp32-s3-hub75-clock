/*
 * 飞机大战 自动游戏（供 faces.c 表盘 19 调用）
 *
 * 规则：
 *   - 星空背景，玩家战机在底部自动左右巡逻并持续射击；
 *   - 敌机从顶部不断出现，缓慢下降并向下开火；
 *   - 玩家子弹击中敌机 → 敌机爆炸；敌机子弹/敌机撞到玩家 → 玩家闪烁无敌；
 *   - 游戏永不结束（自动演示），顶部状态栏显示 HH:MM 时间（得分位置）。
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "display.h"
#include "faces.h"
#include "net_time.h"

#define AF_TOP          8      /* 顶部状态栏高度 */
#define AF_MAX_PB       12     /* 玩家子弹上限 */
#define AF_MAX_EB       16     /* 敌机子弹上限 */
#define AF_MAX_EN       6      /* 敌机上限 */
#define AF_MAX_PART     36     /* 粒子上限 */
#define AF_PLAYER_Y     56     /* 玩家飞机中心 y */
#define AF_STAR_NUM     14     /* 星星数量 */

#define AF_PB_SPEED     4.5f   /* 玩家子弹速度（向上） */
#define AF_EB_SPEED     2.0f   /* 敌机子弹速度（向下） */
#define AF_KEEP_SPAWN   34     /* 敌机生成间隔（帧） */

typedef struct {
    float x;
    float y;
    int alive;
} af_pbullet_t;

typedef struct {
    float x;
    float y;
    int alive;
} af_ebullet_t;

typedef struct {
    float x;
    float y;
    float vy;
    float phase;
    int fire_cd;
    int hp;
    int type;
    int alive;
} af_enemy_t;

typedef struct {
    float x;
    float y;
    float vx;
    float vy;
    int life;
    int alive;
} af_particle_t;

static af_pbullet_t s_pb[AF_MAX_PB];
static af_ebullet_t s_eb[AF_MAX_EB];
static af_enemy_t s_en[AF_MAX_EN];
static af_particle_t s_part[AF_MAX_PART];

static float s_px = 32.0f;        /* 玩家 x */
static int s_pdir = 1;            /* 巡逻方向 */
static int s_inv;                 /* 玩家无敌帧 */
static int s_tick;
static bool s_inited;

/* 星星 */
static uint8_t s_star_x[AF_STAR_NUM];
static uint8_t s_star_y[AF_STAR_NUM];
static uint8_t s_star_speed[AF_STAR_NUM];

static void af_reset(void)
{
    memset(s_pb, 0, sizeof(s_pb));
    memset(s_eb, 0, sizeof(s_eb));
    memset(s_en, 0, sizeof(s_en));
    memset(s_part, 0, sizeof(s_part));

    s_px = 32.0f;
    s_pdir = 1;
    s_inv = 0;
    s_tick = 0;

    for (int i = 0; i < AF_STAR_NUM; i++) {
        s_star_x[i] = (uint8_t)(rand() % 64);
        s_star_y[i] = (uint8_t)(AF_TOP + rand() % (64 - AF_TOP));
        s_star_speed[i] = (uint8_t)(1 + rand() % 2);
    }
}

static af_pbullet_t *af_alloc_pb(void)
{
    for (int i = 0; i < AF_MAX_PB; i++) {
        if (!s_pb[i].alive) {
            return &s_pb[i];
        }
    }
    return NULL;
}

static af_ebullet_t *af_alloc_eb(void)
{
    for (int i = 0; i < AF_MAX_EB; i++) {
        if (!s_eb[i].alive) {
            return &s_eb[i];
        }
    }
    return NULL;
}

static af_enemy_t *af_alloc_en(void)
{
    for (int i = 0; i < AF_MAX_EN; i++) {
        if (!s_en[i].alive) {
            return &s_en[i];
        }
    }
    return NULL;
}

static void af_spawn_particle(float x, float y, int count, bool small)
{
    for (int i = 0; i < AF_MAX_PART && count > 0; i++) {
        if (s_part[i].alive) {
            continue;
        }
        float ang = (float)(rand() % 360) * 0.01745f;
        float spd = small ? (0.6f + (float)(rand() % 10) * 0.08f)
                          : (1.0f + (float)(rand() % 10) * 0.15f);
        s_part[i].x = x;
        s_part[i].y = y;
        s_part[i].vx = cosf(ang) * spd;
        s_part[i].vy = sinf(ang) * spd;
        s_part[i].life = small ? (6 + rand() % 6) : (10 + rand() % 10);
        s_part[i].alive = 1;
        count--;
    }
}

static void af_spawn_enemy(void)
{
    af_enemy_t *e = af_alloc_en();
    if (!e) {
        return;
    }
    memset(e, 0, sizeof(*e));
    e->x = (float)(8 + rand() % 48);
    e->y = (float)(AF_TOP + 2);
    e->vy = 0.55f + (float)(rand() % 7) * 0.09f;
    e->phase = (float)(rand() % 100) * 0.1f;
    e->fire_cd = 18 + rand() % 30;
    e->hp = 1 + rand() % 2;
    e->type = (rand() % 3 == 0) ? 1 : 0;
    e->alive = 1;
}

/* 玩家 AI：躲避附近威胁，否则巡逻 */
static void af_player_ai(void)
{
    float threat_x = -999.0f;
    float threat_dist = 1e9f;
    bool has_threat = false;

    for (int i = 0; i < AF_MAX_EB; i++) {
        if (!s_eb[i].alive) {
            continue;
        }
        float dy = (float)AF_PLAYER_Y - s_eb[i].y;
        if (dy >= -2.0f && dy <= 24.0f) {
            float dx = fabsf(s_eb[i].x - s_px);
            if (dx < threat_dist) {
                threat_dist = dx;
                threat_x = s_eb[i].x;
                has_threat = true;
            }
        }
    }

    for (int i = 0; i < AF_MAX_EN; i++) {
        if (!s_en[i].alive) {
            continue;
        }
        float dy = (float)AF_PLAYER_Y - s_en[i].y;
        if (dy >= -4.0f && dy <= 20.0f) {
            float dx = fabsf(s_en[i].x - s_px);
            if (dx < threat_dist) {
                threat_dist = dx;
                threat_x = s_en[i].x;
                has_threat = true;
            }
        }
    }

    if (has_threat && threat_dist < 14.0f) {
        /* 远离威胁 */
        s_pdir = (threat_x > s_px) ? -1 : 1;
    }

    s_px += (float)s_pdir * 1.6f;
    if (s_px < 6.0f) {
        s_px = 6.0f;
        s_pdir = 1;
    }
    if (s_px > 58.0f) {
        s_px = 58.0f;
        s_pdir = -1;
    }
}

static void af_update(void)
{
    s_tick++;

    if (s_inv > 0) {
        s_inv--;
    }

    af_player_ai();

    /* 玩家开火：每 4 帧一发 */
    if (s_tick % 4 == 0) {
        af_pbullet_t *b = af_alloc_pb();
        if (b) {
            b->x = s_px;
            b->y = (float)AF_PLAYER_Y - 7.0f;
            b->alive = 1;
        }
    }

    /* 敌机生成 */
    if (s_tick % AF_KEEP_SPAWN == 0) {
        af_spawn_enemy();
    }

    /* 玩家子弹 */
    for (int i = 0; i < AF_MAX_PB; i++) {
        if (!s_pb[i].alive) {
            continue;
        }
        s_pb[i].y -= AF_PB_SPEED;
        if (s_pb[i].y < (float)AF_TOP) {
            s_pb[i].alive = 0;
        }
    }

    /* 敌机 */
    for (int i = 0; i < AF_MAX_EN; i++) {
        if (!s_en[i].alive) {
            continue;
        }
        af_enemy_t *e = &s_en[i];
        e->y += e->vy;
        e->x += sinf((float)s_tick * 0.06f + e->phase) * 0.5f;
        if (e->x < 5.0f) {
            e->x = 5.0f;
        }
        if (e->x > 59.0f) {
            e->x = 59.0f;
        }

        /* 敌机开火 */
        if (e->fire_cd > 0) {
            e->fire_cd--;
        } else if (e->y < (float)AF_PLAYER_Y - 14.0f) {
            af_ebullet_t *b = af_alloc_eb();
            if (b) {
                b->x = e->x;
                b->y = e->y + 4.0f;
                b->alive = 1;
            }
            e->fire_cd = 22 + rand() % 40;
        }

        /* 飞出屏幕底部 → 移除 */
        if (e->y > 66.0f) {
            e->alive = 0;
        }
    }

    /* 敌机子弹 */
    for (int i = 0; i < AF_MAX_EB; i++) {
        if (!s_eb[i].alive) {
            continue;
        }
        s_eb[i].y += AF_EB_SPEED;
        if (s_eb[i].y > 66.0f) {
            s_eb[i].alive = 0;
        }
    }

    /* 玩家子弹 vs 敌机 */
    for (int i = 0; i < AF_MAX_PB; i++) {
        if (!s_pb[i].alive) {
            continue;
        }
        for (int j = 0; j < AF_MAX_EN; j++) {
            if (!s_en[j].alive) {
                continue;
            }
            if (fabsf(s_pb[i].x - s_en[j].x) < 5.0f &&
                fabsf(s_pb[i].y - s_en[j].y) < 6.0f) {
                s_pb[i].alive = 0;
                s_en[j].hp--;
                if (s_en[j].hp <= 0) {
                    af_spawn_particle(s_en[j].x, s_en[j].y, 10, false);
                    s_en[j].alive = 0;
                }
                break;
            }
        }
    }

    /* 敌机子弹 vs 玩家（无敌帧内忽略） */
    if (s_inv == 0) {
        for (int i = 0; i < AF_MAX_EB; i++) {
            if (!s_eb[i].alive) {
                continue;
            }
            if (fabsf(s_eb[i].x - s_px) < 4.0f &&
                fabsf(s_eb[i].y - (float)AF_PLAYER_Y) < 6.0f) {
                s_eb[i].alive = 0;
                af_spawn_particle(s_eb[i].x, s_eb[i].y, 5, true);
                s_inv = 36;
            }
        }

        /* 敌机撞玩家 */
        for (int j = 0; j < AF_MAX_EN; j++) {
            if (!s_en[j].alive) {
                continue;
            }
            if (fabsf(s_en[j].x - s_px) < 7.0f &&
                fabsf(s_en[j].y - (float)AF_PLAYER_Y) < 7.0f) {
                af_spawn_particle(s_en[j].x, s_en[j].y, 8, true);
                s_en[j].alive = 0;
                s_inv = 36;
            }
        }
    }

    /* 粒子 */
    for (int i = 0; i < AF_MAX_PART; i++) {
        if (!s_part[i].alive) {
            continue;
        }
        s_part[i].x += s_part[i].vx;
        s_part[i].y += s_part[i].vy;
        s_part[i].life--;
        if (s_part[i].life <= 0) {
            s_part[i].alive = 0;
        }
    }

    /* 星星下移（缓慢） */
    for (int i = 0; i < AF_STAR_NUM; i++) {
        if ((s_tick & 1) == 0) {
            continue;
        }
        s_star_y[i] += 1;
        if (s_star_y[i] >= 64) {
            s_star_y[i] = (uint8_t)AF_TOP;
            s_star_x[i] = (uint8_t)(rand() % 64);
        }
    }
}

/* 绘制玩家战机（9x9，中心在 (x, y)） */
static void af_draw_player(int x, int y)
{
    /* 尾焰（闪烁） */
    if ((s_tick & 1) == 0) {
        disp_fill_rect(x - 1, y + 3, 2, 2, C_RGB(255, 140, 20));
        disp_fill_rect(x - 1, y + 5, 2, 1, C_RGB(255, 60, 10));
    } else {
        disp_fill_rect(x - 1, y + 3, 2, 3, C_RGB(255, 90, 20));
    }

    /* 机身（白） */
    disp_fill_rect(x - 2, y - 2, 4, 5, C_RGB(225, 240, 255));
    disp_fill_rect(x - 1, y - 4, 2, 2, C_RGB(255, 255, 255));

    /* 机翼（青） */
    disp_fill_rect(x - 4, y, 2, 3, C_RGB(60, 170, 255));
    disp_fill_rect(x + 2, y, 2, 3, C_RGB(60, 170, 255));

    /* 驾驶舱 */
    disp_pixel(x, y - 2, C_RGB(50, 90, 255));
}

/* 绘制敌机（7x7，中心在 (x, y)） */
static void af_draw_enemy(int x, int y, int type)
{
    color_t body = (type == 0) ? C_RGB(255, 70, 70) : C_RGB(190, 80, 255);
    color_t lit = (type == 0) ? C_RGB(255, 150, 150) : C_RGB(230, 150, 255);

    disp_fill_rect(x - 3, y - 2, 6, 4, body);
    disp_fill_rect(x - 1, y - 3, 2, 6, body);
    disp_pixel(x - 3, y - 2, lit);
    disp_pixel(x + 3, y - 2, lit);
    disp_pixel(x, y, C_RGB(255, 255, 255));
    disp_pixel(x - 1, y + 2, lit);
    disp_pixel(x + 1, y + 2, lit);
}

static void af_render(void)
{
    disp_clear(C_RGB(0, 0, 0));

    /* 星空背景 */
    for (int i = 0; i < AF_STAR_NUM; i++) {
        int b = 40 + (s_tick * 3 + i * 17) % 160;
        disp_pixel(s_star_x[i], s_star_y[i], C_RGB(b, b, b + 40));
    }

    /* 敌机 */
    for (int i = 0; i < AF_MAX_EN; i++) {
        if (s_en[i].alive) {
            af_draw_enemy((int)s_en[i].x, (int)s_en[i].y, s_en[i].type);
        }
    }

    /* 敌机子弹 */
    for (int i = 0; i < AF_MAX_EB; i++) {
        if (s_eb[i].alive) {
            disp_pixel((int)s_eb[i].x, (int)s_eb[i].y, C_RGB(255, 50, 50));
            disp_pixel((int)s_eb[i].x, (int)s_eb[i].y + 1, C_RGB(255, 120, 120));
        }
    }

    /* 玩家子弹 */
    for (int i = 0; i < AF_MAX_PB; i++) {
        if (!s_pb[i].alive) {
            continue;
        }
        int bx = (int)s_pb[i].x;
        int by = (int)s_pb[i].y;
        disp_pixel(bx, by, C_RGB(255, 255, 160));
        disp_pixel(bx, by + 1, C_RGB(255, 255, 255));
        disp_pixel(bx, by + 2, C_RGB(255, 240, 80));
    }

    /* 玩家（无敌时闪烁） */
    if (s_inv == 0 || (s_inv & 4) < 2) {
        af_draw_player((int)s_px, AF_PLAYER_Y);
    }

    /* 爆炸粒子 */
    for (int i = 0; i < AF_MAX_PART; i++) {
        if (!s_part[i].alive) {
            continue;
        }
        color_t c = (s_part[i].life > 8) ? C_RGB(255, 255, 180)
                    : (s_part[i].life > 4) ? C_RGB(255, 170, 40)
                                           : C_RGB(255, 70, 20);
        disp_pixel((int)s_part[i].x, (int)s_part[i].y, c);
    }

    /* 顶部状态栏：黑底白字时间（得分位置） */
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

void face_airfight(void)
{
    if (!s_inited) {
        s_inited = true;
        af_reset();
    }

    af_update();
    af_render();
}
