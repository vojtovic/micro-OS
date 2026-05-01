#include "storage.h"
#include "hal/hal.h"
#include "config/pin_config.h"
#include "bus/bus_manager.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "storage";
static const char *MOUNT_POINT = "/sdcard";
static sdmmc_card_t *s_card = NULL;
static bool s_mounted = false;

esp_err_t storage_sd_mount(void)
{
    if (s_mounted) {
        ESP_LOGW(TAG, "SD already mounted");
        return ESP_OK;
    }

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files              = 5,
        .allocation_unit_size   = 16 * 1024,
    };

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SD_SPI_HOST;

    sdspi_device_config_t slot_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_cfg.gpio_cs   = PIN_SD_CS;
    slot_cfg.host_id   = SD_SPI_HOST;

    esp_err_t ret = esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &slot_cfg,
                                             &mount_cfg, &s_card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD mount failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_mounted = true;
    ESP_LOGI(TAG, "SD mounted at %s", MOUNT_POINT);
    sdmmc_card_print_info(stdout, s_card);
    return ESP_OK;
}

esp_err_t storage_sd_unmount(void)
{
    if (!s_mounted) {
        ESP_LOGW(TAG, "SD not mounted");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = esp_vfs_fat_sdcard_unmount(MOUNT_POINT, s_card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD unmount failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_card = NULL;
    s_mounted = false;
    ESP_LOGI(TAG, "SD unmounted");
    return ESP_OK;
}

bool storage_sd_is_mounted(void)
{
    return s_mounted;
}

void storage_sd_print_info(void)
{
    if (!s_mounted || !s_card) {
        ESP_LOGW(TAG, "SD not mounted");
        return;
    }
    sdmmc_card_print_info(stdout, s_card);
}

static const hal_storage_ops_t s_sd_ops = {
    .mount      = storage_sd_mount,
    .unmount    = storage_sd_unmount,
    .is_mounted = storage_sd_is_mounted,
    .print_info = storage_sd_print_info,
};

const hal_storage_ops_t *storage_sd_get_ops(void) { return &s_sd_ops; }
