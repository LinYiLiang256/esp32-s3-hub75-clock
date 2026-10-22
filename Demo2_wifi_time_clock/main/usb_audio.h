/*
 * usb_audio.h：电脑音频数据接收（USB 串口）
 *
 * 电脑端脚本采集系统音频、做 FFT 分解为 64 个频带，
 * 通过 UART0（CH343 口，115200）发送固定帧给 ESP32：
 *
 *   [0xAA][0x55] + 64 字节频带能量(0~100) + 1 字节校验和
 *
 * 收到数据后，拾音器面板（环形/横排/密集）自动改用电脑音频显示；
 * 超过 1 秒没有数据则回落到板载麦克风。
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define USB_AUDIO_BANDS 64
#define USB_AUDIO_WAVE_LEN 256

/**
 * @brief 初始化 UART0 接收（与日志共用串口，全双工互不影响）
 */
esp_err_t usb_audio_init(void);

/**
 * @brief 是否正在接收电脑音频（最近 1 秒内收到过有效帧）
 */
bool usb_audio_active(void);

/**
 * @brief 最近一帧的 64 个频带能量（0~1）
 */
const float *usb_audio_bands(void);

/**
 * @brief 是否正在接收电脑波形（最近 1 秒内）
 */
bool usb_audio_wave_active(void);

/**
 * @brief 最近一帧电脑音频波形（int8 -128~127，长度 USB_AUDIO_WAVE_LEN）
 */
const int8_t *usb_audio_waveform(void);

#ifdef __cplusplus
}
#endif
