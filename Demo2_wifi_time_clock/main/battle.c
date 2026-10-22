/*
 * 坦克大战自动游戏（FC 画风，供 faces.c 表盘 17 调用）
 *
 * 画面风格还原 FC《坦克大战》（BATTLE CITY）：
 *   - 8×8 像素红棕砖墙（带砖缝纹理）；
 *   - 像素坦克（黄绿玩家 / 灰白敌人），带履带、炮塔、炮管；
 *   - 底部中央老鹰基地（红底金鹰），被击中则整局重置；
 *   - 场地 8×8 格（每格 8px），黑色背景；
 *   - 玩家自动巡逻、瞄准最近敌人开火；敌人从顶部出现向下进攻；
 *   - 右上角显示 HH:MM 时间（得分位置）。
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "display.h"
#include "faces.h"
#include "net_time.h"

#define BT_CELL        8
#define BT_GRID        8
#define BT_ENEMY_NUM   3
#define BT_BULLET_NUM  10

#define BT_EAGLE_X     28
#define BT_EAGLE_Y     56
#define BT_EAGLE_W     16
#define BT_EAGLE_H     8

/* FC 配色 */
#define COL_BRICK_LIGHT C_RGB(198, 96, 38)
#define COL_BRICK_DARK  C_RGB(110, 48, 18)
#define COL_PLAYER      C_RGB(228, 208, 60)   /* 黄绿 */
#define COL_ENEMY       C_RGB(225, 225, 225)  /* 灰白 */
#define COL_TRACK       C_RGB(58, 48, 40)
#define COL_EAGLE_GOLD  C_RGB(255, 220, 80)
#define COL_EAGLE_BG    C_RGB(190, 40, 40)
#define COL_BULLET      C_RGB(255, 255, 255)

typedef struct {
    float x;
    float y;
    int dir;        /* 0 上 1 右 2 下 3 左 */
    int alive;
    int respawn;
    int fire_cd;
    color_t color;
} bt_tank_t;

typedef struct {
    float x;
    float y;
    float vx;
    float vy;
    int owner;      /* 0 玩家 1 敌人 */
    int active;
} bt_bullet_t;

static uint8_t s_bricks[BT_GRID][BT_GRID];
static bt_tank_t s_player;
static bt_tank_t s_enemies[BT_ENEMY_NUM];
static bt_bullet_t s_bullets[BT_BULLET_NUM];
static int s_eagle_alive;
static int s_tick;
static bool s_inited;

static const int BT_DX[4] = {0, 1, 0, -1};
static const int BT_DY[4] = {-1, 0, 1, 0};

static void bt_reset_bricks(void)
{
    memset(s_bricks, 0, sizeof(s_bricks));

    /* 保护老鹰的砖墙（FC 经典布局） */
    s_bricks[7][2] = 1;
    s_bricks[6][2] = 1;
    s_bricks[7][5] = 1;
    s_bricks[6][5] = 1;
    s_bricks[6][3] = 1;
    s_bricks[6][4] = 1;

    /* 中间区域的砖块 */
    s_bricks[3][1] = 1;
    s_bricks[3][2] = 1;
    s_bricks[3][5] = 1;
    s_bricks[3][6] = 1;
    s_bricks[2][3] = 1;
    s_bricks[2][4] = 1;
}

static int bt_brick_count(void)
{
    int n = 0;
    for (int r = 0; r < BT_GRID; r++) {
        for (int c = 0; c < BT_GRID; c++) {
            n += s_bricks[r][c];
        }
    }
    return n;
}

static void bt_reset_player(void)
{
    s_player.x = 8.0f;
    s_player.y = 48.0f;
    s_player.dir = 0;
    s_player.alive = 1;
    s_player.fire_cd = 15;
}

static void bt_spawn_enemy(bt_tank_t *e, int idx)
{
    e->x = 8.0f + (float)(idx * 20);
    e->y = 0.0f;
    e->dir = 2;
    e->alive = 1;
    e->respawn = 0;
    e->fire_cd = 30 + rand() % 40;
    e->color = COL_ENEMY;
}

static void bt_reset(void)
{
    bt_reset_bricks();
    bt_reset_player();
    s_eagle_alive = 1;

    for (int i = 0; i < BT_ENEMY_NUM; i++) {
        bt_spawn_enemy(&s_enemies[i], i);
    }
    memset(s_bullets, 0, sizeof(s_bullets));
    s_tick = 0;
}

static void bt_fire(bt_tank_t *t, int owner)
{
    for (int i = 0; i < BT_BULLET_NUM; i++) {
        if (!s_bullets[i].active) {
            s_bullets[i].x = t->x + 3.0f;
            s_bullets[i].y = t->y + 3.0f;
            switch (t->dir) {
            case 0:
                s_bullets[i].vx = 0.0f;
                s_bullets[i].vy = -2.4f;
                break;
            case 1:
                s_bullets[i].vx = 2.4f;
                s_bullets[i].vy = 0.0f;
                break;
            case 2:
                s_bullets[i].vx = 0.0f;
                s_bullets[i].vy = 2.4f;
                break;
            default:
                s_bullets[i].vx = -2.4f;
                s_bullets[i].vy = 0.0f;
                break;
            }
            s_bullets[i].owner = owner;
            s_bullets[i].active = 1;
            return;
        }
    }
}

