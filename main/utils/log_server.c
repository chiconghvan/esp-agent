/**
 * ===========================================================================
 * @file log_server.c
 * @brief Web Log Viewer: Ring buffer + HTTP server
 *
 * Hook esp_log vào ring buffer 50 dòng.
 * Phục vụ trang web tĩnh tại cổng 8080.
 * ===========================================================================
 */

#include "log_server.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#include "mdns.h"

/* ──── Cấu hình ──── */
#define LOG_MAX_LINES       50
#define LOG_MAX_LINE_LEN    200
#define LOG_SERVER_PORT     80
#define MDNS_HOSTNAME       "esp-agent"

static const char *TAG = "log_server";

/* ──── Ring buffer ──── */
static char s_log_ring[LOG_MAX_LINES][LOG_MAX_LINE_LEN];
static int  s_log_head  = 0;   /* Vị trí ghi tiếp theo  */
static int  s_log_count = 0;   /* Số dòng đã lưu        */
static uint32_t s_global_log_counter = 0; /* Tổng số log đã sinh ra */

/* ──── Original vprintf function ──── */
static vprintf_like_t s_original_vprintf = NULL;

/* --------------------------------------------------------------------------
 * Custom vprintf: ghi log vào ring buffer + xuất ra UART
 * -------------------------------------------------------------------------- */
static int log_vprintf_hook(const char *fmt, va_list args)
{
    /* 1) Vẫn in ra UART như bình thường */
    int ret = 0;
    if (s_original_vprintf) {
        va_list args_copy;
        va_copy(args_copy, args);
        ret = s_original_vprintf(fmt, args_copy);
        va_end(args_copy);
    }

    /* 2) Format và ghi vào ring buffer */
    char *line = s_log_ring[s_log_head];
    vsnprintf(line, LOG_MAX_LINE_LEN, fmt, args);

    /* Loại bỏ ký tự xuống dòng cuối */
    int len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
        line[--len] = '\0';
    }

    /* Bỏ qua dòng rỗng */
    if (len == 0) return ret;

    s_log_head = (s_log_head + 1) % LOG_MAX_LINES;
    if (s_log_count < LOG_MAX_LINES) s_log_count++;
    s_global_log_counter++;

    return ret;
}

/* --------------------------------------------------------------------------
 * HTML tĩnh: Giao diện log (nền đen, monospace, auto-scroll)
 * -------------------------------------------------------------------------- */
static const char *LOG_HTML =
    "<!DOCTYPE html><html><head>"
    "<meta charset=\"UTF-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>ESP-Agent Log</title>"
    "<style>"
    "*{margin:0;padding:0;box-sizing:border-box}"
    "body{background:#0d1117;color:#c9d1d9;font:12px/1.5 'Courier New',monospace;padding:8px}"
    "h3{color:#58a6ff;margin-bottom:6px;font-size:14px}"
    "#log{background:#161b22;border:1px solid #30363d;border-radius:6px;"
    "padding:10px;height:calc(100vh - 60px);overflow-y:auto;white-space:pre-wrap;word-break:break-all}"
    "#log span{display:block;padding:2px 0;border-bottom:1px solid #21262d}"
    "#log span:hover{background:#1f242c}"
    ".W{color:#d29922}.E{color:#f85149}.I{color:#c9d1d9}.D{color:#8b949e}"
    "</style></head><body>"
    "<h3>&#128225; ESP-Agent Log <span id=\"st\" style=\"color:#8b949e;font-weight:normal\"></span></h3>"
    "<div id=\"log\">Loading...</div>"
    "<script>"
    "var d=document.getElementById('log'),st=document.getElementById('st'),c=0;"
    "function poll(){"
    "fetch('/api/log?c='+c).then(r=>{if(!r.ok)throw new Error(r.status);return r.json()}).then(data=>{"
    "if(data.l.length>0){"
    "var isAtBottom=(d.scrollHeight-d.scrollTop<=d.clientHeight+50);"
    "if(c===0||data.c-c>50)d.innerHTML='';"
    "var h='';data.l.forEach(l=>{"
    "var k='I';if(l.indexOf('W (')>-1||l.indexOf('W:')>-1)k='W';else if(l.indexOf('E (')>-1||l.indexOf('E:')>-1)k='E';else if(l.indexOf('D (')>-1||l.indexOf('D:')>-1)k='D';"
    "h+='<span class=\"'+k+'\">'+l.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;')+'</span>';"
    "});"
    "d.insertAdjacentHTML('beforeend',h);"
    "while(d.childElementCount>100)d.removeChild(d.firstChild);"
    "if(isAtBottom)d.scrollTop=d.scrollHeight;"
    "c=data.c;"
    "}"
    "st.textContent='('+c+' lines)';"
    "}).catch(e=>{st.textContent='(offline)';console.error('Log fetch error:',e);});"
    "}"
    "poll();setInterval(poll,2000);"
    "</script></body></html>";

/* --------------------------------------------------------------------------
 * HTTP Handlers
 * -------------------------------------------------------------------------- */
