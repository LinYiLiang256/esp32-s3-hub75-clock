/*
 * HUB75 灰度刷新驱动实现
 *
 * 每帧时序（1/32 扫描，行地址 0~31，每行同时驱动上下两排）：
 *   for 每一行:
 *     for 每个颜色位 b = 0..3:          # BCM 4 位
 *       组数据（上下两排像素的第 b 位）→ 移位 → 锁存 → 点亮 bcm_base_us<<b
 *
 * 帧缓冲由 display 模块以 RGB444（uint16_t）保存，双缓冲切换。
 */
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_io_i80.h"
#include "esp_rom_sys.h"
#include "esp_log.h"
#include "esp_check.h"

#include "hub75.h"
#include "display.h"

static const char *TAG = "hub75";

/* 颜色数据在字节中的位定义，对应 i80 数据线 D0~D5 */
#define BIT_R1 (1u << 0)
#define BIT_G1 (1u << 1)
#define BIT_B1 (1u << 2)
#define BIT_R2 (1u << 3)
#define BIT_G2 (1u << 4)
#define BIT_B2 (1u << 5)

/* 面板 OE 极性：绝大多数 HUB75 面板低电平有效（低 = 点亮） */
#define HUB75_OE_ACTIVE_LOW 1

/* BCM 位深：4 位 = 16 级/通道 */
#define HUB75_BCM_BITS 4

static struct {
    hub75_config_t cfg;
    esp_lcd_panel_io_handle_t io;
    SemaphoreHandle_t trans_done;
} s_hub75;

/* 当前 BCM 最小时间片（us），由亮度控制 */
static volatile uint32_t s_bcm_base_us = 8;

static inline void oe_on(void)
{
    gpio_set_level(s_hub75.cfg.oe_gpio, HUB75_OE_ACTIVE_LOW ? 0 : 1);
}

static inline void oe_off(void)
{
    gpio_set_level(s_hub75.cfg.oe_gpio, HUB75_OE_ACTIVE_LOW ? 1 : 0);
}

static inline void set_addr(int row)
{
    for (int i = 0; i < 5; i++) {
        gpio_set_level(s_hub75.cfg.addr_gpio[i], (row >> i) & 0x1);
    }
}

static inline void latch(void)
{
    gpio_set_level(s_hub75.cfg.lat_gpio, 1);
    esp_rom_delay_us(1);
    gpio_set_level(s_hub75.cfg.lat_gpio, 0);
}

/* DMA 传输完成回调（中断上下文） */
static bool IRAM_ATTR trans_done_cb(esp_lcd_panel_io_handle_t io,
                                    esp_lcd_panel_io_event_data_t *edata,
                                    void *user_ctx)
{
    BaseType_t task_woken = pdFALSE;
    xSemaphoreGiveFromISR(s_hub75.trans_done, &task_woken);
    return task_woken == pdTRUE;
}

/*
 * 组装一行中某个颜色位的数据：
 *   - 上半屏像素 (row, x) 的 R/G/B 第 bit 位 → R1/G1/B1
 *   - 下半屏像素 (row+32, x) 的 R/G/B 第 bit 位 → R2/G2/B2
 */
static void build_line(const uint16_t *fb, int row, int bit, uint8_t *out)
{
    const int w = s_hub75.cfg.width;
    const int h2 = s_hub75.cfg.height / 2;
    const uint16_t *p1 = &fb[row * w];
    const uint16_t *p2 = &fb[(row + h2) * w];

    for (int x = 0; x < w; x++) {
        uint16_t a = p1[x];
        uint16_t b = p2[x];
        uint8_t v = 0;
        v |= (uint8_t)(((a >> (8 + bit)) & 0x1) << 0); /* R1 */
        v |= (uint8_t)(((a >> (4 + bit)) & 0x1) << 1); /* G1 */
        v |= (uint8_t)(((a >> (0 + bit)) & 0x1) << 2); /* B1 */
        v |= (uint8_t)(((b >> (8 + bit)) & 0x1) << 3); /* R2 */
        v |= (uint8_t)(((b >> (4 + bit)) & 0x1) << 4); /* G2 */
        v |= (uint8_t)(((b >> (0 + bit)) & 0x1) << 5); /* B2 */
        out[x] = v;
    }
}

static void hub75_refresh_task(void *arg)
{
    const hub75_config_t *cfg = &s_hub75.cfg;
    const int scan_rows = cfg->height / 2;

    static uint8_t line[256] __attribute__((aligned(64)));

    while (1) {
        /* 帧开始：处理双缓冲切换，拿到要显示的那一帧 */
        disp_frame_start();
        const uint16_t *fb = disp_front();

        for (int row = 0; row < scan_rows; row++) {
            set_addr(row);

            for (int bit = 0; bit < HUB75_BCM_BITS; bit++) {
                build_line(fb, row, bit, line);

                oe_off(); /* 移位期间消隐 */
                xSemaphoreTake(s_hub75.trans_done, 0);
                esp_lcd_panel_io_tx_color(s_hub75.io, -1, line, cfg->width);
                xSemaphoreTake(s_hub75.trans_done, portMAX_DELAY);

                latch();  /* 锁存到面板 */
                oe_on();  /* 点亮本行本段 */
                esp_rom_delay_us(s_bcm_base_us << bit);
            }
            oe_off(); /* 行结束消隐 */
        }

        /* 帧末消隐，避免最后一行显示时间过长 */
        oe_off();
        vTaskDelay(1);
    }
}

