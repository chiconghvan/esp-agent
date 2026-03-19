/**
 * ===========================================================================
 * @file display_manager.c
 * @brief OLED Display Management using U8g2
 *
 * BOOT -> IDLE (carousel) <-> RESULT / ALERT
 * Using synchronous font for the entire system.
 * ===========================================================================
 */

#include "display_manager.h"
#include "display_config.h"
#include <u8g2.h>
#include "u8g2_esp32_hal.h"
#include "vn_utils.h"
#include "task_database.h"
#include "time_utils.h"
#include "wifi_manager.h"
#include "config.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "nvs.h"
#include <stdio.h>
#include <string.h>

// static const char *TAG = "display"; // Unused
static u8g2_t u8g2;
static SemaphoreHandle_t s_display_mutex = NULL;

/* ──── Screen states ──── */
typedef enum {
    SCREEN_BOOT, SCREEN_IDLE, SCREEN_RESULT, SCREEN_ALERT,
} screen_state_t;

static screen_state_t s_state = SCREEN_BOOT;
static TickType_t s_state_start = 0;

#define RESULT_TIMEOUT_MS   4000
#define ALERT_TIMEOUT_MS    5000
#define IDLE_REFRESH_MS     60000

#define BOOT_BTN_GPIO       9

static volatile bool s_btn_pressed = false;

/* ──── Carousel data ──── */
#define MAX_DL_TASKS        10

typedef struct {
    char id_str[8];        
    char title_lines[2][48]; /* Rút gọn còn 2 dòng tiêu đề */
    char due_str[32];      
    bool truncated;        /* Task bị dài quá 2 dòng */
} carousel_item_t;

static carousel_item_t s_items[MAX_DL_TASKS];
static int s_dl_count = 0;
static int s_cur_idx  = 0;
static int s_wifi_level = 0;

typedef enum {
    VIEW_MODE_DEADLINE,
    VIEW_MODE_TODAY
} carousel_view_mode_t;
static carousel_view_mode_t s_view_mode = VIEW_MODE_DEADLINE;

/* ==========================================================================
 * Button & Init
 * ========================================================================== */
static void IRAM_ATTR btn_isr(void *arg) { s_btn_pressed = true; }

static void button_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << BOOT_BTN_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .intr_type    = GPIO_INTR_NEGEDGE,
    };
    gpio_config(&cfg);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(BOOT_BTN_GPIO, btn_isr, NULL);
}

esp_err_t display_init(int sda_gpio, int scl_gpio)
{
    // Khởi tạo Mutex trước khi dùng
    if (s_display_mutex == NULL) {
        s_display_mutex = xSemaphoreCreateMutex();
    }
    
    xSemaphoreTake(s_display_mutex, portMAX_DELAY);
    u8g2_esp32_hal_t hal = U8G2_ESP32_HAL_DEFAULT;
    hal.bus.i2c.sda = sda_gpio;
    hal.bus.i2c.scl = scl_gpio;
    u8g2_esp32_hal_init(hal);

    u8g2_Setup_ssd1306_i2c_128x64_noname_f(
        &u8g2, U8G2_R0, 
        u8g2_esp32_i2c_byte_cb, 
        u8g2_esp32_gpio_and_delay_cb
    );

    u8g2_InitDisplay(&u8g2);
    u8g2_SetPowerSave(&u8g2, 0);
    u8g2_ClearBuffer(&u8g2);
    xSemaphoreGive(s_display_mutex);
    
    button_init();
    s_state = SCREEN_BOOT;
    return ESP_OK;
}

/* ==========================================================================
 * Custom Layout & Drawing Engine
 * ========================================================================== */

static int get_char_width_custom(uint16_t c)
{
    if (c == ' ') return DISP_WIDTH_SPACE;
    if (c == ':' || c == ',' || c == '.' || c == '\'') return DISP_WIDTH_PUNCTUATION;
    
    // Sử dụng API chuẩn của U8g2 để lấy width của glyph tương ứng với font đang set
    return u8g2_GetGlyphWidth(&u8g2, c);
}

