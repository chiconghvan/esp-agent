#include "network_gatekeeper.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"

static const char *TAG = "net_gate";
static SemaphoreHandle_t s_net_mutex = NULL;

void network_gatekeeper_init(void)
{
    if (s_net_mutex == NULL) {
        s_net_mutex = xSemaphoreCreateMutex();
        if (s_net_mutex == NULL) {
            ESP_LOGE(TAG, "Không thể tạo Network Mutex!");
        } else {
            ESP_LOGI(TAG, "Network Gatekeeper initialized.");
        }
    }
}

void network_lock(void)
{
    if (s_net_mutex == NULL) network_gatekeeper_init();
    
    // ESP_LOGD(TAG, "Locking network resource...");
    xSemaphoreTake(s_net_mutex, portMAX_DELAY);
}

bool network_lock_timeout(uint32_t timeout_ms)
{
    if (s_net_mutex == NULL) network_gatekeeper_init();
    return (xSemaphoreTake(s_net_mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE);
}

void network_unlock(void)
{
    if (s_net_mutex != NULL) {
        // ESP_LOGD(TAG, "Unlocking network resource.");
        xSemaphoreGive(s_net_mutex);
    }
}
