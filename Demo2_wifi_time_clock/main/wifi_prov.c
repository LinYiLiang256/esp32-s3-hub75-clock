/*
 * wifi_prov.c：WiFi 配网 + 设置网页 + captive portal
 */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_check.h"

#include "lwip/sockets.h"
#include "lwip/inet.h"

#include "nvs_flash.h"
#include "nvs.h"

#include "cJSON.h"

#include "wifi_prov.h"
#include "faces.h"
#include "hub75.h"
#include "mic.h"
#include "text.h"
#include "state.h"
#include "web_index.h"

static const char *TAG = "wifi_prov";

#define NVS_NS "ledclock"

/* ------------------------------------------------------------------ */
/* NVS                                                                 */
/* ------------------------------------------------------------------ */


/* 旧版表盘编号 → 新版迁移（删除拟物钟 0 与彩虹钟 2，其余前移两位） */
static uint8_t face_migrate(uint8_t old_id)
{
    static const uint8_t map[20] = {
        0,  /* 旧 0 拟物指针钟（已删除）→ 数字大时钟 */
        0,  /* 旧 1 数字大时钟 → 数字大时钟 */
        1,  /* 旧 2 彩虹指针钟（已删除）→ 环形进度钟 */
        1,  /* 旧 3 环形进度钟 */
        2,  /* 旧 4 复古数码管 */
        3,  /* 旧 5 极简风格 */
        4,  /* 旧 6 拾音器·环形 */
        5,  /* 旧 7 拾音器·横排 */
        6,  /* 旧 8 拾音器·密集 */
        7,  /* 旧 9 图片 */
        8,  /* 旧 10 画板 */
        9,  /* 旧 11 示波器 */
        10, /* 旧 12 表情 */
        11, /* 旧 13 鱼缸 */
        12, /* 旧 14 俄罗斯方块 */
        13, /* 旧 15 贪吃蛇 */
        14, /* 旧 16 Flappy */
        15, /* 旧 17 坦克大战 */
        16, /* 旧 18 吃豆人 */
        17, /* 旧 19 飞机大战 */
    };
    if (old_id < 20) {
        return map[old_id];
    }
    return 0;
}

esp_err_t wifi_cfg_load(wifi_cfg_t *cfg)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }

    memset(cfg, 0, sizeof(*cfg));
    size_t len = sizeof(cfg->ssid);
    err = nvs_get_str(h, "ssid", cfg->ssid, &len);
    if (err != ESP_OK) {
        nvs_close(h);
        return err;
    }
    len = sizeof(cfg->pass);
    if (nvs_get_str(h, "pass", cfg->pass, &len) != ESP_OK) {
        cfg->pass[0] = 0;
    }
    uint8_t face = 0;
    if (nvs_get_u8(h, "face", &face) != ESP_OK) {
        face = 0;
    }
    /*
     * NVS 版本号：不存在（旧版固件写入的数据）时，表盘编号按旧表迁移；
     * 迁移只在内存进行，用户下一次保存配置后即以新编号写回并带版本号。
     */
    uint8_t fv = 0;
    nvs_get_u8(h, "fv", &fv);
    if (fv < 2) {
        face = face_migrate(face);
    }
    if (face >= FACE_COUNT) {
        face = 0;
    }
    cfg->face = face;

    uint8_t bright = 60;
    if (nvs_get_u8(h, "bright", &bright) != ESP_OK || bright < 5 || bright > 100) {
        bright = 60;
    }
    cfg->brightness = bright;

    uint8_t sens = 50;
    if (nvs_get_u8(h, "mic_sens", &sens) != ESP_OK || sens > 100) {
        sens = 50;
    }
    cfg->mic_sens = sens;
    nvs_close(h);
    return ESP_OK;
}

