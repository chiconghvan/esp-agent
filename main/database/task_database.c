/**
 * ===========================================================================
 * @file task_database.c
 * @brief Triển khai cơ sở dữ liệu task trên SPIFFS
 *
 * Mỗi task lưu thành file JSON riêng: /spiffs/tasks/task_XXXX.json
 * Index tổng hợp lưu tại: /spiffs/tasks/index.json
 * SPIFFS được mount lúc init với partition "storage".
 * ===========================================================================
 */

#include "task_database.h"
#include "config.h"
#include "json_parser.h"
#include "time_utils.h"
#include <stdio.h>
#include <inttypes.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include "esp_log.h"
#include "esp_spiffs.h"
#include "cJSON.h"

static const char *TAG = "task_db";

/** Index được cache trong RAM để truy vấn nhanh */
static task_index_t task_index = {0};

/* --------------------------------------------------------------------------
 * Hàm nội bộ: Tạo đường dẫn file task
 * -------------------------------------------------------------------------- */
static void get_task_filepath(uint32_t task_id, char *path, size_t path_size)
{
    snprintf(path, path_size, "%s/task_%04" PRIu32 ".json", TASK_DIR_PATH, task_id);
}

/* --------------------------------------------------------------------------
 * Hàm nội bộ: Serialize task thành cJSON object
 * -------------------------------------------------------------------------- */
static cJSON *task_to_json(const task_record_t *task)
{
    cJSON *json = cJSON_CreateObject();
    if (json == NULL) return NULL;

    cJSON_AddNumberToObject(json, "id", task->id);
    cJSON_AddStringToObject(json, "title", task->title);
    cJSON_AddStringToObject(json, "type", task->type);
    cJSON_AddStringToObject(json, "status", task->status);
    cJSON_AddNumberToObject(json, "created_at", (double)task->created_at);
    cJSON_AddNumberToObject(json, "start_time", (double)task->start_time);
    cJSON_AddNumberToObject(json, "due_time", (double)task->due_time);
    cJSON_AddNumberToObject(json, "completed_at", (double)task->completed_at);
    cJSON_AddNumberToObject(json, "reminder", (double)task->reminder);
    cJSON_AddStringToObject(json, "repeat", task->repeat);
    cJSON_AddNumberToObject(json, "repeat_interval", task->repeat_interval);
    cJSON_AddStringToObject(json, "notes", task->notes);

    return json;
}

/* --------------------------------------------------------------------------
 * Hàm nội bộ: Deserialize cJSON thành task struct
 * -------------------------------------------------------------------------- */
static void json_to_task(const cJSON *json, task_record_t *task)
{
    memset(task, 0, sizeof(task_record_t));

    task->id = (uint32_t)json_get_int(json, "id", 0);

    const char *title = json_get_string(json, "title", "");
    strncpy(task->title, title, sizeof(task->title) - 1);

    const char *type = json_get_string(json, "type", "other");
    strncpy(task->type, type, sizeof(task->type) - 1);

    const char *status = json_get_string(json, "status", "pending");
    strncpy(task->status, status, sizeof(task->status) - 1);

    task->created_at = (time_t)json_get_int(json, "created_at", 0);
    task->start_time = (time_t)json_get_int(json, "start_time", 0);
    task->due_time = (time_t)json_get_int(json, "due_time", 0);
    task->completed_at = (time_t)json_get_int(json, "completed_at", 0);
    task->reminder = (time_t)json_get_int(json, "reminder", 0);

    const char *repeat = json_get_string(json, "repeat", "none");
    strncpy(task->repeat, repeat, sizeof(task->repeat) - 1);

    task->repeat_interval = (int)json_get_int(json, "repeat_interval", 0);

    const char *notes = json_get_string(json, "notes", "");
    strncpy(task->notes, notes, sizeof(task->notes) - 1);
}

/* --------------------------------------------------------------------------
 * Hàm nội bộ: Lưu task xuống file SPIFFS
 * -------------------------------------------------------------------------- */
static esp_err_t save_task_file(const task_record_t *task)
{
    char filepath[64];
    get_task_filepath(task->id, filepath, sizeof(filepath));

    cJSON *json = task_to_json(task);
    if (json == NULL) {
        ESP_LOGE(TAG, "Không thể tạo JSON cho task %" PRIu32, task->id);
        return ESP_ERR_NO_MEM;
    }

    char *json_str = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);

    if (json_str == NULL) {
        return ESP_ERR_NO_MEM;
    }

    FILE *file = fopen(filepath, "w");
    if (file == NULL) {
        ESP_LOGE(TAG, "Không thể mở file để ghi: %s", filepath);
        free(json_str);
        return ESP_FAIL;
    }

    fprintf(file, "%s", json_str);
    fclose(file);
    free(json_str);

    ESP_LOGD(TAG, "Đã lưu task %" PRIu32 ": %s", task->id, filepath);
    return ESP_OK;
}

