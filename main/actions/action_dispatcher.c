/**
 * ===========================================================================
 * @file action_dispatcher.c
 * @brief Kiến trúc 2 bước: B1 (Intent) → B2 (Parse) → Handler
 *
 * B1: Gọi AI xác định intent (9 loại) với prompt ngắn gọn.
 * B2: Gọi AI parse chi tiết (filters/data) với prompt chuyên biệt.
 * Sau đó dispatch đến handler tương ứng.
 * ===========================================================================
 */

#include "action_dispatcher.h"
#include "task_database.h"
#include "openai_client.h"
#include "json_parser.h"
#include "time_utils.h"
#include "config.h"
#include "vector_search.h"
#include "display_manager.h"
#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "dispatcher";

/* Context: danh sách task ID đang hiển thị cho user */
static uint32_t s_context_task_ids[20];
static int s_context_task_count = 0;
static char s_last_action_json[JSON_BUFFER_SIZE] = "(chưa có action nào)";

void dispatcher_set_context_tasks(const uint32_t *ids, int count)
{
    if (count > 20) count = 20;
    s_context_task_count = count;
    if (ids != NULL && count > 0) {
        memcpy(s_context_task_ids, ids, count * sizeof(uint32_t));
    }
}

void action_dispatcher_get_last_json(char *buffer, size_t buffer_size)
{
    if (buffer == NULL || buffer_size == 0) return;
    strncpy(buffer, s_last_action_json, buffer_size - 1);
    buffer[buffer_size - 1] = '\0';
}

/* ==========================================================================
 * PROMPT B1: Intent Classification (ngắn gọn, ~500 tokens)
 * ========================================================================== */
static const char *PROMPT_B1 =
    "Bạn là bộ phân loại ý định cho hệ thống quản lý công việc.\n"
    "Thời gian hiện tại: %s, %s (%s).\n"
    "Context IDs đang hiển thị: [%s].\n\n"
    "Phân loại tin nhắn vào ĐÚNG 1 trong 9 intent:\n\n"
    "MUTATION:\n"
    "• CREATE_TASK — Tạo task/lịch/nhắc việc mới\n"
    "• UPDATE_TASK — Sửa thông tin task (đổi hạn, đổi tên, đổi nhắc nhở...)\n"
    "• COMPLETE_TASK — Đánh dấu hoàn thành (\"xong\", \"đã làm\", \"hoàn thành\")\n"
    "• DELETE_TASK — Hủy hoặc xóa task (\"hủy\", \"bỏ\", \"xóa\", \"delete\")\n\n"
    "QUERY:\n"
    "• QUERY_TASKS — Hỏi danh sách, đếm, lọc (theo thời gian, loại, trạng thái, quá hạn, sắp tới, định kỳ)\n"
    "• GET_TASK_DETAIL — Hỏi chi tiết 1 task cụ thể (thông tin, thời gian, ghi chú)\n"
    "• SEARCH_SEMANTIC — Tìm task theo nội dung/tên, không có hành động cụ thể\n"
    "• TASK_SUMMARY — Thống kê tổng quan (\"tổng kết\", \"báo cáo tiến độ\")\n\n"
    "KHÁC:\n"
    "• CHITCHAT — Không liên quan đến công việc\n\n"
    "QUY TẮC:\n"
    "1. \"bao nhiêu\", \"mấy cái\" → QUERY_TASKS\n"
    "2. \"khi nào\", \"thời hạn\" của 1 task cụ thể → GET_TASK_DETAIL\n"
    "3. Nhập tên task không kèm hành động → SEARCH_SEMANTIC\n"
    "4. \"tổng kết\", \"tình hình chung\" → TASK_SUMMARY\n"
    "5. \"hủy\"/\"bỏ\" hoặc \"xóa hẳn\" → DELETE_TASK\n"
    "6. Sự kiện kéo dài nhiều ngày → CREATE_TASK\n"
    "7. Hỏi CHUNG về danh sách/tất cả (\"các ngày kỉ niệm\", \"những sinh nhật\", \"tất cả lễ\") → QUERY_TASKS\n"
    "8. Hỏi về 1 task CỤ THỂ bằng tên riêng (\"sinh nhật vợ\", \"báo cáo X\", \"họp Y\") → SEARCH_SEMANTIC (ưu tiên hơn quy tắc 7)\n"
    "9. Khi không chắc → ƯU TIÊN QUERY_TASKS hoặc SEARCH_SEMANTIC, TRÁNH CHITCHAT\n\n"
    "CHỈ trả JSON thuần, KHÔNG markdown:\n"
    "{\"intent\": \"...\", \"confidence\": 0.0-1.0}";

