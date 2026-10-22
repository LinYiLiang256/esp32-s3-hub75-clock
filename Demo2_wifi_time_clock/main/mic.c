/*
 * mic.c：MAX9814 模拟麦克风采集 + 频带能量分析（带降噪）
 *
 * 采集：ADC1_CH1（GPIO2）连续模式（DMA），8kHz / 12bit。
 * 分析：每帧 512 样本，对 MIC_NUM_BANDS 个频带（100Hz~3500Hz 对数分布）
 *       做 Goertzel 幅度计算。
 *
 * 降噪处理链（关键）：
 *   1. 一阶高通（约 80Hz）去掉低频嗡嗡声与环境低频噪声；
 *   2. 每个频带维护"自适应噪声底"：底噪快速跟随下降、极慢跟随上升；
 *   3. 噪声门：只有明显高于噪声底（默认 2.7 倍）的信号才计入；
 *   4. 峰值归一化（峰值快速衰减），避免一次响度后长期满格；
 *   5. 攻击快/释放慢的平滑，避免噪声抖动；
 *   6. 总能量低于静音阈值时直接归零（防止安静时闪烁）。
 */
#include <math.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_adc/adc_continuous.h"
#include "esp_check.h"
#include "esp_log.h"

#include "mic.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

static const char *TAG = "mic";

#define MIC_ADC_UNIT       ADC_UNIT_1
#define MIC_ADC_CHANNEL    ADC_CHANNEL_1   /* GPIO2 */
#define MIC_SAMPLE_RATE    8000
#define MIC_FRAME_SAMPLES  512
#define MIC_FRAME_BYTES    (MIC_FRAME_SAMPLES * 2) /* TYPE2: 每样本 2 字节 */

#define MIC_FREQ_MIN       100.0f
#define MIC_FREQ_MAX       3500.0f

/* 一阶高通系数：约 80Hz @ 8kHz（exp(-2π*80/8000)） */
#define MIC_HP_ALPHA       0.94f

/* 噪声底跟踪速度 */
#define NOISE_DOWN_K       0.05f    /* 快速跟随下降 */
#define NOISE_UP_K         0.0015f  /* 缓慢跟随上升（更快适应环境噪声变化） */

/* 峰值衰减：每帧乘以该系数（30fps 下约每秒降 26%） */
#define PEAK_DECAY         0.99f

/* 攻击/释放平滑系数 */
#define ATTACK_K           0.40f
#define RELEASE_K          0.08f

/* 软门限：低于该值视为静音，不显示 */
#define SOFT_GATE          0.12f   /* 低于该值视为静音，不显示 */

/* 整体静音阈值 */
#define MUTE_THRESHOLD     0.05f

static adc_continuous_handle_t s_adc;
static float s_bands[MIC_NUM_BANDS];
static float s_wave[MIC_WAVE_LEN]; /* 最近一帧归一化波形 */
static float s_band_freq[MIC_NUM_BANDS];
static float s_noise[MIC_NUM_BANDS];    /* 自适应噪声底 */
static float s_peak_norm[MIC_NUM_BANDS];/* 峰值归一化 */
static float s_hp_y, s_hp_x;            /* 高通滤波器状态 */
static volatile float s_energy;
static volatile bool s_ready;
static volatile int s_sens = 50;
static volatile bool s_reset = true;

