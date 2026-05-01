#include "internal_fs.h"
#include "services/vconsole.h"
#include "hal/hal.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include <sys/stat.h>

static const char *TAG = "internal_fs";
static const char *PARTITION_LABEL = "internal";
static bool s_mounted = false;

esp_err_t internal_fs_mount(void)
{
    if (s_mounted) {
        ESP_LOGW(TAG, "Internal FS already mounted");
        return ESP_OK;
    }

    esp_vfs_littlefs_conf_t conf = {
        .base_path = "/sys",
        .partition_label = PARTITION_LABEL,
        .format_if_mount_failed = true,
        .dont_mount = false,
    };

    esp_err_t ret = esp_vfs_littlefs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LittleFS mount failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_mounted = true;

    size_t total = 0, used = 0;
    ret = esp_littlefs_info(PARTITION_LABEL, &total, &used);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "/sys mounted (LittleFS): %zu KB total, %zu KB used",
                 total / 1024, used / 1024);
    } else {
        ESP_LOGI(TAG, "/sys mounted (LittleFS)");
    }

    struct stat st;
    if (stat("/sys/etc", &st) != 0) mkdir("/sys/etc", 0755);
    if (stat("/sys/log", &st) != 0) mkdir("/sys/log", 0755);
    if (stat("/sys/cache", &st) != 0) mkdir("/sys/cache", 0755);

    return ESP_OK;
}

esp_err_t internal_fs_unmount(void)
{
    if (!s_mounted) return ESP_ERR_INVALID_STATE;
    esp_vfs_littlefs_unregister(PARTITION_LABEL);
    s_mounted = false;
    ESP_LOGI(TAG, "/sys unmounted");
    return ESP_OK;
}

bool internal_fs_is_mounted(void)
{
    return s_mounted;
}

void internal_fs_print_info(void)
{
    if (!s_mounted) {
        vconsole_printf("/sys: not mounted\n");
        return;
    }
    size_t total = 0, used = 0;
    if (esp_littlefs_info(PARTITION_LABEL, &total, &used) == ESP_OK) {
        vconsole_printf("/sys (LittleFS): %zu KB total, %zu KB used, %zu KB free\n",
               total / 1024, used / 1024, (total - used) / 1024);
    }
}

static const hal_storage_ops_t s_ifs_ops = {
    .mount      = internal_fs_mount,
    .unmount    = internal_fs_unmount,
    .is_mounted = internal_fs_is_mounted,
    .print_info = internal_fs_print_info,
};

const hal_storage_ops_t *internal_fs_get_ops(void) { return &s_ifs_ops; }