static int get_str_width_custom(const char *s)
{
    int total_w = 0;
    u8x8_utf8_init(u8g2_GetU8x8(&u8g2));
    while (*s) {
        uint16_t c = u8x8_utf8_next(u8g2_GetU8x8(&u8g2), (uint8_t)*s++);
        if (c == 0xFFFF) break;
        if (c == 0xFFFE) continue;
        total_w += get_char_width_custom(c);
    }
    return total_w;
}

static void draw_utf8_custom(int x, int y, const char *s)
{
    int cur_x = x;
    u8x8_utf8_init(u8g2_GetU8x8(&u8g2));
    while (*s) {
        uint16_t c = u8x8_utf8_next(u8g2_GetU8x8(&u8g2), (uint8_t)*s++);
        if (c == 0xFFFF) break;
        if (c == 0xFFFE) continue;

        u8g2_DrawGlyph(&u8g2, cur_x, y, c);
        cur_x += get_char_width_custom(c);
    }
}

static void draw_utf8_custom_centered(int y, const char *s)
{
    int w = get_str_width_custom(s);
    int x = (128 - w) / 2;
    if (x < 0) x = 0;
    draw_utf8_custom(x, y, s);
}

/* ==========================================================================
 * Boot progress
 * ========================================================================== */
void display_boot_progress(int percent, const char *label)
{
    if (s_display_mutex == NULL) return;
    xSemaphoreTake(s_display_mutex, portMAX_DELAY);
    
    u8g2_ClearBuffer(&u8g2);
    
    // 1. Vẽ tiêu đề Firmware (Centered)
    u8g2_SetFont(&u8g2, DISP_FONT_HEADER_LABEL);
    int asc_h = u8g2_GetAscent(&u8g2);
    char ver_text[32];
    snprintf(ver_text, sizeof(ver_text), "ESP-Agent v%s", FIRMWARE_VERSION);
    draw_utf8_custom_centered(10 + asc_h, ver_text);
    
    // 2. Vẽ Progress Bar
    u8g2_DrawFrame(&u8g2, 14, 32, 100, 10); 
    if (percent > 0) {
        int bar_w = (percent * 96) / 100;
        u8g2_DrawBox(&u8g2, 16, 34, bar_w, 6);
    }

    // 3. Vẽ nhãn trạng thái (Centered)
    u8g2_SetFont(&u8g2, DISP_FONT_TASK_TITLE);
    int asc_b = u8g2_GetAscent(&u8g2);
    draw_utf8_custom_centered(50 + asc_b, label);
    
    u8g2_SendBuffer(&u8g2);
    xSemaphoreGive(s_display_mutex);
}

/* ==========================================================================
 * Custom Layout & Drawing Engine
 * ========================================================================== */


static int find_split_point_custom(const char *title, int max_w, bool hard_cut)
{
    int cur_w = 0;
    int last_break_idx = -1;
    u8x8_utf8_init(u8g2_GetU8x8(&u8g2));
    const char *p = title;
    const char *char_start = title;
    
    while (*p) {
        uint16_t c = u8x8_utf8_next(u8g2_GetU8x8(&u8g2), (uint8_t)*p++);
        if (c == 0xFFFF) break;
        if (c == 0xFFFE) continue;
        
        int char_w = get_char_width_custom(c);
        if (cur_w + char_w > max_w) {
            if (hard_cut || last_break_idx == -1) return (char_start - title);
            return last_break_idx;
        }
        
        cur_w += char_w;
        // Ngắt tại dấu cách hoặc bất kỳ dấu câu thông dụng nào
        if (c == ' ' || c == ':' || c == ',' || c == '.' || c == '-' || c == ';' || c == '!') {
            last_break_idx = p - title;
        }
        char_start = p;
    }
    return p - title;
}

