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
#include "esp_ota_ops.h"
#include "driver/gpio.h"
#include <stdlib.h>

static const char *TAG = "esp_agent";

/* --------------------------------------------------------------------------
 * Startup banner
 * -------------------------------------------------------------------------- */
static void print_banner(void)
{
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "=============================================");
    ESP_LOGI(TAG, "  ESP-Agent: Trợ lý nhắc việc thông minh");
    ESP_LOGI(TAG, "  Target: ESP32-C3 Super Mini");
    ESP_LOGI(TAG, "  Version: %s", FIRMWARE_VERSION);
    ESP_LOGI(TAG, "=============================================");
    ESP_LOGI(TAG, "");
}

/* --------------------------------------------------------------------------
 * Khởi tạo NVS Flash
 * -------------------------------------------------------------------------- */
static esp_err_t init_nvs(void)
{
    ESP_LOGI(TAG, "[1/5] Khởi tạo NVS Flash...");

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS cần erase, đang xóa...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "[1/5] NVS Flash OK");
    }
    return err;
}

/* --------------------------------------------------------------------------
 * Telegram Polling Loop (chạy trong main task)
 * -------------------------------------------------------------------------- */
static void telegram_polling_loop(void)
{
    ESP_LOGI(TAG, "Bắt đầu Telegram polling loop...");
    ESP_LOGI(TAG, "Đang lắng nghe tin nhắn...");

    telegram_message_t message;
    static char response[TELEGRAM_MSG_BUFFER_SIZE]; // Dùng static (RAM tĩnh) thay vì Stack để tránh Stack Overflow

    while (1) {
        /* Cập nhật đèn LED trạng thái (Sáng khi WiFi OK) */
        bool ready = wifi_manager_is_connected();
        gpio_set_level(SYSTEM_LED_GPIO, ready ? 0 : 1); // 0 = ON (Active Low)

        /* Kiểm tra WiFi */
        if (!ready) {
            ESP_LOGW(TAG, "WiFi mất kết nối, chờ...");
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        /* Long polling: nhận tin nhắn mới */
        esp_err_t err = telegram_bot_get_update(&message);

        if (err == ESP_ERR_NOT_FOUND) {
            /* Timeout, không có tin nhắn mới → tiếp tục polling */
            continue;
        }

        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Lỗi nhận tin nhắn: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(3000));
            continue;
        }

        /* Xử lý Callback Query (nút bấm) trước */
        if (message.is_callback) {
            if (strcmp(message.callback_query, "cmd_deadline_3d") == 0) {
                time_range_t range = time_utils_get_three_day_range();
                /* Cấp phát Heap để tránh Stack Overflow (4KB) */
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
                /* Xác nhận gợi ý của AI */
                const char *confirmed_cmd = message.callback_query + 7;
                memset(response, 0, sizeof(response));
                action_dispatcher_handle(confirmed_cmd, response, sizeof(response));
                if (strlen(response) > 0) {
                    telegram_bot_send_message(message.chat_id, response);
                }
            } else if (strcmp(message.callback_query, "cmd_undo") == 0) {
                /* Xử lý nút Undo */
                memset(response, 0, sizeof(response));
                action_undo_execute(response, sizeof(response));
                telegram_bot_send_message(message.chat_id, response);
            }
            continue;
        }

        /* Bỏ qua tin nhắn rỗng */
        if (strlen(message.text) == 0) {
            continue;
        }

        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, ">>> Tin nhắn từ %s: %s", message.from_first_name, message.text);

        /* Kiểm tra lệnh đặc biệt /status, /last, /t, /deadline, /v */
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
                snprintf(response, sizeof(response), "⚠️ Lỗi: Không đủ RAM để hiển thị JSON.");
            }
        } else if (strcmp(message.text, "/v") == 0) {
            snprintf(response, sizeof(response), "🏷️ <b>Firmware Version:</b> %s\n🔧 <i>Target:</i> ESP32-C3 Super Mini", FIRMWARE_VERSION);
        } else if (strcmp(message.text, "/undo") == 0) {
            /* Lệnh /undo thủ công */
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
                        APPEND_SNPRINTF(response, sizeof(response), w, 
                                      "\n• <b>[#%" PRIu32 "] %s</b> (<i>%s</i>)", results[j].id, results[j].title, db);
                    }
                } else {
                    snprintf(response, sizeof(response), "✅ Hiện không có deadline nào sắp tới.");
                }
                free(results);
            }
        } else if (strncmp(message.text, "/t", 2) == 0 && (message.text[2] == ' ' || message.text[2] == '\0' || (message.text[2] >= '0' && message.text[2] <= '9'))) {
            /* Lệnh /t <id1> <id2> ... - hiển thị chi tiết task */
            memset(response, 0, sizeof(response));
            const char *args = message.text + 2;
            /* Bỏ qua khoảng trắng đầu */
            while (*args == ' ') args++;

            if (strlen(args) == 0) {
                snprintf(response, sizeof(response),
                    "⚠️ Cú pháp: /t <id1> <id2> ...\n"
                    "Ví dụ: /t 1 2 3\n"
                    "Hoặc: /t 1,2,3");
            } else {
                task_database_update_overdue();
                int written = 0;
                int found = 0;
                /* Parse IDs: hỗ trợ cách bằng dấu cách, phẩy, chấm phẩy */
                char buf[256];
                strncpy(buf, args, sizeof(buf) - 1);
                buf[sizeof(buf) - 1] = '\0';

                /* Thay dấu phẩy, chấm phẩy thành dấu cách */
                for (int k = 0; buf[k]; k++) {
                    if (buf[k] == ',' || buf[k] == ';') buf[k] = ' ';
                }

                char *token = strtok(buf, " ");
                while (token != NULL && (size_t)written < sizeof(response) - 300) {
                    uint32_t tid = (uint32_t)atoi(token);
                    if (tid > 0) {
                        task_record_t task;
                        if (task_database_read(tid, &task) == ESP_OK) {
                            if (found > 0) {
                                APPEND_SNPRINTF(response, sizeof(response), written,
                                    "\n──────────\n");
                            }
                            char reminder_buf[64];
                            char due_buf[64];
                            char created_buf[64];
                            char repeat_buf[64];
                            time_utils_format_vietnamese(task.due_time, due_buf, sizeof(due_buf));
                            time_utils_format_vietnamese(task.created_at, created_buf, sizeof(created_buf));
                            time_utils_format_vietnamese(task.reminder, reminder_buf, sizeof(reminder_buf));
                            time_utils_format_repeat(task.repeat, task.repeat_interval, repeat_buf, sizeof(repeat_buf));

                            APPEND_SNPRINTF(response, sizeof(response), written,
                                "📌 <b>[#%" PRIu32 "] %s</b>\n"
                                "<i>🏷️ Phân loại:</i> %s\n"
                                "<i>📅 Thời hạn:</i> %s\n"
                                "<i>🔁 Lặp lại:</i> %s\n"
                                "<i>🕐 Tạo lúc:</i> %s\n"
                                "<i>⏰ Nhắc nhở:</i> %s\n"
                                "<i>📝 Ghi chú:</i> %s\n"
                                "🔵 <i>Trạng thái:</i> %s",
                                task.id, task.title,
                                (strcmp(task.type, "meeting") == 0) ? "Cuộc họp" :
                                (strcmp(task.type, "report") == 0) ? "Báo cáo" :
                                (strcmp(task.type, "reminder") == 0) ? "Nhắc nhở" :
                                (strcmp(task.type, "event") == 0) ? "Sự kiện" :
                                (strcmp(task.type, "anniversary") == 0) ? "Kỉ niệm" : "Khác",
                                due_buf, repeat_buf, created_buf, reminder_buf,
                                strlen(task.notes) > 0 ? task.notes : "(không có)",
                                (strcmp(task.status, "done") == 0) ? "✅ Hoàn thành" :
                                (strcmp(task.status, "cancelled") == 0) ? "❌ Đã hủy" :
                                (strcmp(task.status, "overdue") == 0) ? "⏳ Quá hạn" :
                                "🔵 Đang thực hiện");
                            found++;
                        }
                    }
                    token = strtok(NULL, " ");
                }

                if (found == 0) {
                    snprintf(response, sizeof(response),
                        "❓ Không tìm thấy task nào với ID đã cho");
                }
            }
        } else {
            /* Xử lý tin nhắn → dispatch action → nhận response */
            memset(response, 0, sizeof(response));
            err = action_dispatcher_handle(message.text, response, sizeof(response));

            if (err == ESP_OK && strncmp(response, "SUGGEST|", 8) == 0) {
                /* AI gợi ý câu hỏi thay vì chitchat */
                char *suggestion = response + 8;
                char prompt_buf[RESPONSE_BUFFER_SIZE];
                snprintf(prompt_buf, sizeof(prompt_buf), "🤔 Có phải ý bạn là:\n\"<i>%.200s</i>\"?", suggestion);
                
                char cb_data[64];
                snprintf(cb_data, sizeof(cb_data), "cf_sug|%.50s", suggestion);
                telegram_bot_send_inline_keyboard(message.chat_id, prompt_buf, "✅ Đúng vậy", cb_data);
                memset(response, 0, sizeof(response)); // Không gửi tin nhắn thường nữa
            }

            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Xử lý tin nhắn thất bại");
            }
        }

        /* Gửi response về Telegram */
        if (strlen(response) > 0) {
            err = telegram_bot_send_message(message.chat_id, response);
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "<<< Đã gửi response");
            } else {
                ESP_LOGE(TAG, "<<< Gửi response thất bại");
            }
        }

        /* Delay nhỏ để tránh rate limit */
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
    init_system_led();
    print_banner();

    /* Khởi tạo màn hình OLED (sớm nhất để show boot progress) */
    if (display_init(6, 7) != ESP_OK) {
        ESP_LOGW(TAG, "OLED init thất bại, tiếp tục không màn hình");
    }
    display_boot_progress(5, "Khoi tao NVS...");

    /* Bước 1: Khởi tạo NVS */
    ESP_ERROR_CHECK(init_nvs());
    display_boot_progress(20, "Ket noi WiFi...");

    /* Bước 2: Kết nối WiFi + SNTP */
    ESP_LOGI(TAG, "[2/5] Kết nối WiFi...");
    esp_err_t err = wifi_manager_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi thất bại! Khởi động lại sau 10 giây...");
        display_boot_progress(20, "WiFi THAT BAI!");
        vTaskDelay(pdMS_TO_TICKS(10000));
        esp_restart();
    }
    ESP_LOGI(TAG, "[2/5] WiFi OK");
    display_boot_progress(40, "Dong bo SNTP...");

    /* Đồng bộ thời gian */
    ESP_LOGI(TAG, "[2/5] Đồng bộ thời gian SNTP...");
    err = wifi_manager_start_sntp();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SNTP timeout, tiếp tục với thời gian chưa chính xác");
    }
    display_boot_progress(55, "Khoi tao Database...");

    /* Bước 3: Khởi tạo Database (SPIFFS) */
    ESP_LOGI(TAG, "[3/5] Khởi tạo database...");
    err = task_database_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Database thất bại! Khởi động lại...");
        display_boot_progress(55, "DB THAT BAI!");
        vTaskDelay(pdMS_TO_TICKS(5000));
        esp_restart();
    }
    ESP_LOGI(TAG, "[3/5] Database OK");
    display_boot_progress(60, "Khoi tao Firebase Sync...");

    /* Khởi tạo Firebase Sync Background Queue */
    firebase_sync_init();

    /* PULL ON BOOT: Nạp lại CSDL từ Cloud nếu ổ chứa nội gián SPIFFS đang trống */
    if (task_database_get_index()->count == 0) {
        display_boot_progress(65, "Tuyen vao Firebase...");
        ESP_LOGI(TAG, "Database trống, bắt đầu tải từ Firebase xuống...");
        if (firebase_sync_download_all() == ESP_OK) {
            ESP_LOGI(TAG, "Khôi phục dữ liệu từ Firebase thành công!");
        }
    } else {
        /* PUSH ON BOOT: Nếu local có data, ta chủ động đẩy lên Cloud để đảm bảo Cloud luôn cập nhật */
        ESP_LOGI(TAG, "Phát hiện %d task local, đang đồng bộ lên Cloud...", task_database_get_index()->count);
        firebase_sync_upload_all();
    }

    display_boot_progress(70, "Token Tracker...");

    /* Khởi tạo Token Tracker */
    token_tracker_init();
    display_boot_progress(80, "Telegram Bot...");

    /* Bước 4: Khởi tạo Telegram Bot */
    ESP_LOGI(TAG, "[4/5] Khởi tạo Telegram Bot...");
    err = telegram_bot_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Telegram Bot thất bại!");
    }
    ESP_LOGI(TAG, "[4/5] Telegram Bot OK");
    display_boot_progress(90, "Reminder...");

    /* Bước 5: Bắt đầu Reminder Scheduler */
    ESP_LOGI(TAG, "[5/5] Bắt đầu Reminder Scheduler...");
    err = reminder_scheduler_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Reminder Scheduler thất bại, nhắc nhở sẽ không hoạt động");
    }
    ESP_LOGI(TAG, "[5/5] Reminder Scheduler OK");
    display_boot_progress(100, "San sang!");

    /* Hiển thị thông tin hệ thống */
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "=============================================");
    ESP_LOGI(TAG, "  HỆ THỐNG SẴN SÀNG!");
    ESP_LOGI(TAG, "  Gửi tin nhắn qua Telegram để bắt đầu.");
    ESP_LOGI(TAG, "  Free heap: %lu bytes",
             (unsigned long)esp_get_free_heap_size());
    ESP_LOGI(TAG, "=============================================");
    ESP_LOGI(TAG, "");

    /* Chuyển sang màn hình Idle + start display task */
    vTaskDelay(pdMS_TO_TICKS(500));
    display_show_idle();
    display_start_task();

    /* Đã sẵn sàng -> Bật LED (0 = ON cho ESP32-C3 Super Mini) */
    if (wifi_manager_is_connected()) {
        gpio_set_level(SYSTEM_LED_GPIO, 0);
    }

    /* Bỏ qua gửi thông báo Telegram khởi động để tránh làm phiền */

    /* Main loop: Telegram long polling */
    telegram_polling_loop();
}