/* --------------------------------------------------------------------------
 * Hàm nội bộ: Đọc task từ file SPIFFS
 * -------------------------------------------------------------------------- */
static esp_err_t load_task_file(uint32_t task_id, task_record_t *task)
{
    char filepath[64];
    get_task_filepath(task_id, filepath, sizeof(filepath));

    FILE *file = fopen(filepath, "r");
    if (file == NULL) {
        ESP_LOGW(TAG, "Không tìm thấy file: %s", filepath);
        return ESP_ERR_NOT_FOUND;
    }

    /* Đọc toàn bộ nội dung file (Sử dụng malloc để tránh tràn Stack) */
    char *buffer = (char *)malloc(JSON_BUFFER_SIZE);
    if (buffer == NULL) {
        ESP_LOGE(TAG, "Lỗi cấp phát bộ nhớ cho buffer đọc file");
        fclose(file);
        return ESP_ERR_NO_MEM;
    }

    size_t read_len = fread(buffer, 1, JSON_BUFFER_SIZE - 1, file);
    fclose(file);
    buffer[read_len] = '\0';

    /* Parse JSON */
    cJSON *json = json_parse_string(buffer);
    free(buffer);

    if (json == NULL) {
        ESP_LOGE(TAG, "Lỗi parse JSON từ file: %s", filepath);
        return ESP_FAIL;
    }

    json_to_task(json, task);
    cJSON_Delete(json);

    return ESP_OK;
}

/* --------------------------------------------------------------------------
 * Hàm nội bộ: Cập nhật entry trong index
 * -------------------------------------------------------------------------- */
static void update_index_entry(const task_record_t *task)
{
    /* Tìm entry hiện có */
    for (int i = 0; i < task_index.count; i++) {
        if (task_index.entries[i].id == task->id) {
            /* Cập nhật entry */
            strncpy(task_index.entries[i].status, task->status,
                    sizeof(task_index.entries[i].status) - 1);
            strncpy(task_index.entries[i].type, task->type,
                    sizeof(task_index.entries[i].type) - 1);
            task_index.entries[i].due_time = task->due_time;
            task_index.entries[i].start_time = task->start_time;
            task_index.entries[i].reminder = task->reminder;
            return;
        }
    }

    /* Thêm entry mới */
    if (task_index.count < MAX_TASK_COUNT) {
        task_index_entry_t *entry = &task_index.entries[task_index.count];
        entry->id = task->id;
        strncpy(entry->status, task->status, sizeof(entry->status) - 1);
        strncpy(entry->type, task->type, sizeof(entry->type) - 1);
        entry->due_time = task->due_time;
        entry->start_time = task->start_time;
        entry->reminder = task->reminder;
        task_index.count++;
    }
}

/* --------------------------------------------------------------------------
 * Hàm nội bộ: Load index từ SPIFFS
 * -------------------------------------------------------------------------- */
static esp_err_t load_index(void)
{
    FILE *file = fopen(TASK_INDEX_FILE, "r");
    if (file == NULL) {
        ESP_LOGI(TAG, "Chưa có index file, tạo mới");
        task_index.next_id = 1;
        task_index.count = 0;
        return ESP_OK;
    }

    char buffer[JSON_BUFFER_SIZE];
    size_t read_len = fread(buffer, 1, sizeof(buffer) - 1, file);
    fclose(file);
    buffer[read_len] = '\0';

    cJSON *root = json_parse_string(buffer);
    if (root == NULL) {
        ESP_LOGW(TAG, "Index file hỏng, tạo mới");
        task_index.next_id = 1;
        task_index.count = 0;
        return ESP_OK;
    }

    task_index.next_id = (uint32_t)json_get_int(root, "next_id", 1);
    task_index.count = 0;

    cJSON *tasks_array = json_get_array(root, "tasks");
    if (tasks_array != NULL) {
        int array_size = cJSON_GetArraySize(tasks_array);
        for (int i = 0; i < array_size && i < MAX_TASK_COUNT; i++) {
            cJSON *item = cJSON_GetArrayItem(tasks_array, i);
            if (item == NULL) continue;

            task_index_entry_t *entry = &task_index.entries[task_index.count];
            entry->id = (uint32_t)json_get_int(item, "id", 0);

            const char *status = json_get_string(item, "status", "pending");
            strncpy(entry->status, status, sizeof(entry->status) - 1);

            const char *type = json_get_string(item, "type", "other");
            strncpy(entry->type, type, sizeof(entry->type) - 1);

            entry->due_time = (time_t)json_get_int(item, "due_time", 0);
            entry->start_time = (time_t)json_get_int(item, "start_time", 0);
            entry->reminder = (time_t)json_get_int(item, "reminder", 0);

            task_index.count++;
        }
    }

    ESP_LOGI(TAG, "Đã load index: %d tasks, next_id=%" PRIu32,
             task_index.count, task_index.next_id);

    cJSON_Delete(root);
    return ESP_OK;
}

