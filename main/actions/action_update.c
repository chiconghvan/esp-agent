/**
 * ===========================================================================
 * @file action_update.c
 * @brief Handler cho action UPDATE_TASK
 *
 * Tìm task bằng semantic search → cập nhật các fields được yêu cầu.
 * Nếu title thay đổi → tạo lại embedding.
 * Gọi API: Embedding (1-2 lần).
 * ===========================================================================
 */

#include "action_dispatcher.h"
#include "task_database.h"
#include "action_undo.h"
#include "vector_search.h"
#include "openai_client.h"
#include "json_parser.h"
#include "time_utils.h"
#include "display_manager.h"
#include "response_formatter.h"
#include "config.h"
#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "action_update";

/* Helper: Map type tiếng Việt → tiếng Anh (phòng trường hợp LLM trả về TV) */
static const char *normalize_type(const char *type)
{
    if (type == NULL) return "other";
    /* Đã là tiếng Anh? */
    if (strcmp(type, "meeting") == 0 || strcmp(type, "report") == 0 ||
        strcmp(type, "reminder") == 0 || strcmp(type, "event") == 0 ||
        strcmp(type, "anniversary") == 0 || strcmp(type, "other") == 0) {
        return type;
    }
    /* Map tiếng Việt */
    if (strstr(type, "nh\xe1\xba" "\xaf" "c") != NULL) return "reminder";      /* nhắc */
    if (strstr(type, "b\xc3\xa1o c\xc3\xa1o") != NULL) return "report";         /* báo cáo */
    if (strstr(type, "cu\xe1\xbb\x99" "c h\xe1\xbb\x8dp") != NULL) return "meeting"; /* cuộc họp */
    if (strstr(type, "s\xe1\xbb\xb1" " ki\xe1\xbb\x87n") != NULL) return "event";   /* sự kiện */
    if (strstr(type, "k\xe1\xbb\x89 ni\xe1\xbb\x87m") != NULL) return "anniversary"; /* kỉ niệm */
    if (strstr(type, "sinh nh\xe1\xba\xadt") != NULL) return "anniversary"; /* sinh nhật */
    return "other";
}

