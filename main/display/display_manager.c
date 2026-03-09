/**
 * ===========================================================================
 * @file display_manager.c
 * @brief State machine quản lý màn hình OLED + Carousel deadline
 *
 * BOOT → IDLE (carousel) ↔ RESULT / ALERT
 * Nút BOOT (GPIO9): chuyển deadline tiếp theo (slide animation)
 * ===========================================================================
 */

#include "display_manager.h"
#include "ssd1306.h"
#include "vn_utils.h"
#include "task_database.h"
#include "time_utils.h"
#include "wifi_manager.h"
#include "config.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "display";

/* ──── Screen states ──── */
typedef enum {
    SCREEN_BOOT, SCREEN_IDLE, SCREEN_RESULT, SCREEN_ALERT,
} screen_state_t;

static screen_state_t s_state = SCREEN_BOOT;
static TickType_t s_state_start = 0;

#define RESULT_TIMEOUT_MS   4000
#define ALERT_TIMEOUT_MS    5000
#define IDLE_REFRESH_MS     60000

/* ──── Button (GPIO9 = BOOT) ──── */
#define BOOT_BTN_GPIO       9
#define DEBOUNCE_MS         250

static volatile bool s_btn_pressed = false;
static TickType_t s_last_btn_tick = 0;

/* ──── Carousel data ──── */
#define MAX_DL_TASKS        10

typedef struct {
    char id_str[8];        /* "#3"                   */
    char title_lines[3][48]; /* 3 dòng tiêu đề (ASCII) */
    char due_str[32];      /* "Han: 15:00 hom nay"   */
} carousel_item_t;

static carousel_item_t s_items[MAX_DL_TASKS];
static int s_dl_count = 0;     /* Số deadline tasks    */
static int s_cur_idx  = 0;     /* Index hiện tại       */
static bool s_wifi_ok = true;

/* ==========================================================================
 * Button ISR + init
 * ========================================================================== */
static void IRAM_ATTR btn_isr(void *arg) { s_btn_pressed = true; }

static void button_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << BOOT_BTN_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_NEGEDGE,
    };
    gpio_config(&cfg);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(BOOT_BTN_GPIO, btn_isr, NULL);
}

/* ==========================================================================
 * Init
 * ========================================================================== */
esp_err_t display_init(int sda_gpio, int scl_gpio)
{
    esp_err_t err = ssd1306_init(sda_gpio, scl_gpio);
    if (err != ESP_OK) return err;
    button_init();
    s_state = SCREEN_BOOT;
    return ESP_OK;
}

/* ==========================================================================
 * Boot progress (không đổi)
 * ========================================================================== */
void display_boot_progress(int percent, const char *label)
{
    ssd1306_clear();
    char ver_text[32];
    snprintf(ver_text, sizeof(ver_text), "ESP-Agent %s", FIRMWARE_VERSION);
    ssd1306_draw_string(16, 10, ver_text, false);
    ssd1306_draw_progress(14, 30, 100, percent);

    char ascii[22];
    vn_strip_diacritics(ascii, label, sizeof(ascii));
    int len = strlen(ascii);
    int x = (128 - len * 6) / 2;
    if (x < 0) x = 0;
    ssd1306_draw_string(x, 46, ascii, false);
    ssd1306_update();
}

/* ==========================================================================
 * Helpers: split title, format due time
 * ========================================================================== */
/* Tìm điểm ngắt dòng dựa vào pixel width. word_wrap=true sẽ ưu tiên ngắt tại dấu cách. */
static int find_split_point(const char *s, int max_w, bool word_wrap)
{
    int last_space = -1;
    int cur_w = 0;
    int i = 0;
    while (s[i]) {
        int char_w = 6;
        if (s[i] == ' ') { char_w = 3; last_space = i; }
        else if (s[i] == '.' || s[i] == ',' || s[i] == ':' || s[i] == ';') char_w = 4;
        
        if (cur_w + char_w > max_w) break;
        cur_w += char_w;
        i++;
    }
    if (s[i] == '\0') return i;
    if (word_wrap && last_space != -1) return last_space;
    return i;
}