/* ==========================================================================
 * PROMPT B2: Chuyên biệt cho từng intent
 * ========================================================================== */

static const char *PROMPT_B2_CREATE =
    "Bạn là parser dữ liệu task. Hiện tại: %s, %s (%s).\n"
    "Intent: CREATE_TASK\n"
    "User: \"%s\"\n\n"
    "Parse thông tin và trả JSON:\n"
    "{\n"
    "  \"title\": \"string\",\n"
    "  \"type\": \"meeting|report|reminder|event|anniversary|other\",\n"
    "  \"start_time\": \"ISO8601|null\",\n"
    "  \"due_time\": \"ISO8601|null\",\n"
    "  \"reminder\": \"ISO8601|null\",\n"
    "  \"repeat\": \"none|daily|weekly|monthly|quarterly|yearly\",\n"
    "  \"repeat_interval\": 1,\n"
    "  \"notes\": \"string\"\n"
    "}\n\n"
    "QUY TẮC:\n"
    "1. Sự kiện nhiều ngày: start_time=ngày bắt đầu, due_time=ngày kết thúc\n"
    "2. Chỉ 1 ngày: due_time=ngày đó, start_time=null\n"
    "3. type: báo cáo=report, họp=meeting, nhắc=reminder, sự kiện/cưới/tiệc=event, kỉ niệm/sinh nhật=anniversary\n"
    "4. Không nói giờ → mặc định 08:00\n"
    "CHỈ JSON thuần.";

static const char *PROMPT_B2_QUERY =
    "Bạn là parser truy vấn. Hiện tại: %s, %s (%s).\n"
    "Intent: QUERY_TASKS\n"
    "User: \"%s\"\n\n"
    "Phân tích CẨN THẬN tất cả điều kiện. Mỗi điều kiện = 1 filter.\n\n"
    "QUAN TRỌNG: Tất cả giá trị time PHẢI là ISO8601 thực tế (VD: 2026-03-04T00:00:00).\n"
    "Dựa vào ngày hôm nay để tính toán chính xác.\n"
    "KHÔNG dùng placeholder như {now}, {today}, {monday}.\n\n"
    "Trả JSON:\n"
    "{\n"
    "  \"response_type\": \"list|count\",\n"
    "  \"label\": \"string mô tả ngắn gọn\",\n"
    "  \"filters\": [\n"
    "    {\"field\": \"due_time|start_time|created_at|type|status|repeat|repeat_interval|reminder\",\n"
    "     \"op\": \"equals|not_equals|before|after|between|is_not_null|is_null\",\n"
    "     \"value\": \"string|ISO8601\", \"value_end\": \"ISO8601 nếu between\"}\n"
    "  ],\n"
    "  \"sort\": \"due_time_asc|due_time_desc|created_at_desc|null\",\n"
    "  \"limit\": null\n"
    "}\n\n"
    "QUY TẮC:\n"
    "1. LUÔN thêm status=pending trừ khi user nói rõ. (Hệ thống tự động gộp pending và overdue. Nếu user CHỈ hỏi task quá hạn, dùng status=overdue).\n"
    "2. Hỏi về task LẶP LẠI → filter repeat, KHÔNG thêm filter due_time (vì due_time gốc có thể ở quá khứ)\n"
    "3. Một tuần LUÔN bắt đầu từ Thứ 2 và kết thúc vào Chủ Nhật.\n"
    "4. 'Tuần này': T2 đến CN tuần hiện tại. 'Tuần sau': T2 đến CN tuần kế tiếp.\n"
    "   Ví dụ: Nếu hôm nay là Thứ 5, 05/03/2026 -> Tuần sau là 2026-03-09T00:00:00 đến 2026-03-15T23:59:59.\n"
    "5. 'Deadline' trong khoảng thời gian (hôm nay, tuần này): Nhớ dùng `op=\"before\"` với thời điểm KẾT THÚC của khoảng đó. Các task quá hạn trước đó vẫn sẽ xuất hiện vì chúng có status là overdue/pending.\n"
    "6. User đề cập LOẠI task (báo cáo, họp, nhắc, kỉ niệm...) → LUÔN thêm filter type tương ứng (report, meeting, reminder, anniversary, event, other)\n"
    "CHỈ JSON thuần.";

