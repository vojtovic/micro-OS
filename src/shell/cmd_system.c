#include "cmd_system.h"
#include "services/vconsole.h"
#include "hal/storage.h"
#include "hal/internal_fs.h"
#include "services/klog.h"
#include "esp_console.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <stdlib.h>
#include "shell/shell.h"

static const char *TAG = "cmd_system";

static int cmd_uname(int argc, char **argv)
{
    esp_chip_info_t info;
    esp_chip_info(&info);
    vconsole_printf("micro_arch 0.1.0 ESP32-S3 (rev %d, %d cores) ESP-IDF %s FreeRTOS\n",
           info.revision, info.cores, esp_get_idf_version());
    return 0;
}

static int cmd_uptime(int argc, char **argv)
{
    int64_t us = esp_timer_get_time();
    int sec = (int)(us / 1000000);
    int days = sec / 86400; sec %= 86400;
    int hrs  = sec / 3600;  sec %= 3600;
    int mins = sec / 60;    sec %= 60;
    vconsole_printf("up %dd %dh %dm %ds\n", days, hrs, mins, sec);
    return 0;
}

static int cmd_date(int argc, char **argv)
{
    vconsole_printf("RTC not configured (install rtc driver module)\n");
    return 0;
}

static int cmd_ps(int argc, char **argv)
{
    uint32_t count = uxTaskGetNumberOfTasks();
    char *buf = heap_caps_malloc(count * 50, MALLOC_CAP_SPIRAM);
    if (!buf) {
        vconsole_printf("Out of memory\n");
        return 1;
    }
    vconsole_printf("Name                 State  Prio  Stack  Core\n");
    vconsole_printf("-------------------------------------------\n");
    vTaskList(buf);
    vconsole_printf("%s", buf);
    heap_caps_free(buf);
    return 0;
}

static int cmd_clear(int argc, char **argv)
{
    vconsole_printf("\033[2J\033[H");
    return 0;
}

static int cmd_echo(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        vconsole_printf("%s", argv[i]);
        if (i < argc - 1) vconsole_printf(" ");
    }
    vconsole_printf("\n");
    return 0;
}

static int cmd_hostname(int argc, char **argv)
{
    vconsole_printf("micro_arch\n");
    return 0;
}

static int cmd_env(int argc, char **argv)
{
    esp_chip_info_t info;
    esp_chip_info(&info);
    size_t psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    vconsole_printf("BOARD=esp32s3\n");
    vconsole_printf("CORES=%d\n", info.cores);
    vconsole_printf("PSRAM=%luMB\n", (unsigned long)(psram / (1024 * 1024)));
    vconsole_printf("IDF_VER=%s\n", esp_get_idf_version());
    vconsole_printf("OS=micro_arch\n");
    vconsole_printf("VERSION=0.1.0\n");
    return 0;
}

static int cmd_free(int argc, char **argv)
{
    size_t int_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    size_t int_free  = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t ps_total  = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    size_t ps_free   = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    vconsole_printf("            total       used       free\n");
    vconsole_printf("IRAM   %9lu  %9lu  %9lu\n",
           (unsigned long)int_total,
           (unsigned long)(int_total - int_free),
           (unsigned long)int_free);
    if (ps_total > 0) {
        vconsole_printf("PSRAM  %9lu  %9lu  %9lu\n",
               (unsigned long)ps_total,
               (unsigned long)(ps_total - ps_free),
               (unsigned long)ps_free);
    }
    return 0;
}

static int cmd_lscpu(int argc, char **argv)
{
    esp_chip_info_t info;
    esp_chip_info(&info);
    vconsole_printf("Architecture:    Xtensa LX7\n");
    vconsole_printf("CPU(s):          %d\n", info.cores);
    vconsole_printf("Revision:        %d\n", info.revision);
    vconsole_printf("Features:        %s%s%s\n",
           (info.features & CHIP_FEATURE_WIFI_BGN) ? "WiFi " : "",
           (info.features & CHIP_FEATURE_BLE) ? "BLE " : "",
           (info.features & CHIP_FEATURE_EMB_FLASH) ? "EmbFlash " : "");
    vconsole_printf("CPU MHz:         240\n");
    return 0;
}

static int cmd_shutdown(int argc, char **argv)
{
    vconsole_printf("Shutting down...\n");
    if (storage_sd_is_mounted()) {
        vconsole_printf("  Unmounting /sdcard...\n");
        storage_sd_unmount();
    }
    if (internal_fs_is_mounted()) {
        vconsole_printf("  Unmounting /sys...\n");
        internal_fs_unmount();
    }
    vconsole_printf("  Halting.\n");
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
    return 0;
}

static int cmd_lsblk(int argc, char **argv)
{
    vconsole_printf("NAME         TYPE    SIZE  MOUNT   FS\n");
    vconsole_printf("flash0       flash   16MB\n");

    esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_DATA,
                                                     ESP_PARTITION_SUBTYPE_ANY, NULL);
    while (it) {
        const esp_partition_t *p = esp_partition_get(it);
        if (p) {
            const char *mount = "";
            const char *fs = "";
            if (strcmp(p->label, "internal") == 0) {
                mount = "/sys";
                fs = "littlefs";
            } else if (strcmp(p->label, "nvs") == 0) {
                fs = "nvs";
            }
            vconsole_printf("  %-10s part  %4luKB  %-7s %s\n",
                   p->label, (unsigned long)(p->size / 1024), mount, fs);
        }
        it = esp_partition_next(it);
    }
    esp_partition_iterator_release(it);

    vconsole_printf("sd0          sd     ");
    if (storage_sd_is_mounted()) {
        vconsole_printf("     /sdcard  fat32\n");
    } else {
        vconsole_printf("     (not mounted)\n");
    }
    return 0;
}