static void split_title(const char *title, int id_px, char lines[3][48], int xoff)
{
    /* max width is the screen width minus the starting x position */
    int max_l1 = 128 - (id_px + xoff);
    if (max_l1 < 30) max_l1 = 30;

    /* Dòng 1 */
    int split1 = find_split_point(title, max_l1, true);
    int n1 = split1 < 47 ? split1 : 47;
    memcpy(lines[0], title, n1); lines[0][n1] = '\0';

    const char *rest = title + split1;
    while (*rest == ' ') rest++;
    
    if (*rest == '\0') {
        lines[1][0] = '\0';
        lines[2][0] = '\0';
    } else {
        /* Dòng 2: starts at x = xoff */
        int max_l2 = 128 - xoff;
        int split2 = find_split_point(rest, max_l2, true);
        int n2 = split2 < 47 ? split2 : 47;
        memcpy(lines[1], rest, n2); lines[1][n2] = '\0';

        rest += split2;
        while (*rest == ' ') rest++;

        if (*rest == '\0') {
            lines[2][0] = '\0';
        } else {
            /* Dòng 3: cố gắng lấp đầy pixel, thêm "..." nếu còn dư */
            int max_l3 = 128 - xoff;
            int split3 = find_split_point(rest, max_l3, true);
            if (rest[split3] == '\0') {
                strncpy(lines[2], rest, 47); lines[2][47] = '\0';
            } else {
                int dots_w = 12;
                int cut_idx = find_split_point(rest, max_l3 - dots_w, false);
                int n3 = cut_idx < 44 ? cut_idx : 44;
                memcpy(lines[2], rest, n3); lines[2][n3] = '\0';
                strcat(lines[2], "...");
            }
        }
    }
}

static void format_due(time_t due, char *buf, size_t sz)
{
    if (due <= 0) { snprintf(buf, sz, "Han: --"); return; }
    struct tm ti; localtime_r(&due, &ti);
    time_t now = time_utils_get_now();
    struct tm nt; localtime_r(&now, &nt);
    nt.tm_hour = 0; nt.tm_min = 0; nt.tm_sec = 0;
    time_t t0 = mktime(&nt);

    if (due < t0 + 86400)
        snprintf(buf, sz, "Han: %02d:%02d hom nay", ti.tm_hour, ti.tm_min);
    else if (due < t0 + 2*86400)
        snprintf(buf, sz, "Han: %02d:%02d ngay mai", ti.tm_hour, ti.tm_min);
    else
        snprintf(buf, sz, "Han: %02d:%02d %02d/%02d",
                 ti.tm_hour, ti.tm_min, ti.tm_mday, ti.tm_mon + 1);
}

/* ==========================================================================
 * Refresh idle data – lưu TẤT CẢ deadline tasks cho carousel
 * ========================================================================== */
static void refresh_idle_data(void)
{
    time_t now = time_utils_get_now();
    struct tm ti_now; localtime_r(&now, &ti_now);
    ti_now.tm_hour = 0; ti_now.tm_min = 0; ti_now.tm_sec = 0;
    time_t today_start = mktime(&ti_now);
    time_t dl_end = today_start + (3 * 86400);

    static task_record_t tasks[MAX_DL_TASKS];
    int count = 0;

    if (task_database_query_by_time(today_start, dl_end, NULL, "pending",
                                    tasks, MAX_DL_TASKS, &count) == ESP_OK && count > 0) {
        /* Sort theo due_time tăng dần */
        for (int i = 0; i < count - 1; i++)
            for (int j = 0; j < count - i - 1; j++)
                if (tasks[j].due_time > tasks[j+1].due_time) {
                    task_record_t tmp = tasks[j];
                    tasks[j] = tasks[j+1]; tasks[j+1] = tmp;
                }

        s_dl_count = count;
        for (int i = 0; i < count; i++) {
            snprintf(s_items[i].id_str, sizeof(s_items[i].id_str),
                     "#%lu", (unsigned long)tasks[i].id);
            char ascii[64];
            vn_strip_diacritics(ascii, tasks[i].title, sizeof(ascii));
            int id_px = (strlen(s_items[i].id_str) * 6) + 2; 
            /* offset x là 2px cho phần idle */
            split_title(ascii, id_px, s_items[i].title_lines, 2);
            format_due(tasks[i].due_time, s_items[i].due_str, sizeof(s_items[i].due_str));
        }
        if (s_cur_idx >= count) s_cur_idx = 0;
    } else {
        s_dl_count = 0;
        s_cur_idx  = 0;
    }
}