static const char *PROMPT_B2_MUTATE =
    "Bạn là parser hành động. Hiện tại: %s, %s (%s).\n"
    "Intent: %s\n"
    "User: \"%s\"\n"
    "Context IDs: [%s]\n\n"
    "Trả JSON:\n"
    "{\n"
    "  \"task_ids\": [number],\n"
    "  \"search_query\": \"string nếu không có ID\",\n"
    "  \"delete_mode\": \"soft|hard (chỉ DELETE_TASK)\",\n"
    "  \"updates\": {\n"
    "    \"title\": \"string|null\",\n"
    "    \"type\": \"string|null\",\n"
    "    \"due_time\": \"ISO8601|none|null\",\n"
    "    \"start_time\": \"ISO8601|none|null\",\n"
    "    \"reminder\": \"ISO8601|none|null\",\n"
    "    \"due_offset_days\": \"number|null\",\n"
    "    \"reminder_offset_days\": \"number|null\",\n"
    "    \"repeat\": \"string|null\",\n"
    "    \"repeat_interval\": \"number|null\",\n"
    "    \"notes\": \"string|null\"\n"
    "  }\n"
    "}\n\n"
    "COMPLETE_TASK: chỉ cần task_ids hoặc search_query.\n"
    "DELETE_TASK: hủy/bỏ=soft, xóa hẳn=hard.\n"
    "UPDATE_TASK: chỉ trả field CẦN ĐỔI, field KHÔNG đề cập → null (giữ nguyên). KHÔNG tự ý set field thành \"none\" nếu user không yêu cầu xóa field đó.\n"
    "\"none\" = xóa giá trị field (set=0), CHỈ dùng khi user NÓI RÕ muốn xóa/bỏ field đó.\n"
    "\"các task đó/này\" → dùng Context IDs.\n"
    "CHỈ JSON thuần.";

static const char *PROMPT_B2_DETAIL =
    "Bạn là parser truy vấn chi tiết. Hiện tại: %s, %s (%s).\n"
    "Intent: GET_TASK_DETAIL\n"
    "User: \"%s\"\n"
    "Context IDs: [%s]\n\n"
    "Trả JSON:\n"
    "{\"task_ids\": [number], \"search_query\": \"string nếu mô tả tên task\"}\n\n"
    "QUY TẮC:\n"
    "1. User nói RÕ số ID (\"task 3\", \"#5\") → task_ids=[số đó]\n"
    "2. User nói TÊN task (\"sinh nhật Linh\", \"báo cáo X\") → task_ids=[], search_query=\"tên task\"\n"
    "3. KHÔNG tự đoán ID nếu user không nói số. KHÔNG bịa ID.\n"
    "CHỈ JSON thuần.";

static const char *PROMPT_B2_SEARCH =
    "Bạn là parser tìm kiếm.\n"
    "Intent: SEARCH_SEMANTIC\n"
    "User: \"%s\"\n\n"
    "Trả JSON:\n"
    "{\"search_query\": \"cụm từ chính\", \"status_filter\": \"pending|done|cancelled|null\"}\n"
    "CHỈ JSON thuần.";

static const char *PROMPT_B2_SUMMARY =
    "Bạn là parser thống kê. Hiện tại: %s, %s (%s).\n"
    "Intent: TASK_SUMMARY\n"
    "User: \"%s\"\n\n"
    "Trả JSON:\n"
    "{\"period_start\": \"ISO8601|null\", \"period_end\": \"ISO8601|null\"}\n"
    "Mặc định: đầu tháng → hôm nay.\n"
    "CHỈ JSON thuần.";

static const char *PROMPT_B2_CHITCHAT =
    "Bạn là trợ lý quản lý công việc. User vừa gửi một tin nhắn chitchat: \"%s\"\n\n"
    "Nhiệm vụ của bạn:\n"
    "Gợi ý 1 câu lệnh quản lý công việc (CREATE, QUERY, hoặc GET_DETAIL) phù hợp để kéo User quay lại công việc.\n"
    "Trả về JSON: {\"suggestion\": \"Câu lệnh gợi ý (< 50 ký tự)\"}\n"
    "CHỈ JSON thuần.";

