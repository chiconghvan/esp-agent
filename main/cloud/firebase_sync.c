/**
 * ===========================================================================
 * @file firebase_sync.c
 * @brief Triển khai Firebase Realtime Database Sync
 * ===========================================================================
 */

#include "firebase_sync.h"
#include "config.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "wifi_manager.h"
#include "display_manager.h"
#include <string.h>

static const char *TAG = "firebase_sync";

/* Kiểu request đồng bộ */
typedef enum {
    SYNC_REQ_UPLOAD,
    SYNC_REQ_DELETE
} sync_req_type_t;

/* Cấu trúc message trong Queue */
typedef struct {
    sync_req_type_t type;
    uint32_t task_id;
    task_record_t task_data; // Chi copy data neu la UPLOAD
} sync_msg_t;

static QueueHandle_t s_sync_queue = NULL;

/* --------------------------------------------------------------------------
 * Hàm tiện ích: Build JSON từ Task Record
 * -------------------------------------------------------------------------- */
static char *build_task_json(const task_record_t *task)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;

    cJSON_AddNumberToObject(root, "id", task->id);
    cJSON_AddStringToObject(root, "title", task->title);
    cJSON_AddStringToObject(root, "type", task->type);
    cJSON_AddNumberToObject(root, "created_at", task->created_at);
    cJSON_AddNumberToObject(root, "due_time", task->due_time);
    cJSON_AddNumberToObject(root, "start_time", task->start_time);
    cJSON_AddNumberToObject(root, "reminder", task->reminder);
    cJSON_AddStringToObject(root, "repeat", task->repeat);
    cJSON_AddNumberToObject(root, "repeat_interval", task->repeat_interval);
    cJSON_AddStringToObject(root, "status", task->status);
    cJSON_AddStringToObject(root, "notes", task->notes);
    cJSON_AddNumberToObject(root, "completed_at", task->completed_at);

    char *json_string = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json_string;
}

/* --------------------------------------------------------------------------
 * HTTP Client: Gửi PUT Request lên Firebase
 * -------------------------------------------------------------------------- */
static esp_err_t http_put_task(const task_record_t *task)
{
    if (strlen(FIREBASE_HOST) == 0 || strcmp(FIREBASE_HOST, "YOUR_FIREBASE_HOST_HERE") == 0) {
        return ESP_FAIL; // Chưa cấu hình
    }

    char url[256];
    snprintf(url, sizeof(url), "https://%s/tasks/%" PRIu32 ".json?auth=%s", 
             FIREBASE_HOST, task->id, FIREBASE_AUTH);

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_PUT,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach, // TLS bundle
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Lỗi khởi tạo HTTP client");
        return ESP_FAIL;
    }

    char *post_data = build_task_json(task);
    if (!post_data) {
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, post_data, strlen(post_data));

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status_code = esp_http_client_get_status_code(client);
        if (status_code == 200) {
            ESP_LOGI(TAG, "Upload task #%" PRIu32 " thành công (Firebase)", task->id);
        } else {
            ESP_LOGW(TAG, "Upload Firebase lỗi HTTP status: %d", status_code);
            err = ESP_FAIL;
        }
    } else {
        ESP_LOGE(TAG, "Lỗi HTTP POST/PUT: %s", esp_err_to_name(err));
    }

    free(post_data);
    esp_http_client_cleanup(client);
    return err;
}

/* --------------------------------------------------------------------------
 * HTTP Client: Gửi DELETE Request lên Firebase
 * -------------------------------------------------------------------------- */
