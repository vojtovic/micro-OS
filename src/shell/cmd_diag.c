#include "cmd_diag.h"
#include "services/vconsole.h"
#include "bus/bus_manager.h"
#include "config/pin_config.h"
#include "hal/storage.h"
#include "hal/internal_fs.h"
#include "esp_console.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "cmd_diag";

static int cmd_i2cdetect(int argc, char **argv)
{
    i2c_port_t port = I2C_NUM_0;

    vconsole_printf("     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f\n");

    for (int row = 0; row < 8; row++) {
        vconsole_printf("%02x:", row * 16);
        for (int col = 0; col < 16; col++) {
            uint8_t addr = row * 16 + col;

            if (addr < 0x03 || addr > 0x77) {
                vconsole_printf("   ");
                continue;
            }

            i2c_cmd_handle_t cmd = i2c_cmd_link_create();
            i2c_master_start(cmd);
            i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
            i2c_master_stop(cmd);
            esp_err_t ret = i2c_master_cmd_begin(port, cmd, pdMS_TO_TICKS(50));
            i2c_cmd_link_delete(cmd);

            if (ret == ESP_OK) {
                vconsole_printf(" %02x", addr);
            } else {
                vconsole_printf(" --");
            }
        }
        vconsole_printf("\n");
    }
    return 0;
}

