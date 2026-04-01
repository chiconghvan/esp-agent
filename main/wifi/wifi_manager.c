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
#include "esp_http_server.h"
#include "esp_mac.h"
#include "display_manager.h"
#include "config.h"
#include "esp_sntp.h"
#include "nvs.h"
#include "lwip/sockets.h"

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
    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">"
    "<title>ESP-Agent WiFi Setup</title>"
    "<style>body{font-family:sans-serif;margin:0;padding:20px;background:#f0f2f5;} "
    ".c{max-width:400px;margin:0 auto;background:#fff;padding:20px;border-radius:8px;box-shadow:0 2px 4px rgba(0,0,0,0.1);}"
    "h2{text-align:center;color:#333;} "
    "ul{list-style-type:none;padding:0;margin:0;max-height:300px;overflow-y:auto;}"
    "li{padding:15px;border-bottom:1px solid #ddd;cursor:pointer;display:flex;justify-content:space-between;}"
    "li:hover{background:#f9f9f9;}"
    ".msg{text-align:center;font-weight:bold;margin-top:10px;color:#d9534f;}"
    ".loader{text-align:center;margin:20px 0;color:#007bff;}"
    "#modal{display:none;margin-top:10px;padding:15px;background:#f8f9fa;border-radius:8px;border:1px solid #ddd;}"
    "input{width:90%;padding:10px;margin-bottom:10px;border:1px solid #ccc;border-radius:4px;}"
    "button{width:100%;padding:10px;background:#28a745;color:#fff;border:none;border-radius:4px;font-size:16px;cursor:pointer;}"
    "</style></head>"
    "<body><div class=\"c\"><h2>Chọn Mạng WiFi</h2>"
    "<div id=\"list\"><div class=\"loader\">Đang quét mạng...</div></div>"
    "<div id=\"modal\">"
    "  <p>Nhập mật khẩu cho <b id=\"m-ssid\"></b>:</p>"
    "  <input type=\"password\" id=\"pwd\" placeholder=\"Mật khẩu\">"
    "  <button onclick=\"doConn()\">Kết nối</button>"
    "  <button onclick=\"cModal()\" style=\"background:#6c757d;margin-top:10px;\">Huỷ</button>"
    "</div>"
    "<div id=\"msg\" class=\"msg\"></div>"
    "</div>"
    "<script>"
    "let selSsid='';"
    "function showMsg(m, c){document.getElementById('msg').innerHTML=m;document.getElementById('msg').style.color=c;}"
    "function load(){"
    "  fetch('/scan').then(r=>r.json()).then(data=>{"
    "    let html='<ul>';"
    "    data.forEach(w=>{"
    "      html+=`<li onclick=\"conn('${w.ssid}')\"><span>${w.ssid}</span><b>${w.rssi}dBm</b></li>`;"
    "    });"
    "    html+='</ul>';"
    "    document.getElementById('list').innerHTML=html;"
    "  }).catch(e=>{showMsg('Lỗi quét WiFi','#d9534f');});"
    "}"
    "function conn(ssid){"
    "  selSsid=ssid; document.getElementById('m-ssid').innerText=ssid;"
    "  document.getElementById('list').style.display='none';"
    "  document.getElementById('modal').style.display='block';"
    "  document.getElementById('pwd').value='';"
    "  showMsg('','');"
    "}"
    "function cModal(){"
    "  document.getElementById('list').style.display='block';"
    "  document.getElementById('modal').style.display='none';"
    "}"
    "function doConn(){"
    "  let pass=document.getElementById('pwd').value;"
    "  document.getElementById('modal').style.display='none';"
    "  document.getElementById('list').style.display='block';"
    "  document.getElementById('list').innerHTML='<div class=\"loader\">Đang kết nối. Vui lòng đợi 15s...</div>';"
    "  showMsg('','');"
    "  fetch('/save',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:`ssid=${encodeURIComponent(selSsid)}&pass=${encodeURIComponent(pass)}`})"
    "  .then(r=>r.text()).then(r=>{"
    "    if(r==='OK'){showMsg('Kết nối thành công! Khởi động lại...','#28a745');}"
    "    else{showMsg('Sai mật khẩu hoặc lỗi kết nối.','#d9534f');load();}"
    "  });"
    "}"
    "window.onload=load;"
    "</script>"
    "</body></html>";


