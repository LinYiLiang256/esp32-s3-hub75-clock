/*
 * HUB75 全彩 LED 单元板驱动实现
 *
 * 刷新时序（每一行）：
 *   1) OE 拉高，消隐
 *   2) 切换行地址（A~E）
 *   3) DMA 把本行 64 个像素的数据送进面板移位寄存器
 *   4) LAT 打一个脉冲，把移位寄存器内容锁存到面板行上
 *   5) OE 拉低，该行点亮 line_on_us 微秒
 *   6) 跳到下一行，32 行扫完即为一帧
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

static const char *TAG = "hub75";

/* 单行最大像素数（一行字节数 = 像素数） */
#define HUB75_MAX_LINE_PIXELS 256

/* 颜色数据在字节中的位定义，对应 i80 数据线 D0~D5 */
#define BIT_R1 (1u << 0)
#define BIT_G1 (1u << 1)
#define BIT_B1 (1u << 2)
#define BIT_R2 (1u << 3)
#define BIT_G2 (1u << 4)
#define BIT_B2 (1u << 5)

/* 面板 OE 极性：绝大多数 HUB75 面板 OE 低电平有效（低 = 点亮） */
#define HUB75_OE_ACTIVE_LOW 1

static struct {
    hub75_config_t cfg;
    esp_lcd_panel_io_handle_t io;
    SemaphoreHandle_t trans_done;
    volatile uint8_t color; /* 当前全屏颜色（编码后的字节） */
} s_hub75;

/* ------------------------------------------------------------------ */
/* 控制引脚操作                                                       */
/* ------------------------------------------------------------------ */

static inline void oe_on(void)
{
    gpio_set_level(s_hub75.cfg.oe_gpio, HUB75_OE_ACTIVE_LOW ? 0 : 1);
}

static inline void oe_off(void)
{
    gpio_set_level(s_hub75.cfg.oe_gpio, HUB75_OE_ACTIVE_LOW ? 1 : 0);
}

/* 设置行地址：A~E 分别为 row 的 bit0~bit4 */
static inline void set_addr(int row)
{
    for (int i = 0; i < 5; i++) {
        gpio_set_level(s_hub75.cfg.addr_gpio[i], (row >> i) & 0x1);
    }
}

/* 锁存移位寄存器内容到面板行 */
static inline void latch(void)
{
    gpio_set_level(s_hub75.cfg.lat_gpio, 1);
    esp_rom_delay_us(1);
    gpio_set_level(s_hub75.cfg.lat_gpio, 0);
}

/* ------------------------------------------------------------------ */
/* DMA 传输完成回调（中断上下文）                                     */
/* ------------------------------------------------------------------ */

static bool IRAM_ATTR trans_done_cb(esp_lcd_panel_io_handle_t io,
                                    esp_lcd_panel_io_event_data_t *edata,
                                    void *user_ctx)
{
    BaseType_t task_woken = pdFALSE;
    xSemaphoreGiveFromISR(s_hub75.trans_done, &task_woken);
    return task_woken == pdTRUE;
}

/* ------------------------------------------------------------------ */
/* 刷新任务                                                           */
/* ------------------------------------------------------------------ */

static void hub75_refresh_task(void *arg)
{
    const hub75_config_t *cfg = &s_hub75.cfg;
    const int scan_rows = cfg->height / 2; /* 1/32 扫描：64 行由 32 个行地址驱动 */

    /* DMA 源缓冲区，要求 64 字节对齐（i80 数据需按 cache line 对齐） */
    static uint8_t line[HUB75_MAX_LINE_PIXELS] __attribute__((aligned(64)));
    uint8_t cur_color = 0xFF; /* 与初始值不同，保证第一帧会填充 */

    while (1) {
        uint8_t color = s_hub75.color;
        if (color != cur_color) {
            memset(line, color, cfg->width);
            cur_color = color;
        }

        for (int row = 0; row < scan_rows; row++) {
            oe_off();             /* 移位期间消隐，避免拖影 */
            set_addr(row);        /* 切换行地址 */
            esp_rom_delay_us(2);  /* 等待地址线稳定 */

            /* 清掉可能残留的完成信号，保证与本行传输一一对应 */
            xSemaphoreTake(s_hub75.trans_done, 0);
            esp_lcd_panel_io_tx_color(s_hub75.io, -1, line, cfg->width);
            xSemaphoreTake(s_hub75.trans_done, portMAX_DELAY);

            latch();              /* 锁存本行数据 */
            oe_on();              /* 点亮本行 */
            esp_rom_delay_us(cfg->line_on_us);
        }
        /*
         * 重要：帧末必须先消隐再让出 CPU。
         * 否则 vTaskDelay 的这几个毫秒里，最后锁存的那一行（地址 31）
         * 会一直亮着，导致该行亮度明显高于其它行。
         */
        oe_off();
        /* 每帧让出一次 CPU，避免触发任务看门狗 */
        vTaskDelay(1);
    }
}

