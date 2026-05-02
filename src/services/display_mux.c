#include "display_mux.h"
#include "vconsole.h"
#include "esp_console.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include <string.h>

static const char *TAG = "display_mux";

static display_slot_t s_drivers[DISPLAY_MUX_MAX_DRIVERS];
static TaskHandle_t   s_task_handle = NULL;

#define MUX_TASK_STACK   4096
#define MUX_TASK_PRIO    2
#define MUX_DEBOUNCE_MS  50
#define MUX_LINE_BUF     1024

static display_slot_t *find_slot(void)
{
    for (int i = 0; i < DISPLAY_MUX_MAX_DRIVERS; i++) {
        if (!s_drivers[i].active) return &s_drivers[i];
    }
    return NULL;
}

static display_slot_t *find_name(const char *name)
{
    for (int i = 0; i < DISPLAY_MUX_MAX_DRIVERS; i++) {
        if (s_drivers[i].active && strcmp(s_drivers[i].name, name) == 0)
            return &s_drivers[i];
    }
    return NULL;
}

static void render_all(void)
{
    char *buf = heap_caps_malloc(MUX_LINE_BUF, MALLOC_CAP_SPIRAM);
    if (!buf) return;

    for (int i = 0; i < DISPLAY_MUX_MAX_DRIVERS; i++) {
        if (!s_drivers[i].active || !s_drivers[i].ops->render) continue;

        int rows = s_drivers[i].ops->get_rows
                   ? s_drivers[i].ops->get_rows() : 8;

        size_t len = vconsole_get_lines(rows, buf, MUX_LINE_BUF);
        if (len > 0) {
            s_drivers[i].ops->render(buf, len);
        }
    }

    heap_caps_free(buf);
}

static void display_mux_task(void *arg)
{
    EventGroupHandle_t eg = vconsole_get_event_group();
    if (!eg) {
        ESP_LOGE(TAG, "vconsole event group not available");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Display mux task started");

    while (true) {
        xEventGroupWaitBits(eg, VCONSOLE_NEW_DATA_BIT,
                            pdTRUE, pdFALSE, portMAX_DELAY);

        vTaskDelay(pdMS_TO_TICKS(MUX_DEBOUNCE_MS));
        xEventGroupClearBits(eg, VCONSOLE_NEW_DATA_BIT);

        render_all();
    }
}

esp_err_t display_mux_init(void)
{
    memset(s_drivers, 0, sizeof(s_drivers));

    BaseType_t ret = xTaskCreate(display_mux_task, "disp_mux",
                                  MUX_TASK_STACK, NULL,
                                  MUX_TASK_PRIO, &s_task_handle);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create display mux task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Display multiplexer ready (max %d drivers)",
             DISPLAY_MUX_MAX_DRIVERS);
    return ESP_OK;
}

esp_err_t display_mux_register(const char *name, const display_driver_ops_t *ops)
{
    if (!name || !ops) return ESP_ERR_INVALID_ARG;

    if (find_name(name)) {
        ESP_LOGW(TAG, "Display '%s' already registered", name);
        return ESP_ERR_INVALID_STATE;
    }

    display_slot_t *slot = find_slot();
    if (!slot) {
        ESP_LOGE(TAG, "All %d display slots occupied", DISPLAY_MUX_MAX_DRIVERS);
        return ESP_ERR_NO_MEM;
    }

    strncpy(slot->name, name, DISPLAY_NAME_LEN - 1);
    slot->ops = ops;
    slot->active = true;

    ESP_LOGI(TAG, "Registered display '%s' (%dx%d)",
             name,
             ops->get_cols ? ops->get_cols() : 0,
             ops->get_rows ? ops->get_rows() : 0);
    return ESP_OK;
}

esp_err_t display_mux_unregister(const char *name)
{
    display_slot_t *slot = find_name(name);
    if (!slot) return ESP_ERR_NOT_FOUND;

    ESP_LOGI(TAG, "Unregistered display '%s'", name);
    memset(slot, 0, sizeof(*slot));
    return ESP_OK;
}

int display_mux_count(void)
{
    int n = 0;
    for (int i = 0; i < DISPLAY_MUX_MAX_DRIVERS; i++)
        if (s_drivers[i].active) n++;
    return n;
}

const display_slot_t *display_mux_get(int index)
{
    int n = 0;
    for (int i = 0; i < DISPLAY_MUX_MAX_DRIVERS; i++) {
        if (s_drivers[i].active) {
            if (n == index) return &s_drivers[i];
            n++;
        }
    }
    return NULL;
}

static int cmd_display(int argc, char **argv)
{
    int count = display_mux_count();
    if (count == 0) {
        vconsole_printf("No display drivers registered\n");
        vconsole_printf("Load a display module: modload /sdcard/drivers/oled.elf\n");
        return 0;
    }

    vconsole_printf("%-16s %-6s %-6s %s\n", "NAME", "COLS", "ROWS", "CAPS");
    for (int i = 0; i < count; i++) {
        const display_slot_t *d = display_mux_get(i);
        if (!d) continue;

        int cols = d->ops->get_cols ? d->ops->get_cols() : 0;
        int rows = d->ops->get_rows ? d->ops->get_rows() : 0;
        uint32_t caps = d->ops->get_caps ? d->ops->get_caps() : 0;

        vconsole_printf("%-16s %-6d %-6d", d->name, cols, rows);
        if (caps & DISPLAY_CAP_FAST_REFRESH) vconsole_printf(" fast");
        if (caps & DISPLAY_CAP_PARTIAL_REFRESH) vconsole_printf(" partial");
        if (caps & DISPLAY_CAP_COLOR) vconsole_printf(" color");
        vconsole_printf("\n");
    }
    vconsole_printf("(%d/%d slots)\n", count, DISPLAY_MUX_MAX_DRIVERS);
    return 0;
}

esp_err_t cmd_display_register(void)
{
    const esp_console_cmd_t cmd = {
        .command = "display",
        .help = "List registered display drivers",
        .func = cmd_display,
    };
    return esp_console_cmd_register(&cmd);
}
