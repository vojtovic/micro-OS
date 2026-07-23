#include "bus/bus_manager.h"
#include "hal/storage.h"
#include "hal/internal_fs.h"
#include "shell/shell.h"
#include "services/init.h"
#include "services/hwconf.h"
#include "services/session.h"
#include "services/compositor.h"
#include "services/klog.h"
#include "services/vconsole.h"
#include "services/registry.h"
#include "loader/symtab.h"
#include "loader/module_mgr.h"
#include "services/display_mux.h"
#include "services/input.h"
#include "services/wifi.h"
#include "services/pkg_manager.h"
#include "services/mem_pool.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "main";

static void print_psram_info(void)
{
    size_t total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    size_t free  = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    if (total > 0) {
        ESP_LOGI(TAG, "PSRAM: %.1f MB total, %.1f MB free",
                 total / (1024.0 * 1024.0), free / (1024.0 * 1024.0));
    } else {
        ESP_LOGW(TAG, "PSRAM not detected");
    }
}

void app_main(void)
{
    // Wait for serial monitor to connect
    vTaskDelay(pdMS_TO_TICKS(2000));

    // Kernel log ring buffer — captures all ESP_LOG output from this point
    klog_init();

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  micro_arch v0.2.0");
    ESP_LOGI(TAG, "========================================");

    // Boot beep
    init_boot_beep();

    // Hardware diagnostics
    print_psram_info();

    // Module memory pool — soft IRAM budget for loadable modules.
    // TODO: read module.iram_pool_kb from /sys/etc/loader.conf when present.
    mem_pool_init(MEM_POOL_DEFAULT_IRAM_KB);

    // Virtual console — PSRAM ring buffer for display module output
    esp_err_t vcon_err = vconsole_init(32768);
    if (vcon_err != ESP_OK) {
        ESP_LOGW(TAG, "vconsole init failed — output not buffered");
    }

    // Internal filesystem (LittleFS on flash) — mounts before SD so config survives
    esp_err_t sys_err = internal_fs_mount();
    if (sys_err != ESP_OK) {
        ESP_LOGW(TAG, "/sys mount failed — running without internal storage");
    }

    // Create default fstab + config on the internal flash (needs /sys mounted).
    // Config lives here (not on the SD) so it persists without the card.
    if (sys_err == ESP_OK) {
        init_create_default_fstab();
        init_config();
    }

    // Bus init (SD SPI + I2C — display buses are loaded by modules)
    ESP_ERROR_CHECK(bus_init());

    // Mount filesystems from fstab (SD card etc.)
    esp_err_t sd_err = ESP_FAIL;
    if (sys_err == ESP_OK) {
        init_mount_fstab();
        sd_err = storage_sd_is_mounted() ? ESP_OK : ESP_FAIL;
    } else {
        sd_err = storage_sd_mount();
    }

    if (sd_err == ESP_OK) {
        init_filesystem();
    } else {
        ESP_LOGW(TAG, "SD not available — running without storage");
    }

    // Parse hardware.conf so driver modules can read pins/resolution/orientation
    hwconf_load();

    // Service registry — HAL vtables allow modules to call through the registry
    ESP_ERROR_CHECK(registry_init());
    registry_add("klog",        1, (void *)klog_get_ops(),        NULL);
    registry_add("bus",         1, (void *)bus_get_ops(),         NULL);
    registry_add("internal_fs", 1, (void *)internal_fs_get_ops(), NULL);
    if (vcon_err == ESP_OK)
        registry_add("vconsole", 1, (void *)vconsole_get_ops(), NULL);
    if (sd_err == ESP_OK)
        registry_add("storage", 1, (void *)storage_sd_get_ops(), NULL);

    // Input service — key-event queue for input sources (CardKB) and consumers
    esp_err_t input_err = input_init();
    if (input_err != ESP_OK)
        ESP_LOGW(TAG, "input service init failed — keyboard input unavailable");

    // Session manager — cooperative multitasking (spawn/switch apps)
    if (session_init() != ESP_OK)
        ESP_LOGW(TAG, "session manager init failed — multitasking unavailable");

    // Kernel symbol table — enables ELF modules to resolve kernel functions
    ESP_ERROR_CHECK(symtab_init());

    // Module manager — lifecycle for loadable ELF modules
    ESP_ERROR_CHECK(module_mgr_init());

    // Display multiplexer — watches vconsole and fans out to registered display drivers
    if (vcon_err == ESP_OK) {
        esp_err_t disp_err = display_mux_init();
        if (disp_err != ESP_OK) {
            ESP_LOGW(TAG, "Display mux init failed");
        }
    }

    // Compositor — focus-driven window surfaces for graphical apps
    if (compositor_init() != ESP_OK)
        ESP_LOGW(TAG, "compositor init failed — graphical apps limited");

    // Wi-Fi service — STA mode, connects on demand via shell
    esp_err_t wifi_err = wifi_service_init();
    if (wifi_err != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi init failed — networking unavailable");
    } else {
        registry_add("wifi", 1, (void *)wifi_get_ops(), NULL);
    }

    // Package manager — pacman-style install from remote repo
    ESP_ERROR_CHECK(pkg_manager_init());

    // Shell init (register all commands)
    ESP_ERROR_CHECK(shell_init());

    // Run boot script if SD is available
    if (sd_err == ESP_OK) {
        init_run_bootscript();
    }

    ESP_LOGI(TAG, "Boot complete. Type 'help' for commands.");
    shell_start();
}
