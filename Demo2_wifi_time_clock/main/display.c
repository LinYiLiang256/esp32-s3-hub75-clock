/*
 * display.c：RGB444 帧缓冲 + 绘图原语
 */
#include <string.h>
#include <math.h>

#include "display.h"

/* 双缓冲：s_front 为正在显示的缓冲，渲染任务写入另一个 */
static uint16_t s_fb[2][DISP_W * DISP_H];
static volatile int s_front = 0;
static volatile int s_pending = -1;

void disp_init(void)
{
    memset(s_fb, 0, sizeof(s_fb));
    s_front = 0;
    s_pending = -1;
}

uint16_t *disp_back(void)
{
    return s_fb[(s_front + 1) & 1];
}

void disp_present(void)
{
    s_pending = (s_front + 1) & 1;
}

const uint16_t *disp_front(void)
{
    return s_fb[s_front];
}

void disp_frame_start(void)
{
    int p = s_pending;
    if (p >= 0) {
        s_front = p;
        s_pending = -1;
    }
}

/* ---------------- 颜色 ---------------- */

color_t disp_hsl(float h, float s, float l)
{
    float c = (1.0f - fabsf(2.0f * l - 1.0f)) * s;
    float hp = h / 60.0f;
    float x = c * (1.0f - fabsf(fmodf(hp, 2.0f) - 1.0f));
    float r = 0, g = 0, b = 0;
    int seg = (int)hp % 6;
    if (seg < 0) {
        seg += 6;
    }
    switch (seg) {
    case 0: r = c; g = x; break;
    case 1: r = x; g = c; break;
    case 2: g = c; b = x; break;
    case 3: g = x; b = c; break;
    case 4: r = x; b = c; break;
    default: r = c; b = x; break;
    }
    float m = l - c / 2.0f;
    color_t out;
    out.r = (uint8_t)((r + m) * 255.0f + 0.5f);
    out.g = (uint8_t)((g + m) * 255.0f + 0.5f);
    out.b = (uint8_t)((b + m) * 255.0f + 0.5f);
    return out;
}

static inline uint8_t clamp_u8(float v)
{
    if (v <= 0.0f) {
        return 0;
    }
    if (v >= 255.0f) {
        return 255;
    }
    return (uint8_t)(v + 0.5f);
}

color_t disp_lerp(color_t a, color_t b, float t)
{
    if (t < 0) {
        t = 0;
    }
    if (t > 1) {
        t = 1;
    }
    color_t o;
    o.r = clamp_u8(a.r + (b.r - a.r) * t);
    o.g = clamp_u8(a.g + (b.g - a.g) * t);
    o.b = clamp_u8(a.b + (b.b - a.b) * t);
    return o;
}

/* ---------------- 绘图原语 ---------------- */

static inline void put(uint16_t *fb, int x, int y, color_t c)
{
    if ((unsigned)x < DISP_W && (unsigned)y < DISP_H) {
        fb[y * DISP_W + x] = (uint16_t)(((c.r >> 4) << 8) | ((c.g >> 4) << 4) | (c.b >> 4));
    }
}

void disp_clear(color_t c)
{
    uint16_t v = (uint16_t)(((c.r >> 4) << 8) | ((c.g >> 4) << 4) | (c.b >> 4));
    uint16_t *fb = disp_back();
    for (int i = 0; i < DISP_W * DISP_H; i++) {
        fb[i] = v;
    }
}

void disp_pixel(int x, int y, color_t c)
{
    put(disp_back(), x, y, c);
}

void disp_hline(int x0, int x1, int y, color_t c)
{
    if (x0 > x1) {
        int t = x0;
        x0 = x1;
        x1 = t;
    }
    uint16_t *fb = disp_back();
    for (int x = x0; x <= x1; x++) {
        put(fb, x, y, c);
    }
}

void disp_vline(int x, int y0, int y1, color_t c)
{
    if (y0 > y1) {
        int t = y0;
        y0 = y1;
        y1 = t;
    }
    uint16_t *fb = disp_back();
    for (int y = y0; y <= y1; y++) {
        put(fb, x, y, c);
    }
}

