/*
 * net_time.h：SNTP 网络时间同步
 */
#pragma once

#include <time.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 SNTP（需在 WiFi 连接成功后调用）
 *
 * 时区设置为中国标准时间 CST-8（UTC+8）。
 */
esp_err_t net_time_init(void);

/**
 * @brief 是否已经完成一次时间同步
 */
bool net_time_synced(void);

/**
 * @brief 获取当前本地时间；未同步时返回 0
 */
time_t net_time_now(void);

#ifdef __cplusplus
}
#endif
