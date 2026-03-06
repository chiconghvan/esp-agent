/**
 * ===========================================================================
 * @file wifi_manager.c
 * @brief Triển khai quản lý WiFi thông minh (STA + Captive Portal)
 *
 * - Khởi động: Đọc NVS. Nếu có SSID/Pass → chế độ STA.
 * - Nếu không có hoặc STA thất bại liên tục → chuyển chế độ AP (Captive Portal).
 * - Cung cấp HTTP Server cổng 80 và DNS Server cổng 53 để định tuyến cấu hình.
 * ===========================================================================
 */

#include "wifi_manager.h"
#include "config.h"
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_http_server.h"
#include "esp_mac.h"
#include "nvs.h"
#include "lwip/sockets.h"
#include "lwip/ip_addr.h"

static const char *TAG = "wifi_manager";

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1

static EventGroupHandle_t wifi_event_group = NULL;
static int retry_count = 0;
static bool is_connected = false;

/* --------------------------------------------------------------------------
 * DNS Server siêu cấp (Luôn trỏ về AP IP cho mọi truy vấn)
 * -------------------------------------------------------------------------- */
static void dns_server_task(void *pvParameters)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        vTaskDelete(NULL);
        return;
    }
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(53);

    bind(sock, (struct sockaddr *)&server_addr, sizeof(server_addr));
    char rx_buffer[128];

    while (1) {
        struct sockaddr_in source_addr;
        socklen_t socklen = sizeof(source_addr);
        int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer), 0, (struct sockaddr *)&source_addr, &socklen);
        if (len > 0) {
            char tx_buffer[128];
            memcpy(tx_buffer, rx_buffer, len);
            
            /* Sửa cờ Flags để biến truy vấn thành phản hồi (QR=1) */
            tx_buffer[2] |= 0x80; 
            
            /* Gắn thêm 1 Answer Record */
            tx_buffer[7] = 1; 

            int idx = len;
            /* Tên miền được trỏ lại chính nó (sử dụng pointer) */
            tx_buffer[idx++] = 0xC0;
            tx_buffer[idx++] = 0x0C;
            
            /* Type A, Class IN, TTL 300, Len 4, IP 192.168.4.1 */
            char answer[] = {0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x01, 0x2C, 0x00, 0x04, 192, 168, 4, 1};
            memcpy(tx_buffer + idx, answer, 14);
            idx += 14;

            sendto(sock, tx_buffer, idx, 0, (struct sockaddr *)&source_addr, sizeof(source_addr));
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* --------------------------------------------------------------------------
 * HTTP Server (Trang cấu hình WiFi)
 * -------------------------------------------------------------------------- */
static const char *index_html = 
    "<!DOCTYPE html><html><head><meta charset=\"UTF-8\">"
    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
    "<title>ESP Cấu Hình</title>\n"
    "<style>body{font-family:sans-serif;background:#f0f2f5;display:flex;justify-content:center;align-items:center;height:100vh;margin:0;} "
    ".c{background:#fff;padding:30px;border-radius:10px;box-shadow:0 4px 6px rgba(0,0,0,0.1);max-width:400px;width:100%;text-align:center;} "
    "input{width:90%;padding:10px;margin:10px 0;border-radius:5px;border:1px solid #ddd;} "
    "button{background:#007bff;color:#fff;border:none;padding:12px 20px;border-radius:5px;cursor:pointer;width:100%;font-size:16px;} "
    "button:hover{background:#0056b3;}</style></head>"
    "<body><div class=\"c\"><h2>Cấu hình WiFi ESP-Agent</h2>"
    "<form action=\"/save\" method=\"POST\">"
    "<input type=\"text\" name=\"ssid\" placeholder=\"Tên WiFi\" required><br>"
    "<input type=\"password\" name=\"pass\" placeholder=\"Mật khẩu\"><br>"
    "<button type=\"submit\">Lưu và Khởi động lại</button>"
    "</form></div></body></html>";

static esp_err_t index_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, index_html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t save_post_handler(httpd_req_t *req)
{
    char buf[128];
    int len = req->content_len;
    if (len >= sizeof(buf)) len = sizeof(buf) - 1;
    httpd_req_recv(req, buf, len);
    buf[len] = '\0';

    char *ssid_ptr = strstr(buf, "ssid=");
    char *pass_ptr = strstr(buf, "pass=");
    if (ssid_ptr) {
        char ssid[64] = {0}, pass[64] = {0};
        ssid_ptr += 5;
        char *end_ssid = strchr(ssid_ptr, '&');
        if (!end_ssid) end_ssid = buf + len;
        strncpy(ssid, ssid_ptr, end_ssid - ssid_ptr);

        if (pass_ptr) {
            pass_ptr += 5;
            char *end_pass = strchr(pass_ptr, '&');
            if (!end_pass) end_pass = buf + len;
            strncpy(pass, pass_ptr, end_pass - pass_ptr);
        }

        /* Decode URL (thay dấu + thành space, ...) - Tối giản cho ESP*/
        for(int i=0; ssid[i]; i++) if(ssid[i]=='+') ssid[i]=' ';
        for(int i=0; pass[i]; i++) if(pass[i]=='+') pass[i]=' ';

        ESP_LOGI(TAG, "Nhận WiFi config thay đổi: %s", ssid);

        nvs_handle_t h;
        if (nvs_open("wifi_cfg", NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_str(h, "ssid", ssid);
            nvs_set_str(h, "pass", pass);
            nvs_commit(h);
            nvs_close(h);
        }

        httpd_resp_send(req, "Đã lưu cài đặt. Thiết bị đang khởi động lại...", HTTPD_RESP_USE_STRLEN);
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    }
    return ESP_OK;
}

static void start_captive_portal(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_open_sockets = 3;
    config.lru_purge_enable = true;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t index_uri = { .uri = "/", .method = HTTP_GET, .handler = index_get_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &index_uri);
        
        httpd_uri_t generate_204 = { .uri = "/generate_204", .method = HTTP_GET, .handler = index_get_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &generate_204);

        httpd_uri_t save_uri = { .uri = "/save", .method = HTTP_POST, .handler = save_post_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &save_uri);
    }
    
    xTaskCreate(dns_server_task, "dns_server", 2048, NULL, 5, NULL);
    ESP_LOGI(TAG, "Đã kích hoạt Captive Portal ở 192.168.4.1");
}

/* --------------------------------------------------------------------------
 * STA WiFi Event Handler
 * -------------------------------------------------------------------------- */
static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        is_connected = false;
        if (retry_count < WIFI_MAX_RETRY) {
            retry_count++;
            ESP_LOGW(TAG, "Mất kết nối WiFi, thử lại %d/%d...", retry_count, WIFI_MAX_RETRY);
            esp_wifi_connect();
        } else {
            ESP_LOGE(TAG, "Không thể kết nối WiFi. Kích hoạt AP Mode.");
            xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);
        }
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        is_connected = true;
        retry_count = 0;
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

