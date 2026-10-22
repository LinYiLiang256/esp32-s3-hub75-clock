/*
 * text.c：UTF-8 文本点阵渲染与滚动字幕
 *
 * 字库：textfont.bin（二进制嵌入，编译快）：
 *   - GB2312 符号区（全角标点等）+ 一级常用汉字，24x24 点阵
 *   - ASCII 可打印字符 95 个，12x24 点阵
 *
 * 格式：magic "TFN2" | uint32 count | uint16 cp[count] | uint8 bits[count][78] | uint8 ascii[95][52]
 * 汉字 24x26（每行 3 字节，26 行 = 78 字节）；ASCII 12x26（每行 2 字节，26 行 = 52 字节）。
 */
#include <string.h>

#include "display.h"
#include "text.h"

#define TEXT_H        26
#define TEXT_Y        ((DISP_H - TEXT_H) / 2)   /* 19 */
#define SCROLL_SPEED  1      /* 每帧滚动像素（约 30px/s） */
#define TEXT_MAX_LEN  192    /* 最多字节数（约 64 个汉字） */

/* 嵌入的二进制字库（由 CMake EMBED_FILES 生成） */
extern const uint8_t textfont_bin_start[] asm("_binary_textfont_bin_start");

static char s_text[TEXT_MAX_LEN];
static int s_offset;
static int s_width;
static volatile bool s_active;

static const uint16_t *s_cp;
static const uint8_t *s_bits;
static const uint8_t *s_ascii;
static int s_count;
static bool s_font_ready;

static void textfont_init(void)
{
    if (s_font_ready) {
        return;
    }
    s_font_ready = true;

    const uint8_t *p = textfont_bin_start;
    if (memcmp(p, "TFN2", 4) != 0) {
        s_count = 0;
        return;
    }
    uint32_t count = 0;
    memcpy(&count, p + 4, 4);

    s_count = (int)count;
    s_cp = (const uint16_t *)(p + 8);
    s_bits = p + 8 + count * 2;
    s_ascii = p + 8 + count * 2 + count * 78;
}

/* 解码一个 UTF-8 字符，返回占用字节数 */
static int utf8_decode(const char *s, uint32_t *cp)
{
    const unsigned char *p = (const unsigned char *)s;

    if (p[0] < 0x80) {
        *cp = p[0];
        return 1;
    }
    if ((p[0] & 0xE0) == 0xC0) {
        *cp = ((uint32_t)(p[0] & 0x1F) << 6) | (p[1] & 0x3F);
        return 2;
    }
    if ((p[0] & 0xF0) == 0xE0) {
        *cp = ((uint32_t)(p[0] & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
        return 3;
    }
    if ((p[0] & 0xF8) == 0xF0) {
        *cp = ((uint32_t)(p[0] & 0x07) << 18) | ((uint32_t)(p[1] & 0x3F) << 12) |
              ((uint32_t)(p[2] & 0x3F) << 6) | (p[3] & 0x3F);
        return 4;
    }
    *cp = '?';
    return 1;
}

/* 按 Unicode 码点二分查找点阵 */
static const uint8_t *find_cjk(uint32_t cp)
{
    if (cp > 0xFFFF) {
        return NULL;
    }
    int lo = 0;
    int hi = s_count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        uint16_t mcp = s_cp[mid];
        if (mcp == (uint16_t)cp) {
            return s_bits + (size_t)mid * 78;
        }
        if (mcp < (uint16_t)cp) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return NULL;
}

/* 绘制单个字符，返回占用的宽度 */
static int draw_glyph(uint32_t cp, int x, int y, color_t c)
{
    if (cp < 0x80) {
        if (cp < 32 || cp > 126) {
            return 12;
        }
        const uint8_t *g = s_ascii + (cp - 32) * 52;
        for (int row = 0; row < 26; row++) {
            uint16_t bits = (uint16_t)((g[row * 2] << 8) | g[row * 2 + 1]);
            for (int col = 0; col < 12; col++) {
                if (bits & (0x8000 >> col)) {
                    disp_pixel(x + col, y + row, c);
                }
            }
        }
        return 12;
    }

    const uint8_t *g = find_cjk(cp);
    if (!g) {
        return 24; /* 未收录字符留空 */
    }
    for (int row = 0; row < 26; row++) {
        uint32_t bits = ((uint32_t)g[row * 3] << 16) |
                        ((uint32_t)g[row * 3 + 1] << 8) |
                        g[row * 3 + 2];
        for (int col = 0; col < 24; col++) {
            if (bits & (0x800000u >> col)) {
                disp_pixel(x + col, y + row, c);
            }
        }
    }
    return 24;
}

int text_draw(const char *utf8, int x, int y)
{
    int idx = 0;

    textfont_init();

    while (*utf8) {
        uint32_t cp;
        int n = utf8_decode(utf8, &cp);

        /* 每个字符一个彩虹色 */
        color_t c = disp_hsl((float)((idx * 47) % 360), 0.95f, 0.60f);
        x += draw_glyph(cp, x, y, c);

        idx++;
        utf8 += n;
    }
    return x;
}

int text_width(const char *utf8)
{
    int w = 0;

    while (*utf8) {
        uint32_t cp;
        int n = utf8_decode(utf8, &cp);
        w += (cp < 0x80) ? 12 : 24;
        utf8 += n;
    }
    return w;
}

void text_set(const char *utf8)
{
    strncpy(s_text, utf8, sizeof(s_text) - 1);
    s_text[sizeof(s_text) - 1] = 0;

    s_width = text_width(s_text);
    s_offset = DISP_W; /* 从屏幕右侧外开始 */
    s_active = (s_text[0] != 0);
}

bool text_active(void)
{
    return s_active;
}

void text_scroll_render(void)
{
    disp_clear(C_RGB(0, 0, 0));

    if (s_active) {
        text_draw(s_text, s_offset, TEXT_Y);

        s_offset -= SCROLL_SPEED;
        if (s_offset < -s_width) {
            s_offset = DISP_W; /* 滚出后循环 */
        }
    }

    disp_present();
}