esp_err_t wifi_cfg_save(const wifi_cfg_t *cfg)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    nvs_set_str(h, "ssid", cfg->ssid);
    nvs_set_str(h, "pass", cfg->pass);
    nvs_set_u8(h, "face", cfg->face);
    nvs_set_u8(h, "fv", 2);   /* NVS 表盘编号版本（2 = 删除拟物/彩虹后） */
    nvs_set_u8(h, "bright", cfg->brightness);
    nvs_set_u8(h, "mic_sens", cfg->mic_sens);
    err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "配置已保存：SSID=%s 表盘=%d 亮度=%d", cfg->ssid, cfg->face, cfg->brightness);
    }
    return err;
}

/* ------------------------------------------------------------------ */
/* WiFi 状态                                                          */
/* ------------------------------------------------------------------ */

static SemaphoreHandle_t s_conn_sem;
static volatile bool s_connected = false;
static char s_sta_ip[16];

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_DISCONNECTED) {
            s_connected = false;
            ESP_LOGW(TAG, "WiFi 断开");
            if (s_conn_sem) {
                xSemaphoreGive(s_conn_sem);
            }
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        snprintf(s_sta_ip, sizeof(s_sta_ip), IPSTR, IP2STR(&ev->ip_info.ip));
        s_connected = true;
        ESP_LOGI(TAG, "已获得 IP: %s", s_sta_ip);
        if (s_conn_sem) {
            xSemaphoreGive(s_conn_sem);
        }
    }
}

esp_err_t wifi_prov_init(void)
{
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "esp_wifi_init 失败");

    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                            &wifi_event_handler, NULL, NULL),
                        TAG, "注册 WIFI_EVENT 失败");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                            &wifi_event_handler, NULL, NULL),
                        TAG, "注册 IP_EVENT 失败");

    s_conn_sem = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_conn_sem, ESP_ERR_NO_MEM, TAG, "创建信号量失败");

    ESP_LOGI(TAG, "WiFi 初始化完成");
    return ESP_OK;
}

bool wifi_prov_connect_sta(const wifi_cfg_t *cfg, uint32_t timeout_ms)
{
    wifi_config_t wcfg = {0};
    strncpy((char *)wcfg.sta.ssid, cfg->ssid, sizeof(wcfg.sta.ssid) - 1);
    strncpy((char *)wcfg.sta.password, cfg->pass, sizeof(wcfg.sta.password) - 1);
    /* 全 0 表示开放网络，否则要求 WPA2 以上 */
    wcfg.sta.threshold.authmode = strlen(cfg->pass) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wcfg);
    esp_err_t err = esp_wifi_start();
    if (err != ESP_OK && err != ESP_ERR_WIFI_STATE) {
        ESP_LOGE(TAG, "esp_wifi_start 失败: %d", err);
        return false;
    }

    /* 清掉残留信号后发起连接 */
    xSemaphoreTake(s_conn_sem, 0);
    s_connected = false;
    esp_wifi_connect();

    ESP_LOGI(TAG, "正在连接 WiFi: %s ...", cfg->ssid);
    if (xSemaphoreTake(s_conn_sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE && s_connected) {
        return true;
    }
    ESP_LOGW(TAG, "WiFi 连接超时或失败");
    return false;
}

esp_err_t wifi_prov_start_ap(void)
{
    esp_wifi_set_mode(WIFI_MODE_APSTA);

    wifi_config_t ap_cfg = {0};
    strncpy((char *)ap_cfg.ap.ssid, AP_SSID, sizeof(ap_cfg.ap.ssid) - 1);
    ap_cfg.ap.ssid_len = strlen(AP_SSID);
    strncpy((char *)ap_cfg.ap.password, AP_PASS, sizeof(ap_cfg.ap.password) - 1);
    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap_cfg.ap.channel = 6;

    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg), TAG, "配置 AP 失败");

    esp_err_t err = esp_wifi_start();
    if (err != ESP_OK && err != ESP_ERR_WIFI_STATE) {
        ESP_LOGE(TAG, "启动 AP 失败: %d", err);
        return err;
    }

    ESP_LOGI(TAG, "配网热点已开启：SSID=%s 密码=%s，浏览器访问 http://192.168.4.1",
             AP_SSID, AP_PASS);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* captive portal DNS：所有域名解析到 192.168.4.1                     */
