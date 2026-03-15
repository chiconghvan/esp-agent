/**
 * ===========================================================================
 * @file main.c
 * @brief Entry point cho ESP-Agent: Trợ lý nhắc việc thông minh
 *
 * Luồng khởi tạo:
 *   1. NVS Flash
 *   2. WiFi STA + SNTP
 *   3. SPIFFS Database
 *   4. Telegram Bot
 *   5. Reminder Scheduler
 *   6. Telegram Polling Loop (main loop)
 *
 * Target: ESP32-C3 Super Mini
 * ===========================================================================
 */

#include "safe_append.h"
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_system.h"

#include "config.h"
#include "wifi_manager.h"
#include "telegram_bot.h"
#include "task_database.h"
#include "action_dispatcher.h"
#include "reminder_scheduler.h"
#include "token_tracker.h"
#include "action_undo.h"
#include "firebase_sync.h"
#include "time_utils.h"
#include "display_manager.h"
#include "log_server.h"
#include "response_formatter.h"
#include "esp_ota_ops.h"
#include "driver/gpio.h"
#include "vector_search.h"
#include <stdlib.h>

static const char *TAG = "esp_agent";

/* --------------------------------------------------------------------------
 * Startup banner
 * -------------------------------------------------------------------------- */
static void print_banner(void)
{
    ESP_LOGI(TAG, "=============================================");
    ESP_LOGI(TAG, "  ESP-Agent: Trợ lý nhắc việc (v%s)", FIRMWARE_VERSION);
    ESP_LOGI(TAG, "=============================================");
}

/* --------------------------------------------------------------------------
 * Khởi tạo NVS Flash
 * -------------------------------------------------------------------------- */