static int bt_dir_to(float x0, float y0, float x1, float y1)
{
    float dx = x1 - x0;
    float dy = y1 - y0;
    if (fabsf(dx) > fabsf(dy)) {
        return (dx > 0) ? 1 : 3;
    }
    return (dy > 0) ? 2 : 0;
}

static void bt_move_tank(bt_tank_t *t, float min_y, float max_y)
{
    if ((s_tick % 24) == 0) {
        t->dir = rand() % 4;
    }

    float nx = t->x + (float)BT_DX[t->dir] * 0.8f;
    float ny = t->y + (float)BT_DY[t->dir] * 0.8f;

    if (nx < 0.0f || nx > 56.0f) {
        nx = t->x;
    }
    if (ny < min_y || ny > max_y) {
        ny = t->y;
    }

    /* 不能压到老鹰基地 */
    if (s_eagle_alive &&
        nx + 8.0f > (float)BT_EAGLE_X && nx < (float)(BT_EAGLE_X + BT_EAGLE_W) &&
        ny + 8.0f > (float)BT_EAGLE_Y) {
        nx = t->x;
        ny = t->y;
    }

    t->x = nx;
    t->y = ny;
}

static void bt_update(void)
{
    s_tick++;

    if (bt_brick_count() < 4 && (s_tick % 150) == 0) {
        bt_reset_bricks();
    }

    /* 玩家：巡逻 + 瞄准开火 */
    bt_move_tank(&s_player, 40.0f, 48.0f);

    if (s_player.fire_cd > 0) {
        s_player.fire_cd--;
    } else {
        float best = 1e9f;
        int bi = -1;
        for (int i = 0; i < BT_ENEMY_NUM; i++) {
            if (s_enemies[i].alive) {
                float dx = s_enemies[i].x - s_player.x;
                float dy = s_enemies[i].y - s_player.y;
                float d = dx * dx + dy * dy;
                if (d < best) {
                    best = d;
                    bi = i;
                }
            }
        }
        if (bi >= 0) {
            s_player.dir = bt_dir_to(s_player.x, s_player.y,
                                     s_enemies[bi].x, s_enemies[bi].y);
            bt_fire(&s_player, 0);
            s_player.fire_cd = 24 + rand() % 18;
        }
    }

    /* 敌人 */
    for (int i = 0; i < BT_ENEMY_NUM; i++) {
        bt_tank_t *e = &s_enemies[i];

        if (!e->alive) {
            if (e->respawn > 0) {
                e->respawn--;
            } else {
                bt_spawn_enemy(e, i);
            }
            continue;
        }

        bt_move_tank(e, 0.0f, 40.0f);

        if (e->fire_cd > 0) {
            e->fire_cd--;
        } else {
            if (rand() % 2) {
                e->dir = 2;
            } else {
                e->dir = bt_dir_to(e->x, e->y, s_player.x, s_player.y);
            }
            bt_fire(e, 1);
            e->fire_cd = 50 + rand() % 40;
        }
    }

    /* 子弹 */
    for (int i = 0; i < BT_BULLET_NUM; i++) {
        bt_bullet_t *b = &s_bullets[i];
        if (!b->active) {
            continue;
        }

        b->x += b->vx;
        b->y += b->vy;

        if (b->x < 0.0f || b->x > 63.0f || b->y < 0.0f || b->y > 63.0f) {
            b->active = 0;
            continue;
        }

        /* 撞砖块 */
        int gx = (int)(b->x / BT_CELL);
        int gy = (int)(b->y / BT_CELL);
        if (gx >= 0 && gx < BT_GRID && gy >= 0 && gy < BT_GRID &&
            s_bricks[gy][gx]) {
            s_bricks[gy][gx] = 0;
            b->active = 0;
            continue;
        }

        if (b->owner == 0) {
            for (int k = 0; k < BT_ENEMY_NUM; k++) {
                bt_tank_t *e = &s_enemies[k];
                if (e->alive &&
                    fabsf(b->x - e->x) < 5.0f && fabsf(b->y - e->y) < 5.0f) {
                    e->alive = 0;
                    e->respawn = 40;
                    b->active = 0;
                    break;
                }
            }
        } else {
            if (fabsf(b->x - s_player.x) < 5.0f &&
                fabsf(b->y - s_player.y) < 5.0f) {
                bt_reset_player();
                b->active = 0;
                continue;
            }
            /* 击中老鹰基地 → 整局重置 */
            if (s_eagle_alive &&
                b->x > (float)BT_EAGLE_X && b->x < (float)(BT_EAGLE_X + BT_EAGLE_W) &&
                b->y > (float)BT_EAGLE_Y) {
                s_eagle_alive = 0;
                b->active = 0;
                bt_reset();
                continue;
            }
        }
    }
}

