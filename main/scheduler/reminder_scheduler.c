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

        /* === Phần 1: Gửi reminders thủ công (Hẹn giờ chính xác) === */
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

        /* === Phần 1b: Thông báo khi đến Start Time (Dành cho task có khoảng thời gian) === */
        const task_index_t *idx_start = task_database_get_index();
        for (int i = 0; i < idx_start->count; i++) {
            const task_index_entry_t *e = &idx_start->entries[i];
            if (strcmp(e->status, "pending") != 0 || e->start_time == 0) continue;
            
            // Nếu đã qua start_time
            if (now >= e->start_time) {
                task_record_t ev;
                if (task_database_read(e->id, &ev) == ESP_OK) {
                    // Kiểm tra xem đã thông báo Start Time chưa bằng cách tìm tag ẩn "[S]" trong notes
                    if (strstr(ev.notes, "[S]") == NULL) {
                        char m[RESPONSE_BUFFER_SIZE];
                        char time_buf[64];
                        time_utils_format_vietnamese(ev.start_time, time_buf, sizeof(time_buf));
                        snprintf(m, sizeof(m), "🎬 <b>BẮT ĐẦU: %s</b>\n📌 [#%lu] %s\n⏰ Bắt đầu lúc: %s", 
                                 ev.type, (unsigned long)ev.id, ev.title, time_buf);
                        
                        if (telegram_bot_send_default(m) == ESP_OK) {
                            display_show_alert(ev.id, ev.title, "Started!", 0);
                            
                            // Đánh dấu đã nhắc bằng cách thêm tag vào cuối notes
                            if (strlen(ev.notes) + 5 < sizeof(ev.notes)) {
                                strcat(ev.notes, " [S]");
                                task_database_update(&ev);
                            }
                        }
                    }
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
                    display_show_alert(query_results[i].id, query_results[i].title, ds, (int32_t)remaining);
                    query_results[i].reminder = query_results[i].due_time;
                    task_database_update(&query_results[i]);
                }
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
        }

        /* === Phần 3: Sự kiện hàng ngày (Đã bỏ để gộp vào Daily Briefing 8:00) === */
        // Logic cũ gửi tin nhắn riêng lẻ mỗi ngày cho event dài ngày đã được thay thế.

        /* === Phần 4: Nhắc nhở quá hạn lúc 16:00 === */
        static int s_last_nudge_yday = -1;
        struct tm tm_n; localtime_r(&now, &tm_n);
        if (tm_n.tm_hour == 16 && tm_n.tm_min <= 2 && s_last_nudge_yday != tm_n.tm_yday) {
            task_database_update_overdue(); // Cập nhật lại status
            int od_count = 0;
            if (task_database_query_by_time(1, now, NULL, "overdue", query_results, MAX_QUERY_RESULTS, &od_count) == ESP_OK && od_count > 0) {
                char rep[RESPONSE_BUFFER_SIZE];
                int w = snprintf(rep, sizeof(rep), "⏳ <b>NHẮC NHỞ 16:00</b>\nBạn còn %d việc quá hạn chưa xong:\n", od_count);
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
            task_database_update_overdue(); // Cập nhật trạng thái trước khi gửi briefing
            time_range_t today = time_utils_get_today_range();
            
            char brief[RESPONSE_BUFFER_SIZE];
            int w = snprintf(brief, sizeof(brief), "☀️ <b>CHÀO BUỔI SÁNG!</b>\nHôm nay bạn có các việc sau:\n");
            int task_count = 0;

            const task_index_t *idx = task_database_get_index();
            for (int i = 0; i < idx->count && task_count < 15; i++) {
                const task_index_entry_t *e = &idx->entries[i];
                if (strcmp(e->status, "pending") != 0 && strcmp(e->status, "overdue") != 0) continue;

                bool include_task = false;
                // 1. Task quá hạn
                if (strcmp(e->status, "overdue") == 0) include_task = true;
                // 2. Task đến hạn hôm nay
                else if (e->due_time >= today.start && e->due_time <= today.end) include_task = true;
                // 3. Task bắt đầu hôm nay
                else if (e->start_time >= today.start && e->start_time <= today.end) include_task = true;
                // 4. Task đang diễn ra (bắt đầu trước, kết thúc sau)
                else if (e->start_time > 0 && e->start_time < today.start && e->due_time > today.end) include_task = true;

                if (include_task) {
                    task_record_t t;
                    if (task_database_read(e->id, &t) == ESP_OK) {
                        task_count++;
                        char time_info[64] = "";
                        if (strcmp(t.status, "overdue") == 0) {
                            snprintf(time_info, sizeof(time_info), " [⚠️ QUÁ HẠN]");
                        } else if (t.start_time > 0 && t.due_time > 0) {
                            if (t.start_time < today.start) {
                                int day = (int)((today.start - t.start_time) / 86400) + 1;
                                int total = (int)((t.due_time - t.start_time) / 86400) + 1;
                                snprintf(time_info, sizeof(time_info), " (Ngày %d/%d)", day, total);
                            } else {
                                struct tm tm_st; localtime_r(&t.start_time, &tm_st);
                                snprintf(time_info, sizeof(time_info), " [Bắt đầu %02d:%02d]", tm_st.tm_hour, tm_st.tm_min);
                            }
                        } else if (t.due_time > 0) {
                            struct tm tm_due; localtime_r(&t.due_time, &tm_due);
                            snprintf(time_info, sizeof(time_info), " [Hạn %02d:%02d]", tm_due.tm_hour, tm_due.tm_min);
                        }
                        
                        w += snprintf(brief + w, sizeof(brief) - w, "\n%d. [#%lu] %s%s", 
                                     task_count, (unsigned long)t.id, t.title, time_info);
                    }
                }
            }

            if (task_count > 0) {
                int64_t chat_id = strtoll(TELEGRAM_CHAT_ID, NULL, 10);
                telegram_bot_send_inline_keyboard(chat_id, brief, "📅 Xem Deadline sắp tới", "cmd_deadline_3d");
            } else {
                // Nếu không có việc gì cụ thể, vẫn gửi lời chào
                telegram_bot_send_default("☀️ <b>CHÀO BUỔI SÁNG!</b>\nHôm nay bạn không có việc gì gấp. Chúc một ngày tốt lành!");
            }
            s_last_briefing_yday = tm_n.tm_yday;
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