static void split_title(const char *title, const char *id_str, char lines[2][48], bool *truncated)
{
    *truncated = false;
    u8g2_SetFont(&u8g2, DISP_FONT_TASK_ID);
    int id_w = get_str_width_custom(id_str) + 4;
    
    u8g2_SetFont(&u8g2, DISP_FONT_TASK_TITLE);
    int max_w_l1 = 127 - id_w - DISP_X_PADDING;

    // Dòng 1: Ngắt thông minh tại dấu câu
    int split1 = find_split_point_custom(title, max_w_l1, false);
    strncpy(lines[0], title, split1); lines[0][split1] = '\0';

    const char *rest = title + split1;
    while (*rest == ' ') rest++;

    if (*rest) {
        // Dành chỗ cho dots: 128 - 2(lề) - 2(phải) - 6(dots 5px + 1px gap) = 118px
        int max_w_l2 = 118; 
        int split2 = find_split_point_custom(rest, max_w_l2, true);
        strncpy(lines[1], rest, split2); lines[1][split2] = '\0';
        
        if (rest[split2] != '\0') *truncated = true;
    } else {
        lines[1][0] = '\0';
    }
}

static void format_due(time_t due, char *buf, size_t sz)
{
    if (due <= 0) { snprintf(buf, sz, "Hạn: --"); return; }
    struct tm ti; localtime_r(&due, &ti);
    time_t now = time_utils_get_now();
    struct tm nt; localtime_r(&now, &nt);
    nt.tm_hour = 0; nt.tm_min = 0; nt.tm_sec = 0;
    time_t t0 = mktime(&nt);

    if (due < t0)
        snprintf(buf, sz, "Quá hạn! %02d/%02d", ti.tm_mday, ti.tm_mon + 1);
    else if (due < t0 + 86400)
        snprintf(buf, sz, "Hôm nay, %02d:%02d", ti.tm_hour, ti.tm_min);
    else if (due < t0 + 2*86400)
        snprintf(buf, sz, "Ngày mai, %02d:%02d", ti.tm_hour, ti.tm_min);
    else
        snprintf(buf, sz, "%02d/%02d, %02d:%02d", ti.tm_mday, ti.tm_mon + 1, ti.tm_hour, ti.tm_min);
}

/* ==========================================================================
 * Render logic
 * ========================================================================== */
static void draw_header(void)
{
    u8g2_SetDrawColor(&u8g2, 1);
    u8g2_DrawBox(&u8g2, 0, 0, 128, DISP_HEADER_HEIGHT);
    u8g2_SetDrawColor(&u8g2, 0);
    
    // 1. Vẽ Label (Deadline/Today)
    u8g2_SetFont(&u8g2, DISP_FONT_HEADER_LABEL);
    int asc_label = u8g2_GetAscent(&u8g2);
    // Tính Y để căn giữa: (BarHeight - FontHeight)/2 + Ascent
    int y_label = (DISP_HEADER_HEIGHT - asc_label) / 2 + asc_label;
    u8g2_DrawStr(&u8g2, 2, y_label, s_view_mode == VIEW_MODE_DEADLINE ? "DEADLINE" : "TODAY");

    // 2. Vẽ Đồng hồ
    u8g2_SetFont(&u8g2, DISP_FONT_HEADER_TIME);
    int asc_time = u8g2_GetAscent(&u8g2);
    int y_time = (DISP_HEADER_HEIGHT - asc_time) / 2 + asc_time;
    
    char time_str[8];
    time_t now = time_utils_get_now();
    struct tm ti; localtime_r(&now, &ti);
    snprintf(time_str, sizeof(time_str), "%02d:%02d", ti.tm_hour, ti.tm_min);
    u8g2_DrawStr(&u8g2, 68, y_time, time_str);

    // 3. Vẽ Wi-Fi: Aligned to have 1px gap from bottom like text
    int wifi_x_base = 115;
    // Calculation: Bottom edge = DISP_HEADER_HEIGHT - 2 (for 1px gap from row 9)
    // Icon height is 10. So wifi_y_top + 9 = DISP_HEADER_HEIGHT - 2.
    int wifi_y_top = DISP_HEADER_HEIGHT - 11; 
    if (wifi_y_top < -1) wifi_y_top = -1; // Clamp to avoid excessive cut

    int h_levels[] = {4, 6, 8};
    for(int i=0; i < s_wifi_level && i < 3; i++) {
        int h = h_levels[i];
        u8g2_DrawBox(&u8g2, wifi_x_base + (i*4), wifi_y_top + (10 - h), 3, h);
    }
    
    u8g2_SetDrawColor(&u8g2, 1);
}