/* ==========================================================================
 * Draw helpers
 * ========================================================================== */
static void draw_header(void)
{
    /* Vẽ nền bar cao 10 pixel */
    ssd1306_fill_rect(0, 0, 128, 10, true);
    
    /* Bên trái: "Deadline" */
    ssd1306_draw_string(2, 1, "Deadline", true);
    
    /* Ở giữa: Thời gian hh:mm */
    char time_str[8];
    time_t now = time_utils_get_now();
    struct tm ti;
    localtime_r(&now, &ti);
    snprintf(time_str, sizeof(time_str), "%02d:%02d", ti.tm_hour, ti.tm_min);
    
    /* Căn giữa hh:mm trong khoảng trống còn lại giữa "Deadline" và WiFi */
    /* Deadline (0-48), WiFi (115-128) -> Center gap ≈ 81 -> Start at 81-15=66 or 65 */
    ssd1306_draw_string(65, 1, time_str, true);
    
    /* Bên phải: Biểu tượng Wi-Fi (11x8) theo cấp độ tín hiệu */
    int wifi_level = wifi_manager_get_level();
    ssd1306_draw_wifi_icon(115, 1, wifi_level, true);
}

/* draw_dl_count removed as per user request */

/** Vẽ nội dung 1 task tại x_offset (dùng cho animation) */
static void draw_task_content(int idx, int xoff)
{
    if (idx < 0 || idx >= s_dl_count) return;
    const carousel_item_t *it = &s_items[idx];

    /* Dòng 1: #ID + Title L1 */
    ssd1306_draw_string(xoff, 14, it->id_str, false);
    int id_px = (strlen(it->id_str) * 6) + 2; 
    ssd1306_draw_string_6x8(xoff + id_px, 14, it->title_lines[0], false);

    /* Dòng 2: Title L2 */
    if (it->title_lines[1][0])
        ssd1306_draw_string_6x8(xoff, 24, it->title_lines[1], false);

    /* Dòng 3: Title L3 */
    if (it->title_lines[2][0])
        ssd1306_draw_string_6x8(xoff, 34, it->title_lines[2], false);

    /* Dòng 4: Due time */
    ssd1306_draw_string_6x8(xoff, 46, it->due_str, false);
}

/** Vẽ chấm chỉ thị vị trí carousel */
static void draw_dots(int cur, int total)
{
    if (total <= 1) return;
    int sp = 10;  /* khoảng cách giữa các tâm chấm */
    int tw = (total - 1) * sp;
    int sx = (128 - tw) / 2;
    int cy = 61; /* Dịch xuống sát đáy hơn tí */

    for (int i = 0; i < total; i++) {
        int cx = sx + i * sp;
        if (i == cur) {
            /* Chấm lớn 3×3 (active) */
            ssd1306_fill_rect(cx - 1, cy - 1, 3, 3, true);
        } else {
            /* Chấm nhỏ 1×1 (inactive) */
            ssd1306_draw_pixel(cx, cy, true);
        }
    }
}

/* ==========================================================================
 * Render idle (không animation)
 * ========================================================================== */
static void render_idle(void)
{
    ssd1306_clear();
    draw_header();
    /* draw_dl_count removed */

    if (s_dl_count > 0) {
        draw_task_content(s_cur_idx, 2);
        draw_dots(s_cur_idx, s_dl_count);
    } else {
        ssd1306_draw_string(2, 30, "Khong co task gap", false);
    }
    ssd1306_update();
}

/* ==========================================================================
 * Slide animation: trượt từ phải sang trái
 * ========================================================================== */