static esp_err_t index_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, index_html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t scan_get_handler(httpd_req_t *req)
{
    uint16_t number = 15;
    wifi_ap_record_t *ap_info = (wifi_ap_record_t *)calloc(15, sizeof(wifi_ap_record_t));
    if (!ap_info) {
        return httpd_resp_send_500(req);
    }
    uint16_t ap_count = 0;
    
    wifi_scan_config_t scan_config = {
        .ssid = 0,
        .bssid = 0,
        .channel = 0,
        .show_hidden = false
    };

    esp_wifi_scan_start(&scan_config, true);
    esp_wifi_scan_get_ap_records(&number, ap_info);
    esp_wifi_scan_get_ap_num(&ap_count);

    char *resp = (char *)calloc(1, 2048);
    if (!resp) {
        free(ap_info);
        return httpd_resp_send_500(req);
    }
    
    strcpy(resp, "[");
    for (int i = 0; i < number; i++) {
        char buf[128];
        snprintf(buf, sizeof(buf), "{\"ssid\":\"%s\",\"rssi\":%d}%s", ap_info[i].ssid, ap_info[i].rssi, (i==number-1)?"":",");
        strcat(resp, buf);
    }
    strcat(resp, "]");

    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_send(req, resp, strlen(resp));
    
    free(ap_info);
    free(resp);
    return err;
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
        
        /* Decode URL characters like %20, %21, etc. */
        char* process_decode_ptr = ssid;
        char* process_buf = ssid;
        while (*process_decode_ptr) {
            if (*process_decode_ptr == '%') {
                if (process_decode_ptr[1] && process_decode_ptr[2]) {
                    int c;
                    sscanf(process_decode_ptr + 1, "%2x", &c);
                    *process_buf++ = (char) c;
                    process_decode_ptr += 3;
                } else break;
            } else if (*process_decode_ptr == '+') {
                *process_buf++ = ' ';
                process_decode_ptr++;
            } else {
                *process_buf++ = *process_decode_ptr++;
            }
        }
        *process_buf = '\0';
        
        process_decode_ptr = pass;
        process_buf = pass;
        while (*process_decode_ptr) {
            if (*process_decode_ptr == '%') {
                if (process_decode_ptr[1] && process_decode_ptr[2]) {
                    int c;
                    sscanf(process_decode_ptr + 1, "%2x", &c);
                    *process_buf++ = (char) c;
                    process_decode_ptr += 3;
                } else break;
            } else if (*process_decode_ptr == '+') {
                *process_buf++ = ' ';
                process_decode_ptr++;
            } else {
                *process_buf++ = *process_decode_ptr++;
            }
        }
        *process_buf = '\0';

                /* Thử kết nối */
        wifi_config_t test_config = {0};
        strncpy((char *)test_config.sta.ssid, ssid, sizeof(test_config.sta.ssid));
        strncpy((char *)test_config.sta.password, pass, sizeof(test_config.sta.password));
        
        ESP_LOGI(TAG, "Đang thử kết nối WiFi: %s", ssid);
        
        esp_wifi_disconnect();
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &test_config));
        
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
        retry_count = 0; /* Cho phép bắt đầu tự động retry trong event handler */
        esp_wifi_connect();
        
        /* Chờ kết nối 15s */
        EventBits_t bits = xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(15000));
        
        if (bits & WIFI_CONNECTED_BIT) {
            ESP_LOGI(TAG, "Kết nối WiFi test thành công! Lưu vào NVS...");
            nvs_handle_t h;
            if (nvs_open("wifi_cfg", NVS_READWRITE, &h) == ESP_OK) {
                nvs_set_str(h, "ssid", ssid);
                nvs_set_str(h, "pass", pass);
                nvs_commit(h);
                nvs_close(h);
            }
            httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_restart();
        } else {
            ESP_LOGW(TAG, "Kết nối WiFi test thất bại!");
            httpd_resp_send(req, "FAIL", HTTPD_RESP_USE_STRLEN);
            esp_wifi_disconnect();
        }
    }
    return ESP_OK;
}

static esp_err_t http_404_error_handler(httpd_req_t *req, httpd_err_code_t err)
{
    /* Bắt mọi Request 404 và ép văng "302 Redirect" về màn hình Captive Portal */
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static void start_captive_portal(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_open_sockets = 3;
    config.lru_purge_enable = true;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) == ESP_OK) {
        /* Bắt buộc trỏ mọi URL lạ vế màn hình cài đặt để hiện Pop-up trên điện thoại */
        httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, http_404_error_handler);
        
        httpd_uri_t index_uri = { .uri = "/", .method = HTTP_GET, .handler = index_get_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &index_uri);
        
        httpd_uri_t apple_uri = { .uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = index_get_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &apple_uri);
        
        httpd_uri_t android_uri = { .uri = "/generate_204", .method = HTTP_GET, .handler = index_get_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &android_uri);


        httpd_uri_t scan_uri = { .uri = "/scan", .method = HTTP_GET, .handler = scan_get_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &scan_uri);

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
        
        /* If we are in APSTA mode, we are in the captive portal test connection logic */
        wifi_mode_t current_mode;
        esp_wifi_get_mode(&current_mode);
        if (current_mode == WIFI_MODE_APSTA) {
            ESP_LOGW(TAG, "Test kết nối wifi đang xử lý (nhận event disconnected)");
            if (retry_count < 15) {
                retry_count++;
                esp_wifi_connect(); /* Thử lại auto-reconnect */
            } else {
                xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);
            }
            return;
        }

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

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    if (has_config) {
        ESP_LOGI(TAG, "Thử kết nối trạm (STA) với %s", ssid);

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
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

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

    /* Đợi tối đa 10 giây cho SNTP phản hồi */
    int retry = 0;
    char oled_msg[32];
    while (time(NULL) < 1704067200 && retry < 10) {
        retry++;
        ESP_LOGI(TAG, "Đang chờ SNTP... %d/10", retry);
        snprintf(oled_msg, sizeof(oled_msg), "Dong bo SNTP %d/10", retry);
        display_boot_progress(15, oled_msg);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    
    if (time(NULL) >= 1704067200) {
        ESP_LOGI(TAG, "Đồng bộ SNTP: OK");
        display_boot_progress(25, "Dong bo Thoi gian: OK");
        return ESP_OK;
    }
    
    return ESP_ERR_TIMEOUT;
}

bool wifi_manager_is_connected(void) {
    return is_connected;
}

int wifi_manager_get_level(void) {
    if (!is_connected) return 0;
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        if (ap.rssi > -60) return 3;
        if (ap.rssi > -75) return 2;
        return 1;
    }
    return 0;
}