static esp_err_t http_delete_task(uint32_t task_id)
{
    if (strlen(FIREBASE_HOST) == 0 || strcmp(FIREBASE_HOST, "YOUR_FIREBASE_HOST_HERE") == 0) {
        return ESP_FAIL; 
    }

    char url[256];
    snprintf(url, sizeof(url), "https://%s/tasks/%" PRIu32 ".json?auth=%s", 
             FIREBASE_HOST, task_id, FIREBASE_AUTH);

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_DELETE,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return ESP_FAIL;

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status_code = esp_http_client_get_status_code(client);
        if (status_code == 200) {
            ESP_LOGI(TAG, "Xoá task #%" PRIu32 " thành công trên Firebase", task_id);
        } else {
            ESP_LOGW(TAG, "Lỗi DELETE Firebase HTTP status: %d", status_code);
            err = ESP_FAIL;
        }
    } else {
        ESP_LOGE(TAG, "Lỗi HTTP DELETE: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    return err;
}

/* --------------------------------------------------------------------------
 * FreeRTOS Task: Background Uploader
 * -------------------------------------------------------------------------- */
static void firebase_sync_task(void *pvParameters)
{
    sync_msg_t msg;

    while (1) {
        if (xQueueReceive(s_sync_queue, &msg, portMAX_DELAY)) {
            // Đợi có mạng nếu đang mất
            while (!wifi_manager_is_connected()) {
                vTaskDelay(pdMS_TO_TICKS(5000));
            }

            if (msg.type == SYNC_REQ_UPLOAD) {
                http_put_task(&msg.task_data);
            } else if (msg.type == SYNC_REQ_DELETE) {
                http_delete_task(msg.task_id);
            }
            
            // Delay nhỏ xíu tránh spam host
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */
esp_err_t firebase_sync_init(void)
{
    if (s_sync_queue == NULL) {
        s_sync_queue = xQueueCreate(20, sizeof(sync_msg_t)); // Hàng đợi 20 thao tác
        if (s_sync_queue == NULL) {
            ESP_LOGE(TAG, "Không thể tạo sync queue");
            return ESP_FAIL;
        }

        // Tạo background task (Stack 6KB)
        xTaskCreate(firebase_sync_task, "firebase_sync_task", 6144, NULL, 5, NULL);
        ESP_LOGI(TAG, "Firebase Sync Background Task Started");
    }
    return ESP_OK;
}

esp_err_t firebase_sync_upload_task(const task_record_t *task)
{
    if (!s_sync_queue || !task) return ESP_FAIL;

    sync_msg_t msg = {
        .type = SYNC_REQ_UPLOAD,
        .task_id = task->id,
        .task_data = *task // Copy struct
    };

    if (xQueueSend(s_sync_queue, &msg, 0) != pdTRUE) {
        ESP_LOGE(TAG, "Sync queue đầy, mất kiện UPLOAD task %" PRIu32, task->id);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t firebase_sync_upload_all(void)
{
    const task_index_t *index = task_database_get_index();
    if (index->count == 0) {
        ESP_LOGI(TAG, "CSDL trống, không có gì để upload.");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Bắt đầu enqueue %d task để đẩy lên Cloud...", index->count);
    int pushed_count = 0;

    for (int i = 0; i < index->count; i++) {
        task_record_t task;
        if (task_database_read(index->entries[i].id, &task) == ESP_OK) {
            if (firebase_sync_upload_task(&task) == ESP_OK) {
                pushed_count++;
            }
        }
    }

    ESP_LOGI(TAG, "Đã xếp hàng %d/%d task vào hàng đợi đồng bộ Firebase.", pushed_count, index->count);
    return ESP_OK;
}

esp_err_t firebase_sync_delete_task(uint32_t task_id)
{
    if (!s_sync_queue) return ESP_FAIL;

    sync_msg_t msg = {
        .type = SYNC_REQ_DELETE,
        .task_id = task_id,
    };

    if (xQueueSend(s_sync_queue, &msg, 0) != pdTRUE) {
        ESP_LOGE(TAG, "Sync queue đầy, mất kiện DELETE task %" PRIu32, task_id);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* --------------------------------------------------------------------------
 * HTTP Client: Pull Toàn Bộ Data JSON (Chạy đồng bộ, Block)
 * -------------------------------------------------------------------------- */
#define MAX_HTTP_RECV_BUFFER 32768 // 32KB buffer tải từ DB (nằm trong Heap)

esp_err_t firebase_sync_download_all(void)
{
    if (strlen(FIREBASE_HOST) == 0 || strcmp(FIREBASE_HOST, "YOUR_FIREBASE_HOST_HERE") == 0) {
        ESP_LOGW(TAG, "Chưa config Firebase. Bỏ qua đồng bộ khởi động.");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Bắt đầu Pull Firebase Data...");

    char url[256];
    snprintf(url, sizeof(url), "https://%s/tasks.json?auth=%s", 
             FIREBASE_HOST, FIREBASE_AUTH);

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Lỗi HTTP Client Download All");
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Lỗi mở client: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    esp_http_client_fetch_headers(client);
    if (esp_http_client_get_status_code(client) != 200) {
        ESP_LOGW(TAG, "Firebase get status = %d", esp_http_client_get_status_code(client));
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    char *buffer = (char *)malloc(MAX_HTTP_RECV_BUFFER);
    if (buffer == NULL) {
        ESP_LOGE(TAG, "Không đủ RAM %d bytes để chứa danh sách Database", MAX_HTTP_RECV_BUFFER);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    int total_read_len = 0;
    int read_len;
    while (1) {
        read_len = esp_http_client_read(client, buffer + total_read_len, MAX_HTTP_RECV_BUFFER - total_read_len - 1);
        if (read_len <= 0) break;
        total_read_len += read_len;
        if (total_read_len >= MAX_HTTP_RECV_BUFFER - 1) {
            ESP_LOGW(TAG, "Mạng trả về quá nhiều dữ liệu (> %d KB)! Cắt bớt.", MAX_HTTP_RECV_BUFFER/1024);
            break;
        }
    }
    buffer[total_read_len] = '\0';
    esp_http_client_cleanup(client);

    // Kịch bản DB rỗng trên firebase trả về "null"
    if (strcmp(buffer, "null") == 0 || total_read_len <= 4) {
        ESP_LOGI(TAG, "Firebase hiện rỗng.");
        free(buffer);
        return ESP_OK;
    }

    /* Parse JSON bằng cJSON */
    cJSON *root = cJSON_Parse(buffer);
    if (!root) {
        ESP_LOGE(TAG, "Lỗi Parse JSON Firebase trả về.");
        free(buffer);
        return ESP_FAIL;
    }

    cJSON *item = NULL;
    int restored_count = 0;

    // Firebase khi get /tasks.json sẽ trả về Object chứa Key là ID (VD: "392": { ... })
    // hoac Array neu ID bat dau tu 0 (do tinh chat tu dong array hoa cua Realtime Database).
    // De tuong thich ta lap qua tap hop item root do.
    cJSON_ArrayForEach(item, root) {
        if (cJSON_IsObject(item)) {
            task_record_t t;
            memset(&t, 0, sizeof(task_record_t)); // Init
            
            t.id = cJSON_GetObjectItem(item, "id") ? cJSON_GetObjectItem(item, "id")->valueint : 0;
            if (t.id == 0) continue;

            const char *title = cJSON_GetObjectItem(item, "title") ? cJSON_GetObjectItem(item, "title")->valuestring : "";
            strncpy(t.title, title, sizeof(t.title) - 1);

            const char *type = cJSON_GetObjectItem(item, "type") ? cJSON_GetObjectItem(item, "type")->valuestring : "other";
            strncpy(t.type, type, sizeof(t.type) - 1);

            t.created_at = cJSON_GetObjectItem(item, "created_at") ? cJSON_GetObjectItem(item, "created_at")->valuedouble : 0;
            t.due_time = cJSON_GetObjectItem(item, "due_time") ? cJSON_GetObjectItem(item, "due_time")->valuedouble : 0;
            t.start_time = cJSON_GetObjectItem(item, "start_time") ? cJSON_GetObjectItem(item, "start_time")->valuedouble : 0;
            t.reminder = cJSON_GetObjectItem(item, "reminder") ? cJSON_GetObjectItem(item, "reminder")->valuedouble : 0;
            
            const char *repeat = cJSON_GetObjectItem(item, "repeat") ? cJSON_GetObjectItem(item, "repeat")->valuestring : "none";
            strncpy(t.repeat, repeat, sizeof(t.repeat) - 1);
            
            t.repeat_interval = cJSON_GetObjectItem(item, "repeat_interval") ? cJSON_GetObjectItem(item, "repeat_interval")->valueint : 0;
            
            const char *status = cJSON_GetObjectItem(item, "status") ? cJSON_GetObjectItem(item, "status")->valuestring : "pending";
            strncpy(t.status, status, sizeof(t.status) - 1);

            const char *notes = cJSON_GetObjectItem(item, "notes") ? cJSON_GetObjectItem(item, "notes")->valuestring : "";
            strncpy(t.notes, notes, sizeof(t.notes) - 1);

            t.completed_at = cJSON_GetObjectItem(item, "completed_at") ? cJSON_GetObjectItem(item, "completed_at")->valuedouble : 0;

            // Xoá index cũ và insert DB nội bộ (KHÔNG GỌI API EMBEDDING ngay lúc pull để tránh treo mạng, ta có rebuild boot phase sau đó)
            if (task_database_write_raw(&t) == ESP_OK) {
                 restored_count++;
            }
        }
    }

    if (restored_count > 0) {
        task_database_save_index();
        ESP_LOGI(TAG, "Hoàn tất khôi phục %d task từ Firebase về SPIFFS.", restored_count);
    }

    cJSON_Delete(root);
    free(buffer);

    return ESP_OK;
}