esp_err_t action_update_task(const char *data_json, char *response, size_t response_size)
{
    if (data_json == NULL) {
        format_not_found("để cập nhật", response, response_size);
        return ESP_FAIL;
    }

    cJSON *data = json_parse_string(data_json);
    if (data == NULL) {
        format_error("Lỗi xử lý dữ liệu", response, response_size);
        return ESP_FAIL;
    }

    const char *search_query = json_get_string(data, "search_query", "");
    cJSON *updates = json_get_object(data, "updates");

    /* Fallback: nếu AI không gói trong "updates", dùng "data" trực tiếp */
    cJSON *fields = (updates != NULL) ? updates : data;

    ESP_LOGI(TAG, "Update data: %s, updates=%s", data_json, updates ? "có" : "KHÔNG (dùng data)");

    uint32_t explicit_task_ids[10];
    int explicit_count = 0;
    cJSON *task_ids_json = cJSON_GetObjectItem(data, "task_ids");
    if (task_ids_json != NULL && cJSON_IsArray(task_ids_json)) {
        cJSON *item;
        cJSON_ArrayForEach(item, task_ids_json) {
            if (cJSON_IsNumber(item) && explicit_count < 10) {
                explicit_task_ids[explicit_count++] = (uint32_t)item->valuedouble;
            }
        }
    }

    /* Copy query */
    char query_buf[256];
    strncpy(query_buf, search_query, sizeof(query_buf) - 1);
    query_buf[sizeof(query_buf) - 1] = '\0';

    uint32_t target_task_id = 0;

    if (explicit_count > 0) {
        /* Cập nhật hàng loạt */
        int updated_count = 0;
        int written = snprintf(response, response_size, "\xF0\x9F\x93\x9D Đã cập nhật %d công việc:\n", explicit_count);
        char changes_buf[128] = "";

        /* Array to hold old tasks for Undo */
        task_record_t *old_tasks = malloc(sizeof(task_record_t) * explicit_count);
        int old_tasks_count = 0;
        
        for (int i = 0; i < explicit_count; i++) {
            task_record_t task;
            if (task_database_read(explicit_task_ids[i], &task) == ESP_OK) {
                if (old_tasks) {
                    old_tasks[old_tasks_count++] = task;
                }
                
                int changes_len = 0;
                bool title_changed = false;

                /* Parse các trường cần cập nhật từ fields */
                {
                    const char *new_due = json_get_string(fields, "due_time", NULL);
                    if (new_due != NULL) {
                        if (strcmp(new_due, "none") == 0) {
                            task.due_time = 0;
                            changes_len += snprintf(changes_buf + changes_len, sizeof(changes_buf) - changes_len, "xóa thời hạn, ");
                        } else {
                            time_t parsed = time_utils_parse_iso8601(new_due);
                            if (parsed > 0) {
                                task.due_time = parsed;
                                changes_len += snprintf(changes_buf + changes_len, sizeof(changes_buf) - changes_len, "thời hạn, ");
                            }
                            /* else: parse thất bại → bỏ qua, giữ nguyên */
                        }
                    }
                    /* JSON null = không thay đổi, bỏ qua */
                    int64_t due_offset = json_get_int(fields, "due_offset_days", 0);
                    if (due_offset != 0 && task.due_time > 0) {
                        task.due_time += (time_t)(due_offset * 86400);
                        changes_len += snprintf(changes_buf + changes_len, sizeof(changes_buf) - changes_len, "thời hạn, ");
                    }
                    const char *new_start = json_get_string(fields, "start_time", NULL);
                    if (new_start != NULL) {
                        if (strcmp(new_start, "none") == 0) {
                            task.start_time = 0;
                            changes_len += snprintf(changes_buf + changes_len, sizeof(changes_buf) - changes_len, "xóa bắt đầu, ");
                        } else {
                            time_t parsed = time_utils_parse_iso8601(new_start);
                            if (parsed > 0) {
                                task.start_time = parsed;
                                changes_len += snprintf(changes_buf + changes_len, sizeof(changes_buf) - changes_len, "bắt đầu, ");
                            }
                        }
                    }
                    /* JSON null = không thay đổi, bỏ qua */
                    const char *new_reminder = json_get_string(fields, "reminder", NULL);
                    if (new_reminder != NULL) {
                        if (strcmp(new_reminder, "none") == 0) {
                            task.reminder = 0;
                            changes_len += snprintf(changes_buf + changes_len, sizeof(changes_buf) - changes_len, "xóa nhắc nhở, ");
                        } else {
                            time_t parsed = time_utils_parse_iso8601(new_reminder);
                            if (parsed > 0) {
                                task.reminder = parsed;
                                changes_len += snprintf(changes_buf + changes_len, sizeof(changes_buf) - changes_len, "nhắc nhở, ");
                            }
                        }
                    }
                    /* JSON null = không thay đổi, bỏ qua */
                    int64_t rem_offset = json_get_int(fields, "reminder_offset_days", 0);
                    if (rem_offset != 0 && task.reminder > 0) {
                        task.reminder += (time_t)(rem_offset * 86400);
                        changes_len += snprintf(changes_buf + changes_len, sizeof(changes_buf) - changes_len, "nhắc nhở, ");
                    }
                    const char *new_title = json_get_string(fields, "title", NULL);
                    if (new_title != NULL && strlen(new_title) > 0) {
                        strncpy(task.title, new_title, sizeof(task.title) - 1);
                        title_changed = true;
                        changes_len += snprintf(changes_buf + changes_len, sizeof(changes_buf) - changes_len, "tiêu đề, ");
                    }
                    const char *new_type = json_get_string(fields, "type", NULL);
                    if (new_type != NULL) {
                        const char *mapped = normalize_type(new_type);
                        strncpy(task.type, mapped, sizeof(task.type) - 1);
                        changes_len += snprintf(changes_buf + changes_len, sizeof(changes_buf) - changes_len, "loại, ");
                    }
                    const char *new_repeat = json_get_string(fields, "repeat", NULL);
                    if (new_repeat != NULL) {
                        strncpy(task.repeat, new_repeat, sizeof(task.repeat) - 1);
                        changes_len += snprintf(changes_buf + changes_len, sizeof(changes_buf) - changes_len, "lặp lại, ");
                    }
                    int new_interval = json_get_int(fields, "repeat_interval", -1);
                    if (new_interval >= 0) {
                        task.repeat_interval = new_interval;
                    }
                    const char *new_notes = json_get_string(fields, "notes", NULL);
                    if (new_notes != NULL) {
                        strncpy(task.notes, new_notes, sizeof(task.notes) - 1);
                        changes_len += snprintf(changes_buf + changes_len, sizeof(changes_buf) - changes_len, "ghi chú, ");
                    }
                }
                
                if (changes_len >= 2) changes_buf[changes_len - 2] = '\0';

                if (strcmp(task.status, "overdue") == 0 && (task.due_time == 0 || task.due_time > time_utils_get_now())) {
                    strncpy(task.status, "pending", sizeof(task.status) - 1);
                }

                if (task_database_update(&task) == ESP_OK) {
                    if (title_changed) {
                        float new_emb[EMBEDDING_DIM];
                        if (openai_create_embedding(task.title, new_emb, EMBEDDING_DIM) == ESP_OK) {
                            vector_search_save(task.id, new_emb);
                        }
                    }
                    if (written < response_size) {
                        written += snprintf(response + written, response_size - written, 
                            " - [#%" PRIu32 "] %s (%s)\n", task.id, task.title, changes_buf[0] ? changes_buf : "đổi ID");
                    }
                    updated_count++;
                }
            }
        }

        cJSON_Delete(data);

        if (old_tasks && old_tasks_count > 0) {
            action_undo_save(UNDO_UPDATE, old_tasks, old_tasks_count);
        }
        if (old_tasks) free(old_tasks);

        if (updated_count == 0) {
            format_not_found("theo ID yêu cầu", response, response_size);
        }
        dispatcher_set_context_tasks(NULL, 0);
        return ESP_OK;

    } else {
        if (strlen(query_buf) == 0) {
            cJSON_Delete(data);
            format_not_found("(không có mô tả hoặc ID)", response, response_size);
            return ESP_FAIL;
        }

        ESP_LOGI(TAG, "Update task semantic search: %s", query_buf);

        /* Tạo embedding cho search query */
        float query_embedding[EMBEDDING_DIM];
        esp_err_t err = openai_create_embedding(query_buf, query_embedding, EMBEDDING_DIM);
        if (err != ESP_OK) {
            cJSON_Delete(data);
            format_error("Không thể tạo embedding cho tìm kiếm", response, response_size);
            return err;
        }

        /* Tìm task matching cao nhất */
        search_result_t search_results[SEARCH_TOP_K];
        int found_count = 0;

        err = vector_search_find_similar(query_embedding, search_results,
                                          SEARCH_TOP_K, &found_count);

        if (err != ESP_OK || found_count == 0) {
            cJSON_Delete(data);
            format_not_found(query_buf, response, response_size);
            return ESP_OK;
        }

        target_task_id = search_results[0].task_id;

        /* Đọc task chi tiết */
        task_record_t task;
        err = task_database_read(target_task_id, &task);
        if (err != ESP_OK) {
            cJSON_Delete(data);
            format_not_found("theo ID yêu cầu", response, response_size);
            return ESP_OK; 
        }

        /* Lưu lại trạng thái cũ cho Undo */
        task_record_t old_task = task;

        /* Áp dụng updates */
        char changes_buf[256] = "";
        int changes_len = 0;
        bool title_changed = false;

        {
            const char *new_due = json_get_string(fields, "due_time", NULL);
            if (new_due != NULL) {
                if (strcmp(new_due, "none") == 0) {
                    task.due_time = 0;
                    changes_len += snprintf(changes_buf + changes_len, sizeof(changes_buf) - changes_len, "xóa thời hạn, ");
                } else {
                    time_t parsed = time_utils_parse_iso8601(new_due);
                    if (parsed > 0) {
                        task.due_time = parsed;
                        changes_len += snprintf(changes_buf + changes_len, sizeof(changes_buf) - changes_len, "thời hạn, ");
                    }
                }
            }
            /* JSON null = không thay đổi, bỏ qua */
            int64_t due_offset = json_get_int(fields, "due_offset_days", 0);
            if (due_offset != 0 && task.due_time > 0) {
                task.due_time += (time_t)(due_offset * 86400);
                changes_len += snprintf(changes_buf + changes_len, sizeof(changes_buf) - changes_len, "thời hạn, ");
            }
            const char *new_start = json_get_string(fields, "start_time", NULL);
            if (new_start != NULL) {
                if (strcmp(new_start, "none") == 0) {
                    task.start_time = 0;
                    changes_len += snprintf(changes_buf + changes_len, sizeof(changes_buf) - changes_len, "xóa bắt đầu, ");
                } else {
                    time_t parsed = time_utils_parse_iso8601(new_start);
                    if (parsed > 0) {
                        task.start_time = parsed;
                        changes_len += snprintf(changes_buf + changes_len, sizeof(changes_buf) - changes_len, "bắt đầu, ");
                    }
                }
            }
            /* JSON null = không thay đổi, bỏ qua */
            const char *new_reminder = json_get_string(fields, "reminder", NULL);
            if (new_reminder != NULL) {
                if (strcmp(new_reminder, "none") == 0) {
                    task.reminder = 0;
                    changes_len += snprintf(changes_buf + changes_len, sizeof(changes_buf) - changes_len, "xóa nhắc nhở, ");
                } else {
                    time_t parsed = time_utils_parse_iso8601(new_reminder);
                    if (parsed > 0) {
                        task.reminder = parsed;
                        changes_len += snprintf(changes_buf + changes_len, sizeof(changes_buf) - changes_len, "nhắc nhở, ");
                    }
                }
            }
            /* JSON null = không thay đổi, bỏ qua */
            int64_t rem_offset = json_get_int(fields, "reminder_offset_days", 0);
            if (rem_offset != 0 && task.reminder > 0) {
                task.reminder += (time_t)(rem_offset * 86400);
                changes_len += snprintf(changes_buf + changes_len, sizeof(changes_buf) - changes_len, "nhắc nhở, ");
            }
            const char *new_title = json_get_string(fields, "title", NULL);
            if (new_title != NULL && strlen(new_title) > 0) {
                strncpy(task.title, new_title, sizeof(task.title) - 1);
                title_changed = true;
                changes_len += snprintf(changes_buf + changes_len, sizeof(changes_buf) - changes_len, "tiêu đề, ");
            }
            const char *new_type = json_get_string(fields, "type", NULL);
            if (new_type != NULL) {
                const char *mapped = normalize_type(new_type);
                strncpy(task.type, mapped, sizeof(task.type) - 1);
                changes_len += snprintf(changes_buf + changes_len, sizeof(changes_buf) - changes_len, "loại, ");
            }
            const char *new_repeat = json_get_string(fields, "repeat", NULL);
            if (new_repeat != NULL) {
                strncpy(task.repeat, new_repeat, sizeof(task.repeat) - 1);
                changes_len += snprintf(changes_buf + changes_len, sizeof(changes_buf) - changes_len, "lặp lại, ");
            }
            int new_interval = json_get_int(fields, "repeat_interval", -1);
            if (new_interval >= 0) {
                task.repeat_interval = new_interval;
            }
            const char *new_notes = json_get_string(fields, "notes", NULL);
            if (new_notes != NULL) {
                strncpy(task.notes, new_notes, sizeof(task.notes) - 1);
                changes_len += snprintf(changes_buf + changes_len, sizeof(changes_buf) - changes_len, "ghi chú, ");
            }
        }

        cJSON_Delete(data);

        if (changes_len >= 2) changes_buf[changes_len - 2] = '\0';

        if (strcmp(task.status, "overdue") == 0 && (task.due_time == 0 || task.due_time > time_utils_get_now())) {
            strncpy(task.status, "pending", sizeof(task.status) - 1);
        }

        /* Lưu task đã cập nhật */
        err = task_database_update(&task);
        if (err != ESP_OK) {
            format_error("Không thể cập nhật task", response, response_size);
            return err;
        }

        /* Lưu undo log */
        action_undo_save(UNDO_UPDATE, &old_task, 1);

        if (title_changed) {
            float new_embedding[EMBEDDING_DIM];
            if (openai_create_embedding(task.title, new_embedding, EMBEDDING_DIM) == ESP_OK) {
                vector_search_save(task.id, new_embedding);
            }
        }

        format_task_updated(&task, changes_buf, response, response_size);

        /* Cập nhật màn hình LCD */
        display_show_result("Cap nhat", task.id, task.title);

        ESP_LOGI(TAG, "Task #%" PRIu32 " đã cập nhật: %s", target_task_id, changes_buf);
        return ESP_OK;
    }
}
