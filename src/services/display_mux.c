#include "display_mux.h"
#include "vconsole.h"
#include "esp_console.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "display_mux";

static display_slot_t   s_drivers[DISPLAY_MUX_MAX_DRIVERS];
static TaskHandle_t      s_task_handle = NULL;
// Serializes the render task against (un)register. Without it, render_all()
// can call a driver's render() after modstop has unregistered it and freed
// the module's code — a use-after-free that panics in the module (observed:
// LoadProhibited in eink render during modstop). unregister() holds this
// across the slot clear, so it blocks until any in-flight render finishes;
// the module's memory is only freed after unregister() returns.
static SemaphoreHandle_t s_lock = NULL;

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

// Render the current console tail onto one driver. Caller must hold s_lock.
static void render_slot(display_slot_t *d, char *buf)
{
    if (!d->active || !d->ops->render) return;

    int rows = d->ops->get_rows ? d->ops->get_rows() : 8;
    size_t len = vconsole_get_lines(rows, buf, MUX_LINE_BUF);
    if (len > 0) {
        d->ops->render(buf, len);
    }
}

// Automatic mirror pass, driven by new console data. Only FAST_REFRESH
// displays (e.g. OLED) are mirrored live; slow displays (e-ink, whose full
// refresh takes ~2 s and wears the panel) are skipped here and refreshed on
// demand via display_mux_refresh() / `display refresh`.
static void render_all(void)
{
    char *buf = heap_caps_malloc(MUX_LINE_BUF, MALLOC_CAP_SPIRAM);
    if (!buf) return;

    // Hold the lock across the whole render pass: a driver must not be
    // unregistered/freed while we are inside its render() (see s_lock).
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < DISPLAY_MUX_MAX_DRIVERS; i++) {
        display_slot_t *d = &s_drivers[i];
        if (!d->active) continue;

        uint32_t caps = d->ops->get_caps ? d->ops->get_caps() : 0;
        if (!(caps & DISPLAY_CAP_FAST_REFRESH)) continue;   // slow: on-demand only

        render_slot(d, buf);
    }
    xSemaphoreGive(s_lock);

    heap_caps_free(buf);
}

// On-demand refresh: push the current console to one named display, or to all
// active displays when name is NULL. Used for slow displays that are not part
// of the automatic mirror pass.
esp_err_t display_mux_refresh(const char *name)
{
    char *buf = heap_caps_malloc(MUX_LINE_BUF, MALLOC_CAP_SPIRAM);
    if (!buf) return ESP_ERR_NO_MEM;

    esp_err_t ret = name ? ESP_ERR_NOT_FOUND : ESP_OK;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < DISPLAY_MUX_MAX_DRIVERS; i++) {
        display_slot_t *d = &s_drivers[i];
        if (!d->active) continue;
        if (name && strcmp(d->name, name) != 0) continue;

        render_slot(d, buf);
        ret = ESP_OK;
    }
    xSemaphoreGive(s_lock);

    heap_caps_free(buf);
    return ret;
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

    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        ESP_LOGE(TAG, "Failed to create display mux lock");
        return ESP_ERR_NO_MEM;
    }

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

    // Module .rodata pointers are data-bus addressable — the historic
    // 0x42xxxxxx LoadStoreError was a loader mapping bug, fixed by
    // PATCH-001 in components/elf_loader (see PATCHES.md).
    xSemaphoreTake(s_lock, portMAX_DELAY);

    if (find_name(name)) {
        xSemaphoreGive(s_lock);
        ESP_LOGW(TAG, "Display '%s' already registered", name);
        return ESP_ERR_INVALID_STATE;
    }

    display_slot_t *slot = find_slot();
    if (!slot) {
        xSemaphoreGive(s_lock);
        ESP_LOGE(TAG, "All %d display slots occupied", DISPLAY_MUX_MAX_DRIVERS);
        return ESP_ERR_NO_MEM;
    }

    strncpy(slot->name, name, DISPLAY_NAME_LEN - 1);
    slot->name[DISPLAY_NAME_LEN - 1] = '\0';
    slot->ops = ops;
    slot->active = true;

    ESP_LOGI(TAG, "Registered display '%s' (%dx%d)",
             slot->name,
             ops->get_cols ? ops->get_cols() : 0,
             ops->get_rows ? ops->get_rows() : 0);
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t display_mux_unregister(const char *name)
{
    if (!name) return ESP_ERR_INVALID_ARG;

    // Taking the lock here blocks until any in-flight render_all() pass
    // finishes, so the caller (module stop) can safely free the module
    // once this returns — no render can still be inside its render().
    xSemaphoreTake(s_lock, portMAX_DELAY);
    display_slot_t *slot = find_name(name);
    if (!slot) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "Unregistered display '%s'", slot->name);
    memset(slot, 0, sizeof(*slot));
    xSemaphoreGive(s_lock);
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
    // `display refresh [name]` — push the current console to a slow (on-demand)
    // display such as e-ink, or to all displays when no name is given.
    if (argc >= 2 && strcmp(argv[1], "refresh") == 0) {
        const char *name = (argc >= 3) ? argv[2] : NULL;
        esp_err_t err = display_mux_refresh(name);
        if (err == ESP_ERR_NOT_FOUND)
            vconsole_printf("No display named '%s'\n", argv[2]);
        else if (err != ESP_OK)
            vconsole_printf("Refresh failed: %s\n", esp_err_to_name(err));
        else
            vconsole_printf("Refreshed %s\n", name ? name : "all displays");
        return 0;
    }

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
        .help = "Displays: 'display' lists, 'display refresh [name]' redraws slow (e-ink) displays",
        .func = cmd_display,
    };
    return esp_console_cmd_register(&cmd);
}
