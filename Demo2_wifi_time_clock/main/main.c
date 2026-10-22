/*
 * demo2：WiFi 配网 + 网络时钟（HUB75 64x64 全彩点阵）
 *
 * 工作流程：
 *   1. 上电加载 NVS 中保存的 WiFi 配置；
 *   2. 有配置 → 连接 WiFi → SNTP 对时 → 按选择的表盘循环显示时间；
 *   3. 无配置或连接失败 → 进入 AP 配网模式：
 *        手机连接热点 LED-CLOCK-SETUP（密码 12345678），
 *        浏览器访问 http://192.168.4.1 选择 WiFi、输入密码、选择表盘，
 *        保存后设备自动重启并联网显示时钟；
 *   4. 联网成功后设置网页仍然可用（访问设备 IP 可修改表盘）。
 */
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"

#include "hub75.h"
#include "display.h"
#include "faces.h"
#include "wifi_prov.h"
#include "net_time.h"
#include "mic.h"
#include "text.h"
#include "usb_audio.h"
#include "state.h"

static const char *TAG = "demo2";

/* 跨模块共享的运行时状态（见 state.h） */
volatile int g_face_id = 0;
volatile bool g_provisioning = false;
volatile bool g_text_mode = false;

/* 网页画板图片缓冲（64x64，RGB444，BSS 约 8KB） */
uint16_t g_custom_image[64 * 64];

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

/* ------------------------------------------------------------------ */
/* HUB75 引脚配置（与 demo1 相同，可按需修改）                        */
/* ------------------------------------------------------------------ */
static const hub75_config_t s_panel_cfg = {
    /* R1 G1 B1 R2 G2 B2 + 2 个空闲位（i80 必须 8 位宽，后两个悬空） */
    .data_gpio   = {4, 5, 6, 7, 8, 9, 39, 40},
    .clk_gpio    = 10,          /* HUB75 CLK */
    .dc_gpio     = 21,          /* 空闲，悬空即可 */
    .lat_gpio    = 11,          /* HUB75 LAT / STB */
    .oe_gpio     = 12,          /* HUB75 OE（低电平点亮） */
    .addr_gpio   = {13, 14, 15, 16, 17}, /* HUB75 A B C D E */

    .width       = 64,
    .height      = 64,
    .pclk_hz     = 10 * 1000 * 1000,
    .bcm_base_us = 8,           /* BCM 最小时间片（亮度/闪烁折中） */
};

/* ------------------------------------------------------------------ */
/* 状态画面                                                           */
/* ------------------------------------------------------------------ */

static void show_boot_screen(void)
{
    disp_clear(C_RGB(3, 5, 14));

    const char *title = "LED-CLOCK";
    int w = disp_text5x7_width(title);
    int x = (DISP_W - w) / 2;
    for (int i = 0; title[i]; i++) {
        color_t c = disp_hsl((float)(i * 40 % 360), 0.9f, 0.6f);
        disp_char5x7(x + i * 6, 12, title[i], c);
    }

    const char *sub = "BOOTING";
    w = disp_text5x7_width(sub);
    disp_text5x7((DISP_W - w) / 2, 38, sub, C_RGB(120, 200, 255));

    disp_present();
}

/* WiFi 连接等待画面（animated dots） */
static void show_connecting(int tick)
{
    disp_clear(C_RGB(4, 6, 16));

    const char *label = "WIFI";
    int w = disp_text5x7_width(label);
    disp_text5x7((DISP_W - w) / 2, 18, label, C_RGB(120, 200, 255));

    /* 三个跳动的点 */
    for (int i = 0; i < 3; i++) {
        int active = ((tick / 4) % 4) == i;
        color_t c = active ? C_RGB(255, 230, 90) : C_RGB(60, 70, 100);
        disp_fill_circle(22 + i * 10, 34, 2, c);
    }

    const char *label2 = "CONNECT";
    w = disp_text5x7_width(label2);
    disp_text5x7((DISP_W - w) / 2, 48, label2, C_RGB(150, 160, 200));

    disp_present();
}

