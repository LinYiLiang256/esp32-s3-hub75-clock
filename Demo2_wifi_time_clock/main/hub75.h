/*
 * HUB75 全彩 LED 单元板驱动（ESP32-S3）—— 灰度版本
 *
 * 在 demo1 的基础上扩展为 RGB444 灰度（每通道 4 位 = 16 级，共 4096 色），
 * 采用 BCM（二进制编码调制）时间片：每个颜色位对应一段点亮时间，
 * 位权越高点亮时间越长，人眼混合后即为灰度。
 *
 * 数据流向：
 *   display 模块维护 64x64 的 RGB444 帧缓冲（双缓冲），
 *   刷新任务每帧把帧缓冲拆成 4 个位平面，逐行、逐位平面送到面板。
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief HUB75 面板引脚与刷新参数
 *
 * data_gpio[0..5] 依次对应面板的 R1、G1、B1、R2、G2、B2；
 * data_gpio[6..7] 与 dc_gpio 是 i80 总线必须占用的空闲引脚，悬空即可。
 */
typedef struct {
    int data_gpio[8];    /*!< 8 条并行数据线对应的 GPIO */
    int clk_gpio;        /*!< 面板 CLK（i80 的 WR 信号）*/
    int dc_gpio;         /*!< i80 总线占用，悬空 */
    int lat_gpio;        /*!< 锁存 LAT（STB）*/
    int oe_gpio;         /*!< 输出使能 OE，低电平点亮 */
    int addr_gpio[5];    /*!< 行地址线 A、B、C、D、E */

    int width;           /*!< 水平像素数，64 */
    int height;          /*!< 垂直像素数，64（1/32 扫描）*/
    uint32_t pclk_hz;    /*!< 面板 CLK 频率，建议 10MHz */
    uint32_t bcm_base_us;/*!< BCM 最小时间片（bit0 的显示时间），建议 8us */
} hub75_config_t;

/**
 * @brief 初始化 HUB75 面板（GPIO + i80 总线）
 */
esp_err_t hub75_init(const hub75_config_t *cfg);

/**
 * @brief 启动后台刷新任务（持续把帧缓冲扫描到面板）
 */
esp_err_t hub75_start(void);

/**
 * @brief 设置屏幕亮度
 *
 * @param percent 亮度百分比 5~100（映射到 BCM 时间片 2~12us）
 */
void hub75_set_brightness(int percent);

#ifdef __cplusplus
}
#endif