/* --------------------------------------------------------------------------
 * Core Init
 * -------------------------------------------------------------------------- */
esp_err_t wifi_manager_init(void)
{
    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* Đọc NVS */
    char ssid[64] = {0};
    char pass[64] = {0};
    bool has_config = false;

    nvs_handle_t h;
    if (nvs_open("wifi_cfg", NVS_READONLY, &h) == ESP_OK) {
        size_t s_len = sizeof(ssid);
        if (nvs_get_str(h, "ssid", ssid, &s_len) == ESP_OK && strlen(ssid) > 0) has_config = true;
        size_t p_len = sizeof(pass);
        nvs_get_str(h, "pass", pass, &p_len);
        nvs_close(h);
    }

    if (!has_config) {
        /* Lấy fallback từ config.h nếu có */
        if (strlen(WIFI_SSID) > 0) {
            strncpy(ssid, WIFI_SSID, sizeof(ssid));
            strncpy(pass, WIFI_PASSWORD, sizeof(pass));
            has_config = true;
        }
    }

    esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    if (has_config) {
        ESP_LOGI(TAG, "Thử kết nối trạm (STA) với %s", ssid);
        ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

        wifi_config_t wifi_config = {0};
        strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
        strncpy((char *)wifi_config.sta.password, pass, sizeof(wifi_config.sta.password));

        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
        ESP_ERROR_CHECK(esp_wifi_start());

        EventBits_t bits = xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, portMAX_DELAY);

        if (bits & WIFI_CONNECTED_BIT) {
            ESP_LOGI(TAG, "Kết nối WiFi thành công!");
            return ESP_OK;
        }
    }

    /* Rơi vào chế độ cấu hình Captive Portal (nếu Fail/Không có WiFi) */
    ESP_LOGI(TAG, "Cấu hình WiFi AP Mode...");
    ESP_ERROR_CHECK(esp_wifi_stop());
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    char ap_ssid[32];
    snprintf(ap_ssid, sizeof(ap_ssid), "ESP-Agent-%02X%02X", mac[4], mac[5]);

    wifi_config_t ap_config = {
        .ap = {
            .max_connection = 4,
            .authmode = WIFI_AUTH_OPEN
        },
    };
    strncpy((char *)ap_config.ap.ssid, ap_ssid, sizeof(ap_config.ap.ssid));
    
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "Broadcast AP: %s (Open)", ap_ssid);

    start_captive_portal();

    /* Bị kẹt ở đây cho đến khi user nhập form => esp_restart() */
    while (1) vTaskDelay(pdMS_TO_TICKS(1000));

    return ESP_FAIL;
}

/* --------------------------------------------------------------------------
 * SNTP callback
 * -------------------------------------------------------------------------- */
static void sntp_sync_notification(struct timeval *tv) {
    ESP_LOGI(TAG, "Đã đồng bộ SNTP");
}

esp_err_t wifi_manager_start_sntp(void)
{
    setenv("TZ", TIMEZONE_POSIX, 1);
    tzset();

    if (esp_sntp_enabled()) esp_sntp_stop();

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, SNTP_SERVER);
    sntp_set_time_sync_notification_cb(sntp_sync_notification);
    esp_sntp_init();

    /* Đợi tối đa 1 giây thôi (cần tốc độ khởi động). 
       Việc đồng bộ chính xác sẽ được bổ sung qua Telegram HTTP Date Header sau đó. */
    int retry = 0;
    while (sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED && retry < 1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        retry++;
    }
    return (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) ? ESP_OK : ESP_ERR_TIMEOUT;
}

bool wifi_manager_is_connected(void) {
    return is_connected;
}
