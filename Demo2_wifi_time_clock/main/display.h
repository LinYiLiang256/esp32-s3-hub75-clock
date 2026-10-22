/*
 * display：64x64 RGB444 帧缓冲 + 绘图原语 + 字体
 *
 * 帧缓冲格式：uint16_t，bits 11-8 = R, 7-4 = G, 3-0 = B（各 4 位）。
 * 双缓冲：渲染任务写 back buffer，调用 disp_present() 提交；
 *         hub75 刷新任务在每帧开始时调用 disp_frame_start() 完成切换。
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DISP_W 64
#define DISP_H 64

typedef struct {
    uint8_t r, g, b; /* 0~255 */
} color_t;

#define C_RGB(r, g, b) ((color_t){(r), (g), (b)})

/* ---------------- 帧缓冲管理 ---------------- */
void disp_init(void);
uint16_t *disp_back(void);            /* 渲染写入缓冲 */
void disp_present(void);              /* 提交渲染结果 */
const uint16_t *disp_front(void);     /* 当前显示缓冲（刷新任务用） */
void disp_frame_start(void);          /* 刷新任务每帧开始调用 */

/* ---------------- 颜色 ---------------- */
color_t disp_hsl(float h, float s, float l);       /* h 0~360 */
color_t disp_lerp(color_t a, color_t b, float t);  /* t 0~1 */

/* ---------------- 绘图原语 ---------------- */
void disp_clear(color_t c);
void disp_pixel(int x, int y, color_t c);
void disp_hline(int x0, int x1, int y, color_t c);
void disp_vline(int x, int y0, int y1, color_t c);
void disp_line(int x0, int y0, int x1, int y1, color_t c);
void disp_line_thick(int x0, int y0, int x1, int y1, int w, color_t c);
void disp_circle(int cx, int cy, int r, color_t c);       /* 空心圆 */
void disp_fill_circle(int cx, int cy, int r, color_t c);  /* 实心圆 */
void disp_fill_rect(int x, int y, int w, int h, color_t c);

/* ---------------- 字体 ---------------- */
int  disp_text5x7_width(const char *s);               /* 像素宽度 */
void disp_char5x7(int x, int y, char ch, color_t c);  /* 5x7 小字体 */
void disp_text5x7(int x, int y, const char *s, color_t c);
int  disp_text8x16_width(const char *s);
void disp_char8x16(int x, int y, char ch, color_t c); /* 8x16 大数字 */
void disp_text8x16(int x, int y, const char *s, color_t c);
/* 1.5 倍放大的 8x16 数字（12x24，更粗更大） */
void disp_char8x16_big(int x, int y, char ch, color_t c);

/* ---------------- 字体数据（fonts.c） ---------------- */
extern const uint8_t font8x16_digits[10][16];

/* 5x7 字符查找：未收录字符返回 NULL */
const uint8_t *font5x7_lookup(char ch);

#ifdef __cplusplus
}
#endif