/* --------------------------------------------------------------------------
 * Khởi tạo database
 * -------------------------------------------------------------------------- */
esp_err_t task_database_init(void)
{
    ESP_LOGI(TAG, "Khởi tạo SPIFFS database...");

    /* Mount SPIFFS */
    esp_vfs_spiffs_conf_t spiffs_conf = {
        .base_path = SPIFFS_BASE_PATH,
        .partition_label = "storage",
        .max_files = 10,
        .format_if_mount_failed = true
    };

    esp_err_t err = esp_vfs_spiffs_register(&spiffs_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount thất bại: %s", esp_err_to_name(err));
        return err;
    }

    /* Kiểm tra dung lượng SPIFFS */
    size_t total = 0, used = 0;
    esp_spiffs_info("storage", &total, &used);
    ESP_LOGI(TAG, "SPIFFS: tổng=%zu, đã dùng=%zu, còn=%zu bytes",
             total, used, total - used);

    /* Tạo thư mục tasks và embeddings (SPIFFS flat, nhưng tạo prefix path) */
    /* SPIFFS không hỗ trợ mkdir thật sự, file paths phẳng */

    /* Load index */
    err = load_index();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Load index thất bại, tạo mới");
        task_index.next_id = 1;
        task_index.count = 0;
    }

    ESP_LOGI(TAG, "Database khởi tạo thành công: %d tasks", task_index.count);
    return ESP_OK;
}

/* --------------------------------------------------------------------------
 * Lưu index xuống SPIFFS
 * -------------------------------------------------------------------------- */
esp_err_t task_database_save_index(void)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) return ESP_ERR_NO_MEM;

    cJSON_AddNumberToObject(root, "next_id", task_index.next_id);
    cJSON_AddNumberToObject(root, "count", task_index.count);

    cJSON *tasks_array = cJSON_CreateArray();
    for (int i = 0; i < task_index.count; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "id", task_index.entries[i].id);
        cJSON_AddStringToObject(item, "status", task_index.entries[i].status);
        cJSON_AddStringToObject(item, "type", task_index.entries[i].type);
        cJSON_AddNumberToObject(item, "due_time", (double)task_index.entries[i].due_time);
        cJSON_AddNumberToObject(item, "start_time", (double)task_index.entries[i].start_time);
        cJSON_AddNumberToObject(item, "reminder", (double)task_index.entries[i].reminder);
        cJSON_AddItemToArray(tasks_array, item);
    }
    cJSON_AddItemToObject(root, "tasks", tasks_array);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (json_str == NULL) return ESP_ERR_NO_MEM;

    FILE *file = fopen(TASK_INDEX_FILE, "w");
    if (file == NULL) {
        ESP_LOGE(TAG, "Không thể mở index file để ghi");
        free(json_str);
        return ESP_FAIL;
    }

    fprintf(file, "%s", json_str);
    fclose(file);
    free(json_str);

    ESP_LOGD(TAG, "Đã lưu index: %d tasks", task_index.count);
    return ESP_OK;
}

/* --------------------------------------------------------------------------
 * Tạo task mới
 * -------------------------------------------------------------------------- */
