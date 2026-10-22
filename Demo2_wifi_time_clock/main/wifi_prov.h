/*
 * wifi_prov.h：WiFi 配网 + 设置网页
 *
 * 首次使用（或连接失败）时进入 AP 配网模式：
 *   手机/电脑连接热点 "LED-CLOCK-SETUP"（密码 12345678），
 *   浏览器访问 http://192.168.4.1 选择 WiFi、输入密码、选择表盘，保存后设备重启。
 *
 * 联网成功后同样会启动设置网页（访问设备 IP 可改表盘）。
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AP_SSID "LED-CLOCK-SETUP"
#define AP_PASS "12345678"

typedef struct {
    char ssid[33];
    char pass[65];
    uint8_t face;       /* 表盘 ID */
    uint8_t brightness; /* 亮度 5~100 */
    uint8_t mic_sens;   /* 拾音灵敏度 0~100 */
} wifi_cfg_t;

/* 从 NVS 读取/保存配置 */
esp_err_t wifi_cfg_load(wifi_cfg_t *cfg);
esp_err_t wifi_cfg_save(const wifi_cfg_t *cfg);

/* 初始化网络与 WiFi 驱动（只需调用一次） */
esp_err_t wifi_prov_init(void);

/* STA 模式连接指定 WiFi，成功返回 true（阻塞，超时返回 false） */
bool wifi_prov_connect_sta(const wifi_cfg_t *cfg, uint32_t timeout_ms);

/* 启动 AP 配网模式（APSTA） */
esp_err_t wifi_prov_start_ap(void);

/* 启动 captive portal DNS（所有域名解析到 192.168.4.1） */
esp_err_t wifi_prov_start_dns(void);

/* 启动 HTTP 设置网页（幂等） */
esp_err_t wifi_prov_start_web(void);

/* 保存配置后重启设备 */
void wifi_prov_restart(void);

/* 当前 STA 的 IP 字符串（未联网返回 NULL） */
const char *wifi_prov_sta_ip(void);

#ifdef __cplusplus
}
#endif
