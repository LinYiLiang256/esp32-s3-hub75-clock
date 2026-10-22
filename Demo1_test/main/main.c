/*
 * demo1：HUB75 全彩 LED 单元板（64x64）点亮测试
 *
 * 现象：整屏红色 -> 绿色 -> 蓝色，每 2 秒切换一次，循环往复。
 *
 * 接线请参考工程根目录的 README.md。
 */
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "hub75.h"

static const char *TAG = "demo1";

/* ------------------------------------------------------------------ */
/* 引脚分配（与 README.md 保持一致，可按需修改）                      */
/* ------------------------------------------------------------------ */
static const hub75_config_t s_panel_cfg = {
    /* R1 G1 B1 R2 G2 B2 + 2 个空闲位（i80 必须 8 位宽，后两个悬空） */
    .data_gpio  = {4, 5, 6, 7, 8, 9, 39, 40},
    .clk_gpio   = 10,          /* HUB75 CLK */
    .dc_gpio    = 21,          /* 空闲，悬空即可（i80 要求占用） */
    .lat_gpio   = 11,          /* HUB75 LAT / STB */
    .oe_gpio    = 12,          /* HUB75 OE（低电平点亮） */
    .addr_gpio  = {13, 14, 15, 16, 17}, /* HUB75 A B C D E */

    .width      = 64,          /* 水平像素 */
    .height     = 64,          /* 垂直像素（1/32 扫描） */
    .pclk_hz    = 10 * 1000 * 1000, /* 面板 CLK = 10MHz */
    .line_on_us = 150,         /* 每行点亮 150us，整屏约 160Hz 刷新 */
};

void app_main(void)
{
    ESP_LOGI(TAG, "HUB75 64x64 全彩点阵屏 demo1：红/绿/蓝 每 2 秒切换");

    ESP_ERROR_CHECK(hub75_init(&s_panel_cfg));
    ESP_ERROR_CHECK(hub75_start());

    while (1) {
        ESP_LOGI(TAG, ">>> 红色");
        hub75_fill(255, 0, 0);
        vTaskDelay(pdMS_TO_TICKS(2000));

        ESP_LOGI(TAG, ">>> 绿色");
        hub75_fill(0, 255, 0);
        vTaskDelay(pdMS_TO_TICKS(2000));

        ESP_LOGI(TAG, ">>> 蓝色");
        hub75_fill(0, 0, 255);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}