esp_err_t task_database_create(task_record_t *task)
{
    if (task_index.count >= MAX_TASK_COUNT) {
        ESP_LOGE(TAG, "Database đầy (%d tasks)", MAX_TASK_COUNT);
        return ESP_ERR_NO_MEM;
    }

    /* Gán ID và created_at */
    task->id = task_index.next_id++;
    task->created_at = time_utils_get_now();

    if (strlen(task->status) == 0) {
        strncpy(task->status, "pending", sizeof(task->status) - 1);
    }
    if (strlen(task->repeat) == 0) {
        strncpy(task->repeat, "none", sizeof(task->repeat) - 1);
    }

    /* Lưu file task */
    esp_err_t err = save_task_file(task);
    if (err != ESP_OK) {
        return err;
    }

    /* Cập nhật index */
    update_index_entry(task);

    /* Lưu index */
    err = task_database_save_index();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Lưu index thất bại (task đã lưu)");
    }

    ESP_LOGI(TAG, "Đã tạo task #%" PRIu32 ": %s", task->id, task->title);
    return ESP_OK;
}

/* --------------------------------------------------------------------------
 * Đọc task theo ID
 * -------------------------------------------------------------------------- */
esp_err_t task_database_read(uint32_t task_id, task_record_t *task)
{
    if (task == NULL) return ESP_ERR_INVALID_ARG;
    return load_task_file(task_id, task);
}

/* --------------------------------------------------------------------------
 * Cập nhật task
 * -------------------------------------------------------------------------- */
esp_err_t task_database_update(const task_record_t *task)
{
    if (task == NULL) return ESP_ERR_INVALID_ARG;

    esp_err_t err = save_task_file(task);
    if (err != ESP_OK) return err;

    update_index_entry(task);
    task_database_save_index();

    ESP_LOGI(TAG, "Đã cập nhật task #%" PRIu32, task->id);
    return ESP_OK;
}

/* --------------------------------------------------------------------------
 * Xóa mềm (soft delete)
 * -------------------------------------------------------------------------- */
esp_err_t task_database_soft_delete(uint32_t task_id)
{
    task_record_t task;
    esp_err_t err = load_task_file(task_id, &task);
    if (err != ESP_OK) return err;

    strncpy(task.status, "cancelled", sizeof(task.status) - 1);

    err = save_task_file(&task);
    if (err != ESP_OK) return err;

    update_index_entry(&task);
    task_database_save_index();

    ESP_LOGI(TAG, "Đã hủy task #%" PRIu32 ": %s", task_id, task.title);
    return ESP_OK;
}

/* --------------------------------------------------------------------------
 * Xóa cứng (hard delete) — xóa file + index entry
 * -------------------------------------------------------------------------- */
esp_err_t task_database_hard_delete(uint32_t task_id)
{
    /* Xóa file task */
    char filepath[64];
    get_task_filepath(task_id, filepath, sizeof(filepath));

    if (remove(filepath) != 0) {
        ESP_LOGW(TAG, "Không thể xóa file: %s", filepath);
    }

    /* Xóa entry khỏi index */
    bool found = false;
    for (int i = 0; i < task_index.count; i++) {
        if (task_index.entries[i].id == task_id) {
            /* Dịch các entry phía sau lên */
            for (int j = i; j < task_index.count - 1; j++) {
                task_index.entries[j] = task_index.entries[j + 1];
            }
            task_index.count--;
            found = true;
            break;
        }
    }

    if (!found) {
        ESP_LOGW(TAG, "Không tìm thấy task #%" PRIu32 " trong index", task_id);
        return ESP_ERR_NOT_FOUND;
    }

    task_database_save_index();
    ESP_LOGI(TAG, "Đã xóa cứng task #%" PRIu32, task_id);
    return ESP_OK;
}

/* --------------------------------------------------------------------------
 * Cập nhật trạng thái các task quá hạn
 * -------------------------------------------------------------------------- */
esp_err_t task_database_update_overdue(void)
{
    time_t now = time_utils_get_now();
    bool updated = false;

    for (int i = 0; i < task_index.count; i++) {
        task_index_entry_t *entry = &task_index.entries[i];

        if (strcmp(entry->status, "pending") == 0 && entry->due_time > 0 && entry->due_time < now) {
            strncpy(entry->status, "overdue", sizeof(entry->status) - 1);
            
            task_record_t task;
            if (load_task_file(entry->id, &task) == ESP_OK) {
                strncpy(task.status, "overdue", sizeof(task.status) - 1);
                save_task_file(&task);
            }
            updated = true;
        }
    }

    if (updated) {
        task_database_save_index();
        ESP_LOGI(TAG, "Đã cập nhật trạng thái về overdue cho các task quá hạn");
    }
    
    return ESP_OK;
}

/* --------------------------------------------------------------------------
 * Truy vấn theo khoảng thời gian
 * -------------------------------------------------------------------------- */
