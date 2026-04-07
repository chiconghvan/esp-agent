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

#include "safe_append.h"
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

/* Pending selection state */
static action_type_t s_pending_intent = ACTION_UNKNOWN;
static char *s_pending_data_json = NULL;

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

void dispatcher_set_pending_action(action_type_t intent, const char *data_json)
{
    s_pending_intent = intent;
    if (s_pending_data_json) free(s_pending_data_json);
    s_pending_data_json = data_json ? strdup(data_json) : NULL;
    ESP_LOGI(TAG, "Set pending action: intent=%d", (int)intent);
}

void dispatcher_clear_pending_action(void)
{
    s_pending_intent = ACTION_UNKNOWN;
    if (s_pending_data_json) {
        free(s_pending_data_json);
        s_pending_data_json = NULL;
    }
}

static bool is_id_list(const char *msg, uint32_t *out_ids, int *out_count, int max_ids)
{
    if (msg == NULL || strlen(msg) == 0) return false;
    
    // Kiểm tra xem message có chứa số không
    bool has_digit = false;
    for (const char *p = msg; *p; p++) {
        if (*p >= '0' && *p <= '9') { has_digit = true; break; }
    }
    if (!has_digit) return false;

    // Danh sách các từ được phép xuất hiện (ngoài số và dấu ngăn cách)
    // "chọn", "cái", "số", "task", "và", "id", "#"
    // Nếu có từ khác lạ -> không phải ID list thuần túy -> để AI xử lý
    
    char tmp[256];
    strncpy(tmp, msg, sizeof(tmp)-1);
    tmp[sizeof(tmp)-1] = '\0';
    
    *out_count = 0;
    char *tok = strtok(tmp, " ,;#\t\n\r");
    while (tok != NULL && *out_count < max_ids) {
        // Kiểm tra xem token này là số hay là từ khóa bổ trợ
        char *endptr;
        uint32_t id = (uint32_t)strtoul(tok, &endptr, 10);
        
        if (*endptr == '\0') {
            // Là số thuần túy
            out_ids[(*out_count)++] = id;
        } else {
            // Không phải số -> Kiểm tra xem có phải từ khóa bổ trợ không
            // Chuyển về lowercase để so sánh
            for(int i=0; tok[i]; i++) if(tok[i]>='A' && tok[i]<='Z') tok[i]+=32;
            
            if (strcmp(tok, "task") != 0 && strcmp(tok, "số") != 0 && 
                strcmp(tok, "id") != 0 && strcmp(tok, "và") != 0 &&
                strcmp(tok, "chọn") != 0 && strcmp(tok, "cái") != 0) {
                // Có từ lạ -> Trả về false để AI parse
                return false;
            }
        }
        tok = strtok(NULL, " ,;#\t\n\r");
    }
    
    return (*out_count > 0);
}

/* ==========================================================================
 * PROMPT B1: Intent Classification (ngắn gọn, ~500 tokens)
 * ========================================================================== */
static const char *PROMPT_B1 =
    "Bạn là bộ phân loại ý định cho hệ thống quản lý công việc.\n"
    "%s\n"
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
    "• TASK_SUMMARY — Thống kê tổng quan (\"tổng kết\", \"báo cáo tiến độ\")\n"
    "• VIEW_HISTORY — Xem lịch sử công việc đã xong (\"xem lịch sử\", \"nhật ký xong\", \"những gì đã làm\")\n\n"
    "KHÁC:\n"
    "• CHITCHAT — Không liên quan đến công việc\n\n"
    "QUY TẮC:\n"
    "1. \"bao nhiêu\", \"mấy cái\", \"liệt kê\", \"danh sách\" → QUERY_TASKS\n"
    "2. \"khi nào\", \"bao giờ\", \"thời hạn\", \"mấy giờ\" của 1 task cụ thể → GET_TASK_DETAIL\n"
    "3. Nhập tên task KHÔNG kèm hành động → SEARCH_SEMANTIC\n"
    "4. \"tổng kết\", \"tình hình chung\" → TASK_SUMMARY\n"
    "5. \"hủy\"/\"bỏ\" hoặc \"xóa hẳn\" → DELETE_TASK\n"
    "6. Sự kiện kéo dài nhiều ngày → CREATE_TASK\n"
    "7. Hỏi DANH SÁCH chung về loại (\"các ngày kỉ niệm\", \"những cuộc họp\") → QUERY_TASKS\n"
    "8. Hỏi thông tin 1 task cụ thể (\"về task X\", \"ngày kỉ niệm cưới bao giờ\") → GET_TASK_DETAIL\n"
    "9. Khi không chắc → ƯU TIÊN SEARCH_SEMANTIC nếu có tên task, QUERY_TASKS nếu hỏi số lượng.\n"
    "10. QUAN TRỌNG: Nếu có động từ hành động ở phần đầu (hoàn thành, xong, đã làm, hủy, xóa, sửa, cập nhật, đổi, tạo, nhắc...) → BẮT BUỘC chọn nhóm MUTATION (CREATE/UPDATE/COMPLETE/DELETE).\n\n"
    "CHỈ trả JSON thuần, KHÔNG markdown:\n"
    "{\"intent\": \"...\", \"confidence\": 0.0-1.0}";

