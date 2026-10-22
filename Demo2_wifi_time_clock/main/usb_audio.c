/*
 * usb_audio.c：电脑音频频带数据接收（UART0）
 *
 * 电脑端（tools/pc_audio_sender.py）发送帧：
 *   [0xAA][0x55] + 64 字节(0~100) + 校验和
 *
 * 注意：UART0 同时用于日志输出（TX），接收（RX）独立，互不影响。
 */
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/uart.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "usb_audio.h"

static const char *TAG = "usb_audio";

#define FRAME_HEAD1       0xAA
#define FRAME_HEAD2       0x55
#define FRAME_TYPE_BANDS  0x01
#define FRAME_TYPE_WAVE   0x02

static float s_bands[USB_AUDIO_BANDS];
static int8_t s_wave[USB_AUDIO_WAVE_LEN];
static volatile int64_t s_last_ms;
static volatile bool s_started;
static volatile int64_t s_wave_last_ms;
static volatile bool s_wave_started;

static void rx_task(void *arg)
{
    uint8_t buf[256];
    uint8_t frame[USB_AUDIO_WAVE_LEN + 2];
    int idx = 0;
    int state = 0; /* 0: 等 head1，1: 等 head2，2: 等类型，3: 收数据 */
    int expect = 0;
    uint8_t ftype = 0;

    int64_t last_log = 0;

    while (1) {
        int n = uart_read_bytes(UART_NUM_0, buf, sizeof(buf), pdMS_TO_TICKS(50));
        if (n <= 0) {
            continue;
        }
        for (int i = 0; i < n; i++) {
            uint8_t b = buf[i];

            if (state == 0) {
                if (b == FRAME_HEAD1) {
                    state = 1;
                }
            } else if (state == 1) {
                if (b == FRAME_HEAD2) {
                    state = 2;
                } else {
                    state = (b == FRAME_HEAD1) ? 1 : 0;
                }
            } else if (state == 2) {
                if (b == FRAME_TYPE_BANDS) {
                    ftype = b;
                    expect = USB_AUDIO_BANDS + 1;
                    idx = 0;
                    state = 3;
                } else if (b == FRAME_TYPE_WAVE) {
                    ftype = b;
                    expect = USB_AUDIO_WAVE_LEN + 1;
                    idx = 0;
                    state = 3;
                } else {
                    state = (b == FRAME_HEAD1) ? 1 : 0;
                }
            } else {
                frame[idx++] = b;
                if (idx == expect) {
                    /* 校验：数据字节之和 & 0xFF */
                    uint8_t sum = 0;
                    for (int k = 0; k < expect - 1; k++) {
                        sum += frame[k];
                    }
                    if (sum == frame[expect - 1]) {
                        if (ftype == FRAME_TYPE_BANDS) {
                            for (int k = 0; k < USB_AUDIO_BANDS; k++) {
                                s_bands[k] = (float)frame[k] / 100.0f;
                            }
                            s_last_ms = esp_timer_get_time() / 1000;
                            s_started = true;

                            int64_t now = s_last_ms;
                            if (now - last_log > 3000) {
                                last_log = now;
                                ESP_LOGI(TAG, "收到电脑音频数据（频带+波形）");
                            }
                        } else {
                            memcpy(s_wave, frame, USB_AUDIO_WAVE_LEN);
                            s_wave_last_ms = esp_timer_get_time() / 1000;
                            s_wave_started = true;
                        }
                    }
                    state = 0;
                }
            }
        }
    }
}

esp_err_t usb_audio_init(void)
{
    if (!uart_is_driver_installed(UART_NUM_0)) {
        uart_config_t cfg = {
            .baud_rate = 115200,
            .data_bits = UART_DATA_8_BITS,
            .parity = UART_PARITY_DISABLE,
            .stop_bits = UART_STOP_BITS_1,
            .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
            .source_clk = UART_SCLK_DEFAULT,
        };
        ESP_RETURN_ON_ERROR(uart_param_config(UART_NUM_0, &cfg), TAG, "UART0 参数配置失败");
        ESP_RETURN_ON_ERROR(uart_driver_install(UART_NUM_0, 4096, 0, 0, NULL, 0),
                            TAG, "UART0 驱动安装失败");
    }

    BaseType_t ret = xTaskCreate(rx_task, "usb_audio", 4096, NULL, 4, NULL);
    ESP_RETURN_ON_FALSE(ret == pdPASS, ESP_ERR_NO_MEM, TAG, "创建接收任务失败");

    ESP_LOGI(TAG, "电脑音频接收已就绪（UART0 115200）");
    return ESP_OK;
}

bool usb_audio_active(void)
{
    if (!s_started) {
        return false;
    }
    int64_t now = esp_timer_get_time() / 1000;
    return (now - s_last_ms) < 1000;
}

const float *usb_audio_bands(void)
{
    return s_bands;
}

bool usb_audio_wave_active(void)
{
    if (!s_wave_started) {
        return false;
    }
    int64_t now = esp_timer_get_time() / 1000;
    return (now - s_wave_last_ms) < 1000;
}

const int8_t *usb_audio_waveform(void)
{
    return s_wave;
}