static void slide_to_next(void)
{
    if (s_dl_count <= 1) return;

    int old_idx = s_cur_idx;
    int new_idx = (s_cur_idx + 1) % s_dl_count;

    #define ANIM_STEPS  6
    #define ANIM_DELAY  25   /* ms mỗi frame */

    for (int step = 1; step <= ANIM_STEPS; step++) {
        int shift = (step * 128) / ANIM_STEPS;

        ssd1306_clear();
        draw_header();
        /* draw_dl_count removed */

        /* Nội dung cũ trượt ra trái */
        draw_task_content(old_idx, 2 - shift);
        /* Nội dung mới trượt vào từ phải */
        draw_task_content(new_idx, 130 - shift);

        draw_dots(new_idx, s_dl_count);
        ssd1306_update();
        vTaskDelay(pdMS_TO_TICKS(ANIM_DELAY));
    }

    s_cur_idx = new_idx;
    render_idle();  /* Vẽ lại sạch sau animation */
}

/* ==========================================================================
 * Show Result (không đổi)
 * ========================================================================== */
void display_show_result(const char *action, uint32_t task_id, const char *title)
{
    ssd1306_clear();
    char header[48];
    if (action) {
        char aa[22]; vn_strip_diacritics(aa, action, sizeof(aa));
        snprintf(header, sizeof(header), " %s!", aa);
    } else {
        snprintf(header, sizeof(header), " Hoan thanh!");
    }
    ssd1306_draw_inverted_bar(0, header);

    if (task_id > 0) {
        char d[64]; char aa2[22] = "";
        if (action) vn_strip_diacritics(aa2, action, sizeof(aa2));
        snprintf(d, sizeof(d), "%s #%lu",
            strlen(aa2) > 0 ? aa2 : "Task", (unsigned long)task_id);
        ssd1306_draw_string(2, 20, d, false);
    }
    if (title && strlen(title) > 0) {
        char at[128]; vn_strip_diacritics(at, title, sizeof(at));
        ssd1306_draw_string_wrapped(2, 34, 126, at, false);
    }
    ssd1306_update();
    s_state = SCREEN_RESULT;
    s_state_start = xTaskGetTickCount();
}

/* ==========================================================================
 * Show Alert (không đổi)
 * ========================================================================== */
void display_show_alert(uint32_t task_id, const char *title,
                        const char *due_str, int32_t seconds_left)
{
    ssd1306_clear();

    /* Header inverted bar - SAP DEN HAN! */
    ssd1306_draw_inverted_bar(0, " !! SAP DEN HAN !!");

    /* Chuẩn bị dữ liệu title giống Idle (2 dòng, có dấu ...) */
    char id_str[10];
    snprintf(id_str, sizeof(id_str), "#%lu", (unsigned long)task_id);
    int id_indent = strlen(id_str) + 1;

    char ascii_title[64];
    vn_strip_diacritics(ascii_title, title, sizeof(ascii_title));

    char lines[3][48];
    split_title(ascii_title, id_indent, lines, 2);

    /* Vẽ Task ID + Title dòng 1 */
    char combined[64];
    snprintf(combined, sizeof(combined), "%s %s", id_str, lines[0]);
    ssd1306_draw_string(2, 13, combined, false);

    /* Dòng 2 & 3 */
    if (lines[1][0]) ssd1306_draw_string(2, 23, lines[1], false);
    if (lines[2][0]) ssd1306_draw_string(2, 33, lines[2], false);

    /* Gạch ngang phân cách */
    ssd1306_draw_hline(0, 34, 128);

    /* Thời hạn */
    char due_line[64];
    snprintf(due_line, sizeof(due_line), "Han: %s", due_str ? due_str : "??");
    ssd1306_draw_string(2, 40, due_line, false);

    /* Số ngày/giờ còn lại */
    char status_line[64];
    if (seconds_left < 0) {
        snprintf(status_line, sizeof(status_line), "DA QUA HAN!");
    } else if (seconds_left < 3600) {
        snprintf(status_line, sizeof(status_line), "Han: < 1 GIO!");
    } else if (seconds_left < 86400) {
        snprintf(status_line, sizeof(status_line), "Con %ld gio", (long)(seconds_left / 3600));
    } else {
        int days = seconds_left / 86400;
        if (days == 1) snprintf(status_line, sizeof(status_line), "Con 1 ngay");
        else snprintf(status_line, sizeof(status_line), "Con %d ngay", days);
    }
    ssd1306_draw_string(2, 52, status_line, false);

    ssd1306_update();

    /* Flash effect: nhấp nháy 3 lần */
    for (int i = 0; i < 3; i++) {
        vTaskDelay(pdMS_TO_TICKS(300));
        ssd1306_invert_display(true);
        vTaskDelay(pdMS_TO_TICKS(300));
        ssd1306_invert_display(false);
    }

    s_state = SCREEN_ALERT;
    s_state_start = xTaskGetTickCount();
}