static esp_err_t init_nvs(void)
{
    ESP_LOGD(TAG, "Khởi tạo NVS Flash...");
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS cần erase, đang xóa...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

/* --------------------------------------------------------------------------
 * Telegram Polling Loop (chạy trong main task)
 * -------------------------------------------------------------------------- */
static void telegram_polling_loop(void)
{
    ESP_LOGI(TAG, "Hệ thống sẵn sàng! Lắng nghe tin nhắn...");

    telegram_message_t message;
    static char response[TELEGRAM_MSG_BUFFER_SIZE]; 

    while (1) {
        bool ready = wifi_manager_is_connected();
        gpio_set_level(SYSTEM_LED_GPIO, ready ? 0 : 1);

        if (!ready) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        esp_err_t err = telegram_bot_get_update(&message);
        if (err == ESP_ERR_NOT_FOUND) continue;

        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Lỗi kết nối Telegram: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(3000));
            continue;
        }

        /* Xử lý Callback Query (nút bấm) */
        if (message.is_callback) {
            // ... (giữ nguyên logic xử lý callback)
            if (strcmp(message.callback_query, "cmd_deadline_3d") == 0) {
                time_range_t range = time_utils_get_three_day_range();
                task_record_t *results = (task_record_t *)calloc(10, sizeof(task_record_t));
                int found = 0;
                if (results) {
                    task_database_query_by_time(range.start, range.end, NULL, "pending", results, 10, &found);
                    char rep[RESPONSE_BUFFER_SIZE];
                    if (found > 0) {
                        int w = snprintf(rep, sizeof(rep), "📅 **DEADLINE**\n───────────────\n");
                        for (int j = 0; j < found; j++) {
                            char db[32]; time_utils_format_date_short(results[j].due_time, db, sizeof(db));
                            APPEND_SNPRINTF(rep, sizeof(rep), w, "\n• [#%lu] %s\n  ⏳ Hạn: %s", (unsigned long)results[j].id, results[j].title, db);
                        }
                    } else {
                        snprintf(rep, sizeof(rep), "✅ Tuyệt vời! Bạn không có deadline nào trong 3 ngày tới.");
                    }
                    telegram_bot_send_message(message.chat_id, rep);
                    free(results);
                }
            } else if (strncmp(message.callback_query, "cf_sug|", 7) == 0) {
                const char *confirmed_cmd = message.callback_query + 7;
                memset(response, 0, sizeof(response));
                action_dispatcher_handle(confirmed_cmd, response, sizeof(response));
                if (strlen(response) > 0) telegram_bot_send_message(message.chat_id, response);
            } else if (strcmp(message.callback_query, "cmd_undo") == 0) {
                memset(response, 0, sizeof(response));
                action_undo_execute(response, sizeof(response));
                telegram_bot_send_message(message.chat_id, response);
            }
            continue;
        }

        if (strlen(message.text) == 0) continue;

        ESP_LOGI(TAG, ">>> Tin nhắn từ %s: %s", message.from_first_name, message.text);

        if (strcmp(message.text, "/status") == 0) {
            memset(response, 0, sizeof(response));
            token_tracker_format_status(response, sizeof(response));
        } else if (strcmp(message.text, "/last") == 0) {
            memset(response, 0, sizeof(response));
            char *last_json = (char *)malloc(JSON_BUFFER_SIZE);
            if (last_json != NULL) {
                action_dispatcher_get_last_json(last_json, JSON_BUFFER_SIZE);
                snprintf(response, sizeof(response), "🔍 <b>Action JSON cuối cùng:</b>\n\n<code>%s</code>", last_json);
                free(last_json);
            } else {
                snprintf(response, sizeof(response), "⚠️ Lỗi RAM.");
            }
        } else if (strcmp(message.text, "/v") == 0) {
            snprintf(response, sizeof(response), "🏷️ <b>Version:</b> %s", FIRMWARE_VERSION);
        } else if (strcmp(message.text, "/undo") == 0) {
            memset(response, 0, sizeof(response));
            action_undo_execute(response, sizeof(response));
        } else if (strcmp(message.text, "/deadline") == 0) {
            time_range_t range = time_utils_get_three_day_range();
            task_record_t *results = (task_record_t *)calloc(10, sizeof(task_record_t));
            int found = 0;
            if (results) {
                task_database_query_by_time(range.start, range.end, NULL, "pending", results, 10, &found);
                if (found > 0) {
                    int w = snprintf(response, sizeof(response), "📅 <b>DEADLINE</b>\n");
                    for (int j = 0; j < found; j++) {
                        char db[32]; time_utils_format_date_short(results[j].due_time, db, sizeof(db));
                        APPEND_SNPRINTF(response, sizeof(response), w, "\n• <b>[#%" PRIu32 "] %s</b> (<i>%s</i>)", results[j].id, results[j].title, db);
                    }
                } else {
                    snprintf(response, sizeof(response), "✅ Không có deadline.");
                }
                free(results);
            }
        } else if (strncmp(message.text, "/t", 2) == 0) {
            // ... (giữ nguyên logic hiển thị task)
            memset(response, 0, sizeof(response));
            const char *args = message.text + 2;
            while (*args == ' ') args++;
            if (strlen(args) == 0) {
                snprintf(response, sizeof(response), "⚠️ Cú pháp: /t <id1>");
            } else {
                task_database_update_overdue();
                int written = 0, found = 0;
                char buf[256]; strncpy(buf, args, sizeof(buf)-1); buf[sizeof(buf)-1]='\0';
                for (int k=0; buf[k]; k++) if (buf[k]==','||buf[k]==';') buf[k]=' ';
                char *token = strtok(buf, " ");
                while (token != NULL && (size_t)written < sizeof(response) - 500) {
                    uint32_t tid = (uint32_t)atoi(token);
                    if (tid > 0) {
                        task_record_t task;
                        if (task_database_read(tid, &task) == ESP_OK) {
                            if (found > 0) APPEND_SNPRINTF(response, sizeof(response), written, "\n\n");
                            char detail_buf[800];
                            format_task_detail_full(&task, detail_buf, sizeof(detail_buf));
                            APPEND_SNPRINTF(response, sizeof(response), written, "%s", detail_buf);
                            found++;
                        }
                    }
                    token = strtok(NULL, " ");
                }
                if (found == 0) snprintf(response, sizeof(response), "❓ Không tìm thấy ID");
            }
        } else {
            memset(response, 0, sizeof(response));
            err = action_dispatcher_handle(message.text, response, sizeof(response));
            if (err == ESP_OK && strncmp(response, "SUGGEST|", 8) == 0) {
                char *suggestion = response + 8;
                char prompt_buf[RESPONSE_BUFFER_SIZE];
                snprintf(prompt_buf, sizeof(prompt_buf), "🤔 Có phải bạn muốn:\n\"<i>%.200s</i>\"?", suggestion);
                char cb_data[64]; snprintf(cb_data, sizeof(cb_data), "cf_sug|%.50s", suggestion);
                telegram_bot_send_inline_keyboard(message.chat_id, prompt_buf, "✅ Đúng", cb_data);
                memset(response, 0, sizeof(response));
            }
        }

        if (strlen(response) > 0) {
            err = telegram_bot_send_message(message.chat_id, response);
            if (err == ESP_OK) ESP_LOGI(TAG, "<<< Đã gửi response");
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/* --------------------------------------------------------------------------
 * Khởi tạo LED hệ thống
 * -------------------------------------------------------------------------- */
static void init_system_led(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << SYSTEM_LED_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    gpio_set_level(SYSTEM_LED_GPIO, 1); // Mặc định tắt (High)
}

/* --------------------------------------------------------------------------
 * Entry Point: app_main
 * -------------------------------------------------------------------------- */
void app_main(void)
{
    /* Hook log vào ring buffer (phải gọi TRƯỚC mọi ESP_LOG) */
    log_server_init();

    /* Thiết lập Log: Chỉ hiện thông tin từ App, ẩn log rác hệ thống */
    esp_log_level_set("*", ESP_LOG_WARN);
    esp_log_level_set("esp_agent", ESP_LOG_INFO);
    esp_log_level_set("telegram_bot", ESP_LOG_INFO);
    esp_log_level_set("dispatcher", ESP_LOG_INFO);
    esp_log_level_set("task_db", ESP_LOG_INFO);
    esp_log_level_set("firebase_sync", ESP_LOG_INFO);
    esp_log_level_set("openai_client", ESP_LOG_INFO);
    esp_log_level_set("token_tracker", ESP_LOG_INFO);
    esp_log_level_set("log_server", ESP_LOG_INFO);
    esp_log_level_set("action_search", ESP_LOG_INFO);
    esp_log_level_set("action_create", ESP_LOG_INFO);
    esp_log_level_set("action_update", ESP_LOG_INFO);
    esp_log_level_set("action_delete", ESP_LOG_INFO);
    esp_log_level_set("action_complete", ESP_LOG_INFO);
    esp_log_level_set("action_query", ESP_LOG_INFO);
    esp_log_level_set("action_summary", ESP_LOG_INFO);
    esp_log_level_set("action_detail", ESP_LOG_INFO);
    esp_log_level_set("action_undo", ESP_LOG_INFO);
    esp_log_level_set("vector_search", ESP_LOG_INFO);

    init_system_led();
    print_banner();

    /* Khởi tạo màn hình OLED (sớm nhất để show boot progress) */
    if (display_init(DISPLAY_SDA_GPIO, DISPLAY_SCL_GPIO) != ESP_OK) {
        ESP_LOGW(TAG, "OLED init thất bại, tiếp tục không màn hình");
    }
    display_boot_progress(10, "Starting...");

    /* Bước 1: Khởi tạo NVS */
    ESP_ERROR_CHECK(init_nvs());

    /* Bước 2: Kết nối WiFi */
    display_boot_progress(30, "Ket noi Wifi");
    ESP_LOGD(TAG, "Kết nối WiFi...");
    esp_err_t err = wifi_manager_init();
    if (err != ESP_OK) {
        display_boot_progress(30, "WiFi Error!");
        ESP_LOGE(TAG, "WiFi thất bại! Khởi động lại sau 10 giây...");
        vTaskDelay(pdMS_TO_TICKS(10000));
        esp_restart();
    }

    /* Đồng bộ thời gian SNTP ngay sau khi có mạng (Hàm này tự cập nhật % nội bộ) */
    wifi_manager_start_sntp();

    /* Bước 3: Khởi tạo Database (SPIFFS) */
    display_boot_progress(65, "Khoi tao SPIFFS");
    ESP_LOGD(TAG, "Khởi tạo database...");
    err = task_database_init();
    if (err != ESP_OK) {
        display_boot_progress(65, "DB Error!");
        ESP_LOGE(TAG, "Database thất bại! Khởi động lại...");
        vTaskDelay(pdMS_TO_TICKS(5000));
        esp_restart();
    }

#if ENABLE_FIREBASE_SYNC
    /* Khởi tạo Firebase Sync Background Queue */
    display_boot_progress(75, "Dong bo du lieu");
    firebase_sync_init();

    /* PULL ON BOOT: Nạp lại CSDL từ Cloud nếu ổ chứa nội gián SPIFFS đang trống */
    if (task_database_get_index()->count == 0) {
        ESP_LOGI(TAG, "Database trống, đang tải từ Cloud...");
        firebase_sync_download_all();
    } else {
        /* PUSH ON BOOT */
        firebase_sync_upload_all();
    }
    display_boot_progress(80, "Done Sync.");
#endif

#if ENABLE_OPENAI_CLIENT
    /* Kiểm tra và khôi phục Vector Embedding bị thiếu (Hàm này tự cập nhật % nội bộ) */
    vector_search_audit_and_rebuild();
#endif

    /* Khởi tạo Token Tracker */
    token_tracker_init();

    /* Khởi động Log Web Server (sau khi WiFi + IP ổn định) */
    log_server_start();

#if ENABLE_TELEGRAM_BOT
    /* Bước 4: Khởi tạo Telegram Bot */
    display_boot_progress(95, "Ket noi Telegram");
    err = telegram_bot_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Telegram Bot thất bại!");
    }
#endif

    /* Bước 5: Bắt đầu Reminder Scheduler */
    err = reminder_scheduler_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Reminder Scheduler thất bại");
    }

    /* Chuyển sang màn hình Idle + start display task */
    display_boot_progress(100, "SAN SANG");
    vTaskDelay(pdMS_TO_TICKS(500));
    display_show_idle();
    display_start_task();

    /* Đã sẵn sàng -> Bật LED (0 = ON cho ESP32-C3 Super Mini) */
    if (wifi_manager_is_connected()) {
        gpio_set_level(SYSTEM_LED_GPIO, 0);
    }

    /* Main loop: Telegram long polling */
    telegram_polling_loop();
}