void disp_line(int x0, int y0, int x1, int y1, color_t c)
{
    uint16_t *fb = disp_back();
    int dx = abs(x1 - x0);
    int dy = -abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    for (;;) {
        put(fb, x0, y0, c);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

void disp_line_thick(int x0, int y0, int x1, int y1, int w, color_t c)
{
    uint16_t *fb = disp_back();
    int dx = abs(x1 - x0);
    int dy = -abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    int r = w / 2;

    for (;;) {
        for (int oy = -r; oy <= r; oy++) {
            for (int ox = -r; ox <= r; ox++) {
                if (ox * ox + oy * oy <= r * r + r) {
                    put(fb, x0 + ox, y0 + oy, c);
                }
            }
        }
        if (x0 == x1 && y0 == y1) {
            break;
        }
        int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

void disp_circle(int cx, int cy, int r, color_t c)
{
    uint16_t *fb = disp_back();
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        put(fb, cx + x, cy + y, c);
        put(fb, cx + y, cy + x, c);
        put(fb, cx - y, cy + x, c);
        put(fb, cx - x, cy + y, c);
        put(fb, cx - x, cy - y, c);
        put(fb, cx - y, cy - x, c);
        put(fb, cx + y, cy - x, c);
        put(fb, cx + x, cy - y, c);
        y++;
        if (err < 0) {
            err += 2 * y + 1;
        } else {
            x--;
            err += 2 * (y - x) + 1;
        }
    }
}

void disp_fill_circle(int cx, int cy, int r, color_t c)
{
    uint16_t *fb = disp_back();
    for (int y = -r; y <= r; y++) {
        int span = (int)sqrtf((float)(r * r - y * y));
        for (int x = -span; x <= span; x++) {
            put(fb, cx + x, cy + y, c);
        }
    }
}

void disp_fill_rect(int x, int y, int w, int h, color_t c)
{
    uint16_t *fb = disp_back();
    for (int j = y; j < y + h; j++) {
        for (int i = x; i < x + w; i++) {
            put(fb, i, j, c);
        }
    }
}

/* ---------------- 字体 ---------------- */

int disp_text5x7_width(const char *s)
{
    int n = (int)strlen(s);
    return n > 0 ? n * 6 - 1 : 0;
}

void disp_char5x7(int x, int y, char ch, color_t c)
{
    const uint8_t *g = font5x7_lookup(ch);
    if (!g) {
        return;
    }
    uint16_t *fb = disp_back();
    for (int col = 0; col < 5; col++) {
        uint8_t bits = g[col];
        for (int row = 0; row < 7; row++) {
            if (bits & (1u << row)) {
                put(fb, x + col, y + row, c);
            }
        }
    }
}

void disp_text5x7(int x, int y, const char *s, color_t c)
{
    while (*s) {
        disp_char5x7(x, y, *s, c);
        x += 6;
        s++;
    }
}

int disp_text8x16_width(const char *s)
{
    int n = (int)strlen(s);
    return n > 0 ? n * 9 - 1 : 0;
}

void disp_char8x16(int x, int y, char ch, color_t c)
{
    if (ch < '0' || ch > '9') {
        return; /* 8x16 仅收录数字 */
    }
    const uint8_t *g = font8x16_digits[ch - '0'];
    uint16_t *fb = disp_back();
    for (int row = 0; row < 16; row++) {
        uint8_t bits = g[row];
        for (int col = 0; col < 8; col++) {
            if (bits & (1u << col)) {
                put(fb, x + col, y + row, c);
            }
        }
    }
}

void disp_text8x16(int x, int y, const char *s, color_t c)
{
    while (*s) {
        if (*s >= '0' && *s <= '9') {
            disp_char8x16(x, y, *s, c);
            x += 9;
        } else {
            x += 4; /* 冒号等由调用方单独绘制，这里留空 */
        }
        s++;
    }
}

/* 以 1.5 倍最近邻放大绘制 8x16 数字：每个字宽 12、高 24 */
void disp_char8x16_big(int x, int y, char ch, color_t c)
{
    if (ch < '0' || ch > '9') {
        return;
    }
    const uint8_t *g = font8x16_digits[ch - '0'];
    uint16_t *fb = disp_back();
    for (int j = 0; j < 24; j++) {
        int sy = j * 2 / 3;
        uint8_t bits = g[sy];
        for (int i = 0; i < 12; i++) {
            if (bits & (1u << (i * 2 / 3))) {
                put(fb, x + i, y + j, c);
            }
        }
    }
}
