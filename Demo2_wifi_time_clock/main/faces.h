/*
 * faces.h：时钟表盘渲染
 *
 * 共 18 种表盘，可在配网网页中选择：
 *   0 - 数字大时钟（Digital）
 *   1 - 环形进度钟（Ring）
 *   2 - 复古数码管（Retro LED）
 *   3 - 极简风格（弹跳时分 + 底部移动秒数）
 *   4 - 拾音器·环形（MAX9814 音乐律动频谱）
 *   5 - 拾音器·横排（底部柱形频谱 + 白色峰值方块）
 *   6 - 拾音器·密集（64 列频带、无间隔、冷到暖渐变）
 *   7 - 图片（显示 image/image.h 中转换的 64x64 图片）
 *   8 - 画板（显示网页画板绘制的自定义图片）
 *   9 - 示波器·李萨如（音频波形 XY 轨迹）
 *  10 - 表情动画（开心/生气/惊讶/眨眼/爱心 循环播放）
 *  11 - 鱼缸（大小鱼游动动画）
 *  12 - 俄罗斯方块（AI 自动游戏动画）
 *  13 - 贪吃蛇（BFS 自动寻路吃苹果）
 *  14 - Flappy Bird（自动飞行穿管道）
 *  15 - 坦克大战（自动对战动画）
 *  16 - 吃豆人（自动追逐吃豆）
 *  17 - 飞机大战（星空自动射击游戏）
 *  18 - 2D 赛车（路上超车躲避）
 */
#pragma once

#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FACE_COUNT 19

/* 拾音器表盘 ID：环形（4）、横排（5）、密集（6） */
#define FACE_SPECTRUM 4
#define FACE_SPECTRUM_LINEAR 5
#define FACE_SPECTRUM_DENSE 6

/* 图片表盘 ID（image/image.h 中的 64x64 RGB565 图片） */
#define FACE_IMAGE 7

/* 网页画板表盘 ID（自定义 64x64 图片） */
#define FACE_CUSTOM 8

/* 示波器·李萨如 表盘 ID（麦克风/电脑音频波形） */
#define FACE_SCOPE 9

/* 表情动画 表盘 ID（机器人面部表情循环播放） */
#define FACE_EMOJI 10

/* 鱼缸 表盘 ID（大小鱼游动动画） */
#define FACE_AQUARIUM 11

/* 俄罗斯方块 表盘 ID（自动游戏动画） */
#define FACE_TETRIS 12

/* 贪吃蛇 表盘 ID（自动游戏动画） */
#define FACE_SNAKE 13

/* Flappy Bird 表盘 ID（自动飞行动画） */
#define FACE_FLAPPY 14

/* 坦克大战 表盘 ID（自动游戏动画） */
#define FACE_BATTLE 15

/* 吃豆人 表盘 ID（自动游戏动画） */
#define FACE_PACMAN 16

/* 飞机大战 表盘 ID（自动游戏动画） */
#define FACE_AIRFIGHT 17

/* 2D 赛车 表盘 ID（自动游戏动画） */
#define FACE_RACER 18

/**
 * @brief 渲染指定表盘到帧缓冲并提交显示
 */
void face_render(int face_id, const struct tm *t);

/**
 * @brief 俄罗斯方块自动游戏（tetris.c）
 */
void face_tetris(void);

/**
 * @brief 贪吃蛇自动游戏（snake.c）
 */
void face_snake(void);

/**
 * @brief Flappy Bird 自动游戏（flappy.c）
 */
void face_flappy(void);

/**
 * @brief 坦克大战自动游戏（battle.c）
 */
void face_battle(void);

/**
 * @brief 吃豆人自动游戏（pacman.c）
 */
void face_pacman(void);

/**
 * @brief 飞机大战自动游戏（shooter.c）
 */
void face_airfight(void);

/**
 * @brief 2D 赛车自动游戏（racer.c）
 */
void face_racer(void);

#ifdef __cplusplus
}
#endif
