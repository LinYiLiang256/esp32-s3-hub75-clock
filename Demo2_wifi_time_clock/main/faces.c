/*
 * faces.c：5 种时钟表盘渲染实现
 */
#include <stdio.h>
#include <math.h>

#include "display.h"
#include "faces.h"
#include "mic.h"
#include "image.h"
#include "net_time.h"
#include "state.h"
#include "usb_audio.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

/* ------------------------------------------------------------------ */
/* 公共辅助                                                            */
/* ------------------------------------------------------------------ */

/* 两位补零格式化 */
static void fmt2(char *dst, int v)
{
    dst[0] = (char)('0' + (v / 10) % 10);
    dst[1] = (char)('0' + v % 10);
    dst[2] = '\0';
}

/* "YYYY-MM-DD" */
static void fmt_date(char *dst, const struct tm *t)
{
    int y = t->tm_year + 1900;
    int m = t->tm_mon + 1;
    int d = t->tm_mday;
    dst[0] = (char)('0' + (y / 1000) % 10);
    dst[1] = (char)('0' + (y / 100) % 10);
    dst[2] = (char)('0' + (y / 10) % 10);
    dst[3] = (char)('0' + y % 10);
    dst[4] = '-';
    fmt2(dst + 5, m);
    dst[7] = '-';
    fmt2(dst + 8, d);
}

/* "HH:MM" */
static void fmt_hm(char *dst, const struct tm *t)
{
    fmt2(dst, t->tm_hour);
    dst[2] = ':';
    fmt2(dst + 3, t->tm_min);
}

/* 频谱数据源：优先电脑脚本音频（USB 串口），否则回落到板载麦克风 */
static const float *spectrum_bands(void)
{
    return usb_audio_active() ? usb_audio_bands() : mic_bands();
}

static float spectrum_energy(void)
{
    if (usb_audio_active()) {
        const float *b = usb_audio_bands();
        float sum = 0.0f;
        for (int i = 0; i < USB_AUDIO_BANDS; i++) {
            sum += b[i];
        }
        return sum / (float)USB_AUDIO_BANDS;
    }
    return mic_energy();
}

/* 从圆心按角度（0° 指向 12 点，顺时针）画一根指针 */
static void draw_hand(int cx, int cy, float angle_deg, int len, int width, color_t c)
{
    float rad = angle_deg * M_PI / 180.0f;
    int x1 = cx + (int)lroundf(sinf(rad) * (float)len);
    int y1 = cy - (int)lroundf(cosf(rad) * (float)len);
    disp_line_thick(cx, cy, x1, y1, width, c);
}

/* ------------------------------------------------------------------ */
/* 表盘 0：数字大时钟                                                 */
/* ------------------------------------------------------------------ */
static void face_digital(const struct tm *t)
{
    char date[16];

    /* 垂直渐变背景 */
    for (int y = 0; y < DISP_H; y++) {
        float k = (float)y / (float)(DISP_H - 1);
        color_t c = disp_lerp(C_RGB(2, 6, 22), C_RGB(22, 4, 36), k);
        for (int x = 0; x < DISP_W; x++) {
            disp_pixel(x, y, c);
        }
    }

    /* 日期：5x7 紧凑排版，12 字符 x 5px = 60px */
    fmt_date(date, t);
    color_t dc = C_RGB(90, 200, 255);
    int x = 2;
    for (const char *p = date; *p; p++) {
        disp_char5x7(x, 3, *p, dc);
        x += 5;
    }

    /* 时间 HH:MM，颜色随小时/分钟渐变 */
    color_t tc = disp_hsl((float)((t->tm_hour * 30 + t->tm_min / 2) % 360), 0.9f, 0.62f);
    char hh[3], mm[3];
    fmt2(hh, t->tm_hour);
    fmt2(mm, t->tm_min);

    int x0 = 11;
    disp_text8x16(x0, 22, hh, tc);
    if (t->tm_sec % 2 == 0) {
        disp_fill_rect(x0 + 19, 25, 3, 3, tc);
        disp_fill_rect(x0 + 19, 31, 3, 3, tc);
    }
    disp_text8x16(x0 + 22, 22, mm, tc);

    /* 底部：秒数 + 进度条 */
    char ss[3];
    fmt2(ss, t->tm_sec);
    color_t sc = C_RGB(120, 255, 180);
    int sw = disp_text5x7_width(ss);
    disp_text5x7((DISP_W - sw) / 2, 47, ss, sc);

    int bar_w = (t->tm_sec * (DISP_W - 8)) / 59;
    disp_hline(4, 4 + bar_w, 58, C_RGB(60, 120, 90));
    disp_pixel(4 + bar_w, 58, C_RGB(160, 255, 200));
}