/* Goertzel 算法：计算 x 中 freq 频率分量的幅度 */
static float goertzel(const float *x, int n, float freq, float fs)
{
    float w = 2.0f * (float)M_PI * freq / fs;
    float coeff = 2.0f * cosf(w);
    float s1 = 0.0f, s2 = 0.0f;

    for (int i = 0; i < n; i++) {
        float s0 = x[i] + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    float p = s1 * s1 + s2 * s2 - coeff * s1 * s2;
    if (p < 0.0f) {
        p = 0.0f;
    }
    return sqrtf(p);
}

static void mic_task(void *arg)
{
    static uint8_t dma_buf[2048];
    static float samples[MIC_FRAME_SAMPLES];

    while (1) {
        uint32_t len = 0;
        esp_err_t ret = adc_continuous_read(s_adc, dma_buf, sizeof(dma_buf), &len,
                                            pdMS_TO_TICKS(100));
        if (ret != ESP_OK || len < 4) {
            continue;
        }

        int n = (int)(len / 2);
        if (n > MIC_FRAME_SAMPLES) {
            n = MIC_FRAME_SAMPLES;
        }

        /* ---- 1. 解析 12bit 采样 ---- */
        const uint16_t *raw = (const uint16_t *)dma_buf;
        for (int i = 0; i < n; i++) {
            samples[i] = (float)(raw[i] & 0x0FFF);
        }

        /* ---- 2. 一阶高通滤波（去直流 + 低频噪声） ---- */
        for (int i = 0; i < n; i++) {
            float x = samples[i];
            s_hp_y = MIC_HP_ALPHA * (s_hp_y + x - s_hp_x);
            s_hp_x = x;
            samples[i] = s_hp_y;
        }

        /* ---- 2.5 保存归一化波形（示波器使用） ---- */
        {
            float maxv = 0.0f;
            for (int i = 0; i < n; i++) {
                float a = fabsf(samples[i]);
                if (a > maxv) {
                    maxv = a;
                }
            }
            if (maxv > 30.0f) {
                float inv = 1.0f / maxv;
                for (int i = 0; i < n; i++) {
                    s_wave[i] = samples[i] * inv;
                }
            } else {
                /* 安静时输出平直波 */
                for (int i = 0; i < n; i++) {
                    s_wave[i] = 0.0f;
                }
            }
        }

        /* ---- 3. 各频带能量 + 降噪 ---- */
        /* 灵敏度 0~100 映射到噪声门倍率 6.0~1.0（越低越严格） */
        float mult = 6.0f - (float)s_sens * 0.05f;
        bool reset = s_reset;
        float total = 0.0f;

        for (int b = 0; b < MIC_NUM_BANDS; b++) {
            float mag = goertzel(samples, n, s_band_freq[b], MIC_SAMPLE_RATE);
            float avg = mag / (float)n;

            if (reset) {
                s_noise[b] = avg;
                s_peak_norm[b] = 1e-4f;
                s_bands[b] = 0.0f;
            } else {
                /* 自适应噪声底：下降快、上升极慢 */
                if (avg < s_noise[b]) {
                    s_noise[b] += (avg - s_noise[b]) * NOISE_DOWN_K;
                } else {
                    s_noise[b] += (avg - s_noise[b]) * NOISE_UP_K;
                }
            }

            /* 噪声门：减去噪声底的倍率部分 */
            float sig = avg - s_noise[b] * mult;
            if (sig < 0.0f) {
                sig = 0.0f;
            }

            /* 峰值归一化（峰值快速衰减） */
            if (sig > s_peak_norm[b]) {
                s_peak_norm[b] = sig;
            } else {
                s_peak_norm[b] *= PEAK_DECAY;
            }
            if (s_peak_norm[b] < 1e-4f) {
                s_peak_norm[b] = 1e-4f;
            }

            float norm = sig / s_peak_norm[b];
            if (norm > 1.0f) {
                norm = 1.0f;
            }
            /* 归一化后再次压缩低端：安静环境下的微小波动被压掉，
             * 只有明显的声音才会让柱子明显跳动 */
            norm = norm * norm;

            /* 攻击快、释放慢 */
            float k = (norm > s_bands[b]) ? ATTACK_K : RELEASE_K;
            s_bands[b] += (norm - s_bands[b]) * k;

            /* 软门限：小值直接归零，防止噪声闪烁 */
            if (s_bands[b] < SOFT_GATE) {
                s_bands[b] = 0.0f;
            }
            total += s_bands[b];
        }
        s_reset = false;

        /* ---- 4. 整体静音检测 ---- */
        float energy = total / (float)MIC_NUM_BANDS;
        if (energy < MUTE_THRESHOLD) {
            energy = 0.0f;
            for (int b = 0; b < MIC_NUM_BANDS; b++) {
                s_bands[b] = 0.0f;
            }
        }
        s_energy = energy;
        s_ready = true;
    }
}

esp_err_t mic_init(void)
{
    /* 频带中心频率：100Hz~3500Hz 对数分布 */
    float ratio = MIC_FREQ_MAX / MIC_FREQ_MIN;
    for (int b = 0; b < MIC_NUM_BANDS; b++) {
        float t = (float)b / (float)(MIC_NUM_BANDS - 1);
        s_band_freq[b] = MIC_FREQ_MIN * powf(ratio, t);
        s_noise[b] = 0.0f;
        s_peak_norm[b] = 1e-4f;
    }
    s_hp_x = 0.0f;
    s_hp_y = 0.0f;
    s_reset = true;

    adc_continuous_handle_cfg_t adc_cfg = {
        .max_store_buf_size = 4096,
        .conv_frame_size = MIC_FRAME_BYTES,
        .flags.flush_pool = false,
    };
    ESP_RETURN_ON_ERROR(adc_continuous_new_handle(&adc_cfg, &s_adc), TAG, "ADC 初始化失败");

    adc_digi_pattern_config_t pattern = {
        .atten = ADC_ATTEN_DB_12,
        .channel = MIC_ADC_CHANNEL,
        .unit = MIC_ADC_UNIT,
        .bit_width = ADC_BITWIDTH_12,
    };
    adc_continuous_config_t dig_cfg = {
        .sample_freq_hz = MIC_SAMPLE_RATE,
        .conv_mode = ADC_CONV_SINGLE_UNIT_1,
        .format = ADC_DIGI_OUTPUT_FORMAT_TYPE2,
        .pattern_num = 1,
        .adc_pattern = &pattern,
    };
    ESP_RETURN_ON_ERROR(adc_continuous_config(s_adc, &dig_cfg), TAG, "ADC 配置失败");
    ESP_RETURN_ON_ERROR(adc_continuous_start(s_adc), TAG, "ADC 启动失败");

    BaseType_t ret = xTaskCreate(mic_task, "mic", 8192, NULL, 5, NULL);
    ESP_RETURN_ON_FALSE(ret == pdPASS, ESP_ERR_NO_MEM, TAG, "创建麦克风任务失败");

    ESP_LOGI(TAG, "MAX9814 拾音器已启动（ADC1_CH1/GPIO2，%dHz，%d 个频带，降噪开启）",
             MIC_SAMPLE_RATE, MIC_NUM_BANDS);
    return ESP_OK;
}

const float *mic_bands(void)
{
    return s_ready ? s_bands : NULL;
}

const float *mic_waveform(void)
{
    return s_ready ? s_wave : NULL;
}

float mic_energy(void)
{
    return s_ready ? s_energy : 0.0f;
}

void mic_set_sensitivity(int percent)
{
    if (percent < 0) {
        percent = 0;
    }
    if (percent > 100) {
        percent = 100;
    }
    s_sens = percent;
    s_reset = true; /* 重新学习噪声底 */
    ESP_LOGI(TAG, "拾音灵敏度设置为 %d", percent);
}

int mic_sensitivity(void)
{
    return s_sens;
}