static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/log");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t log_page_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, LOG_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t log_api_handler(httpd_req_t *req)
{
    /* Parse query string ?c=... để lấy cursor */
    char query[32];
    uint32_t cursor = 0;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char param[16];
        if (httpd_query_key_value(query, "c", param, sizeof(param)) == ESP_OK) {
            cursor = (uint32_t)atoi(param);
        }
    }

    /* Thiết lập Header Chunked Transfer */
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    /* Tính toán số log cần gửi (chỉ gửi log mới tính từ cursor) */
    uint32_t oldest_cursor = s_global_log_counter - s_log_count;
    if (cursor < oldest_cursor || cursor > s_global_log_counter) {
        cursor = oldest_cursor; /* Client bị bỏ lại quá xa, gửi lại từ đầu buffer */
    }

    int send_count = s_global_log_counter - cursor;

    /* Bắt đầu JSON object {"c": <counter>, "l": [...arrays]} */
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"c\":%" PRIu32 ",\"l\":[", s_global_log_counter);
    httpd_resp_send_chunk(req, buf, strlen(buf));

    int ring_start = (s_log_head - s_log_count + LOG_MAX_LINES) % LOG_MAX_LINES;
    int start_i = s_log_count - send_count;

    for (int i = 0; i < send_count; i++) {
        int idx = (ring_start + start_i + i) % LOG_MAX_LINES;
        
        /* Build từng item JSON an toàn */
        char item_buf[LOG_MAX_LINE_LEN * 2 + 8]; /* Buffer dư để escape */
        int offset = 0;
        
        if (i > 0) item_buf[offset++] = ',';
        item_buf[offset++] = '"';
        
        const char *src = s_log_ring[idx];
        while (*src && offset < (int)sizeof(item_buf) - 5) {
            if (*src == '"') { item_buf[offset++] = '\\'; item_buf[offset++] = '"'; }
            else if (*src == '\\') { item_buf[offset++] = '\\'; item_buf[offset++] = '\\'; }
            else if (*src == '\n') { item_buf[offset++] = '\\'; item_buf[offset++] = 'n'; }
            else if (*src == '\r') { item_buf[offset++] = '\\'; item_buf[offset++] = 'r'; }
            else if (*src == '\t') { item_buf[offset++] = '\\'; item_buf[offset++] = 't'; }
            else if ((unsigned char)*src < 32) { /* skip */ }
            else { item_buf[offset++] = *src; }
            src++;
        }
        item_buf[offset++] = '"';
        
        /* Gửi từng dòng một (chunk) */
        if (httpd_resp_send_chunk(req, item_buf, offset) != ESP_OK) {
            ESP_LOGD(TAG, "Ngắt kết nối khi đang gửi chunk (Client đã thoát)");
            return ESP_FAIL; /* Break thẳng và bỏ qua đoạn gửi kết thúc array JSON */
        }
    }

    /* Kết thúc JSON object */
    httpd_resp_send_chunk(req, "]}", 2);
    /* Gửi chunk rỗng để báo hiệu kết thúc HTTP Transfer */
    httpd_resp_send_chunk(req, NULL, 0);

    return ESP_OK;
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */
esp_err_t log_server_init(void)
{
    /* Clear ring buffer */
    memset(s_log_ring, 0, sizeof(s_log_ring));
    s_log_head = 0;
    s_log_count = 0;

    /* Hook ESP_LOG output */
    s_original_vprintf = esp_log_set_vprintf(log_vprintf_hook);

    return ESP_OK;
}

static void init_mdns(void)
{
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mDNS init failed: %s", esp_err_to_name(err));
        return;
    }
    mdns_hostname_set(MDNS_HOSTNAME);
    mdns_instance_name_set("ESP-Agent Log");

    /* Thêm service HTTP */
    mdns_service_add("ESP-Agent-Web", "_http", "_tcp", LOG_SERVER_PORT, NULL, 0);

    ESP_LOGI(TAG, "mDNS: http://%s.local", MDNS_HOSTNAME);
}

esp_err_t log_server_start(void)
{
    /* Khởi tạo mDNS */
    init_mdns();

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = LOG_SERVER_PORT;
    config.max_open_sockets = 2;
    config.lru_purge_enable = true;
    config.stack_size = 4096;

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Không thể khởi tạo Log Server: %s", esp_err_to_name(err));
        return err;
    }

    httpd_uri_t root_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_get_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &root_uri);

    httpd_uri_t log_uri = {
        .uri = "/log",
        .method = HTTP_GET,
        .handler = log_page_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &log_uri);

    httpd_uri_t api_uri = {
        .uri = "/api/log",
        .method = HTTP_GET,
        .handler = log_api_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &api_uri);

    /* Lấy IP thực tế để hiển thị */
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip_info;
    if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
        ESP_LOGI(TAG, "Web Log: http://" IPSTR "/log", IP2STR(&ip_info.ip));
        ESP_LOGI(TAG, "Web Log: http://esp-agent.local/log");
    } else {
        ESP_LOGI(TAG, "Web Log: http://esp-agent.local/log (IP chưa sẵn sàng)");
    }

    return ESP_OK;
}