/* ==========================================================================
 * PROMPT B2: Chuyên biệt cho từng intent
 * ========================================================================== */

static const char *PROMPT_B2_CREATE =
    "Bạn là parser tạo task.\n"
    "%s\n"
    "Intent: CREATE_TASK\n"
    "User: \"%s\"\n\n"
    "Trả JSON:\n"
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
    "1. Sự kiện nhiều ngày: start_time=ngày đầu, due_time=ngày cuối. Chỉ 1 ngày: due_time=ngày đó, start_time=null.\n"
    "2. Loại: báo cáo=report, họp=meeting, nhắc=reminder, sự kiện/cưới/tiệc=event, kỉ niệm/sinh nhật=anniversary.\n"
    "3. Không nói giờ → mặc định 08:00.\n"
    "4. Thiếu năm → dùng năm hiện tại. TUYỆT ĐỐI KHÔNG dùng năm 1970.\n"
    "5. TRA BẢNG thời gian ở trên cho \"ngày mai\", \"tuần sau\"... KHÔNG tự tính.\n"
    "6. Chu kỳ lặp (hàng tuần/tháng/quý): Thời hạn (due_time) PHẢI là kỳ KẾ TIẾP trong TƯƠNG LAI. Ví dụ: Nếu hôm nay (T6) user bảo 'thứ 2 hàng tuần', thì due_time phải ở TUẦN SAU (T2=...).\n"
    "CHỈ JSON thuần.";