/* ------------------------------------------------------------------ */

static void dns_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "DNS socket 创建失败");
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(53);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "DNS socket 绑定失败");
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "DNS 服务器已启动（captive portal）");

    uint8_t buf[512];
    uint8_t resp[600];
    while (1) {
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
        if (n < 12) {
            continue;
        }

        memcpy(resp, buf, (size_t)n);
        resp[2] = 0x81; /* QR=1, RD=1 */
        resp[3] = 0x80; /* RA=1 */
        resp[6] = 0x00;
        resp[7] = 0x01; /* ANCOUNT=1 */
        resp[8] = 0x00;
        resp[9] = 0x00;
        resp[10] = 0x00;
        resp[11] = 0x00;

        int off = n;
        resp[off++] = 0xC0; /* 指向问题域名的指针 */
        resp[off++] = 0x0C;
        resp[off++] = 0x00;
        resp[off++] = 0x01; /* TYPE A */
        resp[off++] = 0x00;
        resp[off++] = 0x01; /* CLASS IN */
        resp[off++] = 0x00;
        resp[off++] = 0x00;
        resp[off++] = 0x00;
        resp[off++] = 0x3C; /* TTL 60 */
        resp[off++] = 0x00;
        resp[off++] = 0x04; /* RDLENGTH 4 */
        resp[off++] = 192;
        resp[off++] = 168;
        resp[off++] = 4;
        resp[off++] = 1;

        sendto(sock, resp, (size_t)off, 0, (struct sockaddr *)&from, fl);
    }
}

esp_err_t wifi_prov_start_dns(void)
{
    BaseType_t ret = xTaskCreate(dns_task, "dns_srv", 8192, NULL, 3, NULL);
    ESP_RETURN_ON_FALSE(ret == pdPASS, ESP_ERR_NO_MEM, TAG, "创建 DNS 任务失败");
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* HTTP 服务器                                                         */
/* ------------------------------------------------------------------ */

static httpd_handle_t s_server = NULL;

/* base64 字符解码表 */
static int b64_val(char c)
{
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '+') {
        return 62;
    }
    if (c == '/') {
        return 63;
    }
    return -1;
}

/* 轻量 base64 解码，返回解码字节数 */
static int base64_decode(const char *src, uint8_t *dst, size_t dst_size)
{
    size_t o = 0;
    uint32_t acc = 0;
    int bits = 0;

    for (const char *p = src; *p; p++) {
        if (*p == '=') {
            break;
        }
        int v = b64_val(*p);
        if (v < 0) {
            continue; /* 跳过空白等 */
        }
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o < dst_size) {
                dst[o++] = (uint8_t)(acc >> bits);
            }
        }
    }
    return (int)o;
}

static int recv_body(httpd_req_t *req, char *buf, int max)
{
    int total = 0;
    while (total < max - 1) {
        int n = httpd_req_recv(req, buf + total, (size_t)(max - 1 - total));
        if (n <= 0) {
            if (n == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            break;
        }
        total += n;
    }
    buf[total] = 0;
    return total;
}

/* GET / ：设置页面 */
static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr(req, WEB_INDEX_HTML);
    return ESP_OK;
}

/* GET /scan ：扫描附近 WiFi，返回 JSON */
static esp_err_t scan_handler(httpd_req_t *req)
{
    wifi_scan_config_t sc = {0};
    esp_wifi_scan_start(&sc, true);

    uint16_t count = 0;
    esp_wifi_scan_get_ap_num(&count);
    if (count > 30) {
        count = 30;
    }
    if (count == 0) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "[]");
        return ESP_OK;
    }

    wifi_ap_record_t *recs = calloc(count, sizeof(wifi_ap_record_t));
    if (!recs) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem");
        return ESP_FAIL;
    }
    esp_wifi_scan_get_ap_records(&count, recs);

    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < count; i++) {
        if (recs[i].ssid[0] == 0) {
            continue;
        }
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "ssid", (const char *)recs[i].ssid);
        cJSON_AddNumberToObject(o, "rssi", recs[i].rssi);
        cJSON_AddNumberToObject(o, "auth", recs[i].authmode);
        cJSON_AddItemToArray(arr, o);
    }
    free(recs);

    char *json = cJSON_PrintUnformatted(arr);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json ? json : "[]");
    if (json) {
        free(json);
    }
    cJSON_Delete(arr);
    return ESP_OK;
}