/* ------------------------------------------------------------------ */
/* 表盘 1：环形进度钟                                                 */
/* ------------------------------------------------------------------ */
static void face_ring(const struct tm *t)
{
    char buf[8];

    disp_clear(C_RGB(3, 5, 12));

    /* 外环：秒进度（60 秒 → 360°），彩虹渐变 */
    int sec_deg = t->tm_sec * 6;
    for (int a = 0; a < sec_deg; a++) {
        float rad = ((float)a - 90.0f) * M_PI / 180.0f;
        int x = 32 + (int)lroundf(sinf(rad) * 29.0f);
        int y = 32 - (int)lroundf(cosf(rad) * 29.0f);
        color_t c = disp_hsl(fmodf((float)a + t->tm_sec * 4.0f, 360.0f), 0.9f, 0.55f);
        disp_pixel(x, y, c);
    }
    /* 外环底色（未完成的进度部分） */
    for (int a = 0; a < 360; a += 4) {
        float rad = ((float)a - 90.0f) * M_PI / 180.0f;
        int x = 32 + (int)lroundf(sinf(rad) * 29.0f);
        int y = 32 - (int)lroundf(cosf(rad) * 29.0f);
        if (a >= sec_deg) {
            disp_pixel(x, y, C_RGB(30, 34, 50));
        }
    }

    /* 内环：分钟进度（60 分钟 → 360°） */
    int min_deg = (t->tm_min * 6) + (t->tm_sec / 10);
    for (int a = 0; a < min_deg; a += 2) {
        float rad = ((float)a - 90.0f) * M_PI / 180.0f;
        int x = 32 + (int)lroundf(sinf(rad) * 22.0f);
        int y = 32 - (int)lroundf(cosf(rad) * 22.0f);
        disp_pixel(x, y, C_RGB(80, 200, 255));
    }

    /* 中心数字 HH:MM */
    fmt_hm(buf, t);
    int w = disp_text5x7_width(buf);
    disp_text5x7((DISP_W - w) / 2, 28, buf, C_RGB(255, 255, 255));

    /* 中心下方日期 */
    fmt2(buf, t->tm_mon + 1);
    buf[2] = '-';
    fmt2(buf + 3, t->tm_mday);
    w = disp_text5x7_width(buf);
    disp_text5x7((DISP_W - w) / 2, 38, buf, C_RGB(150, 220, 160));
}

/* ------------------------------------------------------------------ */
/* 表盘 2：复古数码管                                                 */
/* ------------------------------------------------------------------ */
static void face_retro_seg(const struct tm *t)
{
    char date[16];
    char hh[3], mm[3];

    /* 黑色背景，四角轻微暗红辉光 */
    disp_clear(C_RGB(0, 0, 0));
    for (int y = 0; y < DISP_H; y++) {
        for (int x = 0; x < DISP_W; x++) {
            float dx = (float)(x - 32);
            float dy = (float)(y - 32);
            float d = sqrtf(dx * dx + dy * dy) / 32.0f;
            if (d > 0.6f) {
                color_t c = disp_lerp(C_RGB(0, 0, 0), C_RGB(24, 8, 2), (d - 0.6f) / 0.4f);
                disp_pixel(x, y, c);
            }
        }
    }

    /* 暖橙色大数字 */
    color_t tc = C_RGB(255, 140, 30);
    color_t tc2 = C_RGB(255, 170, 60);
    fmt2(hh, t->tm_hour);
    fmt2(mm, t->tm_min);

    int x0 = 11;
    disp_text8x16(x0, 18, hh, tc);
    if (t->tm_sec % 2 == 0) {
        disp_fill_rect(x0 + 19, 21, 3, 3, tc2);
        disp_fill_rect(x0 + 19, 27, 3, 3, tc2);
    }
    disp_text8x16(x0 + 22, 18, mm, tc);

    /* 底部日期（绿色）+ 边框刻度线 */
    fmt_date(date, t);
    color_t dc = C_RGB(60, 230, 120);
    int x = 2;
    for (const char *p = date; *p; p++) {
        disp_char5x7(x, 44, *p, dc);
        x += 5;
    }

    /* 外围点线装饰 */
    color_t ec = C_RGB(120, 70, 20);
    for (int i = 0; i < 10; i++) {
        disp_pixel(3 + i * 6, 60, ec);
    }
}

/* ------------------------------------------------------------------ */
/* 表盘 3：极简风格（弹跳的时分 + 底部移动的秒数）                     */
/* ------------------------------------------------------------------ */