static const char *PROMPT_B2_QUERY =
    "Bạn là parser truy vấn.\n"
    "%s\n"
    "Intent: QUERY_TASKS\n"
    "User: \"%s\"\n\n"
    "Trả JSON:\n"
    "{\n"
    "  \"response_type\": \"list|count\",\n"
    "  \"label\": \"mô tả ngắn gọn\",\n"
    "  \"filters\": [\n"
    "    {\"field\": \"chỉ chọn 1: due_time/start_time/created_at/type/status/repeat\",\n"
    "     \"op\": \"chỉ chọn 1: equals/not_equals/before/after/between/is_not_null/is_null\",\n"
    "     \"value\": \"ISO8601|string\", \"value_end\": \"ISO8601 nếu between\"}\n"
    "  ],\n"
    "  \"sort\": \"due_time_asc|due_time_desc|created_at_desc|null\",\n"
    "  \"limit\": null\n"
    "}\n\n"
    "QUY TẮC THỜI GIAN (TRA BẢNG, KHÔNG TỰ TÍNH):\n"
    "1. \"hôm nay\" → between Hôm_nay+T00:00:00 → Hôm_nay+T23:59:59.\n"
    "2. \"ngày mai\" → between Ngày_mai+T00:00:00 → Ngày_mai+T23:59:59.\n"
    "3. \"tuần này\"/\"trong tuần\" → between T2+T00:00:00 → CN+T23:59:59.\n"
    "4. \"cuối tuần\"/\"đến cuối tuần\"/\"hết tuần\"/\"từ nay đến cuối tuần\" → between Hôm_nay+T00:00:00 → CN+T23:59:59.\n"
    "5. \"tháng này\" → between Đầu_tháng+T00:00:00 → Cuối_tháng+T23:59:59.\n"
    "6. NGÀY CỤ THỂ khác → between 00:00:00 → 23:59:59 của ngày đó.\n"
    "7. TRUY VẤN MỞ (\"sắp tới\", \"tiếp theo\", \"gần nhất\") → op=\"after\" value=Bây_giờ, sort=due_time_asc. KHÔNG bịa mốc kết thúc.\n"
    "8. Task LẶP LẠI → filter repeat, KHÔNG thêm due_time.\n\n"
    "QUY TẮC FILTER:\n"
    "9. LUÔN thêm status=pending (trừ khi user nói rõ). Hệ thống tự gộp pending+overdue. User CHỈ hỏi \"quá hạn\" → status=overdue.\n"
    "10. Giá trị status TUYỆT ĐỐI CHỈ LÀ: pending, done, cancelled, overdue. User hỏi ĐÃ HOÀN THÀNH → value=\"done\" (KHÔNG DÙNG \"complete\").\n"
    "11. User đề cập LOẠI (báo cáo, họp, nhắc...) → thêm filter type (report, meeting, reminder, anniversary, event, other).\n"
    "12. MỖI ĐIỀU KIỆN LÀ 1 OBJECT RIÊNG BIỆT TRONG MẢNG filters. KHÔNG dùng ký tự `|` để gộp nhiều field (TUYỆT ĐỐI KHÔNG làm `status|repeat`).\n"
    "CHỈ JSON thuần.";

static const char *PROMPT_B2_MUTATE =
    "Bạn là parser hành động.\n"
    "%s\n"
    "Intent: %s\n"
    "User: \"%s\"\n\n"
    "Trả JSON:\n"
    "{\n"
    "  \"task_ids\": [number],\n"
    "  \"search_query\": \"string nếu không có ID cụ thể\",\n"
    "  \"filters\": [\n"
    "    {\"field\": \"status\", \"op\": \"equals\", \"value\": \"pending|done|overdue\"},\n"
    "    {\"field\": \"type\", \"op\": \"equals\", \"value\": \"report|meeting|reminder|event|anniversary\"}\n"
    "  ],\n"
    "  \"updates\": {\"title\": \"string\", \"type\": \"string\", \"due_time\": \"ISO8601\"}\n"
    "}\n\n"
    "QUY TẮC CHỌN TASK (ƯU TIÊN 1 - CỰC KỲ QUAN TRỌNG):\n"
    "1. CHỈ CHỌN 1 TRONG 2 CHẾ ĐỘ:\n"
    "   - CHẾ ĐỘ ID: User nói số (\"số 5\") hoặc đại từ (\"nó\", \"đó\") → task_ids:[ID], search_query:null.\n"
    "   - CHẾ ĐỘ TÌM KIẾM: User nói TÊN/MÔ TẢ task (VD: \"kỉ niệm cưới\", \"báo cáo\") → task_ids:[] (BẮT BUỘC RỖNG), search_query:\"tên task\".\n"
    "2. TUYỆT ĐỐI NGHIÊM CẤM: Không được điền task_ids nếu user đang tìm theo tên.\n"
    "3. TỰ ĐỘNG PHÁT HIỆN TYPE và đưa vào filters. VD: \"xong báo cáo test\" → search_query=\"test\", filters:[{field:\"type\", op:\"=\", value:\"report\"}].\n"
    "4. LÀM SẠCH search_query: PHẢI XÓA bỏ các từ chỉ loại (báo cáo, họp, kỉ niệm...) khỏi search_query.\n"
    "   - VD: \"hủy họp sáng nay\" → search_query: \"sáng nay\", type: \"meeting\".\n"
    "   - VD: \"xong báo cáo test\" → search_query: \"test\", type: \"report\".\n\n"
    "Context IDs đang hiển thị (CHỈ DÙNG CHO CHẾ ĐỘ ID): [%s]\n"
    "CHỈ JSON thuần.";