/* POST /save ：保存 WiFi 与表盘选择，然后重启 */
/* GET /config ：返回当前配置（不包含密码） */
static esp_err_t config_handler(httpd_req_t *req)
{
    wifi_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    wifi_cfg_load(&cfg);

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "ssid", cfg.ssid);
    cJSON_AddNumberToObject(o, "face", cfg.face);
    cJSON_AddNumberToObject(o, "brightness", cfg.brightness);
    cJSON_AddNumberToObject(o, "mic_sens", cfg.mic_sens);
    char *json = cJSON_PrintUnformatted(o);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json ? json : "{}");
    if (json) {
        free(json);
    }
    cJSON_Delete(o);
    return ESP_OK;
}

/* POST /save ：保存 WiFi/表盘/亮度；配网模式保存后重启，时钟模式立即生效 */
static esp_err_t save_handler(httpd_req_t *req)
{
    /* 画板图片 base64 约 11KB，需要较大的请求缓冲 */
    static char body[16384];
    recv_body(req, body, sizeof(body));

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
        return ESP_OK;
    }

    /* 文字请求：只处理滚动字幕，不要求 WiFi 配置 */
    cJSON *j_text = cJSON_GetObjectItem(root, "text");
    if (cJSON_IsString(j_text)) {
        if (j_text->valuestring[0]) {
            text_set(j_text->valuestring);
            g_text_mode = true;
            ESP_LOGI(TAG, "滚动字幕：%s", j_text->valuestring);
        } else {
            g_text_mode = false;
            ESP_LOGI(TAG, "停止滚动字幕");
        }
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":true}");
        cJSON_Delete(root);
        return ESP_OK;
    }

    /* 画板图片请求：base64 编码的 64x64 RGB444（8192 字节） */
    cJSON *j_img = cJSON_GetObjectItem(root, "image");
    if (cJSON_IsString(j_img) && j_img->valuestring[0]) {
        static uint8_t img_buf[8192];
        int out_len = base64_decode(j_img->valuestring, img_buf, sizeof(img_buf));
        bool ok = (out_len == (int)sizeof(g_custom_image));
        if (ok) {
            for (int i = 0; i < 64 * 64; i++) {
                g_custom_image[i] = (uint16_t)((img_buf[i * 2] << 8) | img_buf[i * 2 + 1]);
            }
            g_face_id = FACE_CUSTOM;
            g_text_mode = false;
            ESP_LOGI(TAG, "画板图片已更新（%dx%d）", 64, 64);
        }
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, ok ? "{\"ok\":true}" : "{\"ok\":false}");
        cJSON_Delete(root);
        return ESP_OK;
    }

    cJSON *j_ssid = cJSON_GetObjectItem(root, "ssid");
    cJSON *j_pass = cJSON_GetObjectItem(root, "pass");
    cJSON *j_face = cJSON_GetObjectItem(root, "face");
    cJSON *j_bright = cJSON_GetObjectItem(root, "brightness");
    cJSON *j_sens = cJSON_GetObjectItem(root, "mic_sens");

    wifi_cfg_t cfg = {0};
    bool ok = false;

    if (cJSON_IsString(j_ssid) && j_ssid->valuestring[0]) {
        strncpy(cfg.ssid, j_ssid->valuestring, sizeof(cfg.ssid) - 1);

        if (cJSON_IsString(j_pass) && j_pass->valuestring[0]) {
            strncpy(cfg.pass, j_pass->valuestring, sizeof(cfg.pass) - 1);
        } else {
            /* 密码留空：沿用已保存的密码 */
            wifi_cfg_t old;
            if (wifi_cfg_load(&old) == ESP_OK && strcmp(old.ssid, cfg.ssid) == 0) {
                strncpy(cfg.pass, old.pass, sizeof(cfg.pass) - 1);
            }
        }

        cfg.face = cJSON_IsNumber(j_face) ? (uint8_t)j_face->valueint : 0;
        if (cfg.face >= FACE_COUNT) {
            cfg.face = 0;
        }
        cfg.brightness = cJSON_IsNumber(j_bright) ? (uint8_t)j_bright->valueint : 60;
        if (cfg.brightness < 5) {
            cfg.brightness = 5;
        }
        if (cfg.brightness > 100) {
            cfg.brightness = 100;
        }
        cfg.mic_sens = cJSON_IsNumber(j_sens) ? (uint8_t)j_sens->valueint : 50;
        if (cfg.mic_sens > 100) {
            cfg.mic_sens = 100;
        }
        ok = (wifi_cfg_save(&cfg) == ESP_OK);
    }
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, ok ? "{\"ok\":true}" : "{\"ok\":false}");

    if (ok) {
        /* 表盘、亮度、拾音灵敏度立即生效 */
        g_text_mode = false;
        g_face_id = cfg.face;
        hub75_set_brightness(cfg.brightness);
        mic_set_sensitivity(cfg.mic_sens);

        if (g_provisioning) {
            ESP_LOGI(TAG, "配网配置完成，准备重启进入联网模式...");
            vTaskDelay(pdMS_TO_TICKS(800));
            esp_restart();
        } else {
            ESP_LOGI(TAG, "管理设置已更新并立即生效");
        }
    }
    return ESP_OK;
}