/* 时分数字整体尺寸：4 个 12px 宽数字 + 冒号 + 间距 = 54 x 24 */
#define MINIMAL_TIME_W 54
#define MINIMAL_TIME_H 24
#define MINIMAL_SEC_Y  56

static void face_minimal(const struct tm *t)
{
    /* 时分块在框内弹跳移动（像屏保图案一样，碰到边界反弹） */
    static int mx = 0, my = 0;
    static int dx = 1, dy = 1;

    const int max_x = DISP_W - MINIMAL_TIME_W;              /* 10 */
    const int max_y = MINIMAL_SEC_Y - 2 - MINIMAL_TIME_H;   /* 30 */

    char hh[3], mm[3], ss[3];
    fmt2(hh, t->tm_hour);
    fmt2(mm, t->tm_min);
    fmt2(ss, t->tm_sec);

    /* 纯黑背景：只有数值是亮的 */
    disp_clear(C_RGB(0, 0, 0));

    /*
     * 每分钟换一种主色（12 色轮换：12 分钟一轮），
     * 4 个数字在同一主色上做小渐变，整体更丰富。
     */
    int base = (t->tm_min * 30) % 360;
    color_t c0 = disp_hsl((float)base, 0.95f, 0.60f);
    color_t c1 = disp_hsl((float)((base + 25) % 360), 0.95f, 0.66f);
    color_t c2 = disp_hsl((float)((base + 50) % 360), 0.95f, 0.60f);
    color_t c3 = disp_hsl((float)((base + 75) % 360), 0.95f, 0.66f);

    disp_char8x16_big(mx, my, hh[0], c0);
    disp_char8x16_big(mx + 13, my, hh[1], c1);
    /* 冒号：上下两个 2x2 亮点 */
    disp_fill_rect(mx + 26, my + 7, 2, 2, c2);
    disp_fill_rect(mx + 26, my + 15, 2, 2, c2);
    disp_char8x16_big(mx + 29, my, mm[0], c2);
    disp_char8x16_big(mx + 42, my, mm[1], c3);

    /* 位置更新：每帧移动 1px（约 5px/s，缓慢移动） */
    mx += dx;
    my += dy;
    if (mx <= 0) {
        mx = 0;
        dx = 1;
    }
    if (mx >= max_x) {
        mx = max_x;
        dx = -1;
    }
    if (my <= 0) {
        my = 0;
        dy = 1;
    }
    if (my >= max_y) {
        my = max_y;
        dy = -1;
    }

    /* 秒：底部一行，随时间从最左缓慢移到最右（59 秒到达边缘），
     * 颜色取时分的互补色，每分钟同步轮换 */
    color_t sc = disp_hsl((float)((base + 180) % 360), 1.0f, 0.62f);
    int sec_w = disp_text5x7_width(ss);
    int sx = (t->tm_sec * (DISP_W - sec_w)) / 59;
    disp_text5x7(sx, MINIMAL_SEC_Y, ss, sc);
}

/* ------------------------------------------------------------------ */
/* 表盘 4：拾音器（MAX9814 音乐律动频谱）                             */
/* ------------------------------------------------------------------ */

#define SPEC_BARS      20              /* 环形柱子数（从 64 频带采样） */
#define SPEC_R0        8               /* 柱子起点半径 */
#define SPEC_MAX_LEN   22              /* 柱子最大长度 */
#define SPEC_HALF_DEG  5.0f            /* 柱子半宽（度） */

/* 白色峰值方块：被柱子顶起后独立缓慢下落 */
static float s_spec_peak[SPEC_BARS];

static void draw_spec_bar(int bar, float height, color_t c)
{
    float ang_center = (float)bar * (360.0f / SPEC_BARS) - 90.0f;
    int len = (int)(height * SPEC_MAX_LEN);

    /* 柱宽：沿角度画 11 条射线（±5°） */
    for (int k = -5; k <= 5; k++) {
        float ang = (ang_center + (float)k * (SPEC_HALF_DEG / 5.0f)) * M_PI / 180.0f;
        float cang = cosf(ang);
        float sang = sinf(ang);
        for (int r = SPEC_R0; r <= SPEC_R0 + len; r++) {
            int x = 32 + (int)lroundf(cang * (float)r);
            int y = 32 + (int)lroundf(sang * (float)r);
            disp_pixel(x, y, c);
        }
    }
}