static const char *PROMPT_B2_DETAIL =
    "Bạn là parser truy vấn chi tiết.\n"
    "%s\n"
    "Intent: GET_TASK_DETAIL\n"
    "User: \"%s\"\n\n"
    "Trả JSON:\n"
    "{\"task_ids\": [number], \"search_query\": \"string\", \"type_filter\": \"meeting|report|reminder|event|anniversary|other|null\"}\n\n"
    "QUY TẮC ƯU TIÊN 1 (CẤM SAI):\n"
    "1. CHẾ ĐỘ TÌM KIẾM (Nếu User nhắc đến tên task như \"kỉ niệm cưới\", \"họp\", \"báo cáo\") → task_ids PHẢI LÀ [].\n"
    "2. CHẾ ĐỘ ID (Chỉ dùng khi user nói \"nó\", \"đó\", \"số 3\" hoặc KHÔNG tên task) → task_ids:[ID], search_query:null.\n"
    "3. Xử lý \"kỉ niệm cưới bao giờ\" → task_ids:[], search_query:\"cưới\", type_filter:\"anniversary\".\n"
    "4. LÀM SẠCH: Sau khi nhận diện loại, PHẢI XÓA từ đó (báo cáo, họp, kỉ niệm...) trong search_query.\n"
    "   - VD: \"về báo cáo test 2\" → search_query: \"test 2\".\n"
    "5. TỰ ĐỘNG PHÁT HIỆN LOẠI và điền type_filter.\n\n"
    "Context IDs đang hiển thị (CHỈ DÙNG TRONG CHẾ ĐỘ ID): [%s]\n"
    "CHỈ JSON thuần.";

static const char *PROMPT_B2_SEARCH =
    "Bạn là parser tìm kiếm.\n"
    "Intent: SEARCH_SEMANTIC\n"
    "User: \"%s\"\n\n"
    "Trả JSON:\n"
    "{\"search_query\": \"cụm từ chính\", \"status_filter\": \"pending|done|cancelled|overdue|null\", \"type_filter\": \"meeting|report|reminder|event|anniversary|other|null\"}\n"
    "QUY TẮC:\n"
    "1. search_query CHỈ chứa nội dung tìm kiếm cốt lõi, LOẠI BỔ từ \"tìm\", \"search\", \"kiếm\" VÀ các từ chỉ loại task.\n"
    "2. Nếu user tìm task đã hoàn thành, status_filter=\"done\" (KHÔNG DÙNG complete).\n"
    "3. TỰ ĐỘNG PHÁT HIỆN TYPE từ ngữ cảnh:\n"
    "   - \"báo cáo\", \"report\", \"BC\" → type_filter=\"report\"\n"
    "   - \"họp\", \"meeting\", \"cuộc họp\" → type_filter=\"meeting\"\n"
    "   - \"nhắc\", \"nhắc nhở\", \"reminder\" → type_filter=\"reminder\"\n"
    "   - \"sự kiện\", \"event\", \"tiệc\", \"cưới\", \"lễ\" → type_filter=\"event\"\n"
    "   - \"kỉ niệm\", \"sinh nhật\", \"anniversary\" → type_filter=\"anniversary\"\n"
    "   - Không rõ loại → type_filter=null\n"
    "4. Khi phát hiện type, LOẠI BỎ từ chỉ loại khỏi search_query. VD: \"Báo cáo test 2\" → search_query=\"test 2\", type_filter=\"report\".\n"
    "5. Nếu query CHỈ CÓ từ chỉ loại (VD: \"báo cáo\") → search_query=null, type_filter=\"report\".\n"
    "CHỈ JSON thuần.";

static const char *PROMPT_B2_SUMMARY =
    "Bạn là parser thống kê.\n"
    "%s\n"
    "Intent: TASK_SUMMARY\n"
    "User: \"%s\"\n\n"
    "Trả JSON:\n"
    "{\"period_start\": \"ISO8601|null\", \"period_end\": \"ISO8601|null\"}\n"
    "Mặc định: đầu tháng → hôm nay. TRA BẢNG thời gian ở trên.\n"
    "CHỈ JSON thuần.";