/* ==========================================================================
 * Helpers
 * ========================================================================== */

/** Build context IDs string */
static void build_context_ids_str(char *buf, size_t buf_size)
{
    buf[0] = '\0';
    int offset = 0;
    for (int i = 0; i < s_context_task_count && offset < (int)buf_size - 10; i++) {
        offset += snprintf(buf + offset, buf_size - offset,
                           "%" PRIu32 "%s", s_context_task_ids[i],
                           (i == s_context_task_count - 1) ? "" : ", ");
    }
}

/** Parse intent string → enum */
static action_type_t parse_intent(const char *intent_str)
{
    if (intent_str == NULL) return ACTION_UNKNOWN;

    if (strcmp(intent_str, "CREATE_TASK") == 0)      return ACTION_CREATE_TASK;
    if (strcmp(intent_str, "UPDATE_TASK") == 0)       return ACTION_UPDATE_TASK;
    if (strcmp(intent_str, "COMPLETE_TASK") == 0)     return ACTION_COMPLETE_TASK;
    if (strcmp(intent_str, "DELETE_TASK") == 0)       return ACTION_DELETE_TASK;
    if (strcmp(intent_str, "QUERY_TASKS") == 0)       return ACTION_QUERY_TASKS;
    if (strcmp(intent_str, "GET_TASK_DETAIL") == 0)   return ACTION_GET_DETAIL;
    if (strcmp(intent_str, "SEARCH_SEMANTIC") == 0)   return ACTION_SEARCH_SEMANTIC;
    if (strcmp(intent_str, "TASK_SUMMARY") == 0)      return ACTION_TASK_SUMMARY;
    if (strcmp(intent_str, "CHITCHAT") == 0)          return ACTION_CHITCHAT;

    return ACTION_UNKNOWN;
}

/* ==========================================================================
 * B1: Intent Classification
 * ========================================================================== */
static esp_err_t step_b1_classify(const char *user_message,
                                   action_type_t *out_intent,
                                   float *out_confidence,
                                   char **out_b1_json)
{
    char date_buf[32], weekday_buf[32], iso_buf[32], context_ids[128];
    time_t now = time_utils_get_now();
    time_utils_format_date_short(now, date_buf, sizeof(date_buf));
    time_utils_get_weekday_name(now, weekday_buf, sizeof(weekday_buf));
    time_utils_format_iso8601(now, iso_buf, sizeof(iso_buf));
    build_context_ids_str(context_ids, sizeof(context_ids));

    /* Build prompt B1 */
    char prompt[2048];
    snprintf(prompt, sizeof(prompt), PROMPT_B1, iso_buf, date_buf, weekday_buf, context_ids);

    /* Gọi AI */
    char llm_response[512];
    esp_err_t err = openai_chat_completion(prompt, user_message,
                                            llm_response, sizeof(llm_response));
    if (err != ESP_OK) return err;

    /* Parse JSON */
    cJSON *parsed = json_extract_from_text(llm_response);
    if (parsed == NULL) {
        ESP_LOGE(TAG, "B1: Không parse được: %s", llm_response);
        return ESP_FAIL;
    }

    const char *intent_str = json_get_string(parsed, "intent", "UNKNOWN");
    *out_intent = parse_intent(intent_str);
    *out_confidence = (float)json_get_double(parsed, "confidence", 0.0);

    ESP_LOGI(TAG, "B1: intent=%s, confidence=%.2f", intent_str, *out_confidence);
    if (out_b1_json != NULL) {
        *out_b1_json = cJSON_Print(parsed);
    }

    cJSON_Delete(parsed);

    return ESP_OK;
}

/* ==========================================================================
 * B2: Query Parser — build prompt chuyên biệt theo intent
 * ========================================================================== */