static int cmd_devls(int argc, char **argv)
{
    bool verbose = (argc >= 2 && strcmp(argv[1], "-v") == 0);

    vconsole_printf("DEVICE           STATUS       TYPE\n");
    vconsole_printf("─────────────────────────────────────\n");

    size_t psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    if (psram > 0) {
        vconsole_printf("psram0           ready        memory\n");
        if (verbose) {
            vconsole_printf("  size: %.1f MB, free: %.1f MB\n",
                   psram / (1024.0 * 1024.0),
                   heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / (1024.0 * 1024.0));
        }
    } else {
        vconsole_printf("psram0           not-present  memory\n");
    }

    vconsole_printf("spi3 (SD)        ready        bus\n");
    if (verbose) {
        vconsole_printf("  CLK=%d MOSI=%d MISO=%d\n", PIN_SD_CLK, PIN_SD_MOSI, PIN_SD_MISO);
    }

    if (storage_sd_is_mounted()) {
        vconsole_printf("sd0              mounted      storage\n");
        if (verbose) vconsole_printf("  mount: /sdcard, CS=%d\n", PIN_SD_CS);
    } else {
        vconsole_printf("sd0              not-mounted  storage\n");
    }

    vconsole_printf("i2c0             ready        bus\n");
    if (verbose) {
        vconsole_printf("  SDA=%d SCL=%d, 100kHz\n", PIN_I2C_SDA, PIN_I2C_SCL);
    }

    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (CARDKB_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(I2C_NUM_0, cmd, pdMS_TO_TICKS(50));
    i2c_cmd_link_delete(cmd);

    if (ret == ESP_OK) {
        vconsole_printf("cardkb           ready        input\n");
        if (verbose) vconsole_printf("  addr: 0x%02X on i2c0\n", CARDKB_ADDR);
    } else {
        vconsole_printf("cardkb           not-present  input\n");
    }

    if (internal_fs_is_mounted()) {
        vconsole_printf("littlefs0        mounted      filesystem\n");
        if (verbose) vconsole_printf("  mount: /sys, partition: internal\n");
    } else {
        vconsole_printf("littlefs0        not-mounted  filesystem\n");
    }

    return 0;
}

static int cmd_watchdog(int argc, char **argv)
{
    if (argc < 2) {
        vconsole_printf("Usage: watchdog <status|feed>\n");
        return 1;
    }

    if (strcmp(argv[1], "status") == 0) {
        vconsole_printf("Task WDT:   enabled (default ESP-IDF config)\n");
        vconsole_printf("Idle WDT:   enabled (feeds automatically)\n");
        vconsole_printf("Timeout:    5s (default)\n");
        vconsole_printf("Use 'watchdog feed' to manually feed from this task.\n");
    } else if (strcmp(argv[1], "feed") == 0) {
        esp_err_t ret = esp_task_wdt_reset();
        if (ret == ESP_OK) {
            vconsole_printf("Watchdog fed.\n");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            vconsole_printf("Shell task not subscribed to WDT (safe).\n");
        } else {
            vconsole_printf("WDT feed error: %s\n", esp_err_to_name(ret));
        }
    } else {
        vconsole_printf("Usage: watchdog <status|feed>\n");
        return 1;
    }
    return 0;
}

static int cmd_loglevel(int argc, char **argv)
{
    const struct { const char *name; esp_log_level_t level; } levels[] = {
        { "none",    ESP_LOG_NONE },
        { "error",   ESP_LOG_ERROR },
        { "warn",    ESP_LOG_WARN },
        { "info",    ESP_LOG_INFO },
        { "debug",   ESP_LOG_DEBUG },
        { "verbose", ESP_LOG_VERBOSE },
    };
    const int n_levels = sizeof(levels) / sizeof(levels[0]);

    if (argc == 1) {
        vconsole_printf("Usage: loglevel <level>           (set global)\n");
        vconsole_printf("       loglevel <tag> <level>     (set per-tag)\n");
        vconsole_printf("Levels: none, error, warn, info, debug, verbose\n");
        return 0;
    }

    const char *level_str;
    const char *tag_str;

    if (argc == 2) {
        tag_str = "*";
        level_str = argv[1];
    } else {
        tag_str = argv[1];
        level_str = argv[2];
    }

    for (int i = 0; i < n_levels; i++) {
        if (strcasecmp(level_str, levels[i].name) == 0) {
            esp_log_level_set(tag_str, levels[i].level);
            if (strcmp(tag_str, "*") == 0) {
                vconsole_printf("Global log level: %s\n", levels[i].name);
            } else {
                vconsole_printf("Log level for '%s': %s\n", tag_str, levels[i].name);
            }
            return 0;
        }
    }

    vconsole_printf("Unknown level: %s\n", level_str);
    vconsole_printf("Valid: none, error, warn, info, debug, verbose\n");
    return 1;
}

static int cmd_gpio(int argc, char **argv)
{
    if (argc < 3) {
        vconsole_printf("Usage: gpio read <pin>\n");
        vconsole_printf("       gpio write <pin> <0|1>\n");
        vconsole_printf("       gpio mode <pin> <in|out>\n");
        return 1;
    }

    int pin = atoi(argv[2]);
    if (pin < 0 || pin > 48) {
        vconsole_printf("Invalid pin: %d (0-48)\n", pin);
        return 1;
    }

    if (strcmp(argv[1], "read") == 0) {
        int val = gpio_get_level(pin);
        vconsole_printf("GPIO %d = %d\n", pin, val);

    } else if (strcmp(argv[1], "write") == 0) {
        if (argc < 4) {
            vconsole_printf("Usage: gpio write <pin> <0|1>\n");
            return 1;
        }
        int val = atoi(argv[3]);
        gpio_set_level(pin, val ? 1 : 0);
        vconsole_printf("GPIO %d <- %d\n", pin, val ? 1 : 0);

    } else if (strcmp(argv[1], "mode") == 0) {
        if (argc < 4) {
            vconsole_printf("Usage: gpio mode <pin> <in|out>\n");
            return 1;
        }
        gpio_mode_t mode;
        if (strcmp(argv[3], "in") == 0) {
            mode = GPIO_MODE_INPUT;
        } else if (strcmp(argv[3], "out") == 0) {
            mode = GPIO_MODE_OUTPUT;
        } else {
            vconsole_printf("Mode must be 'in' or 'out'\n");
            return 1;
        }
        esp_err_t ret = gpio_set_direction(pin, mode);
        if (ret != ESP_OK) {
            vconsole_printf("Failed: %s\n", esp_err_to_name(ret));
            return 1;
        }
        vconsole_printf("GPIO %d mode: %s\n", pin, argv[3]);

    } else {
        vconsole_printf("Unknown subcommand: %s\n", argv[1]);
        return 1;
    }
    return 0;
}

static int cmd_sync(int argc, char **argv)
{
    vconsole_printf("Filesystem status:\n");

    if (storage_sd_is_mounted()) {
        vconsole_printf("  /sdcard: mounted (FAT auto-syncs on fclose)\n");
    } else {
        vconsole_printf("  /sdcard: not mounted\n");
    }

    if (internal_fs_is_mounted()) {
        vconsole_printf("  /sys: mounted (LittleFS auto-syncs on fclose)\n");
    } else {
        vconsole_printf("  /sys: not mounted\n");
    }

    vconsole_printf("All writes are flushed on file close. Use 'shutdown' for clean unmount.\n");

    return 0;
}

static int cmd_i2cget(int argc, char **argv)
{
    if (argc < 3) {
        vconsole_printf("Usage: i2cget <addr> <reg> [len]\n");
        vconsole_printf("  addr/reg in hex (0x5F) or decimal\n");
        return 1;
    }

    uint8_t addr = (uint8_t)strtol(argv[1], NULL, 0);
    uint8_t reg  = (uint8_t)strtol(argv[2], NULL, 0);
    int len = (argc >= 4) ? atoi(argv[3]) : 1;
    if (len < 1) len = 1;
    if (len > 32) len = 32;

    uint8_t data[32];
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_READ, true);
    if (len > 1)
        i2c_master_read(cmd, data, len - 1, I2C_MASTER_ACK);
    i2c_master_read_byte(cmd, &data[len - 1], I2C_MASTER_NACK);
    i2c_master_stop(cmd);

    esp_err_t ret = i2c_master_cmd_begin(I2C_NUM_0, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);

    if (ret != ESP_OK) {
        vconsole_printf("I2C read failed: %s\n", esp_err_to_name(ret));
        return 1;
    }

    vconsole_printf("0x%02X reg 0x%02X:", addr, reg);
    for (int i = 0; i < len; i++) vconsole_printf(" 0x%02X", data[i]);
    vconsole_printf("\n");
    return 0;
}

