/*
 * mic.h：MAX9814 模拟麦克风采集 + 频带能量分析
 *
 * 使用 ADC1_CH1（GPIO2）连续采样（DMA），每帧 512 样本做
 * MIC_NUM_BANDS 个频带的 Goertzel 分析，输出 0~1 归一化能量。
 *
 * 降噪处理链：
 *   1) 一阶高通滤波（去除低频环境噪声/电源哼声）；
 *   2) 每频带自适应噪声底跟踪；
 *   3) 噪声门（信号需高于噪声底 N 倍才有效）；
 *   4) 峰值归一化 + 快速衰减；
 *   5) 攻击/释放平滑；
 *   6) 整体静音检测。
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MIC_NUM_BANDS 64
#define MIC_WAVE_LEN 512

/**
 * @brief 初始化 MAX9814 采集（GPIO2 / ADC1_CH1），启动后台分析任务
 */
esp_err_t mic_init(void);

/**
 * @brief 获取各频带能量（0~1），未就绪时返回 NULL
 */
const float *mic_bands(void);

/**
 * @brief 总能量（0~1），用于节拍/波纹效果
 */
float mic_energy(void);

/**
 * @brief 最近一帧原始波形（归一化 -1~1，长度 MIC_FRAME_SAMPLES=512）
 *
 * 供示波器表盘绘制使用；未就绪时返回 NULL。
 */
const float *mic_waveform(void);

/**
 * @brief 设置拾音灵敏度 0~100
 *
 * 越高越灵敏（噪声门越宽松），越低越"安静"（噪声门更严格）。
 * 默认 50。
 */
void mic_set_sensitivity(int percent);

/**
 * @brief 读取当前灵敏度
 */
int mic_sensitivity(void);

#ifdef __cplusplus
}
#endif