static void face_spectrum(const struct tm *t)
{
    (void)t;

    /* 全黑背景 */
    disp_clear(C_RGB(0, 0, 0));

    /* 节拍包络：快速上升、缓慢下降 */
    static float s_env = 0.0f;
    float energy = spectrum_energy();
    if (energy > s_env) {
        s_env = energy;
    } else {
        s_env *= 0.90f;
    }

    /* 中心波纹：随节拍脉动的圆环 */
    int ring = 3 + (int)(s_env * 6.0f);
    color_t rc = disp_hsl(200.0f + s_env * 100.0f, 0.9f, 0.30f + s_env * 0.35f);
    disp_circle(32, 32, ring, rc);

    const float *bands = spectrum_bands();
    if (!bands) {
        disp_present();
        return;
    }

    for (int b = 0; b < SPEC_BARS; b++) {
        /* 从 64 个频带中等间隔取样 */
        float cur = bands[b * MIC_NUM_BANDS / SPEC_BARS];

        /* 峰值方块：被顶起后缓慢下落 */
        float peak = s_spec_peak[b];
        if (cur > peak) {
            peak = cur; /* 柱子把白块顶上去 */
        } else {
            peak -= 0.022f; /* 缓慢下落 */
            if (peak < 0.0f) {
                peak = 0.0f;
            }
        }
        s_spec_peak[b] = peak;

        /* 柱子颜色：按频率映射色相（低频红 → 高频紫） */
        float hue = (float)b * (360.0f / SPEC_BARS);
        color_t c = disp_hsl(hue, 0.92f, 0.50f + 0.15f * bands[b]);
        draw_spec_bar(b, cur, c);

        /* 白色峰值方块：独立于柱子，恒白色；落到底后隐藏 */
        if (peak > 0.10f) {
            float ang = ((float)b * (360.0f / SPEC_BARS) - 90.0f) * M_PI / 180.0f;
            int rp = SPEC_R0 + (int)(peak * SPEC_MAX_LEN);
            int px = 32 + (int)lroundf(cosf(ang) * (float)rp);
            int py = 32 + (int)lroundf(sinf(ang) * (float)rp);
            disp_fill_rect(px - 1, py - 1, 2, 2, C_RGB(255, 255, 255));
        }
    }

    disp_present();
}

/* ------------------------------------------------------------------ */
/* 表盘 5：拾音器·横排（柱子从底部向上顶白色方块）                     */
/* ------------------------------------------------------------------ */

#define LIN_BARS     21              /* 横排柱子数（从 64 频带采样，贴边多一根） */
#define LIN_X0       0               /* 第一根柱子贴左边 */
#define LIN_STEP     3               /* 柱子间距（2px 柱 + 1px 间隙） */
#define LIN_W        2               /* 柱子宽度 */
#define LIN_BASE_Y   63              /* 底部基线 */
#define LIN_MAX_H    56              /* 最大柱高 */

static float s_lin_peak[LIN_BARS];

static void face_spectrum_linear(const struct tm *t)
{
    (void)t;

    disp_clear(C_RGB(0, 0, 0));

    /* 节拍包络：快速上升、缓慢下降，驱动顶部能量条 */
    static float s_env = 0.0f;
    float energy = spectrum_energy();
    if (energy > s_env) {
        s_env = energy;
    } else {
        s_env *= 0.90f;
    }

    /* 顶部节拍能量条 */
    int bar_len = (int)(s_env * (float)DISP_W);
    if (bar_len > 0) {
        color_t ec = disp_hsl(200.0f + s_env * 120.0f, 0.9f, 0.55f);
        disp_fill_rect(0, 0, bar_len, 2, ec);
    }

    const float *bands = spectrum_bands();
    if (!bands) {
        disp_present();
        return;
    }

    for (int b = 0; b < LIN_BARS; b++) {
        float e = bands[b * MIC_NUM_BANDS / LIN_BARS];
        float cur = e;

        /* 白色峰值方块：被顶起后独立缓慢下落 */
        float peak = s_lin_peak[b];
        if (cur > peak) {
            peak = cur;
        } else {
            peak -= 0.022f;
            if (peak < 0.0f) {
                peak = 0.0f;
            }
        }
        s_lin_peak[b] = peak;

        int x = LIN_X0 + b * LIN_STEP;

        /* 彩色柱子：从底部向上生长，颜色冷→暖渐变（低频蓝紫 → 高频橙红） */
        int h = (int)(cur * (float)LIN_MAX_H);
        float hue = 220.0f - (float)b * 220.0f / (float)(LIN_BARS - 1);
        color_t c = disp_hsl(hue, 0.95f, 0.45f + 0.25f * e);
        disp_fill_rect(x, LIN_BASE_Y - h, LIN_W, h + 1, c);

        /* 白色峰值方块（2x2），落到底后隐藏 */
        if (peak > 0.10f) {
            int py = LIN_BASE_Y - (int)(peak * (float)LIN_MAX_H);
            disp_fill_rect(x, py - 1, LIN_W, 2, C_RGB(255, 255, 255));
        }
    }

    disp_present();
}

