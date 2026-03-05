/**
 * ===========================================================================
 * @file action_dispatcher.h
 * @brief Phân phối tin nhắn đến action handler tương ứng
 *
 * Kiến trúc 2 bước:
 *   B1: Gọi AI xác định intent (1 trong 9 loại)
 *   B2: Gọi AI parse chi tiết (filters/data) theo intent
 *   → Dispatch đến handler tương ứng
 * ===========================================================================
 */

#ifndef ACTION_DISPATCHER_H
#define ACTION_DISPATCHER_H

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>
#include <time.h>

/* ============================== Intent Enum ============================== */

typedef enum {
    /* Mutation */
    ACTION_CREATE_TASK = 0,
    ACTION_UPDATE_TASK,
    ACTION_COMPLETE_TASK,
    ACTION_DELETE_TASK,
    /* Query */
    ACTION_QUERY_TASKS,
    ACTION_GET_DETAIL,
    ACTION_SEARCH_SEMANTIC,
    ACTION_TASK_SUMMARY,
    /* Other */
    ACTION_CHITCHAT,
    ACTION_UNKNOWN
} action_type_t;

/* =========================== Filter Schema ============================== */

typedef enum {
    FILTER_OP_EQUALS,       /* field == value */
    FILTER_OP_NOT_EQUALS,   /* field != value */
    FILTER_OP_BEFORE,       /* field < value (cho time) */
    FILTER_OP_AFTER,        /* field > value (cho time) */
    FILTER_OP_BETWEEN,      /* value <= field <= value_end */
    FILTER_OP_CONTAINS,     /* strstr(field, value) */
    FILTER_OP_IS_NULL,      /* field == 0 hoặc rỗng */
    FILTER_OP_IS_NOT_NULL,  /* field != 0 */
} filter_op_t;

typedef struct {
    char field[20];         /* "due_time","type","status","repeat"... */
    filter_op_t op;
    time_t time_value;      /* cho các field time_t */
    time_t time_value_end;  /* cho op BETWEEN */
    char str_value[32];     /* cho các field string */
} query_filter_t;

#define MAX_FILTERS 5

typedef struct {
    char response_type[8];  /* "list" | "count" */
    char label[64];         /* mô tả câu hỏi */
    query_filter_t filters[MAX_FILTERS];
    int filter_count;
    char sort[20];          /* "due_time_asc" ... */
    int limit;              /* 0 = không limit */
} query_request_t;

/* ========================== Dispatcher API =============================== */

/**
 * @brief Xử lý tin nhắn: B1 (intent) → B2 (parse) → dispatch → response
 */
esp_err_t action_dispatcher_handle(const char *user_message,
                                    char *response_buffer,
                                    size_t buffer_size);

/**
 * @brief Ghi nhận danh sách task ID vừa hiển thị cho user (context)
 */
void dispatcher_set_context_tasks(const uint32_t *ids, int count);

/**
 * @brief Lấy JSON của action cuối cùng vừa thực hiện
 */
void action_dispatcher_get_last_json(char *buffer, size_t buffer_size);

/* ========================= Action Handlers =============================== */

esp_err_t action_create_task(const char *data_json, char *response, size_t response_size);
esp_err_t action_update_task(const char *data_json, char *response, size_t response_size);
esp_err_t action_complete_task(const char *data_json, char *response, size_t response_size);
esp_err_t action_delete_task(const char *data_json, char *response, size_t response_size);
esp_err_t action_delete_confirm_hard(char *response, size_t response_size);
esp_err_t action_query_tasks(const char *data_json, char *response, size_t response_size);
esp_err_t action_get_detail(const char *data_json, char *response, size_t response_size);
esp_err_t action_search_semantic(const char *data_json, char *response, size_t response_size);
esp_err_t action_task_summary(const char *data_json, char *response, size_t response_size);

#endif /* ACTION_DISPATCHER_H */