static esp_err_t step_b2_parse(action_type_t intent,
                                const char *user_message,
                                char **out_data_json)
{
    char date_buf[32], weekday_buf[32], iso_buf[32], context_ids[128];
    time_t now = time_utils_get_now();
    time_utils_format_date_short(now, date_buf, sizeof(date_buf));
    time_utils_get_weekday_name(now, weekday_buf, sizeof(weekday_buf));
    time_utils_format_iso8601(now, iso_buf, sizeof(iso_buf));
    build_context_ids_str(context_ids, sizeof(context_ids));

    char prompt[2048];

    switch (intent) {
        case ACTION_CREATE_TASK:
            snprintf(prompt, sizeof(prompt), PROMPT_B2_CREATE,
                     iso_buf, date_buf, weekday_buf, user_message);
            break;

        case ACTION_QUERY_TASKS:
            snprintf(prompt, sizeof(prompt), PROMPT_B2_QUERY,
                     iso_buf, date_buf, weekday_buf, user_message);
            break;

        case ACTION_UPDATE_TASK:
            snprintf(prompt, sizeof(prompt), PROMPT_B2_MUTATE,
                     iso_buf, date_buf, weekday_buf, "UPDATE_TASK", user_message, context_ids);
            break;

        case ACTION_COMPLETE_TASK:
            snprintf(prompt, sizeof(prompt), PROMPT_B2_MUTATE,
                     iso_buf, date_buf, weekday_buf, "COMPLETE_TASK", user_message, context_ids);
            break;

        case ACTION_DELETE_TASK:
            snprintf(prompt, sizeof(prompt), PROMPT_B2_MUTATE,
                     iso_buf, date_buf, weekday_buf, "DELETE_TASK", user_message, context_ids);
            break;

        case ACTION_GET_DETAIL:
            snprintf(prompt, sizeof(prompt), PROMPT_B2_DETAIL,
                     iso_buf, date_buf, weekday_buf, user_message, context_ids);
            break;

        case ACTION_SEARCH_SEMANTIC:
            snprintf(prompt, sizeof(prompt), PROMPT_B2_SEARCH,
                     user_message);
            break;

        case ACTION_TASK_SUMMARY:
            snprintf(prompt, sizeof(prompt), PROMPT_B2_SUMMARY,
                     iso_buf, date_buf, weekday_buf, user_message);
            break;

        case ACTION_CHITCHAT:
            snprintf(prompt, sizeof(prompt), PROMPT_B2_CHITCHAT,
                     user_message);
            break;

        default:
            return ESP_FAIL;
    }

    /* Gọi AI lần 2 */
    char llm_response[JSON_BUFFER_SIZE];
    esp_err_t err = openai_chat_completion(prompt, user_message,
                                            llm_response, sizeof(llm_response));
    if (err != ESP_OK) return err;

    /* Extract JSON */
    cJSON *parsed = json_extract_from_text(llm_response);
    if (parsed == NULL) {
        ESP_LOGE(TAG, "B2: Không parse được: %s", llm_response);
        return ESP_FAIL;
    }

    *out_data_json = cJSON_Print(parsed);
    cJSON_Delete(parsed);

    ESP_LOGI(TAG, "B2: data=%s", *out_data_json ? *out_data_json : "(null)");
    return ESP_OK;
}

/* ==========================================================================
 * Xử lý tin nhắn chính: B1 → B2 → Dispatch
 * ========================================================================== */