/* ------------------------------------------------------------------ */
/* 表盘 6：拾音器·密集（64 列 = 64 个频带，冷到暖渐变）               */
/* ------------------------------------------------------------------ */

/* 密集版：每列的白色峰值（被顶起后独立缓慢下落） */
static float s_dense_peak[DISP_W];

static void face_spectrum_dense(const struct tm *t)
{
    (void)t;

    disp_clear(C_RGB(0, 0, 0));

    /* 顶部节拍能量条：随总音量脉动 */
    static float s_env = 0.0f;
    float energy = spectrum_energy();
    if (energy > s_env) {
        s_env = energy;
    } else {
        s_env *= 0.90f;
    }
    int bar_len = (int)(s_env * (float)DISP_W);
    if (bar_len > 0) {
        color_t ec = disp_hsl(200.0f + s_env * 120.0f, 0.9f, 0.55f);
        disp_fill_rect(0, 0, bar_len, 2, ec);
    }

    const float *bands = spectrum_bands();
    if (!bands) {
        disp_present();
        return;
    }

    for (int x = 0; x < DISP_W; x++) {
        /* 每一列对应一个频带 */
        float e = bands[x];
        int h = (int)(e * 56.0f + 0.5f);

        /* 白色峰值：被柱子顶起后缓慢下落 */
        float cur = e;
        float peak = s_dense_peak[x];
        if (cur > peak) {
            peak = cur;
        } else {
            peak -= 0.022f;
            if (peak < 0.0f) {
                peak = 0.0f;
            }
        }
        s_dense_peak[x] = peak;

        /* 彩色柱子：低频冷色（蓝紫）→ 高频暖色（橙红） */
        if (h > 0) {
            float hue = 220.0f - (float)x * 220.0f / (float)(DISP_W - 1);
            color_t c = disp_hsl(hue, 0.95f, 0.45f + 0.25f * e);
            disp_vline(x, DISP_H - 1 - h, DISP_H - 1, c);
        }

        /* 白色峰值方块（1 列宽 × 2 行高），落到底后隐藏 */
        if (peak > 0.10f) {
            int py = DISP_H - 1 - (int)(peak * 56.0f);
            disp_vline(x, py - 1, py, C_RGB(255, 255, 255));
        }
    }

    disp_present();
}

/* ------------------------------------------------------------------ */
/* 表盘 7：图片（image/image.h：64x64 RGB565）                        */
/* ------------------------------------------------------------------ */

static void face_image(const struct tm *t)
{
    (void)t;

    disp_clear(C_RGB(0, 0, 0));

    const uint8_t *p = IMAGE;
    for (int y = 0; y < 64; y++) {
        for (int x = 0; x < 64; x++) {
            /* RGB565 大端（高字节在前） */
            uint16_t px = (uint16_t)((p[0] << 8) | p[1]);
            p += 2;

            uint8_t r5 = (px >> 11) & 0x1F;
            uint8_t g6 = (px >> 5) & 0x3F;
            uint8_t b5 = px & 0x1F;

            disp_pixel(x, y, C_RGB((r5 << 3) | (r5 >> 2),
                                   (g6 << 2) | (g6 >> 4),
                                   (b5 << 3) | (b5 >> 2)));
        }
    }

    disp_present();
}

/* ------------------------------------------------------------------ */
/* 表盘 8：网页画板（自定义图片）                                    */
/* ------------------------------------------------------------------ */

static void face_custom(const struct tm *t)
{
    (void)t;

    disp_clear(C_RGB(0, 0, 0));

    for (int y = 0; y < 64; y++) {
        for (int x = 0; x < 64; x++) {
            uint16_t v = g_custom_image[y * 64 + x];
            uint8_t r = (v >> 8) & 0xF;
            uint8_t g = (v >> 4) & 0xF;
            uint8_t b = v & 0xF;
            disp_pixel(x, y, C_RGB((r << 4) | r, (g << 4) | g, (b << 4) | b));
        }
    }

    disp_present();
}

/* ------------------------------------------------------------------ */
/* 表盘 9：示波器·李萨如（音频波形 XY 轨迹）                         */
/* ------------------------------------------------------------------ */