static void draw_task_content(int idx, int xoff)
{
    if (idx < 0 || idx >= s_dl_count) return;
    const carousel_item_t *it = &s_items[idx];

    // 1. Xác định metrics của Title
    u8g2_SetFont(&u8g2, DISP_FONT_TASK_TITLE);
    int ascent_title = u8g2_GetAscent(&u8g2);
    
    // 2. Xác định metrics của Due
    u8g2_SetFont(&u8g2, DISP_FONT_TASK_DUE);
    int ascent_due = u8g2_GetAscent(&u8g2);

    // Tính toán Y động: Tọa độ vẽ (Baseline) = Lề trên + Độ cao ký tự so với baseline
    int y_l1 = DISP_HEADER_HEIGHT + DISP_MARGIN_TOP + ascent_title;
    int y_l2 = y_l1 + DISP_LINE_SPACING;
    int y_due = y_l2 + DISP_MARGIN_DUE + ascent_due;

    // 1. Vẽ ID
    u8g2_SetFont(&u8g2, DISP_FONT_TASK_ID);
    draw_utf8_custom(xoff, y_l1, it->id_str);
    int id_w = get_str_width_custom(it->id_str) + 4;

    // 2. Vẽ Title L1
    u8g2_SetFont(&u8g2, DISP_FONT_TASK_TITLE);
    draw_utf8_custom(xoff + id_w, y_l1, it->title_lines[0]);

    // 3. Vẽ Title L2
    if (it->title_lines[1][0]) {
        draw_utf8_custom(xoff, y_l2, it->title_lines[1]);
        
        // Nếu bị cắt, vẽ 3 điểm ảnh (1x1px mỗi ô, cách nhau 1px)
        if (it->truncated) {
            int text_w = get_str_width_custom(it->title_lines[1]);
            int dot_start_x = xoff + text_w + 1; 
            if (dot_start_x > 122) dot_start_x = 122; 
            
            int dot_y = y_l2 - 1; 
            for (int d = 0; d < 3; d++) {
                u8g2_DrawPixel(&u8g2, dot_start_x + (d * 2), dot_y);
            }
        }
    }

    // 4. Vẽ Due Time
    u8g2_SetFont(&u8g2, DISP_FONT_TASK_DUE);
    draw_utf8_custom(xoff, y_due, it->due_str);
}

static void draw_dots(int cur, int total)
{
    if (total <= 1) return;
    int sx = (128 - (total - 1) * 8) / 2;
    for (int i = 0; i < total; i++) {
        if (i == cur) u8g2_DrawDisc(&u8g2, sx + i * 8, 62, 1, U8G2_DRAW_ALL);
        else u8g2_DrawPixel(&u8g2, sx + i * 8, 62);
    }
}

static void refresh_idle_data(void)
{
    time_t now = time_utils_get_now();
    struct tm ti_now; localtime_r(&now, &ti_now);
    ti_now.tm_hour = 0; ti_now.tm_min = 0; ti_now.tm_sec = 0;
    time_t today_start = mktime(&ti_now);
    time_t dl_end = today_start + (3 * 86400);

    if (s_view_mode == VIEW_MODE_TODAY) dl_end = today_start + 86400 - 1;

    static task_record_t tasks[MAX_DL_TASKS];
    int count = 0;

    if (task_database_query_by_time(today_start, dl_end, NULL, "pending",
                                    tasks, MAX_DL_TASKS, &count) == ESP_OK && count > 0) {
        s_dl_count = count;
        for (int i = 0; i < count; i++) {
            snprintf(s_items[i].id_str, sizeof(s_items[i].id_str), "#%lu", (unsigned long)tasks[i].id);
            split_title(tasks[i].title, s_items[i].id_str, s_items[i].title_lines, &s_items[i].truncated);
            format_due(tasks[i].due_time, s_items[i].due_str, sizeof(s_items[i].due_str));
        }
        if (s_cur_idx >= count) s_cur_idx = 0;
    } else {
        s_dl_count = 0;
    }
}