static const char *PROMPT_B2_HISTORY =
    "Bạn là parser lịch sử công việc.\n"
    "Intent: VIEW_HISTORY\n"
    "User: \"%s\"\n\n"
    "Trả JSON:\n"
    "{\"limit\": 10, \"period\": \"all|recent|today\"}\n"
    "Mặc định: limit=10, period=\"recent\".\n"
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
        APPEND_SNPRINTF(buf, buf_size, offset,
                           "%" PRIu32 "%s", s_context_task_ids[i],
                           (i == s_context_task_count - 1) ? "" : ", ");
    }
}

/**
 * Build rich time context string for prompts.
 */
static void build_time_context(char *buf, size_t buf_size)
{
    time_t now = time_utils_get_now();
    char iso_now[32], date_now[32], weekday_now[32];
    time_utils_format_iso8601(now, iso_now, sizeof(iso_now));
    time_utils_format_date_short(now, date_now, sizeof(date_now));
    time_utils_get_weekday_name(now, weekday_now, sizeof(weekday_now));

    /* Tomorrow */
    time_range_t tomorrow = time_utils_get_tomorrow_range();
    char iso_tomorrow[32], weekday_tomorrow[32];
    time_utils_format_iso8601(tomorrow.start, iso_tomorrow, sizeof(iso_tomorrow));
    iso_tomorrow[10] = '\0';
    time_utils_get_weekday_name(tomorrow.start, weekday_tomorrow, sizeof(weekday_tomorrow));

    /* This week */
    time_range_t week = time_utils_get_this_week_range();
    char iso_mon[32], iso_sun[32];
    time_utils_format_iso8601(week.start, iso_mon, sizeof(iso_mon));
    iso_mon[10] = '\0';
    time_utils_format_iso8601(week.end, iso_sun, sizeof(iso_sun));
    iso_sun[10] = '\0';

    /* Next week */
    time_range_t next_week = time_utils_get_next_week_range();
    char iso_nxt_mon[32], iso_nxt_sun[32];
    time_utils_format_iso8601(next_week.start, iso_nxt_mon, sizeof(iso_nxt_mon));
    iso_nxt_mon[10] = '\0';
    time_utils_format_iso8601(next_week.end, iso_nxt_sun, sizeof(iso_nxt_sun));
    iso_nxt_sun[10] = '\0';

    /* This month */
    time_range_t month = time_utils_get_this_month_range();
    char iso_month_start[32], iso_month_end[32];
    time_utils_format_iso8601(month.start, iso_month_start, sizeof(iso_month_start));
    iso_month_start[10] = '\0';
    time_utils_format_iso8601(month.end, iso_month_end, sizeof(iso_month_end));
    iso_month_end[10] = '\0';

    /* Today date only */
    char iso_today[32];
    time_range_t today = time_utils_get_today_range();
    time_utils_format_iso8601(today.start, iso_today, sizeof(iso_today));
    iso_today[10] = '\0';

    /* Current Quarter */
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    int current_q = (tm_now.tm_mon / 3) + 1;

    snprintf(buf, buf_size,
        "Bây giờ: %s (%s, %s)\n"
        "Hôm nay: %s | Ngày mai: %s (%s)\n"
        "Tuần này: T2=%s → CN=%s\n"
        "Tuần sau: T2=%s → CN=%s\n"
        "Tháng này: %s → %s\n"
        "Quý hiện tại: Q%d",
        iso_now, weekday_now, date_now,
        iso_today, iso_tomorrow, weekday_tomorrow,
        iso_mon, iso_sun,
        iso_nxt_mon, iso_nxt_sun,
        iso_month_start, iso_month_end,
        current_q);
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
    if (strcmp(intent_str, "VIEW_HISTORY") == 0)      return ACTION_VIEW_HISTORY;
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
    char time_context[384], context_ids[128];
    build_time_context(time_context, sizeof(time_context));
    build_context_ids_str(context_ids, sizeof(context_ids));

    char *prompt = (char *)malloc(4096);
    if (!prompt) return ESP_ERR_NO_MEM;
    snprintf(prompt, 4096, PROMPT_B1, time_context, context_ids);

    char llm_response[512];
    esp_err_t err = openai_chat_completion(prompt, user_message,
                                            llm_response, sizeof(llm_response));
    free(prompt);
    if (err != ESP_OK) return err;

    cJSON *parsed = json_extract_from_text(llm_response);
    if (parsed == NULL) return ESP_FAIL;

    const char *intent_str = json_get_string(parsed, "intent", "UNKNOWN");
    *out_intent = parse_intent(intent_str);
    *out_confidence = (float)json_get_double(parsed, "confidence", 0.0);

    if (out_b1_json != NULL) *out_b1_json = cJSON_Print(parsed);
    cJSON_Delete(parsed);
    return ESP_OK;
}

