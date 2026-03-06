/**
 * ===========================================================================
 * @file response_formatter.c
 * @brief Triển khai format response tiếng Việt tự nhiên
 * ===========================================================================
 */

#include "response_formatter.h"
#include "action_dispatcher.h"
#include "time_utils.h"
#include "telegram_bot.h"
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

static const char *get_type_name_vn(const char *type)
{
    if (strcmp(type, "meeting") == 0)   return "Cuộc họp";
    if (strcmp(type, "report") == 0)    return "Báo cáo";
    if (strcmp(type, "reminder") == 0)  return "Nhắc nhở";
    if (strcmp(type, "event") == 0)     return "Sự kiện";
    if (strcmp(type, "anniversary") == 0) return "Kỉ niệm";
    return "Khác";
}

static const char *get_type_icon(const char *type)
{
    if (strcmp(type, "meeting") == 0)   return "🤝";
    if (strcmp(type, "report") == 0)    return "📄";
    if (strcmp(type, "reminder") == 0)  return "⏰";
    if (strcmp(type, "deadline") == 0)  return "🔴";
    if (strcmp(type, "event") == 0)     return "🎯";
    if (strcmp(type, "anniversary") == 0) return "🎂";
    return "📋";
}

static const char *get_status_icon(const char *status)
{
    if (strcmp(status, "done") == 0)       return "✅";
    if (strcmp(status, "cancelled") == 0)  return "❌";
    if (strcmp(status, "overdue") == 0)    return "⏳";
    return "🔵";
}

static int append_task_detail(const task_record_t *task, char *buffer, size_t remaining)
{
    char due_buf[64], created_buf[64], repeat_buf[64];
    time_utils_format_vietnamese(task->due_time, due_buf, sizeof(due_buf));
    time_utils_format_vietnamese(task->created_at, created_buf, sizeof(created_buf));
    time_utils_format_repeat(task->repeat, task->repeat_interval, repeat_buf, sizeof(repeat_buf));

    return snprintf(buffer, remaining,
        "📌 [#%" PRIu32 "] %s\n"
        "🏷️ Phân loại: %s\n"
        "📅 Thời hạn: %s\n"
        "🔁 Lặp lại: %s\n"
        "🕐 Tạo lúc: %s",
        task->id, task->title, get_type_name_vn(task->type), due_buf, repeat_buf, created_buf);
}

char *format_task_created(const task_record_t *task, char *buffer, size_t buffer_size)
{
    char reminder_buf[64];
    time_utils_format_vietnamese(task->reminder, reminder_buf, sizeof(reminder_buf));
    int written = snprintf(buffer, buffer_size, "✅ Đã tạo công việc mới:\n");
    if (written > 0 && (size_t)written < buffer_size)
        written += append_task_detail(task, buffer + written, buffer_size - written);
    if (written > 0 && (size_t)written < buffer_size)
        snprintf(buffer + written, buffer_size - written, "\n⏰ Nhắc nhở: %s", reminder_buf);
    return buffer;
}

char *format_task_list(const task_index_entry_t **matches, int count, const char *label,
                       char *buffer, size_t buffer_size)
{
    if (count == 0) {
        snprintf(buffer, buffer_size, "📭 Không có lịch nào %s", label);
        return buffer;
    }

    int written = snprintf(buffer, buffer_size, "📋 %s:\n───────────────\n", label);
    uint32_t context_ids[20];
    int context_count = 0;

    for (int i = 0; i < count; i++) {
        task_record_t t;
        if (task_database_read(matches[i]->id, &t) != ESP_OK) continue;

        if (context_count < 20) context_ids[context_count++] = t.id;

        char item[512];
        char due_buf[64], repeat_buf[64], created_buf[64];
        time_utils_format_vietnamese(t.due_time, due_buf, sizeof(due_buf));
        time_utils_format_date_short(t.created_at, created_buf, sizeof(created_buf));
        time_utils_format_repeat(t.repeat, t.repeat_interval, repeat_buf, sizeof(repeat_buf));

        int item_len = snprintf(item, sizeof(item),
            "%d. %s [#%" PRIu32 "] %s %s\n"
            "   🏷️ Phân loại: %s\n"
            "   📅 Hạn: %s\n"
            "   🔁 Lặp: %s\n"
            "   🕐 Tạo: %s\n\n",
            i + 1, get_type_icon(t.type), t.id, t.title,
            get_status_icon(t.status), get_type_name_vn(t.type),
            due_buf, repeat_buf, created_buf);

        if (written + item_len + 100 > buffer_size - 100) {
            telegram_bot_send_default(buffer);
            written = snprintf(buffer, buffer_size, "📋 %s (tiếp theo):\n───────────────\n", label);
        }
        strcat(buffer + written, item);
        written += item_len;
    }

    if ((size_t)written < buffer_size - 64)
        snprintf(buffer + written, buffer_size - written, "───────────────\n📊 Tổng cộng: %d task", count);

    dispatcher_set_context_tasks(context_ids, context_count);
    return buffer;
}