static void render_idle(void)
{
    if (s_display_mutex == NULL) return;
    xSemaphoreTake(s_display_mutex, portMAX_DELAY);

    u8g2_ClearBuffer(&u8g2);
    draw_header();
    if (s_dl_count > 0) {
        draw_task_content(s_cur_idx, DISP_X_PADDING);
        draw_dots(s_cur_idx, s_dl_count);
    } else {
        u8g2_SetFont(&u8g2, DISP_FONT_TASK_TITLE);
        draw_utf8_custom_centered(35, "KH\xC3\x94NG C\xC3\x93 C\xC3\x94NG VI\xE1\xBB\x86" "C");
    }
    u8g2_SendBuffer(&u8g2);
    xSemaphoreGive(s_display_mutex);
}

static void slide_to_next(void)
{
    if (s_dl_count <= 1) return;
    int new_idx = (s_cur_idx + 1) % s_dl_count;

#if DISP_ENABLE_ANIMATION
    int old_idx = s_cur_idx;
    // Animation 12 bước, mỗi bước trượt nhanh hơn (~9ms delay + I2C time)
    for (int step = 1; step <= 6; step++) {
        int shift = (step * 128) / 6;
        if (s_display_mutex == NULL) return;
        xSemaphoreTake(s_display_mutex, portMAX_DELAY);
        u8g2_ClearBuffer(&u8g2);
        draw_header();
        draw_task_content(old_idx, DISP_X_PADDING - shift);
        draw_task_content(new_idx, 128 + DISP_X_PADDING - shift);
        draw_dots(new_idx, s_dl_count);
        u8g2_SendBuffer(&u8g2);
        xSemaphoreGive(s_display_mutex);
        vTaskDelay(pdMS_TO_TICKS(2)); 
    }
#endif

    s_cur_idx = new_idx;
    render_idle();
}

/* ==========================================================================
 * Public Interfaces
 * ========================================================================== */
void display_show_result(const char *action, uint32_t task_id, const char *title)
{
    if (s_display_mutex == NULL) return;
    xSemaphoreTake(s_display_mutex, portMAX_DELAY);

    u8g2_ClearBuffer(&u8g2);
    u8g2_SetDrawColor(&u8g2, 1);
    u8g2_DrawBox(&u8g2, 0, 0, 128, DISP_HEADER_HEIGHT);
    u8g2_SetDrawColor(&u8g2, 0);
    u8g2_SetFont(&u8g2, DISP_FONT_HEADER_LABEL);
    
    const char *action_str = action ? action : "HOAN THANH";
    draw_utf8_custom(2, DISP_HEADER_Y_OFFSET, action_str);

    u8g2_SetDrawColor(&u8g2, 1);
    u8g2_SetFont(&u8g2, DISP_FONT_TASK_TITLE);
    int ascent = u8g2_GetAscent(&u8g2);
    
    // Tọa độ Y động cho kết quả
    int y0 = DISP_HEADER_HEIGHT + 6 + ascent;
    int y1 = y0 + DISP_LINE_SPACING;

    if (task_id > 0) {
        char d[32]; snprintf(d, sizeof(d), "Task #%lu:", (unsigned long)task_id);
        draw_utf8_custom(2, y0, d);
    }
    if (title) {
        draw_utf8_custom(2, y1, title);
    }
    u8g2_SendBuffer(&u8g2);
    xSemaphoreGive(s_display_mutex);
    s_state = SCREEN_RESULT;
    s_state_start = xTaskGetTickCount();
}

