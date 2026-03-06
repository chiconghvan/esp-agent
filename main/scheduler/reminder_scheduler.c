/**
 * ===========================================================================
 * @file reminder_scheduler.c
 * @brief Triển khai bộ lập lịch nhắc nhở tự động (Sửa lỗi Stack Overflow)
 * ===========================================================================
 */

#include "reminder_scheduler.h"
#include "task_database.h"
#include "telegram_bot.h"
#include "response_formatter.h"
#include "time_utils.h"
#include "display_manager.h"
#include "config.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "scheduler";
static TaskHandle_t scheduler_task_handle = NULL;
static bool scheduler_running = false;

#define MAX_QUERY_RESULTS 5

static void reminder_task(void *arg)
{
    ESP_LOGI(TAG, "Reminder task bắt đầu (mỗi %d giây)", REMINDER_CHECK_INTERVAL_SEC);

    /* Cấp phát bộ nhớ Heap thay vì Stack để tránh Stack Overflow (Guru Meditation) */
    task_record_t *query_results = (task_record_t *)calloc(MAX_QUERY_RESULTS, sizeof(task_record_t));
    if (query_results == NULL) {
        ESP_LOGE(TAG, "Không đủ bộ nhớ Heap cho scheduler!");
        vTaskDelete(NULL);
        return;
    }

    while (scheduler_running) {
        vTaskDelay(pdMS_TO_TICKS(REMINDER_CHECK_INTERVAL_SEC * 1000));
        if (!scheduler_running) break;

        time_t now = time_utils_get_now();

        /* === Phần 1: Gửi reminders thủ công === */
        int due_count = 0;
        if (task_database_query_due_reminders(query_results, MAX_QUERY_RESULTS, &due_count) == ESP_OK && due_count > 0) {
            for (int i = 0; i < due_count; i++) {
                char notification[RESPONSE_BUFFER_SIZE];
                format_reminder(&query_results[i], notification, sizeof(notification));
                if (telegram_bot_send_default(notification) == ESP_OK) {
                    display_show_alert(query_results[i].id, query_results[i].title, "Remind!", 0);
                    query_results[i].reminder = 0;
                    task_database_update(&query_results[i]);
                }
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
        }

        /* === Phần 2: Cảnh báo sắp đến hạn (Hôm nay - Mai - Kia) === */
        int approaching_count = 0;
        time_range_t range_3d = time_utils_get_three_day_range();
        if (task_database_query_by_time(now, range_3d.end, NULL, "pending", query_results, MAX_QUERY_RESULTS, &approaching_count) == ESP_OK) {
            for (int i = 0; i < approaching_count; i++) {
                if (query_results[i].reminder > 0) continue;
                char msg[RESPONSE_BUFFER_SIZE];
                time_t remaining = query_results[i].due_time - now;
                int hours = (int)(remaining / 3600);
                if (hours <= 24) snprintf(msg, sizeof(msg), "⚠️ SẮP ĐẾN HẠN!\n📌 [#%lu] %s\n⏳ Còn %d giờ!", (unsigned long)query_results[i].id, query_results[i].title, hours);
                else snprintf(msg, sizeof(msg), "🔔 Nhắc deadline:\n📌 [#%lu] %s\n⏳ Còn %d ngày", (unsigned long)query_results[i].id, query_results[i].title, hours / 24);
                
                if (telegram_bot_send_default(msg) == ESP_OK) {
                    struct tm tm_due; localtime_r(&query_results[i].due_time, &tm_due);
                    char ds[32]; snprintf(ds, sizeof(ds), "%02d:%02d %02d/%02d", tm_due.tm_hour, tm_due.tm_min, tm_due.tm_mday, tm_due.tm_mon + 1);
                    display_show_alert(query_results[i].id, query_results[i].title, ds, hours / 24);
                    query_results[i].reminder = query_results[i].due_time;
                    task_database_update(&query_results[i]);
                }
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
        }

        /* === Phần 3: Sự kiện hàng ngày === */
        struct tm tm_now; localtime_r(&now, &tm_now);
        tm_now.tm_hour = 0; tm_now.tm_min = 0; tm_now.tm_sec = 0;
        time_t today_start = mktime(&tm_now);
        time_t today_end = today_start + 86400 - 1;

        const task_index_t *idx = task_database_get_index();
        for (int i = 0; i < idx->count; i++) {
            const task_index_entry_t *e = &idx->entries[i];
            if (strcmp(e->status, "pending") != 0 || e->start_time == 0 || e->due_time == 0) continue;
            if (today_start < e->start_time || today_start > e->due_time) continue;
            if (e->reminder >= today_start && e->reminder <= today_end) continue;

            task_record_t ev; /* Biến đơn lẻ thì không sao */
            if (task_database_read(e->id, &ev) == ESP_OK) {
                int d = (int)((today_start - ev.start_time) / 86400) + 1;
                int tot = (int)((ev.due_time - ev.start_time) / 86400) + 1;
                char m[RESPONSE_BUFFER_SIZE];
                snprintf(m, sizeof(m), "📅 Sự kiện (ngày %d/%d):\n📌 [#%lu] %s", d, tot, (unsigned long)ev.id, ev.title);
                if (telegram_bot_send_default(m) == ESP_OK) {
                    display_show_alert(ev.id, ev.title, "Đang diễn ra", tot - d);
                    ev.reminder = now;
                    task_database_update(&ev);
                }
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
        }

        /* === Phần 4: Nhắc nhở quá hạn lúc 16:00 === */
        static int s_last_nudge_yday = -1;
        struct tm tm_n; localtime_r(&now, &tm_n);
        if (tm_n.tm_hour == 16 && tm_n.tm_min <= 2 && s_last_nudge_yday != tm_n.tm_yday) {
            task_database_update_overdue(); // Cập nhật lại status
            int od_count = 0;
            if (task_database_query_by_time(1, now, NULL, "overdue", query_results, MAX_QUERY_RESULTS, &od_count) == ESP_OK && od_count > 0) {
                char rep[RESPONSE_BUFFER_SIZE];
                int w = snprintf(rep, sizeof(rep), "⏳ **NHẮC NHỞ 16:00**\nBạn còn %d việc quá hạn chưa xong:\n", od_count);
                for (int j = 0; j < od_count && w < sizeof(rep) - 100; j++)
                    w += snprintf(rep + w, sizeof(rep) - w, "\n%d. [#%lu] %s", j+1, (unsigned long)query_results[j].id, query_results[j].title);
                strncat(rep, "\n\n💪 Cố gắng hoàn thành nhé!", sizeof(rep) - strlen(rep) - 1);
                telegram_bot_send_default(rep);
                s_last_nudge_yday = tm_n.tm_yday;
            }
        }

        /* === Phần 5: Daily Briefing lúc 8:00 sáng === */
        static int s_last_briefing_yday = -1;
        if (tm_n.tm_hour == 8 && tm_n.tm_min <= 2 && s_last_briefing_yday != tm_n.tm_yday) {
            time_range_t today = time_utils_get_today_range();
            int today_count = 0;
            if (task_database_query_by_time(today.start, today.end, NULL, "pending", query_results, MAX_QUERY_RESULTS, &today_count) == ESP_OK && today_count > 0) {
                char brief[RESPONSE_BUFFER_SIZE];
                int w = snprintf(brief, sizeof(brief), "☀️ **CHÀO BUỔI SÁNG!**\nHôm nay bạn có %d việc cần xử lý:\n", today_count);
                for (int j = 0; j < today_count && w < sizeof(brief) - 100; j++)
                    w += snprintf(brief + w, sizeof(brief) - w, "\n%d. [#%lu] %s", j+1, (unsigned long)query_results[j].id, query_results[j].title);
                
                int64_t chat_id = strtoll(TELEGRAM_CHAT_ID, NULL, 10);
                telegram_bot_send_inline_keyboard(chat_id, brief, "📅 Xem Deadline sắp tới", "cmd_deadline_3d");
                s_last_briefing_yday = tm_n.tm_yday;
            } else {
                s_last_briefing_yday = tm_n.tm_yday; // Không có việc thì vẫn đánh dấu đã kiểm tra
            }
        }
    }
    free(query_results);
    scheduler_task_handle = NULL;
    vTaskDelete(NULL);
}

esp_err_t reminder_scheduler_start(void) {
    if (scheduler_task_handle) return ESP_OK;
    scheduler_running = true;
    return (xTaskCreate(reminder_task, "reminder", 8192, NULL, 5, &scheduler_task_handle) == pdPASS) ? ESP_OK : ESP_FAIL;
}

esp_err_t reminder_scheduler_stop(void) {
    scheduler_running = false;
    return ESP_OK;
}