char *format_task_list_short(const task_index_entry_t **matches, int count, const char *label,
                             char *buffer, size_t buffer_size)
{
    if (count == 0) {
        snprintf(buffer, buffer_size, "📭 Không có %s nào.", label);
        return buffer;
    }

    int written = snprintf(buffer, buffer_size, "📄 %s:\n───────────────\n", label);
    uint32_t context_ids[20];
    int context_count = 0;

    for (int i = 0; i < count; i++) {
        task_record_t t;
        if (task_database_read(matches[i]->id, &t) != ESP_OK) continue;

        if (context_count < 20) context_ids[context_count++] = t.id;

        char item[256];
        int item_len = snprintf(item, sizeof(item), "%d. %s [#%" PRIu32 "] %s %s\n", 
                                i + 1, get_type_icon(t.type), t.id, 
                                t.title, get_status_icon(t.status));

        if (written + item_len + 100 > buffer_size - 100) {
            telegram_bot_send_default(buffer);
            written = snprintf(buffer, buffer_size, "📄 %s (tiếp theo):\n───────────────\n", label);
        }
        strcat(buffer + written, item);
        written += item_len;
    }

    if ((size_t)written < buffer_size - 64)
        snprintf(buffer + written, buffer_size - written, "───────────────\n📊 Tổng cộng: %d task", count);

    dispatcher_set_context_tasks(context_ids, context_count);
    return buffer;
}

char *format_task_count(int count, const char *label, char *buffer, size_t buffer_size)
{
    if (count == 0) snprintf(buffer, buffer_size, "📭 Không có %s nào.", label);
    else snprintf(buffer, buffer_size, "📊 Có %d %s.", count, label);
    return buffer;
}

char *format_task_completed(const task_record_t *task, char *buffer, size_t buffer_size)
{
    char completed_buf[64];
    time_utils_format_vietnamese(task->completed_at, completed_buf, sizeof(completed_buf));
    int written = snprintf(buffer, buffer_size, "✅ Đã hoàn thành:\n");
    if (written > 0 && (size_t)written < buffer_size)
        written += append_task_detail(task, buffer + written, buffer_size - written);
    if (written > 0 && (size_t)written < buffer_size)
        snprintf(buffer + written, buffer_size - written, "\n✔️ Xong lúc: %s\n🎉 Tốt lắm!", completed_buf);
    dispatcher_set_context_tasks(&task->id, 1);
    return buffer;
}

char *format_task_deleted(const task_record_t *task, char *buffer, size_t buffer_size)
{
    int written = snprintf(buffer, buffer_size, "🗑️ Đã hủy:\n");
    if (written > 0 && (size_t)written < buffer_size)
        append_task_detail(task, buffer + written, buffer_size - written);
    dispatcher_set_context_tasks(NULL, 0);
    return buffer;
}

char *format_task_updated(const task_record_t *task, const char *changes, char *buffer, size_t buffer_size)
{
    int written = snprintf(buffer, buffer_size, "📝 Đã cập nhật:\n");
    if (written > 0 && (size_t)written < buffer_size)
        written += append_task_detail(task, buffer + written, buffer_size - written);
    if (written > 0 && (size_t)written < buffer_size && changes != NULL)
        snprintf(buffer + written, buffer_size - written, "\n✏️ Thay đổi: %s", changes);
    dispatcher_set_context_tasks(&task->id, 1);
    return buffer;
}

char *format_reminder(const task_record_t *task, char *buffer, size_t buffer_size)
{
    int written = snprintf(buffer, buffer_size, "⏰ NHẮC NHỞ:\n");
    if (written > 0 && (size_t)written < buffer_size)
        written += append_task_detail(task, buffer + written, buffer_size - written);
    if (written > 0 && (size_t)written < buffer_size)
        snprintf(buffer + written, buffer_size - written, "\n💡 Trả lời 'xong' để đánh dấu hoàn thành");
    return buffer;
}

char *format_not_found(const char *context, char *buffer, size_t buffer_size)
{
    snprintf(buffer, buffer_size, "❓ Không tìm thấy công việc phù hợp %s", context ? context : "");
    return buffer;
}

char *format_error(const char *error_msg, char *buffer, size_t buffer_size)
{
    snprintf(buffer, buffer_size, "⚠️ Lỗi: %s\nVui lòng thử lại sau.", error_msg ? error_msg : "Không xác định");
    return buffer;
}