void display_show_alert(uint32_t task_id, const char *title, const char *due_str, int32_t seconds_left)
{
    if (s_display_mutex == NULL) return;
    xSemaphoreTake(s_display_mutex, portMAX_DELAY);

    u8g2_ClearBuffer(&u8g2);
    u8g2_SetDrawColor(&u8g2, 1);
    u8g2_DrawBox(&u8g2, 0, 0, 128, DISP_HEADER_HEIGHT);
    u8g2_SetDrawColor(&u8g2, 0);
    u8g2_SetFont(&u8g2, DISP_FONT_HEADER_LABEL);
    draw_utf8_custom(2, DISP_HEADER_Y_OFFSET, "SAP DEN HAN!");

    u8g2_SetDrawColor(&u8g2, 1);
    u8g2_SetFont(&u8g2, DISP_FONT_TASK_TITLE);
    int asc = u8g2_GetAscent(&u8g2);
    
    // Tọa độ Y động cho cảnh báo
    int y0 = DISP_HEADER_HEIGHT + 5 + asc;
    int y1 = y0 + DISP_LINE_SPACING;
    int y2 = y1 + DISP_LINE_SPACING;

    char id_str[16]; snprintf(id_str, sizeof(id_str), "#%lu", (unsigned long)task_id);
    draw_utf8_custom(2, y0, id_str);
    
    draw_utf8_custom(35, y0, title);

    char due[32]; snprintf(due, sizeof(due), "LUC: %s", due_str);
    draw_utf8_custom(2, y1, due);

    char status[64];
    if (seconds_left < 0) strcpy(status, "DA QUA HAN!");
    else snprintf(status, sizeof(status), "CON %ld PHUT", (long)(seconds_left/60));
    draw_utf8_custom(2, y2, status);

    u8g2_SendBuffer(&u8g2);
    xSemaphoreGive(s_display_mutex);

    s_state = SCREEN_ALERT;
    s_state_start = xTaskGetTickCount();
}

void display_show_idle(void)
{
    refresh_idle_data();
    render_idle();
    s_state = SCREEN_IDLE;
    s_state_start = xTaskGetTickCount();
}

static void display_task(void *arg)
{
    TickType_t last_idle_refresh = 0;
    TickType_t last_wifi_check = 0;

    while (1) {
        TickType_t now = xTaskGetTickCount();
        if ((now - last_wifi_check) * portTICK_PERIOD_MS >= 2000) {
            s_wifi_level = wifi_manager_get_level();
            last_wifi_check = now;
        }

        uint32_t elapsed = (now - s_state_start) * portTICK_PERIOD_MS;

        if (s_btn_pressed) {
            s_btn_pressed = false;
            vTaskDelay(pdMS_TO_TICKS(50)); // Debounce
            if (gpio_get_level(BOOT_BTN_GPIO) == 0) {
                // Đã nhấn: Đợi thả nút lần 1
                while (gpio_get_level(BOOT_BTN_GPIO) == 0) vTaskDelay(pdMS_TO_TICKS(10));
                
                // Đã thả: Đợi xem có nhấn lần 2 trong vòng 300ms không
                bool double_click = false;
                for (int i = 0; i < 30; i++) {
                    vTaskDelay(pdMS_TO_TICKS(10));
                    if (gpio_get_level(BOOT_BTN_GPIO) == 0) {
                        double_click = true;
                        break;
                    }
                }

                if (double_click) {
                    // Double Click: Chuyển VIEW MODE
                    s_view_mode = (s_view_mode == VIEW_MODE_DEADLINE) ? VIEW_MODE_TODAY : VIEW_MODE_DEADLINE;
                    s_cur_idx = 0;
                    display_show_idle();
                    // Đợi thả nút lần 2
                    while (gpio_get_level(BOOT_BTN_GPIO) == 0) vTaskDelay(pdMS_TO_TICKS(10));
                } else {
                    // Single Click: Next hoặc quay lại Idle
                    if (s_state == SCREEN_IDLE) slide_to_next();
                    else display_show_idle();
                }
            }
        }

        switch (s_state) {
            case SCREEN_RESULT: if (elapsed >= RESULT_TIMEOUT_MS) display_show_idle(); break;
            case SCREEN_ALERT:  if (elapsed >= ALERT_TIMEOUT_MS) display_show_idle(); break;
            case SCREEN_IDLE:
                if ((now - last_idle_refresh) * portTICK_PERIOD_MS >= IDLE_REFRESH_MS) {
                    refresh_idle_data();
                    last_idle_refresh = now;
                    render_idle();
                } else if (elapsed % 5000 < 20) {
                    render_idle();
                }
                break;
            default: break;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void display_start_task(void)
{
    xTaskCreate(display_task, "display", 4096, NULL, 3, NULL);
}