esp_err_t task_database_query_by_time(time_t start, time_t end,
                                       const char *type, const char *status,
                                       task_record_t *results, int max_results,
                                       int *found_count)
{
    *found_count = 0;

    for (int i = 0; i < task_index.count && *found_count < max_results; i++) {
        task_index_entry_t *entry = &task_index.entries[i];

        /* Bỏ qua task đã hủy */
        if (strcmp(entry->status, "cancelled") == 0) continue;

        /* Lọc theo type (nếu có) */
        if (type != NULL && strlen(type) > 0) {
            if (strcmp(entry->type, type) != 0) continue;
        }

        /* Lọc theo status (nếu có) */
        if (status != NULL && strlen(status) > 0) {
            if (strcmp(status, "pending") == 0 || strcmp(status, "incomplete") == 0) {
                if (strcmp(entry->status, "pending") != 0 && strcmp(entry->status, "overdue") != 0) continue;
            } else if (strcmp(status, "completed") == 0) {
                if (strcmp(entry->status, "done") != 0) continue;
            } else {
                if (strcmp(entry->status, status) != 0) continue;
            }
        }

        /* Kiểm tra due_time, start_time hoặc reminder nằm trong range */
        bool in_range = false;
        if (entry->due_time > 0 && entry->due_time >= start && entry->due_time <= end) {
            in_range = true;
        }
        if (entry->start_time > 0 && entry->start_time >= start && entry->start_time <= end) {
            in_range = true;
        }
        if (entry->reminder > 0 && entry->reminder >= start && entry->reminder <= end) {
            in_range = true;
        }

        if (in_range) {
            esp_err_t err = load_task_file(entry->id, &results[*found_count]);
            if (err == ESP_OK) {
                (*found_count)++;
            }
        }
    }

    ESP_LOGI(TAG, "Query by time: tìm thấy %d tasks", *found_count);
    return ESP_OK;
}

/* --------------------------------------------------------------------------
 * Truy vấn theo type và/hoặc status
 * -------------------------------------------------------------------------- */
esp_err_t task_database_query_by_type(const char *type, const char *status,
                                       task_record_t *results, int max_results,
                                       int *found_count)
{
    *found_count = 0;

    for (int i = 0; i < task_index.count && *found_count < max_results; i++) {
        task_index_entry_t *entry = &task_index.entries[i];

        /* Lọc theo type (nếu có) */
        if (type != NULL && strlen(type) > 0) {
            if (strcmp(entry->type, type) != 0) continue;
        }

        /* Lọc theo status (nếu có) */
        if (status != NULL && strlen(status) > 0) {
            if (strcmp(status, "pending") == 0 || strcmp(status, "incomplete") == 0) {
                if (strcmp(entry->status, "pending") != 0 && strcmp(entry->status, "overdue") != 0) continue;
            } else if (strcmp(status, "completed") == 0) {
                if (strcmp(entry->status, "done") != 0) continue;
            } else {
                if (strcmp(entry->status, status) != 0) continue;
            }
        } else {
            /* Mặc định bỏ qua cancelled */
            if (strcmp(entry->status, "cancelled") == 0) continue;
        }

        esp_err_t err = load_task_file(entry->id, &results[*found_count]);
        if (err == ESP_OK) {
            (*found_count)++;
        }
    }

    ESP_LOGI(TAG, "Query by type: tìm thấy %d tasks", *found_count);
    return ESP_OK;
}

/* --------------------------------------------------------------------------
 * Truy vấn reminders đến hạn
 * -------------------------------------------------------------------------- */
esp_err_t task_database_query_due_reminders(task_record_t *results, int max_results,
                                             int *found_count)
{
    *found_count = 0;
    time_t now = time_utils_get_now();

    for (int i = 0; i < task_index.count && *found_count < max_results; i++) {
        task_index_entry_t *entry = &task_index.entries[i];

        /* Chỉ tìm tasks pending, có reminder, và đến hạn */
        if (strcmp(entry->status, "pending") != 0) continue;
        if (entry->reminder == 0) continue;
        if (entry->reminder > now) continue;

        esp_err_t err = load_task_file(entry->id, &results[*found_count]);
        if (err == ESP_OK) {
            (*found_count)++;
        }
    }

    return ESP_OK;
}

/* --------------------------------------------------------------------------
 * Lấy con trỏ đến index
 * -------------------------------------------------------------------------- */
const task_index_t *task_database_get_index(void)
{
    return &task_index;
}