static int cmd_which(int argc, char **argv)
{
    if (argc < 2) {
        vconsole_printf("Usage: which <command>\n");
        return 1;
    }

    const char *dirs[] = { "/sdcard/bin", "/sdcard/drivers" };
    char path[256];
    for (int d = 0; d < 2; d++) {
        snprintf(path, sizeof(path), "%s/%s", dirs[d], argv[1]);
        struct stat st;
        if (stat(path, &st) == 0) {
            vconsole_printf("%s\n", path);
            return 0;
        }
        snprintf(path, sizeof(path), "%s/%s.elf", dirs[d], argv[1]);
        if (stat(path, &st) == 0) {
            vconsole_printf("%s\n", path);
            return 0;
        }
    }

    vconsole_printf("%s: not found\n", argv[1]);
    return 1;
}

static int cmd_dmesg(int argc, char **argv)
{
    klog_dump();
    return 0;
}

static int cmd_sleep(int argc, char **argv)
{
    if (argc < 2) {
        vconsole_printf("Usage: sleep <seconds>\n");
        return 1;
    }
    int ms = (int)(strtof(argv[1], NULL) * 1000);
    if (ms < 10) ms = 10;
    if (ms > 300000) ms = 300000;
    vTaskDelay(pdMS_TO_TICKS(ms));
    return 0;
}

static int cmd_time(int argc, char **argv)
{
    if (argc < 2) {
        vconsole_printf("Usage: time <command> [args...]\n");
        return 1;
    }

    char line[256];
    int pos = 0;
    for (int i = 1; i < argc && pos < (int)sizeof(line) - 1; i++) {
        if (i > 1) line[pos++] = ' ';
        int n = snprintf(line + pos, sizeof(line) - pos, "%s", argv[i]);
        pos += n;
    }
    line[pos] = '\0';

    int64_t start = esp_timer_get_time();
    int ret;
    esp_console_run(line, &ret);
    int64_t elapsed = esp_timer_get_time() - start;

    vconsole_printf("\nreal  %lld.%03lld s\n",
           (long long)(elapsed / 1000000),
           (long long)((elapsed / 1000) % 1000));
    return ret;
}

static uint32_t crc32_byte(uint32_t crc, uint8_t byte)
{
    crc ^= byte;
    for (int i = 0; i < 8; i++) {
        if (crc & 1)
            crc = (crc >> 1) ^ 0xEDB88320;
        else
            crc >>= 1;
    }
    return crc;
}

static int cmd_crc32(int argc, char **argv)
{
    if (argc < 2) {
        vconsole_printf("Usage: crc32 <file>\n");
        return 1;
    }

    char path[256];
    shell_resolve_path(argv[1], path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f) {
        vconsole_printf("Cannot open: %s\n", path);
        return 1;
    }

    uint32_t crc = 0xFFFFFFFF;
    long total = 0;
    unsigned char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        for (size_t i = 0; i < n; i++)
            crc = crc32_byte(crc, buf[i]);
        total += (long)n;
    }
    fclose(f);

    crc ^= 0xFFFFFFFF;
    vconsole_printf("%08lx  %ld  %s\n", (unsigned long)crc, total, path);
    return 0;
}

esp_err_t cmd_system_register(void)
{
    const esp_console_cmd_t cmds[] = {
        { .command = "uname",    .help = "System info",                .func = cmd_uname },
        { .command = "uptime",   .help = "Time since boot",           .func = cmd_uptime },
        { .command = "date",     .help = "Show date/time",            .func = cmd_date },
        { .command = "ps",       .help = "List running tasks",        .func = cmd_ps },
        { .command = "clear",    .help = "Clear screen",              .func = cmd_clear },
        { .command = "echo",     .help = "Print text",                .func = cmd_echo },
        { .command = "hostname", .help = "Show hostname",             .func = cmd_hostname },
        { .command = "env",      .help = "Show environment",          .func = cmd_env },
        { .command = "free",     .help = "Memory usage (Linux-style)",.func = cmd_free },
        { .command = "lscpu",    .help = "CPU information",           .func = cmd_lscpu },
        { .command = "shutdown", .help = "Clean unmount and halt",    .func = cmd_shutdown },
        { .command = "lsblk",   .help = "List block devices",        .func = cmd_lsblk },
        { .command = "which",   .help = "Find command in /bin",      .func = cmd_which },
        { .command = "dmesg",   .help = "Kernel log ring buffer",    .func = cmd_dmesg },
        { .command = "sleep",   .help = "Delay: sleep <seconds>",  .func = cmd_sleep },
        { .command = "time",    .help = "Measure: time <command>", .func = cmd_time },
        { .command = "crc32",   .help = "File checksum: crc32 <file>", .func = cmd_crc32 },
    };

    for (int i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        esp_err_t ret = esp_console_cmd_register(&cmds[i]);
        if (ret != ESP_OK) return ret;
    }
    return ESP_OK;
}