/* captive portal 探测路径：跳转到设置页 */
static esp_err_t captive_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_sendstr(req, "redirect");
    return ESP_OK;
}

static esp_err_t favicon_handler(httpd_req_t *req)
{
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no favicon");
    return ESP_OK;
}

esp_err_t wifi_prov_start_web(void)
{
    if (s_server) {
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 12;
    config.lru_purge_enable = true;

    ESP_RETURN_ON_ERROR(httpd_start(&s_server, &config), TAG, "启动 HTTP 服务器失败");

    static const httpd_uri_t uri_index = {
        .uri = "/", .method = HTTP_GET, .handler = index_handler, .user_ctx = NULL,
    };
    static const httpd_uri_t uri_scan = {
        .uri = "/scan", .method = HTTP_GET, .handler = scan_handler, .user_ctx = NULL,
    };
    static const httpd_uri_t uri_save = {
        .uri = "/save", .method = HTTP_POST, .handler = save_handler, .user_ctx = NULL,
    };
    static const httpd_uri_t uri_config = {
        .uri = "/config", .method = HTTP_GET, .handler = config_handler, .user_ctx = NULL,
    };
    static const httpd_uri_t uri_captive1 = {
        .uri = "/generate_204", .method = HTTP_GET, .handler = captive_handler, .user_ctx = NULL,
    };
    static const httpd_uri_t uri_captive2 = {
        .uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = captive_handler, .user_ctx = NULL,
    };
    static const httpd_uri_t uri_favicon = {
        .uri = "/favicon.ico", .method = HTTP_GET, .handler = favicon_handler, .user_ctx = NULL,
    };

    httpd_register_uri_handler(s_server, &uri_index);
    httpd_register_uri_handler(s_server, &uri_scan);
    httpd_register_uri_handler(s_server, &uri_save);
    httpd_register_uri_handler(s_server, &uri_config);
    httpd_register_uri_handler(s_server, &uri_captive1);
    httpd_register_uri_handler(s_server, &uri_captive2);
    httpd_register_uri_handler(s_server, &uri_favicon);

    ESP_LOGI(TAG, "设置网页已启动（http://192.168.4.1 或设备 IP）");
    return ESP_OK;
}

void wifi_prov_restart(void)
{
    esp_restart();
}

const char *wifi_prov_sta_ip(void)
{
    return s_connected ? s_sta_ip : NULL;
}