static void face_scope(const struct tm *t)
{
    (void)t;

    disp_clear(C_RGB(0, 0, 0));

    const int cx = 32;
    const int cy = 32;
    const int amp = 28;
    const int half = USB_AUDIO_WAVE_LEN / 2; /* 128：前半 L，后半 R */

    color_t c = C_RGB(80, 255, 220);

    if (usb_audio_wave_active()) {
        /*
         * 电脑音频：真正的立体声 XY 示波器——
         * 左声道 → X，右声道 → Y（示波器音乐的标准玩法）。
         */
        const int8_t *w = usb_audio_waveform();

        int px = cx + (int)lroundf((float)w[0] * (float)amp / 127.0f);
        int py = cy + (int)lroundf((float)w[half] * (float)amp / 127.0f);

        for (int i = 1; i < half; i++) {
            int x = cx + (int)lroundf((float)w[i] * (float)amp / 127.0f);
            int y = cy + (int)lroundf((float)w[half + i] * (float)amp / 127.0f);
            disp_line(px, py, x, y, c);
            px = x;
            py = y;
        }
    } else {
        /* 板载麦克风：单声道，用相位偏移模拟李萨如 */
        const float *wave = mic_waveform();
        if (!wave) {
            disp_present();
            return;
        }

        const int offset = MIC_WAVE_LEN / 6;

        int px = cx + (int)lroundf(wave[0] * (float)amp);
        int py = cy + (int)lroundf(wave[offset] * (float)amp);

        for (int i = 1; i < MIC_WAVE_LEN; i++) {
            int x = cx + (int)lroundf(wave[i] * (float)amp);
            int y = cy + (int)lroundf(wave[(i + offset) % MIC_WAVE_LEN] * (float)amp);
            disp_line(px, py, x, y, c);
            px = x;
            py = y;
        }
    }

    disp_present();
}

/* ------------------------------------------------------------------ */
/* 表盘 10：表情动画（机器人面部表情循环播放）                         */
/* ------------------------------------------------------------------ */

/* 弧线：角度 0=右，90=上（y 轴向下） */
static void draw_arc(int cx, int cy, int r, float a0, float a1, color_t c)
{
    for (float a = a0; a <= a1; a += 2.5f) {
        float rad = a * M_PI / 180.0f;
        int x = cx + (int)lroundf(cosf(rad) * (float)r);
        int y = cy - (int)lroundf(sinf(rad) * (float)r);
        disp_pixel(x, y, c);
    }
}

/* 心形（参数方程） */
static void draw_heart(int cx, int cy, int scale, color_t c)
{
    for (float t = 0.0f; t < 2.0f * M_PI; t += 0.08f) {
        float x = 16.0f * sinf(t) * sinf(t) * sinf(t);
        float y = -(13.0f * cosf(t) - 5.0f * cosf(2.0f * t)
                    - 2.0f * cosf(3.0f * t) - cosf(4.0f * t));
        int px = cx + (int)lroundf(x * (float)scale / 16.0f);
        int py = cy + (int)lroundf(y * (float)scale / 16.0f);
        disp_pixel(px, py, c);
        disp_pixel(px + 1, py, c);
        disp_pixel(px, py + 1, c);
    }
}

/* 开心：^ ^ 笑眼 + U 形嘴 */
static void emoji_happy(void)
{
    draw_arc(20, 28, 9, 30, 150, C_RGB(255, 220, 80));
    draw_arc(44, 28, 9, 30, 150, C_RGB(255, 220, 80));
    draw_arc(32, 40, 13, 200, 340, C_RGB(255, 140, 60));
}

/* 生气：斜眉 + 红眼 + 撇嘴 */
static void emoji_angry(void)
{
    disp_line_thick(10, 16, 26, 24, 2, C_RGB(255, 60, 60));
    disp_line_thick(54, 16, 38, 24, 2, C_RGB(255, 60, 60));
    disp_fill_circle(20, 32, 4, C_RGB(255, 90, 90));
    disp_fill_circle(44, 32, 4, C_RGB(255, 90, 90));
    draw_arc(32, 50, 10, 30, 150, C_RGB(255, 60, 60));
}

/* 惊讶：大圆眼 + O 形嘴 */
static void emoji_surprised(void)
{
    disp_circle(20, 26, 8, C_RGB(120, 220, 255));
    disp_circle(20, 26, 7, C_RGB(120, 220, 255));
    disp_circle(44, 26, 8, C_RGB(120, 220, 255));
    disp_circle(44, 26, 7, C_RGB(120, 220, 255));
    disp_circle(32, 48, 4, C_RGB(120, 220, 255));
}

/* 眨眼：一只闭眼 + 一只圆眼 + 微笑 */
static void emoji_wink(void)
{
    disp_line_thick(12, 28, 28, 28, 2, C_RGB(200, 180, 255));
    disp_circle(44, 28, 8, C_RGB(200, 180, 255));
    disp_circle(44, 28, 7, C_RGB(200, 180, 255));
    draw_arc(32, 42, 11, 200, 340, C_RGB(200, 180, 255));
}