/* FC 风格砖墙（8x8：2x2 个 4x4 红砖 + 深缝） */
static void bt_draw_brick(int px, int py)
{
    for (int by = 0; by < 2; by++) {
        for (int bx = 0; bx < 2; bx++) {
            int x0 = px + bx * 4;
            int y0 = py + by * 4;
            disp_fill_rect(x0, y0, 4, 4, COL_BRICK_LIGHT);
            disp_fill_rect(x0, y0, 4, 1, COL_BRICK_DARK);
            disp_fill_rect(x0, y0, 1, 4, COL_BRICK_DARK);
        }
    }
}

/* FC 风格像素坦克（8x8） */
static void bt_draw_tank(float fx, float fy, int dir, color_t body)
{
    int x = (int)fx;
    int y = (int)fy;

    /* 履带（两侧深色竖条） */
    disp_fill_rect(x, y + 1, 2, 6, COL_TRACK);
    disp_fill_rect(x + 6, y + 1, 2, 6, COL_TRACK);
    /* 车体 */
    disp_fill_rect(x + 1, y + 1, 6, 6, body);
    /* 炮塔（中间亮块） */
    disp_fill_rect(x + 2, y + 2, 4, 4, disp_lerp(body, C_RGB(255, 255, 255), 0.25f));
    /* 炮管 */
    switch (dir) {
    case 0:
        disp_fill_rect(x + 3, y, 2, 2, body);
        break;
    case 1:
        disp_fill_rect(x + 7, y + 3, 2, 2, body);
        break;
    case 2:
        disp_fill_rect(x + 3, y + 7, 2, 2, body);
        break;
    default:
        disp_fill_rect(x - 1, y + 3, 2, 2, body);
        break;
    }
}

/* FC 老鹰基地（16x8：红底 + 金色鹰） */
static void bt_draw_eagle(void)
{
    /* 红底砖纹 */
    for (int by = 0; by < 2; by++) {
        for (int bx = 0; bx < 2; bx++) {
            int x0 = BT_EAGLE_X + bx * 8;
            int y0 = BT_EAGLE_Y + by * 4;
            disp_fill_rect(x0, y0, 8, 4, COL_EAGLE_BG);
            disp_fill_rect(x0, y0, 8, 1, C_RGB(120, 20, 20));
        }
    }

    if (!s_eagle_alive) {
        return;
    }

    /* 金色老鹰：翅膀 + 身体 */
    int cx = BT_EAGLE_X + 8;
    int cy = BT_EAGLE_Y + 4;
    disp_fill_rect(cx - 1, cy - 2, 2, 4, COL_EAGLE_GOLD); /* 身体 */
    disp_fill_rect(cx - 4, cy - 2, 2, 2, COL_EAGLE_GOLD); /* 左翅 */
    disp_fill_rect(cx + 2, cy - 2, 2, 2, COL_EAGLE_GOLD); /* 右翅 */
    disp_fill_rect(cx - 5, cy, 10, 1, COL_EAGLE_GOLD);    /* 展开翼 */
    disp_pixel(cx - 1, cy - 3, COL_EAGLE_GOLD);
    disp_pixel(cx + 1, cy - 3, COL_EAGLE_GOLD);
}

static void bt_render(void)
{
    /* 黑色背景 */
    disp_clear(C_RGB(0, 0, 0));

    /* 砖墙 */
    for (int r = 0; r < BT_GRID; r++) {
        for (int c = 0; c < BT_GRID; c++) {
            if (s_bricks[r][c]) {
                bt_draw_brick(c * BT_CELL, r * BT_CELL);
            }
        }
    }

    /* 老鹰基地 */
    bt_draw_eagle();

    /* 子弹 */
    for (int i = 0; i < BT_BULLET_NUM; i++) {
        if (s_bullets[i].active) {
            color_t bc = (s_bullets[i].owner == 0)
                             ? C_RGB(255, 255, 120)
                             : C_RGB(255, 120, 100);
            disp_fill_rect((int)s_bullets[i].x, (int)s_bullets[i].y, 2, 2, bc);
        }
    }

    /* 敌人坦克 */
    for (int i = 0; i < BT_ENEMY_NUM; i++) {
        if (s_enemies[i].alive) {
            bt_draw_tank(s_enemies[i].x, s_enemies[i].y,
                         s_enemies[i].dir, s_enemies[i].color);
        }
    }

    /* 玩家坦克 */
    if (s_player.alive) {
        bt_draw_tank(s_player.x, s_player.y, s_player.dir, COL_PLAYER);
    }

    /* 右上角时间 */
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

void face_battle(void)
{
    if (!s_inited) {
        s_inited = true;
        bt_reset();
    }

    bt_update();
    bt_render();
}
