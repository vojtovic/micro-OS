#include "symtab.h"
#include "esp_elf.h"
#include "esp_log.h"

#include "module_mgr.h"
#include "services/vconsole.h"
#include "services/registry.h"
#include "bus/bus_manager.h"
#include "services/display_mux.h"
#include "services/wifi.h"
#include "services/http_client.h"

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "driver/gpio.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "symtab";

static SemaphoreHandle_t wrap_xSemaphoreCreateMutex(void)
{
    return xSemaphoreCreateMutex();
}

static BaseType_t wrap_xSemaphoreTake(SemaphoreHandle_t sem, TickType_t ticks)
{
    return xSemaphoreTake(sem, ticks);
}

static BaseType_t wrap_xSemaphoreGive(SemaphoreHandle_t sem)
{
    return xSemaphoreGive(sem);
}

static const struct esp_elfsym s_symtab[] = {
    ESP_ELFSYM_EXPORT(display_mux_register),
    ESP_ELFSYM_EXPORT(display_mux_unregister),
    ESP_ELFSYM_EXPORT(gpio_config),
    ESP_ELFSYM_EXPORT(gpio_get_level),
    ESP_ELFSYM_EXPORT(gpio_set_level),
    ESP_ELFSYM_EXPORT(http_download_file),
    ESP_ELFSYM_EXPORT(http_get),
    ESP_ELFSYM_EXPORT(http_response_free),
    ESP_ELFSYM_EXPORT(heap_caps_free),
    ESP_ELFSYM_EXPORT(heap_caps_malloc),
    ESP_ELFSYM_EXPORT(memcpy),
    ESP_ELFSYM_EXPORT(memset),
    ESP_ELFSYM_EXPORT(module_register),
    ESP_ELFSYM_EXPORT(printf),
    ESP_ELFSYM_EXPORT(registry_add),
    ESP_ELFSYM_EXPORT(registry_find),
    ESP_ELFSYM_EXPORT(registry_set_state),
    ESP_ELFSYM_EXPORT(snprintf),
    ESP_ELFSYM_EXPORT(strlen),
    ESP_ELFSYM_EXPORT(vTaskDelay),
    ESP_ELFSYM_EXPORT(vTaskDelete),
    ESP_ELFSYM_EXPORT(vconsole_printf),
    ESP_ELFSYM_EXPORT(vconsole_putchar),
    ESP_ELFSYM_EXPORT(vconsole_write),
    ESP_ELFSYM_EXPORT(wrap_xSemaphoreCreateMutex),
    ESP_ELFSYM_EXPORT(wrap_xSemaphoreGive),
    ESP_ELFSYM_EXPORT(wrap_xSemaphoreTake),
    ESP_ELFSYM_EXPORT(xEventGroupCreate),
    ESP_ELFSYM_EXPORT(xEventGroupSetBits),
    ESP_ELFSYM_EXPORT(xEventGroupWaitBits),
    ESP_ELFSYM_EXPORT(xTaskCreate),
    ESP_ELFSYM_END,
};

int symtab_count(void)
{
    return (sizeof(s_symtab) / sizeof(s_symtab[0])) - 1;
}

esp_err_t symtab_init(void)
{
    int ret = esp_elf_register_symbol(s_symtab);
    if (ret != 0) {
        ESP_LOGE(TAG, "Failed to register symbol table: %d", ret);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Registered %d kernel symbols", symtab_count());
    return ESP_OK;
}
