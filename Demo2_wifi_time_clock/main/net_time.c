/*
 * net_time.c：SNTP 网络时间同步
 */
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>

#include "esp_sntp.h"
#include "esp_log.h"

#include "net_time.h"

static const char *TAG = "net_time";

static volatile bool s_synced = false;

static void sntp_sync_cb(struct timeval *tv)
{
    s_synced = true;
    ESP_LOGI(TAG, "SNTP 时间同步完成");
}

esp_err_t net_time_init(void)
{
    /* 时区：中国标准时间 UTC+8（POSIX TZ 字符串） */
    setenv("TZ", "CST-8", 1);
    tzset();

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_setservername(1, "cn.pool.ntp.org");
    esp_sntp_set_time_sync_notification_cb(sntp_sync_cb);
    esp_sntp_init();

    ESP_LOGI(TAG, "SNTP 已启动（ntp.aliyun.com / cn.pool.ntp.org）");
    return ESP_OK;
}

bool net_time_synced(void)
{
    return s_synced;
}

time_t net_time_now(void)
{
    if (!s_synced) {
        return 0;
    }
    time_t now = 0;
    time(&now);
    return now;
}