static int cmd_i2cset(int argc, char **argv)
{
    if (argc < 4) {
        vconsole_printf("Usage: i2cset <addr> <reg> <value> [value...]\n");
        return 1;
    }

    uint8_t addr = (uint8_t)strtol(argv[1], NULL, 0);
    uint8_t reg  = (uint8_t)strtol(argv[2], NULL, 0);

    int count = argc - 3;
    if (count > 32) count = 32;

    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    for (int i = 0; i < count; i++) {
        uint8_t val = (uint8_t)strtol(argv[3 + i], NULL, 0);
        i2c_master_write_byte(cmd, val, true);
    }
    i2c_master_stop(cmd);

    esp_err_t ret = i2c_master_cmd_begin(I2C_NUM_0, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);

    if (ret != ESP_OK) {
        vconsole_printf("I2C write failed: %s\n", esp_err_to_name(ret));
        return 1;
    }

    vconsole_printf("Wrote %d byte(s) to 0x%02X reg 0x%02X\n", count, addr, reg);
    return 0;
}

static int cmd_beep(int argc, char **argv)
{
    int freq_hz = 1000;
    int duration_ms = 200;

    if (argc >= 2) freq_hz = atoi(argv[1]);
    if (argc >= 3) duration_ms = atoi(argv[2]);
    if (freq_hz < 100) freq_hz = 100;
    if (freq_hz > 8000) freq_hz = 8000;
    if (duration_ms < 10) duration_ms = 10;
    if (duration_ms > 5000) duration_ms = 5000;

    ledc_timer_config_t timer_cfg = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num       = LEDC_TIMER_0,
        .freq_hz         = (uint32_t)freq_hz,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    esp_err_t ret = ledc_timer_config(&timer_cfg);
    if (ret != ESP_OK) {
        vconsole_printf("Buzzer timer error: %s\n", esp_err_to_name(ret));
        return 1;
    }

    ledc_channel_config_t ch_cfg = {
        .gpio_num   = PIN_BUZZER,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LEDC_CHANNEL_0,
        .timer_sel  = LEDC_TIMER_0,
        .duty       = 128,
        .hpoint     = 0,
    };
    ret = ledc_channel_config(&ch_cfg);
    if (ret != ESP_OK) {
        vconsole_printf("Buzzer channel error: %s\n", esp_err_to_name(ret));
        return 1;
    }

    vTaskDelay(pdMS_TO_TICKS(duration_ms));

    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);

    vconsole_printf("Beep: %d Hz, %d ms\n", freq_hz, duration_ms);
    return 0;
}

esp_err_t cmd_diag_register(void)
{
    const esp_console_cmd_t cmds[] = {
        { .command = "i2cdetect", .help = "Scan I2C bus for devices",           .func = cmd_i2cdetect },
        { .command = "i2cget",    .help = "Read I2C: i2cget <addr> <reg> [len]",.func = cmd_i2cget },
        { .command = "i2cset",    .help = "Write I2C: i2cset <addr> <reg> <val>",.func = cmd_i2cset },
        { .command = "devls",     .help = "List system devices and status",     .func = cmd_devls },
        { .command = "watchdog",  .help = "WDT status/feed: watchdog <status|feed>", .func = cmd_watchdog },
        { .command = "loglevel",  .help = "Set log level: loglevel [tag] <level>",   .func = cmd_loglevel },
        { .command = "gpio",      .help = "GPIO ops: gpio <read|write|mode> <pin>",  .func = cmd_gpio },
        { .command = "sync",      .help = "Flush filesystem writes",            .func = cmd_sync },
        { .command = "beep",      .help = "Buzzer: beep [freq_hz] [duration_ms]",.func = cmd_beep },
    };

    for (int i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        esp_err_t ret = esp_console_cmd_register(&cmds[i]);
        if (ret != ESP_OK) return ret;
    }
    return ESP_OK;
}