/* ------------------------------------------------------------------ */
/* 对外接口                                                           */
/* ------------------------------------------------------------------ */

esp_err_t hub75_init(const hub75_config_t *cfg)
{
    ESP_RETURN_ON_FALSE(cfg, ESP_ERR_INVALID_ARG, TAG, "配置为空");
    ESP_RETURN_ON_FALSE(cfg->width > 0 && cfg->width <= HUB75_MAX_LINE_PIXELS,
                        ESP_ERR_INVALID_ARG, TAG, "width 超出范围");
    ESP_RETURN_ON_FALSE((cfg->height % 2) == 0 && cfg->height >= 2,
                        ESP_ERR_INVALID_ARG, TAG, "height 必须为偶数");
    ESP_RETURN_ON_FALSE(cfg->pclk_hz > 0 && cfg->line_on_us > 0,
                        ESP_ERR_INVALID_ARG, TAG, "pclk_hz / line_on_us 无效");

    memcpy(&s_hub75.cfg, cfg, sizeof(*cfg));

    /* ---- 1. 手动控制引脚：LAT、OE、A~E ---- */
    int manual_pins[] = {
        cfg->lat_gpio, cfg->oe_gpio,
        cfg->addr_gpio[0], cfg->addr_gpio[1], cfg->addr_gpio[2],
        cfg->addr_gpio[3], cfg->addr_gpio[4],
    };
    uint64_t pin_mask = 0;
    for (int i = 0; i < sizeof(manual_pins) / sizeof(manual_pins[0]); i++) {
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

    oe_off();  /* 上电先消隐 */
    gpio_set_level(cfg->lat_gpio, 0);
    for (int i = 0; i < 5; i++) {
        gpio_set_level(cfg->addr_gpio[i], 0);
    }

    /* ---- 2. 创建 i80 总线 ---- */
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

    /* ---- 3. 创建面板 IO（byteswap/latch 等都不需要）---- */
    esp_lcd_panel_io_i80_config_t io_cfg = {
        .cs_gpio_num = -1, /* 独占整条 i80 总线 */
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
            /* 数据在 CLK 下降沿输出，面板在 CLK 上升沿采样 */
            .pclk_active_neg = 1,
            .pclk_idle_low = 1,
        },
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i80(bus, &io_cfg, &s_hub75.io),
                        TAG, "创建 i80 面板 IO 失败");

    /* ---- 4. 完成信号量 ---- */
    s_hub75.trans_done = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_hub75.trans_done, ESP_ERR_NO_MEM, TAG, "创建信号量失败");

    s_hub75.color = 0; /* 初始熄灭 */

    ESP_LOGI(TAG, "HUB75 %dx%d 初始化完成，i80 时钟 %u Hz",
             cfg->width, cfg->height, (unsigned)cfg->pclk_hz);
    return ESP_OK;
}

esp_err_t hub75_start(void)
{
    ESP_RETURN_ON_FALSE(s_hub75.io != NULL, ESP_ERR_INVALID_STATE, TAG, "尚未初始化");
    BaseType_t ret = xTaskCreatePinnedToCore(hub75_refresh_task, "hub75_refresh",
                                             4096, NULL, 5, NULL, 1);
    ESP_RETURN_ON_FALSE(ret == pdPASS, ESP_ERR_NO_MEM, TAG, "创建刷新任务失败");
    return ESP_OK;
}

void hub75_fill(uint8_t r, uint8_t g, uint8_t b)
{
    uint8_t v = 0;
    if (r) {
        v |= BIT_R1 | BIT_R2;
    }
    if (g) {
        v |= BIT_G1 | BIT_G2;
    }
    if (b) {
        v |= BIT_B1 | BIT_B2;
    }
    s_hub75.color = v;
}