esp_err_t hub75_init(const hub75_config_t *cfg)
{
    ESP_RETURN_ON_FALSE(cfg, ESP_ERR_INVALID_ARG, TAG, "配置为空");
    ESP_RETURN_ON_FALSE(cfg->width > 0 && cfg->width <= 256, ESP_ERR_INVALID_ARG, TAG, "width 超出范围");
    ESP_RETURN_ON_FALSE((cfg->height % 2) == 0 && cfg->height >= 2, ESP_ERR_INVALID_ARG, TAG, "height 必须为偶数");
    ESP_RETURN_ON_FALSE(cfg->pclk_hz > 0 && cfg->bcm_base_us > 0, ESP_ERR_INVALID_ARG, TAG, "时钟/时间片无效");

    memcpy(&s_hub75.cfg, cfg, sizeof(*cfg));

    /* ---- 手动控制引脚：LAT、OE、A~E ---- */
    int manual_pins[] = {
        cfg->lat_gpio, cfg->oe_gpio,
        cfg->addr_gpio[0], cfg->addr_gpio[1], cfg->addr_gpio[2],
        cfg->addr_gpio[3], cfg->addr_gpio[4],
    };
    uint64_t pin_mask = 0;
    for (int i = 0; i < (int)(sizeof(manual_pins) / sizeof(manual_pins[0])); i++) {
        ESP_RETURN_ON_FALSE(manual_pins[i] >= 0, ESP_ERR_INVALID_ARG, TAG, "控制引脚无效");
        pin_mask |= (1ULL << manual_pins[i]);
    }

    gpio_config_t gpio_cfg = {
        .pin_bit_mask = pin_mask,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&gpio_cfg), TAG, "配置控制引脚失败");

    oe_off();
    gpio_set_level(cfg->lat_gpio, 0);
    for (int i = 0; i < 5; i++) {
        gpio_set_level(cfg->addr_gpio[i], 0);
    }

    /* ---- 创建 i80 总线 ---- */
    esp_lcd_i80_bus_config_t bus_cfg = {
        .dc_gpio_num = cfg->dc_gpio,
        .wr_gpio_num = cfg->clk_gpio,
        .clk_src = LCD_CLK_SRC_PLL160M,
        .bus_width = 8,
        .max_transfer_bytes = 256,
        .dma_burst_size = 16,
    };
    for (int i = 0; i < 8; i++) {
        ESP_RETURN_ON_FALSE(cfg->data_gpio[i] >= 0, ESP_ERR_INVALID_ARG, TAG, "数据引脚无效");
        bus_cfg.data_gpio_nums[i] = cfg->data_gpio[i];
    }

    esp_lcd_i80_bus_handle_t bus = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_new_i80_bus(&bus_cfg, &bus), TAG, "创建 i80 总线失败");

    esp_lcd_panel_io_i80_config_t io_cfg = {
        .cs_gpio_num = -1,
        .pclk_hz = cfg->pclk_hz,
        .trans_queue_depth = 2,
        .on_color_trans_done = trans_done_cb,
        .user_ctx = NULL,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .dc_levels = {
            .dc_idle_level = 0,
            .dc_cmd_level = 0,
            .dc_dummy_level = 0,
            .dc_data_level = 0,
        },
        .flags = {
            .cs_active_high = 0,
            .reverse_color_bits = 0,
            .swap_color_bytes = 0,
            .pclk_active_neg = 1,   /* 数据在 CLK 下降沿输出，面板上升沿采样 */
            .pclk_idle_low = 1,
        },
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i80(bus, &io_cfg, &s_hub75.io),
                        TAG, "创建 i80 面板 IO 失败");

    s_hub75.trans_done = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_hub75.trans_done, ESP_ERR_NO_MEM, TAG, "创建信号量失败");

    s_bcm_base_us = cfg->bcm_base_us ? cfg->bcm_base_us : 8;

    ESP_LOGI(TAG, "HUB75 %dx%d 灰度(4bit) 初始化完成", cfg->width, cfg->height);
    return ESP_OK;
}

void hub75_set_brightness(int percent)
{
    if (percent < 5) {
        percent = 5;
    }
    if (percent > 100) {
        percent = 100;
    }
    /* 5% -> 约 2.5us，100% -> 12us，线性映射 */
    s_bcm_base_us = 2 + (uint32_t)percent * 10 / 100;
    ESP_LOGI(TAG, "亮度设置为 %d%%（BCM 时间片 %u us）", percent, (unsigned)s_bcm_base_us);
}

esp_err_t hub75_start(void)
{
    ESP_RETURN_ON_FALSE(s_hub75.io != NULL, ESP_ERR_INVALID_STATE, TAG, "尚未初始化");
    BaseType_t ret = xTaskCreatePinnedToCore(hub75_refresh_task, "hub75_refresh",
                                             4096, NULL, 6, NULL, 1);
    ESP_RETURN_ON_FALSE(ret == pdPASS, ESP_ERR_NO_MEM, TAG, "创建刷新任务失败");
    return ESP_OK;
}
