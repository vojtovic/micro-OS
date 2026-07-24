#include "init.h"
#include "config/pin_config.h"
#include "hal/storage.h"
#include "hal/internal_fs.h"
#include "services/vconsole.h"
#include "services/app.h"
#include "esp_console.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "init";

static void ensure_dir(const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0) return;
    if (mkdir(path, 0755) == 0) {
        ESP_LOGI(TAG, "Created: %s", path);
    } else {
        ESP_LOGW(TAG, "Failed to create: %s", path);
    }
}

static void ensure_file(const char *path, const char *content)
{
    struct stat st;
    if (stat(path, &st) == 0) return;
    FILE *f = fopen(path, "w");
    if (!f) {
        ESP_LOGW(TAG, "Cannot create: %s", path);
        return;
    }
    fputs(content, f);
    fclose(f);
    ESP_LOGI(TAG, "Created: %s", path);
}

// Config lives on the internal flash (/sys/etc, LittleFS) so it persists on the
// device and survives without the SD card — no re-uploading config to the card.
// User data (/sdcard/home) and packages (drivers/apps) stay on the SD.
esp_err_t init_config(void)
{
    ensure_dir("/sys/etc");

    ensure_file("/sys/etc/hardware.conf",
        "# Hardware pin configuration\n"
        "# Loaded by driver modules at runtime\n"
        "\n"
        "[display_spi]\n"
        "clk=12\n"
        "mosi=11\n"
        "\n"
        "[oled]\n"
        "cs=10\n"
        "dc=21\n"
        "rst=47\n"
        "width=128\n"
        "height=64\n"
        "rotation=0        # 0 or 180 (hardware flip)\n"
        "\n"
        "[eink]\n"
        "cs=16\n"
        "dc=15\n"
        "rst=17\n"
        "busy=18\n"
        "rotation=90       # 0/180 portrait 240x360, 90/270 landscape 360x240\n"
        "\n"
        "[buzzer]\n"
        "pin=4\n"
    );

    ensure_file("/sys/etc/hostname", "micro_arch\n");

    ensure_file("/sys/etc/init.conf",
        "# init.conf — boot script, executed line-by-line on startup.\n"
        "#\n"
        "# Each non-comment line is run like a shell prompt: first a built-in\n"
        "# command is tried, then an app/driver from /sdcard/bin/<name>.elf.\n"
        "# This is the Arch-style \"what starts at boot\" file.\n"
        "#\n"
        "# Examples — uncomment to enable:\n"
        "#\n"
        "# echo Welcome to micro_arch      # print a banner\n"
        "# oled                            # load the OLED display driver\n"
        "# cardkb                          # load the CardKB keyboard driver\n"
        "# gfxdemo                         # launch the graphics demo\n"
    );

    ESP_LOGI(TAG, "Config ready in /sys/etc");
    return ESP_OK;
}

esp_err_t init_filesystem(void)
{
    struct stat st;
    bool first_boot = (stat("/sdcard/bin", &st) != 0);

    ensure_dir("/sdcard/bin");
    ensure_dir("/sdcard/drivers");
    ensure_dir("/sdcard/home");
    ensure_dir("/sdcard/tmp");
    ensure_dir("/sdcard/var");
    ensure_dir("/sdcard/var/log");

    if (first_boot) {
        ESP_LOGI(TAG, "First boot: SD hierarchy created");
    } else {
        ESP_LOGI(TAG, "SD hierarchy OK");
    }
    return ESP_OK;
}

esp_err_t init_run_bootscript(void)
{
    FILE *f = fopen("/sys/etc/init.conf", "r");
    if (!f) {
        ESP_LOGI(TAG, "No init.conf found — skipping");
        return ESP_OK;
    }

    char line[256];
    int count = 0;
    while (fgets(line, sizeof(line), f)) {
        // strip newline
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';

        if (len == 0 || line[0] == '#') continue;

        ESP_LOGI(TAG, "init.conf: %s", line);
        int ret;
        esp_err_t err = esp_console_run(line, &ret);
        if (err == ESP_ERR_NOT_FOUND) {
            // Not a built-in — try to run it as an app/driver from
            // /sdcard/bin/<name>.elf, exactly like the shell does. This lets
            // init.conf autostart modules (e.g. "oled", "cardkb", "gfxdemo")
            // — Arch-style "what starts at boot".
            char split[256];
            strncpy(split, line, sizeof(split) - 1);
            split[sizeof(split) - 1] = '\0';
            char *argv[16];
            int argc = esp_console_split_argv(split, argv, 16);
            if (argc <= 0 || app_run(argv[0], argc, argv) == APP_NOT_FOUND)
                ESP_LOGW(TAG, "Unknown command: %s", line);
        } else if (err != ESP_OK && err != ESP_ERR_INVALID_ARG) {
            ESP_LOGW(TAG, "Command failed: %s (%s)", line, esp_err_to_name(err));
        }
        count++;
    }
    fclose(f);

    if (count > 0) {
        ESP_LOGI(TAG, "Executed %d commands from init.conf", count);
    }
    return ESP_OK;
}