/* 爱心：心形眼 + 微笑 */
static void emoji_love(void)
{
    draw_heart(20, 26, 7, C_RGB(255, 120, 180));
    draw_heart(44, 26, 7, C_RGB(255, 120, 180));
    draw_arc(32, 42, 11, 200, 340, C_RGB(255, 120, 180));
}

static void face_emoji(const struct tm *t)
{
    (void)t;

    /* 每个表情持续约 3 秒（30fps × 90 帧） */
    static int tick = 0;
    tick++;

    int emoji = (tick / 90) % 5;

    disp_clear(C_RGB(0, 0, 0));

    switch (emoji) {
    case 0:
        emoji_happy();
        break;
    case 1:
        emoji_angry();
        break;
    case 2:
        emoji_surprised();
        break;
    case 3:
        emoji_wink();
        break;
    default:
        emoji_love();
        break;
    }

    disp_present();
}

/* ------------------------------------------------------------------ */
/* 表盘 11：鱼缸（大小鱼游动动画）                                    */
/* ------------------------------------------------------------------ */

#define AQUA_FISH_NUM 5

static struct {
    float x;
    float y;
    float speed;   /* 像素/秒 */
    int size;      /* 鱼身长度 */
    color_t color;
    float phase;   /* 摆尾相位 */
    float wag;     /* 摆尾幅度 */
} s_fish[AQUA_FISH_NUM];

static bool s_aqua_inited = false;

static void aqua_init(void)
{
    s_aqua_inited = true;

    s_fish[0].x = 8;   s_fish[0].y = 16; s_fish[0].speed = 16; s_fish[0].size = 18;
    s_fish[0].color = C_RGB(255, 140, 50); s_fish[0].phase = 0.0f;  s_fish[0].wag = 1.0f;

    s_fish[1].x = 40;  s_fish[1].y = 32; s_fish[1].speed = 24; s_fish[1].size = 12;
    s_fish[1].color = C_RGB(70, 200, 255); s_fish[1].phase = 1.5f; s_fish[1].wag = 0.8f;

    s_fish[2].x = 20;  s_fish[2].y = 45; s_fish[2].speed = 13; s_fish[2].size = 12;
    s_fish[2].color = C_RGB(255, 220, 70); s_fish[2].phase = 3.0f; s_fish[2].wag = 0.9f;

    s_fish[3].x = 55;  s_fish[3].y = 24; s_fish[3].speed = 30; s_fish[3].size = 8;
    s_fish[3].color = C_RGB(200, 120, 255); s_fish[3].phase = 0.7f; s_fish[3].wag = 0.7f;

    s_fish[4].x = 4;   s_fish[4].y = 38; s_fish[4].speed = 21; s_fish[4].size = 8;
    s_fish[4].color = C_RGB(120, 255, 130); s_fish[4].phase = 2.2f; s_fish[4].wag = 0.7f;
}

/* 扫描线椭圆 */
static void draw_ellipse(int cx, int cy, int rx, int ry, color_t c)
{
    for (int dy = -ry; dy <= ry; dy++) {
        float t = 1.0f - (float)(dy * dy) / (float)(ry * ry);
        if (t < 0.0f) {
            continue;
        }
        int half = (int)((float)rx * sqrtf(t));
        disp_hline(cx - half, cx + half, cy + dy, c);
    }
}

/* 小鱼（向右游） */
static void draw_fish(float x, float y, int size, color_t c, float phase, float wag)
{
    int cx = (int)x;
    int cy = (int)y;
    int rx = size / 2;
    int ry = size * 3 / 10;
    if (ry < 2) {
        ry = 2;
    }

    /* 尾巴：三角 + 摆动 */
    float w = sinf(phase) * 2.5f * wag;
    int tx = cx - rx;
    int ty = cy + (int)w;
    int t1x = tx - 4;
    int t1y = cy - ry - (int)w;
    int t2x = tx - 4;
    int t2y = cy + ry + (int)w;
    disp_line(tx, ty, t1x, t1y, c);
    disp_line(tx, ty, t2x, t2y, c);
    disp_line(t1x, t1y, t2x, t2y, c);

    /* 身体 */
    draw_ellipse(cx, cy, rx, ry, c);

    /* 眼睛 */
    disp_fill_circle(cx + rx - 2, cy - 1, 1, C_RGB(255, 255, 255));
    disp_pixel(cx + rx - 2, cy, C_RGB(0, 0, 0));
}

