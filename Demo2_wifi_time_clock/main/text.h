/*
 * text.h：UTF-8 文本点阵渲染与滚动字幕
 *
 * 字库：GB2312 一级常用汉字（16x16）+ ASCII（8x16）。
 * 汉字按 Unicode 码点二分查找，支持中英文混排。
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 在 (x, y) 绘制 UTF-8 文本（超出屏幕部分自动裁剪）
 *
 * 每个字符按顺序使用彩虹渐变色。
 * @return 绘制结束的 x 坐标
 */
int text_draw(const char *utf8, int x, int y);

/**
 * @brief 计算文本像素宽度（ASCII 8px，汉字 16px）
 */
int text_width(const char *utf8);

/**
 * @brief 设置要滚动显示的文字（空串表示不显示）
 */
void text_set(const char *utf8);

/**
 * @brief 是否已设置文字
 */
bool text_active(void);

/**
 * @brief 渲染一帧滚动字幕并推进位置（每帧调用）
 */
void text_scroll_render(void);

#ifdef __cplusplus
}
#endif