esp_err_t init_create_default_fstab(void)
{
    struct stat st;
    if (stat(FSTAB_PATH, &st) == 0) return ESP_OK;

    FILE *f = fopen(FSTAB_PATH, "w");
    if (!f) {
        ESP_LOGW(TAG, "Cannot create %s", FSTAB_PATH);
        return ESP_FAIL;
    }

    fputs(
        "# /sys/etc/fstab — filesystem mount table\n"
        "# <device>   <mountpoint>  <fstype>  <options>\n"
        "#\n"
        "# Devices:  sd0 = SD card SPI, internal = LittleFS flash\n"
        "# Options:  defaults, ro, noauto\n"
        "#\n"
        "sd0       /sdcard    fat     defaults\n"
        "internal  /sys       littlefs defaults\n",
        f
    );
    fclose(f);
    ESP_LOGI(TAG, "Created default %s", FSTAB_PATH);
    return ESP_OK;
}

esp_err_t init_parse_fstab(fstab_t *fstab)
{
    memset(fstab, 0, sizeof(*fstab));

    FILE *f = fopen(FSTAB_PATH, "r");
    if (!f) {
        ESP_LOGW(TAG, "No fstab at %s", FSTAB_PATH);
        return ESP_ERR_NOT_FOUND;
    }

    char line[128];
    while (fgets(line, sizeof(line), f) && fstab->count < FSTAB_MAX_ENTRIES) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';

        if (len == 0 || line[0] == '#') continue;

        fstab_entry_t *e = &fstab->entries[fstab->count];
        int n = sscanf(line, "%31s %31s %15s %31s",
                       e->device, e->mountpoint, e->fstype, e->options);
        if (n >= 3) {
            if (n < 4) strncpy(e->options, "defaults", sizeof(e->options) - 1);
            fstab->count++;
        } else {
            ESP_LOGW(TAG, "fstab: bad line: %s", line);
        }
    }
    fclose(f);

    ESP_LOGI(TAG, "Parsed fstab: %d entries", fstab->count);
    return ESP_OK;
}

esp_err_t init_mount_fstab(void)
{
    fstab_t fstab;
    esp_err_t err = init_parse_fstab(&fstab);
    if (err != ESP_OK) return err;

    int mounted = 0;
    for (int i = 0; i < fstab.count; i++) {
        fstab_entry_t *e = &fstab.entries[i];

        if (strcmp(e->options, "noauto") == 0) {
            ESP_LOGI(TAG, "fstab: skip %s (noauto)", e->mountpoint);
            continue;
        }

        if (strcmp(e->device, "internal") == 0) {
            if (internal_fs_is_mounted()) {
                ESP_LOGI(TAG, "fstab: %s already mounted", e->mountpoint);
                mounted++;
                continue;
            }
            if (internal_fs_mount() == ESP_OK) mounted++;
            else ESP_LOGW(TAG, "fstab: failed to mount %s", e->mountpoint);

        } else if (strcmp(e->device, "sd0") == 0) {
            if (storage_sd_is_mounted()) {
                ESP_LOGI(TAG, "fstab: %s already mounted", e->mountpoint);
                mounted++;
                continue;
            }
            if (storage_sd_mount() == ESP_OK) mounted++;
            else ESP_LOGW(TAG, "fstab: failed to mount %s", e->mountpoint);

        } else {
            ESP_LOGW(TAG, "fstab: unknown device '%s'", e->device);
        }
    }

    ESP_LOGI(TAG, "fstab: %d/%d filesystems mounted", mounted, fstab.count);
    return ESP_OK;
}

static int cmd_fstab(int argc, char **argv)
{
    fstab_t fstab;
    if (init_parse_fstab(&fstab) != ESP_OK) {
        vconsole_printf("No fstab found at %s\n", FSTAB_PATH);
        return 1;
    }

    vconsole_printf("DEVICE          MOUNTPOINT      FSTYPE    OPTIONS\n");
    for (int i = 0; i < fstab.count; i++) {
        fstab_entry_t *e = &fstab.entries[i];
        vconsole_printf("%-15s %-15s %-9s %s\n",
                        e->device, e->mountpoint, e->fstype, e->options);
    }
    return 0;
}

esp_err_t cmd_fstab_register(void)
{
    const esp_console_cmd_t cmd = {
        .command = "fstab",
        .help = "Display filesystem mount table from /sys/etc/fstab",
        .func = cmd_fstab,
    };
    return esp_console_cmd_register(&cmd);
}

void init_boot_beep(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << PIN_BUZZER),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    for (int i = 0; i < 2; i++) {
        gpio_set_level(PIN_BUZZER, 1);
        vTaskDelay(pdMS_TO_TICKS(70));
        gpio_set_level(PIN_BUZZER, 0);
        vTaskDelay(pdMS_TO_TICKS(90));
    }
}
