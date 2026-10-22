/*
 * HUB75 全彩 LED 单元板驱动（ESP32-S3）
 *
 * 实现思路：
 *   - 颜色数据（R1/G1/B1/R2/G2/B2）挂在 LCD_CAM 的 i80 并行总线上，
 *     由 DMA 自动把一整行 64 个像素的字节打进面板的移位寄存器；
 *   - 行地址 A~E、锁存 LAT、使能 OE 由普通 GPIO 控制；
 *   - 同一时刻移位与显示分离（移位时 OE 消隐），逐行扫描完成整屏刷新。
 *
 * 说明：demo1 只做"点亮纯色"，每像素每个颜色通道 1 bit（亮/灭），
 *       不做 PWM 灰度调制，因此代码简单、时序容易保证。
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief HUB75 面板引脚配置
 *
 * data_gpio[0..5] 依次对应面板的 R1、G1、B1、R2、G2、B2；
 * data_gpio[6..7] 是 i80 总线必须占用的空闲引脚，不接面板（悬空即可）；
 * dc_gpio        是 i80 总线必须占用的空闲引脚，不接面板（悬空即可）。
 */
typedef struct {
    int data_gpio[8];    /*!< 8 条并行数据线对应的 GPIO */
    int clk_gpio;        /*!< 面板 CLK（对应 i80 的 WR 信号）*/
    int dc_gpio;         /*!< i80 总线占用，悬空即可 */
    int lat_gpio;        /*!< 锁存 LAT（STB）*/
    int oe_gpio;         /*!< 输出使能 OE，低电平点亮 */
    int addr_gpio[5];    /*!< 行地址线 A、B、C、D、E */

    int width;           /*!< 水平像素数，64 */
    int height;          /*!< 垂直像素数，64 */
    uint32_t pclk_hz;    /*!< 面板 CLK 频率 */
    uint32_t line_on_us; /*!< 每行点亮时间（us），越大越亮 */
} hub75_config_t;

/**
 * @brief 初始化 HUB75 面板（GPIO + i80 总线）
 */
esp_err_t hub75_init(const hub75_config_t *cfg);

/**
 * @brief 启动后台刷新任务
 */
esp_err_t hub75_start(void);

/**
 * @brief 全屏填充一个颜色（仅支持亮/灭，非 0 即点亮）
 *
 * @note 该函数只是更新目标颜色，下一帧刷新时即生效。
 */
void hub75_fill(uint8_t r, uint8_t g, uint8_t b);

#ifdef __cplusplus
}
#endif
