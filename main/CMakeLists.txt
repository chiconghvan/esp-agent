# ===========================================================================
# Main Component CMakeLists.txt cho ESP-Agent
# Liệt kê tất cả source files từ các subdirectory
# ===========================================================================

set(COMPONENT_SRCS
    "main.c"
    "wifi/wifi_manager.c"
    "telegram/telegram_bot.c"
    "openai/openai_client.c"
    "database/task_database.c"
    "database/vector_search.c"
    "actions/action_dispatcher.c"
    "actions/action_create.c"
    "actions/action_query.c"
    "actions/action_detail.c"
    "actions/action_search.c"
    "actions/action_summary.c"
    "actions/action_undo.c"
    "cloud/firebase_sync.c"
    "actions/action_complete.c"
    "actions/action_delete.c"
    "actions/action_update.c"
    "scheduler/reminder_scheduler.c"
    "utils/time_utils.c"
    "utils/json_parser.c"
    "utils/response_formatter.c"
    "utils/token_tracker.c"
    "display/ssd1306.c"
    "display/vn_utils.c"
    "display/display_manager.c"
)

set(COMPONENT_INCLUDE_DIRS
    "."
    "wifi"
    "telegram"
    "openai"
    "database"
    "actions"
    "scheduler"
    "utils"
    "display"
    "cloud"
)

idf_component_register(
    SRCS ${COMPONENT_SRCS}
    INCLUDE_DIRS ${COMPONENT_INCLUDE_DIRS}
    REQUIRES
        nvs_flash
        esp_wifi
        esp_event
        esp_http_client
        esp-tls
        spiffs
        fatfs
        json
        esp_timer
        esp_netif
        lwip
        mbedtls
        driver
        esp_http_server
        app_update
)