/* 水草（底部摆动） */
static void draw_weed(int x, int base_y, int h, float phase, color_t c)
{
    for (int i = 0; i < h; i++) {
        int off = (int)(sinf(phase + (float)i * 0.45f) * 1.8f);
        disp_pixel(x + off, base_y - i, c);
        if (i % 2 == 0) {
            disp_pixel(x + off + 1, base_y - i, c);
        }
    }
}

static void face_aquarium(const struct tm *t)
{
    (void)t;

    if (!s_aqua_inited) {
        aqua_init();
    }

    /* 水背景：上深下浅的蓝色渐变 */
    for (int y = 0; y < 64; y++) {
        color_t c = disp_lerp(C_RGB(6, 26, 66), C_RGB(18, 90, 150),
                              (float)y / 63.0f);
        for (int x = 0; x < 64; x++) {
            disp_pixel(x, y, c);
        }
    }

    /* 沙底 */
    for (int y = 59; y < 64; y++) {
        disp_hline(0, 63, y, C_RGB(120, 92, 48));
    }
    /* 沙底上的小石头 */
    disp_fill_circle(12, 61, 2, C_RGB(150, 120, 70));
    disp_fill_circle(36, 62, 2, C_RGB(140, 110, 60));
    disp_fill_circle(52, 61, 1, C_RGB(160, 130, 80));

    /* 水草 */
    draw_weed(8, 58, 12, s_fish[0].phase, C_RGB(40, 180, 90));
    draw_weed(24, 58, 9, s_fish[1].phase, C_RGB(30, 150, 80));
    draw_weed(46, 58, 14, s_fish[2].phase, C_RGB(40, 180, 90));
    draw_weed(58, 58, 7, s_fish[3].phase, C_RGB(30, 150, 80));

    /* 气泡（上升） */
    static int bubble_tick = 0;
    bubble_tick++;
    for (int i = 0; i < 3; i++) {
        int tt = (bubble_tick + i * 31) % 120;
        int bx = 14 + i * 18 + (int)(sinf((float)tt * 0.08f) * 3.0f);
        int by = 56 - tt / 2;
        if (by > 3 && by < 56) {
            disp_circle(bx, by, 1 + (i % 2), C_RGB(140, 210, 255));
        }
    }

    /* 小鱼游动 */
    for (int i = 0; i < AQUA_FISH_NUM; i++) {
        s_fish[i].x += s_fish[i].speed * 0.033f;
        s_fish[i].phase += 0.18f * s_fish[i].wag;
        if (s_fish[i].x > (float)(64 + s_fish[i].size)) {
            s_fish[i].x = (float)(-s_fish[i].size);
        }
        draw_fish(s_fish[i].x, s_fish[i].y, s_fish[i].size,
                  s_fish[i].color, s_fish[i].phase, s_fish[i].wag);
    }

    /* 右上角时间：HH:MM（白色底板 + 黑色数字，对比度最高） */
    time_t now = net_time_now();
    if (now > 0) {
        struct tm tmv;
        localtime_r(&now, &tmv);

        char tbuf[6];
        fmt2(tbuf, tmv.tm_hour);
        tbuf[2] = ':';
        fmt2(tbuf + 3, tmv.tm_min);

        int tw = disp_text5x7_width(tbuf);
        int bx = 64 - tw - 2;

        /* 黑色数字（灭灯像素）直接叠加在鱼缸画面上 */
        disp_text5x7(bx, 2, tbuf, C_RGB(0, 0, 0));
    }

    disp_present();
}

/* ------------------------------------------------------------------ */
/* 对外接口                                                           */
/* ------------------------------------------------------------------ */
void face_render(int face_id, const struct tm *t)
{
    switch (face_id) {
    case 0:
        face_digital(t);
        break;
    case 1:
        face_ring(t);
        break;
    case 2:
        face_retro_seg(t);
        break;
    case 3:
        face_minimal(t);
        break;
    case 4:
        face_spectrum(t);
        break;
    case 5:
        face_spectrum_linear(t);
        break;
    case 6:
        face_spectrum_dense(t);
        break;
    case 7:
        face_image(t);
        break;
    case 8:
        face_custom(t);
        break;
    case 9:
        face_scope(t);
        break;
    case 10:
        face_emoji(t);
        break;
    case 11:
        face_aquarium(t);
        break;
    case 12:
        face_tetris();
        break;
    case 13:
        face_snake();
        break;
    case 14:
        face_flappy();
        break;
    case 15:
        face_battle();
        break;
    case 16:
        face_pacman();
        break;
    case 17:
        face_airfight();
        break;
    case 18:
        face_racer();
        break;
    default:
        face_digital(t);
        break;
    }
    disp_present();
}
