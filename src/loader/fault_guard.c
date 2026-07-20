#include "fault_guard.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "fault_guard";
static bool s_in_module_call = false;
static bool s_last_faulted = false;

#define MIN_STACK_WORDS 512
#define LEAK_WARN_BYTES 4096

bool fault_guard_in_module(void)
{
    return s_in_module_call;
}

bool fault_guard_last_faulted(void)
{
    return s_last_faulted;
}

esp_err_t fault_guard_call(int (*fn)(void), int *result, bool check_leak)
{
    if (!fn) {
        ESP_LOGE(TAG, "NULL function pointer");
        s_last_faulted = true;
        return ESP_ERR_INVALID_ARG;
    }

    UBaseType_t stack_left = uxTaskGetStackHighWaterMark(NULL);
    if (stack_left < MIN_STACK_WORDS) {
        ESP_LOGE(TAG, "Stack too low for module call: %lu words free",
                 (unsigned long)stack_left);
        s_last_faulted = true;
        return ESP_FAIL;
    }

    size_t heap_before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    s_in_module_call = true;
    s_last_faulted = false;

    int ret = fn();

    s_in_module_call = false;

    if (result) *result = ret;

    size_t heap_after = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    if (check_leak && heap_before > heap_after &&
        (heap_before - heap_after) > LEAK_WARN_BYTES) {
        ESP_LOGW(TAG, "Module call leaked ~%zu bytes of PSRAM",
                 heap_before - heap_after);
    }

    if (ret != 0) {
        ESP_LOGW(TAG, "Module function returned %d", ret);
    }

    return ESP_OK;
}