/* 等待时间同步画面 */
static void show_sync_wait(int tick)
{
    disp_clear(C_RGB(4, 6, 16));

    /* 旋转的彩虹圆环 */
    int spin = tick * 7;
    for (int a = 0; a < 360; a += 4) {
        float rad = (float)(a + spin) * M_PI / 180.0f;
        int x = 32 + (int)lroundf(sinf(rad) * 20.0f);
        int y = 32 - (int)lroundf(cosf(rad) * 20.0f);
        disp_pixel(x, y, disp_hsl((float)a, 0.9f, 0.55f));
    }

    const char *label = "SYNC";
    int w = disp_text5x7_width(label);
    disp_text5x7((DISP_W - w) / 2, 29, label, C_RGB(255, 255, 255));

    disp_present();
}

/* AP 配网提示画面 */
static void show_setup_screen(int tick)
{
    disp_clear(C_RGB(4, 6, 20));

    /* 顶部彩虹标题 */
    const char *title = "LED-CLOCK";
    int w = disp_text5x7_width(title);
    int x = (DISP_W - w) / 2;
    for (int i = 0; title[i]; i++) {
        color_t c = disp_hsl((float)((i * 40 + tick * 3) % 360), 0.9f, 0.6f);
        disp_char5x7(x + i * 6, 4, title[i], c);
    }

    /* 旋转圆环 */
    int spin = tick * 9;
    for (int a = 0; a < 360; a += 4) {
        float rad = (float)(a + spin) * M_PI / 180.0f;
        int px = 32 + (int)lroundf(sinf(rad) * 24.0f);
        int py = 30 - (int)lroundf(cosf(rad) * 24.0f);
        disp_pixel(px, py, disp_hsl((float)a, 0.95f, 0.55f));
    }

    /* 中心 SETUP */
    const char *label = "SETUP";
    w = disp_text5x7_width(label);
    disp_text5x7((DISP_W - w) / 2, 27, label, C_RGB(255, 255, 255));

    /* 底部 IP 提示：192.168.4.1（紧凑 5px 间距） */
    const char *ip = "192.168.4.1";
    x = (DISP_W - (int)strlen(ip) * 5) / 2;
    color_t ic = C_RGB(120, 230, 255);
    for (int i = 0; ip[i]; i++) {
        disp_char5x7(x + i * 5, 54, ip[i], ic);
    }

    disp_present();
}

/* ------------------------------------------------------------------ */
/* 时钟主循环                                                         */
/* ------------------------------------------------------------------ */