/* ==========================================================================
 * B2: Query Parser
 * ========================================================================== */
static esp_err_t step_b2_parse(action_type_t intent,
                                const char *user_message,
                                char **out_data_json)
{
    char time_context[384], context_ids[128];
    build_time_context(time_context, sizeof(time_context));
    build_context_ids_str(context_ids, sizeof(context_ids));

    const size_t prompt_size = 5120;
    char *prompt = (char *)malloc(prompt_size);
    if (!prompt) return ESP_ERR_NO_MEM;

    switch (intent) {
        case ACTION_CREATE_TASK: snprintf(prompt, prompt_size, PROMPT_B2_CREATE, time_context, user_message); break;
        case ACTION_QUERY_TASKS: snprintf(prompt, prompt_size, PROMPT_B2_QUERY, time_context, user_message); break;
        case ACTION_UPDATE_TASK: snprintf(prompt, prompt_size, PROMPT_B2_MUTATE, time_context, "UPDATE_TASK", user_message, context_ids); break;
        case ACTION_COMPLETE_TASK: snprintf(prompt, prompt_size, PROMPT_B2_MUTATE, time_context, "COMPLETE_TASK", user_message, context_ids); break;
        case ACTION_DELETE_TASK: snprintf(prompt, prompt_size, PROMPT_B2_MUTATE, time_context, "DELETE_TASK", user_message, context_ids); break;
        case ACTION_GET_DETAIL: snprintf(prompt, prompt_size, PROMPT_B2_DETAIL, time_context, user_message, context_ids); break;
        case ACTION_SEARCH_SEMANTIC: snprintf(prompt, prompt_size, PROMPT_B2_SEARCH, user_message); break;
        case ACTION_TASK_SUMMARY: snprintf(prompt, prompt_size, PROMPT_B2_SUMMARY, time_context, user_message); break;
        case ACTION_VIEW_HISTORY: snprintf(prompt, prompt_size, PROMPT_B2_HISTORY, user_message); break;
        case ACTION_CHITCHAT: snprintf(prompt, prompt_size, PROMPT_B2_CHITCHAT, user_message); break;
        default: free(prompt); return ESP_FAIL;
    }

    char *llm_response = (char *)malloc(JSON_BUFFER_SIZE);
    if (!llm_response) { free(prompt); return ESP_ERR_NO_MEM; }
    
    esp_err_t err = openai_chat_completion(prompt, user_message, llm_response, JSON_BUFFER_SIZE);
    free(prompt);
    if (err != ESP_OK) { free(llm_response); return err; }

    cJSON *parsed = json_extract_from_text(llm_response);
    if (parsed == NULL) { free(llm_response); return ESP_FAIL; }
    free(llm_response);

    *out_data_json = cJSON_Print(parsed);
    cJSON_Delete(parsed);
    return ESP_OK;
}

/* ==========================================================================
 * action_dispatcher_handle
 * ========================================================================== */