/* ==========================================================================
 * Show Idle
 * ========================================================================== */
void display_show_idle(void)
{
    refresh_idle_data();
    render_idle();
    s_state = SCREEN_IDLE;
    s_state_start = xTaskGetTickCount();
}

/* ==========================================================================
 * Background task: timeout + button + refresh
 * ========================================================================== */
static void display_task(void *arg)
{
    TickType_t last_idle_refresh = 0;

    while (1) {
        s_wifi_ok = wifi_manager_is_connected();
        TickType_t now = xTaskGetTickCount();
        uint32_t elapsed = (now - s_state_start) * portTICK_PERIOD_MS;

        /* ── Xử lý nút BOOT ── */
        if (s_btn_pressed) {
            s_btn_pressed = false;
            
            TickType_t press_start = now;
            bool long_pressed = false;

            /* Check long press >= 5s */
            while (gpio_get_level(BOOT_BTN_GPIO) == 0) {
                if ((xTaskGetTickCount() - press_start) * portTICK_PERIOD_MS >= 5000) {
                    long_pressed = true;
                    ESP_LOGW(TAG, "Đã giữ nút 5s! Xóa cài đặt WiFi...");
                    ssd1306_clear();
                    ssd1306_draw_string(5, 30, "XOA WIFI... REBOOT", false);
                    ssd1306_update();
                    
                    nvs_handle_t h;
                    if (nvs_open("wifi_cfg", NVS_READWRITE, &h) == ESP_OK) {
                        nvs_erase_all(h);
                        nvs_commit(h);
                        nvs_close(h);
                    }
                    vTaskDelay(pdMS_TO_TICKS(1000));
                    esp_restart();
                }
                vTaskDelay(pdMS_TO_TICKS(50));
            }

            if (!long_pressed && (now - s_last_btn_tick) * portTICK_PERIOD_MS >= DEBOUNCE_MS) {
                s_last_btn_tick = now;
                if (s_state == SCREEN_IDLE && s_dl_count > 1) {
                    ESP_LOGI(TAG, "Button → slide %d→%d",
                             s_cur_idx, (s_cur_idx + 1) % s_dl_count);
                    slide_to_next();
                }
            }
        }

        /* ── State machine ── */
        switch (s_state) {
            case SCREEN_RESULT:
                if (elapsed >= RESULT_TIMEOUT_MS) {
                    ESP_LOGI(TAG, "Result timeout → Idle");
                    display_show_idle();
                }
                break;
            case SCREEN_ALERT:
                if (elapsed >= ALERT_TIMEOUT_MS) {
                    ESP_LOGI(TAG, "Alert timeout → Idle");
                    display_show_idle();
                }
                break;
            case SCREEN_IDLE:
                if ((now - last_idle_refresh) * portTICK_PERIOD_MS >= IDLE_REFRESH_MS) {
                    refresh_idle_data();
                    render_idle();
                    last_idle_refresh = now;
                }
                break;
            case SCREEN_BOOT:
                break;
        }

        vTaskDelay(pdMS_TO_TICKS(100));  /* 100ms để nút phản hồi nhanh */
    }
}

void display_start_task(void)
{
    xTaskCreate(display_task, "display", 6144, NULL, 3, NULL);
    ESP_LOGI(TAG, "Display task started");
}
