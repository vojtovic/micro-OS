#include "symtab.h"
#include "esp_elf.h"
#include "esp_log.h"

#include "module_mgr.h"
#include "kernel_abi.h"
#include "services/vconsole.h"
#include "services/registry.h"
#include "services/input.h"
#include "services/app.h"
#include "services/hwconf.h"
#include "services/compositor.h"
#include "services/session.h"
#include "services/ui.h"
#include "services/duo.h"
#include "services/dma_service.h"
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
    ESP_ELFSYM_EXPORT(display_mux_present),
    ESP_ELFSYM_EXPORT(display_mux_render_text),
    ESP_ELFSYM_EXPORT(display_mux_dims),

    /* Compositor — focus-driven window surfaces for graphical apps */
    ESP_ELFSYM_EXPORT(comp_open_gfx),
    ESP_ELFSYM_EXPORT(comp_open_text),
    ESP_ELFSYM_EXPORT(comp_window_fb),
    ESP_ELFSYM_EXPORT(comp_commit),
    ESP_ELFSYM_EXPORT(comp_set_text),
    ESP_ELFSYM_EXPORT(comp_close),

    /* duo — dual-display cooperation toolkit (e-ink main + OLED active) */
    ESP_ELFSYM_EXPORT(duo_open),
    ESP_ELFSYM_EXPORT(duo_close),
    ESP_ELFSYM_EXPORT(duo_menu),
    ESP_ELFSYM_EXPORT(duo_value),
    ESP_ELFSYM_EXPORT(duo_message),
    ESP_ELFSYM_EXPORT(duo_eink_fb),
    ESP_ELFSYM_EXPORT(duo_oled_fb),
    ESP_ELFSYM_EXPORT(duo_eink_w),
    ESP_ELFSYM_EXPORT(duo_eink_h),
    ESP_ELFSYM_EXPORT(duo_eink_commit),
    ESP_ELFSYM_EXPORT(duo_oled_commit),

    /* UI widget toolkit */
    ESP_ELFSYM_EXPORT(ui_create),
    ESP_ELFSYM_EXPORT(ui_destroy),
    ESP_ELFSYM_EXPORT(ui_add_label),
    ESP_ELFSYM_EXPORT(ui_add_choice),
    ESP_ELFSYM_EXPORT(ui_add_button),
    ESP_ELFSYM_EXPORT(ui_choice_get),
    ESP_ELFSYM_EXPORT(ui_render),
    ESP_ELFSYM_EXPORT(ui_handle_key),
    ESP_ELFSYM_EXPORT(display_mux_grab),
    ESP_ELFSYM_EXPORT(display_mux_release),
    ESP_ELFSYM_EXPORT(gpio_config),
    ESP_ELFSYM_EXPORT(gpio_get_level),
    ESP_ELFSYM_EXPORT(gpio_set_level),
    ESP_ELFSYM_EXPORT(http_download_file),
    ESP_ELFSYM_EXPORT(http_get),
    ESP_ELFSYM_EXPORT(http_response_free),
    ESP_ELFSYM_EXPORT(heap_caps_free),
    ESP_ELFSYM_EXPORT(heap_caps_malloc),
    ESP_ELFSYM_EXPORT(memcpy),
    ESP_ELFSYM_EXPORT(memmove),
    ESP_ELFSYM_EXPORT(memset),
    ESP_ELFSYM_EXPORT(module_register),
    ESP_ELFSYM_EXPORT(printf),
    ESP_ELFSYM_EXPORT(registry_add),
    ESP_ELFSYM_EXPORT(registry_provide),
    ESP_ELFSYM_EXPORT(registry_vtable),
    ESP_ELFSYM_EXPORT(registry_find),
    ESP_ELFSYM_EXPORT(registry_set_state),
    ESP_ELFSYM_EXPORT(snprintf),
    ESP_ELFSYM_EXPORT(strlen),
    ESP_ELFSYM_EXPORT(strcpy),
    ESP_ELFSYM_EXPORT(strncpy),
    ESP_ELFSYM_EXPORT(strcmp),
    ESP_ELFSYM_EXPORT(strncmp),
    ESP_ELFSYM_EXPORT(strchr),
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

    /* Phase 5.3a: kernel graphics ABI (gfx_*, cache_*, dma_buf_*) */
    ESP_ELFSYM_EXPORT(gfx_fb_alloc),
    ESP_ELFSYM_EXPORT(gfx_fb_free),
    ESP_ELFSYM_EXPORT(gfx_set_pixel),
    ESP_ELFSYM_EXPORT(gfx_get_pixel),
    ESP_ELFSYM_EXPORT(gfx_fill_rect),
    ESP_ELFSYM_EXPORT(gfx_draw_rect),
    ESP_ELFSYM_EXPORT(gfx_draw_line),
    ESP_ELFSYM_EXPORT(gfx_draw_circle),
    ESP_ELFSYM_EXPORT(gfx_copy_rect),
    ESP_ELFSYM_EXPORT(gfx_blit_masked),
    ESP_ELFSYM_EXPORT(gfx_font_get),
    ESP_ELFSYM_EXPORT(gfx_draw_char),
    ESP_ELFSYM_EXPORT(gfx_draw_text),
    ESP_ELFSYM_EXPORT(gfx_draw_char_scaled),
    ESP_ELFSYM_EXPORT(gfx_draw_text_scaled),

    /* Input service — for input-source drivers (CardKB) */
    ESP_ELFSYM_EXPORT(input_push_key),
    ESP_ELFSYM_EXPORT(input_get_key),

    /* App runtime — for a launcher app that browses/runs other apps */
    ESP_ELFSYM_EXPORT(app_run),
    ESP_ELFSYM_EXPORT(app_list),
    ESP_ELFSYM_EXPORT(session_spawn),   /* launch an app as a background task */
    ESP_ELFSYM_EXPORT(app_read_file),
    ESP_ELFSYM_EXPORT(app_write_file),

    /* I2C bus access — for input-source drivers (CardKB at 0x5F) */
    ESP_ELFSYM_EXPORT(bus_i2c_read),
    ESP_ELFSYM_EXPORT(bus_i2c_write),

    /* Hardware config — modules read pins/resolution/orientation from conf */
    ESP_ELFSYM_EXPORT(hwconf_get_int),
    ESP_ELFSYM_EXPORT(hwconf_get_str),

    /* Display SPI (hardware SPI2 + DMA) — for display driver modules */
    ESP_ELFSYM_EXPORT(disp_spi_add),
    ESP_ELFSYM_EXPORT(disp_spi_write),

    ESP_ELFSYM_EXPORT(cache_flush_range),
    ESP_ELFSYM_EXPORT(cache_invalidate_range),
    ESP_ELFSYM_EXPORT(dma_buf_alloc),
    ESP_ELFSYM_EXPORT(dma_buf_sync_for_device),
    ESP_ELFSYM_EXPORT(dma_buf_sync_for_cpu),
    ESP_ELFSYM_EXPORT(dma_buf_free),

    ESP_ELFSYM_END,
};

int symtab_count(void)
{
    return (sizeof(s_symtab) / sizeof(s_symtab[0])) - 1;
}

bool symtab_has_prefix(const char *prefix)
{
    if (!prefix || !*prefix) return false;
    size_t plen = strlen(prefix);
    for (int i = 0; s_symtab[i].name != NULL; i++) {
        if (strncmp(s_symtab[i].name, prefix, plen) == 0) return true;
    }
    return false;
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