esp_err_t action_dispatcher_handle(const char *user_message,
                                    char *response_buffer,
                                    size_t buffer_size)
{
    if (user_message == NULL || response_buffer == NULL) return ESP_ERR_INVALID_ARG;

    /* ======= Quick Commands ======= */
    if (strcmp(user_message, "/alltask") == 0) {
        return action_query_tasks("{\"response_type\":\"short\",\"label\":\"tất cả công việc\",\"filters\":[]}", response_buffer, buffer_size);
    }
    if (strcmp(user_message, "/confirm") == 0) return action_delete_confirm_hard(response_buffer, buffer_size);

    /* ======= ID Selection ======= */
    uint32_t selected_ids[20];
    int selected_count = 0;
    if (s_pending_intent != ACTION_UNKNOWN && s_pending_data_json != NULL &&
        is_id_list(user_message, selected_ids, &selected_count, 20)) {
        cJSON *root = cJSON_Parse(s_pending_data_json);
        if (root) {
            cJSON_DeleteItemFromObject(root, "task_ids");
            cJSON *arr = cJSON_CreateArray();
            for (int i = 0; i < selected_count; i++) cJSON_AddItemToArray(arr, cJSON_CreateNumber(selected_ids[i]));
            cJSON_AddItemToObject(root, "task_ids", arr);
            cJSON_DeleteItemFromObject(root, "search_query");
            char *merged = cJSON_PrintUnformatted(root);
            action_type_t intent = s_pending_intent;
            dispatcher_clear_pending_action();
            esp_err_t res = ESP_FAIL;
            if (intent == ACTION_COMPLETE_TASK) res = action_complete_task(merged, response_buffer, buffer_size);
            else if (intent == ACTION_UPDATE_TASK) res = action_update_task(merged, response_buffer, buffer_size);
            else if (intent == ACTION_DELETE_TASK) res = action_delete_task(merged, response_buffer, buffer_size);
            free(merged); cJSON_Delete(root); return res;
        }
    }
    dispatcher_clear_pending_action();

    /* ======= B1 & B2 ======= */
    action_type_t intent = ACTION_UNKNOWN;
    float confidence = 0.0f;
    char *b1_json = NULL;
    if (step_b1_classify(user_message, &intent, &confidence, &b1_json) != ESP_OK) return ESP_FAIL;

    if (confidence < 0.8f) {
        snprintf(response_buffer, buffer_size, "❓ Tôi không chắc chắn hiểu yêu cầu này (%.0f%%). Bạn nói rõ hơn nhé!", confidence * 100);
        if (b1_json) free(b1_json); 
        return ESP_OK;
    }

    char *data_json = NULL;
    if (step_b2_parse(intent, user_message, &data_json) != ESP_OK) { 
        if (b1_json) free(b1_json); 
        return ESP_FAIL; 
    }

    snprintf(s_last_action_json, sizeof(s_last_action_json), "【B1 Intent】\n%s\n\n【B2 Parse】\n%s", b1_json ? b1_json : "", data_json ? data_json : "");
    if (b1_json) free(b1_json);

    if (intent == ACTION_CHITCHAT) {
        cJSON *root = cJSON_Parse(data_json);
        snprintf(response_buffer, buffer_size, "SUGGEST|%s", json_get_string(root, "suggestion", "Hãy thử hỏi về lịch trình của bạn!"));
        if (root) cJSON_Delete(root); 
        if (data_json) free(data_json); 
        return ESP_OK;
    }

    esp_err_t res = ESP_OK;
    switch (intent) {
        case ACTION_CREATE_TASK: res = action_create_task(data_json, response_buffer, buffer_size); break;
        case ACTION_UPDATE_TASK: res = action_update_task(data_json, response_buffer, buffer_size); break;
        case ACTION_COMPLETE_TASK: res = action_complete_task(data_json, response_buffer, buffer_size); break;
        case ACTION_DELETE_TASK: res = action_delete_task(data_json, response_buffer, buffer_size); break;
        case ACTION_QUERY_TASKS: res = action_query_tasks(data_json, response_buffer, buffer_size); break;
        case ACTION_GET_DETAIL: res = action_get_detail(data_json, response_buffer, buffer_size); break;
        case ACTION_SEARCH_SEMANTIC: res = action_search_semantic(data_json, response_buffer, buffer_size); break;
        case ACTION_TASK_SUMMARY: res = action_task_summary(data_json, response_buffer, buffer_size); break;
        case ACTION_VIEW_HISTORY: res = action_view_history(data_json, response_buffer, buffer_size); break;
        default: res = ESP_FAIL; break;
    }
    if (data_json) free(data_json);
    return res;
}