esp_err_t action_dispatcher_handle(const char *user_message,
                                    char *response_buffer,
                                    size_t buffer_size)
{
    if (user_message == NULL || response_buffer == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, ">>> Xử lý: %s", user_message);

    /* ======= Xử lý lệnh nhanh (Quick Commands) ======= */
    if (strcmp(user_message, "/alltask") == 0) {
        ESP_LOGI(TAG, "Quick command: /alltask");
        const char *all_query = "{\"response_type\":\"short\",\"label\":\"tất cả công việc\",\"filters\":[]}";
        return action_query_tasks(all_query, response_buffer, buffer_size);
    }

    if (strcmp(user_message, "/confirm") == 0) {
        ESP_LOGI(TAG, "Quick command: /confirm hard delete");
        return action_delete_confirm_hard(response_buffer, buffer_size);
    }

    /* ======= B1: Xác định intent ======= */
    action_type_t intent = ACTION_UNKNOWN;
    float confidence = 0.0f;

    char *b1_json = NULL;

    esp_err_t err = step_b1_classify(user_message, &intent, &confidence, &b1_json);
    if (err != ESP_OK) {
        snprintf(response_buffer, buffer_size,
            "\xE2\x9A\xA0\xEF\xB8\x8F Lỗi kết nối AI. Vui lòng thử lại sau.");
        return err;
    }

    /* Kiểm tra confidence */
    if (confidence < 0.8f || intent == ACTION_UNKNOWN) {
        if (b1_json != NULL) {
            snprintf(s_last_action_json, sizeof(s_last_action_json), "【B1 Intent】\n%s\n\n【B2 Parse】\n(không thực hiện do confidence thấp)", b1_json);
            free(b1_json);
        }
        snprintf(response_buffer, buffer_size,
            "\xE2\x9D\x93 Tôi không chắc chắn hiểu yêu cầu này (%.0f%%). "
            "Bạn có thể nói rõ hơn được không?\n\n"
            "\xF0\x9F\x92\xA1 Ví dụ:\n"
            "\xE2\x80\xA2 \"Nhắc tôi nộp báo cáo ngày 31/3\"\n"
            "\xE2\x80\xA2 \"Tuần này có gì?\"\n"
            "\xE2\x80\xA2 \"Hoàn thành task báo cáo\"",
            confidence * 100);
        return ESP_OK;
    }

    /* ======= B2: Parse chi tiết ======= */
    char *data_json = NULL;
    err = step_b2_parse(intent, user_message, &data_json);
    if (err != ESP_OK) {
        if (b1_json != NULL) {
            snprintf(s_last_action_json, sizeof(s_last_action_json), "【B1 Intent】\n%s\n\n【B2 Parse】\n(Lỗi)", b1_json);
            free(b1_json);
        }
        snprintf(response_buffer, buffer_size,
            "\xE2\x9A\xA0\xEF\xB8\x8F Không phân tích được yêu cầu. Vui lòng thử lại.");
        return err;
    }

    /* Lưu JSON cuối cùng để hỗ trợ lệnh /last */
    if (b1_json != NULL || data_json != NULL) {
        snprintf(s_last_action_json, sizeof(s_last_action_json), 
                 "【B1 Intent】\n%s\n\n【B2 Parse】\n%s", 
                 b1_json ? b1_json : "(null)", 
                 data_json ? data_json : "(null)");
    }
    
    if (b1_json != NULL) {
        free(b1_json);
        b1_json = NULL;
    }

    /* CHITCHAT: Xử lý gợi ý từ B2 */
    if (intent == ACTION_CHITCHAT) {
        cJSON *root = cJSON_Parse(data_json);
        const char *sug = json_get_string(root, "suggestion", "");
        if (strlen(sug) > 0) {
            snprintf(response_buffer, buffer_size, "SUGGEST|%s", sug);
        } else {
            snprintf(response_buffer, buffer_size, 
                "\xF0\x9F\x98\x8A Tôi là trợ lý quản lý công việc. Hãy thử hỏi về lịch trình của bạn nhé!");
        }
        if (root) cJSON_Delete(root);
        if (data_json) free(data_json);
        return ESP_OK;
    }

    /* ======= Dispatch đến handler ======= */
    switch (intent) {
        case ACTION_CREATE_TASK:
            err = action_create_task(data_json, response_buffer, buffer_size);
            break;
        case ACTION_UPDATE_TASK:
            err = action_update_task(data_json, response_buffer, buffer_size);
            break;
        case ACTION_COMPLETE_TASK:
            err = action_complete_task(data_json, response_buffer, buffer_size);
            break;
        case ACTION_DELETE_TASK:
            err = action_delete_task(data_json, response_buffer, buffer_size);
            break;
        case ACTION_QUERY_TASKS:
            err = action_query_tasks(data_json, response_buffer, buffer_size);
            break;
        case ACTION_GET_DETAIL:
            err = action_get_detail(data_json, response_buffer, buffer_size);
            break;
        case ACTION_SEARCH_SEMANTIC:
            err = action_search_semantic(data_json, response_buffer, buffer_size);
            break;
        case ACTION_TASK_SUMMARY:
            err = action_task_summary(data_json, response_buffer, buffer_size);
            break;
        default:
            snprintf(response_buffer, buffer_size,
                "\xE2\x9D\x93 Không hiểu yêu cầu. Vui lòng thử lại.");
            err = ESP_OK;
            break;
    }

    /* Dọn dẹp */
    if (data_json != NULL) {
        free(data_json);
    }

    return err;
}