static void clock_loop(const wifi_cfg_t *cfg)
{
    int tick = 0;
    int link_check = 0;

    while (1) {
        /* 每 5 秒检查一次 WiFi，掉线则自动重连 */
        if (++link_check >= 25) {
            link_check = 0;
            if (wifi_prov_sta_ip() == NULL) {
                ESP_LOGW(TAG, "WiFi 掉线，尝试重连...");
                if (wifi_prov_connect_sta(cfg, 15000)) {
                    ESP_LOGI(TAG, "WiFi 重连成功");
                    wifi_prov_start_ap(); /* 恢复管理热点 */
                }
            }
        }

        /* 滚动字幕模式：30fps 刷新 */
        if (g_text_mode) {
            text_scroll_render();
            vTaskDelay(pdMS_TO_TICKS(33));
            continue;
        }


        /* 拾音器/示波器/表情/鱼缸/俄罗斯方块（6/7/8/11/12/13/14）：30fps */
        if ((g_face_id >= FACE_SPECTRUM && g_face_id <= FACE_SPECTRUM_DENSE) ||
            g_face_id == FACE_SCOPE || g_face_id == FACE_EMOJI ||
            g_face_id == FACE_AQUARIUM || g_face_id == FACE_TETRIS ||
            g_face_id == FACE_SNAKE || g_face_id == FACE_FLAPPY ||
            g_face_id == FACE_BATTLE || g_face_id == FACE_PACMAN ||
            g_face_id == FACE_AIRFIGHT || g_face_id == FACE_RACER) {
            face_render(g_face_id, NULL);
            vTaskDelay(pdMS_TO_TICKS(33));
            continue;
        }

        time_t now = net_time_now();
        if (now > 0) {
            struct tm t;
            localtime_r(&now, &t);
            face_render(g_face_id, &t); /* 管理页面修改后立即生效 */
        } else {
            show_sync_wait(tick);
        }
        tick++;
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

/* ------------------------------------------------------------------ */
/* 入口                                                               */
/* ------------------------------------------------------------------ */

void app_main(void)
{
    /* NVS（WiFi 与配置存储都需要） */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_LOGI(TAG, "================ demo2 网络时钟启动 ================");

    /* 显示屏 */
    disp_init();
    ESP_ERROR_CHECK(hub75_init(&s_panel_cfg));
    ESP_ERROR_CHECK(hub75_start());
    show_boot_screen();
    vTaskDelay(pdMS_TO_TICKS(300));

    /* 麦克风（MAX9814 拾音器） */
    ESP_ERROR_CHECK(mic_init());

    /* 电脑音频接收（USB 串口，收到数据时优先显示电脑音乐） */
    ESP_ERROR_CHECK(usb_audio_init());

    /* 网络 */
    ESP_ERROR_CHECK(wifi_prov_init());

    wifi_cfg_t cfg;
    bool have_cfg = (wifi_cfg_load(&cfg) == ESP_OK) && (cfg.ssid[0] != 0);

    if (have_cfg) {
        ESP_LOGI(TAG, "发现已保存配置：SSID=%s 表盘=%d", cfg.ssid, cfg.face);

        for (int i = 0; i < 40; i++) { /* 连接过程中显示动画 */
            show_connecting(i);
            vTaskDelay(pdMS_TO_TICKS(100));
            if (i > 3) {
                break; /* 只在开始阶段画几帧，后面交给连接阻塞 */
            }
        }

        if (wifi_prov_connect_sta(&cfg, 20000)) {
            ESP_LOGI(TAG, "WiFi 已连接，设备 IP: %s", wifi_prov_sta_ip());
            /* 保持管理热点常开：随时连 LED-CLOCK-SETUP 访问 192.168.4.1 进管理页 */
            wifi_prov_start_ap();
            wifi_prov_start_web();

            net_time_init();

            /* 等待时间同步（最多 20 秒） */
            for (int i = 0; i < 100 && !net_time_synced(); i++) {
                show_sync_wait(i);
                vTaskDelay(pdMS_TO_TICKS(200));
            }
            if (!net_time_synced()) {
                ESP_LOGW(TAG, "时间同步超时，继续显示");
            }

            g_face_id = cfg.face;
            hub75_set_brightness(cfg.brightness);
            mic_set_sensitivity(cfg.mic_sens);

            ESP_LOGI(TAG, "进入时钟显示，表盘 ID=%d 亮度=%d%% 拾音灵敏度=%d",
                     g_face_id, cfg.brightness, cfg.mic_sens);
            clock_loop(&cfg);
        }

        ESP_LOGW(TAG, "WiFi 连接失败，进入配网模式");
    } else {
        ESP_LOGW(TAG, "没有保存的 WiFi 配置，进入配网模式");
    }

    /* 配网模式 */
    g_provisioning = true;
    if (have_cfg) {
        hub75_set_brightness(cfg.brightness);
    }
    ESP_ERROR_CHECK(wifi_prov_start_ap());
    ESP_ERROR_CHECK(wifi_prov_start_dns());
    ESP_ERROR_CHECK(wifi_prov_start_web());

    int tick = 0;
    while (1) {
        show_setup_screen(tick++);
        vTaskDelay(pdMS_TO_TICKS(150));
    }
}
