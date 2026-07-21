#include "shell.h"
#include "shell/cmd_files.h"
#include "shell/cmd_system.h"
#include "shell/cmd_diag.h"
#include "shell/cmd_module.h"
#include "shell/cmd_bench.h"
#include "services/display_mux.h"
#include "services/input.h"
#include "services/app.h"
#include "services/wifi.h"
#include "services/pkg_manager.h"
#include "services/mem_pool.h"
#include "services/config.h"
#include "services/registry.h"
#include "services/vconsole.h"
#include "services/init.h"
#include "hal/storage.h"
#include "esp_console.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

static const char *TAG = "shell";

#define HISTORY_SIZE 32
#define CWD_SIZE     256

static char s_cwd[CWD_SIZE] = "/sdcard";
static char s_history[HISTORY_SIZE][256];
static int  s_hist_count = 0;

const char *shell_get_cwd(void)
{
    return s_cwd;
}

void shell_set_cwd(const char *path)
{
    strncpy(s_cwd, path, CWD_SIZE - 1);
    s_cwd[CWD_SIZE - 1] = '\0';
}

static void normalize_path(char *path)
{
    char tmp[CWD_SIZE];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    char *parts[64];
    int lens[64];
    int depth = 0;

    char *p = tmp;
    while (*p) {
        while (*p == '/') p++;
        if (*p == '\0') break;

        char *seg = p;
        while (*p && *p != '/') p++;
        int len = p - seg;

        if (len == 1 && seg[0] == '.') {
            continue;
        } else if (len == 2 && seg[0] == '.' && seg[1] == '.') {
            if (depth > 0) depth--;
        } else {
            parts[depth] = seg;
            lens[depth] = len;
            if (depth < 63) depth++;
        }
    }

    char *out = path;
    if (depth == 0) {
        out[0] = '/';
        out[1] = '\0';
        return;
    }
    for (int i = 0; i < depth; i++) {
        *out++ = '/';
        memcpy(out, parts[i], lens[i]);
        out += lens[i];
    }
    *out = '\0';
}

void shell_resolve_path(const char *input, char *output, size_t output_size)
{
    if (input[0] == '/') {
        if (strncmp(input, "/sdcard", 7) == 0 || strncmp(input, "/sys", 4) == 0) {
            strncpy(output, input, output_size);
        } else {
            snprintf(output, output_size, "/sdcard%s", input);
        }
    } else {
        size_t cwd_len = strlen(s_cwd);
        if (cwd_len > 1 && s_cwd[cwd_len - 1] == '/')
            snprintf(output, output_size, "%s%s", s_cwd, input);
        else
            snprintf(output, output_size, "%s/%s", s_cwd, input);
    }
    output[output_size - 1] = '\0';
    normalize_path(output);
}

static void history_add(const char *line)
{
    int idx = s_hist_count % HISTORY_SIZE;
    strncpy(s_history[idx], line, 255);
    s_history[idx][255] = '\0';
    s_hist_count++;
}

// ── reboot ──────────────────────────────────────────────────

static int cmd_reboot(int argc, char **argv)
{
    vconsole_printf("Rebooting...\n");
    esp_restart();
    return 0;
}

// ── mem ─────────────────────────────────────────────────────

static int cmd_mem(int argc, char **argv)
{
    size_t iram_total = heap_caps_get_total_size(
        MALLOC_CAP_INTERNAL | MALLOC_CAP_EXEC | MALLOC_CAP_32BIT);
    size_t iram_free  = heap_caps_get_free_size(
        MALLOC_CAP_INTERNAL | MALLOC_CAP_EXEC | MALLOC_CAP_32BIT);
    size_t dram_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t dram_free  = heap_caps_get_free_size (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t dma_total  = heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    size_t dma_free   = heap_caps_get_free_size (MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    size_t psram_free  = heap_caps_get_free_size (MALLOC_CAP_SPIRAM);

    size_t pool_budget = mem_pool_iram_budget();
    size_t pool_used   = mem_pool_iram_used();
    size_t pool_dram   = mem_pool_dram_used();

    vconsole_printf("Region              Total       Free       Used\n");
    vconsole_printf("Internal DRAM    %7.1f KB %7.1f KB %7.1f KB\n",
                    dram_total / 1024.0, dram_free / 1024.0,
                    (dram_total - dram_free) / 1024.0);
    vconsole_printf("  └ DMA-capable  %7.1f KB %7.1f KB %7.1f KB\n",
                    dma_total / 1024.0, dma_free / 1024.0,
                    (dma_total - dma_free) / 1024.0);
    vconsole_printf("Internal IRAM    %7.1f KB %7.1f KB %7.1f KB\n",
                    iram_total / 1024.0, iram_free / 1024.0,
                    (iram_total - iram_free) / 1024.0);
    vconsole_printf("  └ module pool  %7.1f KB budget, %7.1f KB used\n",
                    pool_budget / 1024.0, pool_used / 1024.0);
    vconsole_printf("Module DRAM use  %7.1f KB\n", pool_dram / 1024.0);

    if (psram_total > 0) {
        vconsole_printf("PSRAM            %7.1f KB %7.1f KB %7.1f KB\n",
                        psram_total / 1024.0, psram_free / 1024.0,
                        (psram_total - psram_free) / 1024.0);
    } else {
        vconsole_printf("PSRAM: not available\n");
    }
    return 0;
}

// ── mount / unmount ─────────────────────────────────────────

static int cmd_mount(int argc, char **argv)
{
    if (storage_sd_is_mounted()) {
        vconsole_printf("SD already mounted\n");
        return 0;
    }
    esp_err_t ret = storage_sd_mount();
    return (ret == ESP_OK) ? 0 : 1;
}

static int cmd_unmount(int argc, char **argv)
{
    esp_err_t ret = storage_sd_unmount();
    return (ret == ESP_OK) ? 0 : 1;
}

// ── cd ──────────────────────────────────────────────────────

static int cmd_cd(int argc, char **argv)
{
    if (argc < 2) {
        shell_set_cwd("/sdcard");
        return 0;
    }
    char resolved[256];
    shell_resolve_path(argv[1], resolved, sizeof(resolved));

    struct stat st;
    if (stat(resolved, &st) != 0 || !S_ISDIR(st.st_mode)) {
        vconsole_printf("No such directory: %s\n", resolved);
        return 1;
    }
    shell_set_cwd(resolved);
    return 0;
}

// ── pwd ─────────────────────────────────────────────────────

static int cmd_pwd(int argc, char **argv)
{
    vconsole_printf("%s\n", shell_get_cwd());
    return 0;
}

// ── ls ──────────────────────────────────────────────────────

static int cmd_ls(int argc, char **argv)
{
    char path[256];
    if (argc > 1) {
        shell_resolve_path(argv[1], path, sizeof(path));
    } else {
        strncpy(path, shell_get_cwd(), sizeof(path));
        path[sizeof(path) - 1] = '\0';
    }

    DIR *dir = opendir(path);
    if (!dir) {
        vconsole_printf("Cannot open directory: %s\n", path);
        return 1;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        char type = (entry->d_type == DT_DIR) ? 'd' : '-';
        vconsole_printf("  %c  %s\n", type, entry->d_name);
    }
    closedir(dir);
    return 0;
}

// ── cat ─────────────────────────────────────────────────────

static int cmd_cat(int argc, char **argv)
{
    if (argc < 2) {
        vconsole_printf("Usage: cat <file>\n");
        return 1;
    }

    char path[256];
    shell_resolve_path(argv[1], path, sizeof(path));

    FILE *f = fopen(path, "r");
    if (!f) {
        vconsole_printf("Cannot open: %s\n", path);
        return 1;
    }

    char buf[256];
    while (fgets(buf, sizeof(buf), f)) {
        vconsole_write(buf, strlen(buf));
    }
    fclose(f);
    return 0;
}

// ── history ─────────────────────────────────────────────────

static int cmd_history(int argc, char **argv)
{
    int start = (s_hist_count > HISTORY_SIZE) ? s_hist_count - HISTORY_SIZE : 0;
    int count = (s_hist_count > HISTORY_SIZE) ? HISTORY_SIZE : s_hist_count;
    for (int i = 0; i < count; i++) {
        int idx = (start + i) % HISTORY_SIZE;
        vconsole_printf("  %d  %s\n", start + i + 1, s_history[idx]);
    }
    return 0;
}

// ── registration ────────────────────────────────────────────

esp_err_t shell_init(void)
{
    esp_console_config_t console_cfg = {
        .max_cmdline_args   = 8,
        .max_cmdline_length = 256,
    };
    esp_err_t ret = esp_console_init(&console_cfg);
    if (ret != ESP_OK) return ret;

    esp_console_register_help_command();

    const esp_console_cmd_t cmds[] = {
        { .command = "reboot",  .help = "Reboot the device",       .func = cmd_reboot },
        { .command = "mem",     .help = "Show memory stats",       .func = cmd_mem },
        { .command = "mount",   .help = "Mount SD card",           .func = cmd_mount },
        { .command = "unmount", .help = "Unmount SD card",         .func = cmd_unmount },
        { .command = "ls",      .help = "List directory [path]",   .func = cmd_ls },
        { .command = "cat",     .help = "Print file contents",     .func = cmd_cat },
        { .command = "cd",      .help = "Change directory",        .func = cmd_cd },
        { .command = "pwd",     .help = "Print working directory", .func = cmd_pwd },
        { .command = "history", .help = "Command history",         .func = cmd_history },
    };

    for (int i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        ret = esp_console_cmd_register(&cmds[i]);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register cmd '%s'", cmds[i].command);
            return ret;
        }
    }

    // File operation commands (write, mkdir, rm, cp, mv, etc.)
    ret = cmd_files_register();
    if (ret != ESP_OK) return ret;

    // System info commands (uname, uptime, ps, free, etc.)
    ret = cmd_system_register();
    if (ret != ESP_OK) return ret;

    // Diagnostic commands (i2cdetect, devls, gpio, loglevel, etc.)
    ret = cmd_diag_register();
    if (ret != ESP_OK) return ret;

    // Micro-benchmarks (bench cpu|mem|gfx)
    ret = cmd_bench_register();
    if (ret != ESP_OK) return ret;

    // Input service diagnostic command (input push|read|status)
    ret = cmd_input_register();
    if (ret != ESP_OK) return ret;

    // Config parser command
    ret = cmd_config_register();
    if (ret != ESP_OK) return ret;

    // Service registry command
    ret = cmd_service_register();
    if (ret != ESP_OK) return ret;

    // Virtual console command
    ret = cmd_vconsole_register();
    if (ret != ESP_OK) return ret;

    // Fstab command
    ret = cmd_fstab_register();
    if (ret != ESP_OK) return ret;

    // Module management commands (modload, lsmod, etc.)
    ret = cmd_module_register();
    if (ret != ESP_OK) return ret;

    // Display driver listing
    ret = cmd_display_register();
    if (ret != ESP_OK) return ret;

    // Wi-Fi commands
    ret = cmd_wifi_register();
    if (ret != ESP_OK) return ret;

    // Package manager commands
    ret = cmd_pkg_register();
    if (ret != ESP_OK) return ret;

    ESP_LOGI(TAG, "Shell initialized");
    return ESP_OK;
}

static int shell_read_line(char *buf, size_t buf_size)
{
    vconsole_printf("[%s] uarch> ", s_cwd);

    int i = 0;
    while (i < (int)buf_size - 1) {
        // All input (CardKB + UART, via the input service's UART bridge) comes
        // through one unified stream, so apps and the shell read the same way.
        int c = input_get_key(100);
        if (c < 0) continue;    // nothing yet — keep waiting (prompt already up)
        if (c == '\n' || c == '\r') {
            vconsole_putchar('\n');
            break;
        }
        if (c == '\b' || c == 127) {
            if (i > 0) {
                i--;
                vconsole_printf("\b \b");
            }
            continue;
        }
        if (c < 0x20 || c > 0x7e) continue;
        buf[i++] = (char)c;
        vconsole_putchar(c);
    }
    buf[i] = '\0';
    return i;
}

void shell_start(void)
{
    vconsole_printf("\n========================================\n");
    vconsole_printf("  micro_arch v0.2.0\n");
    vconsole_printf("  Type 'help' for available commands\n");
    vconsole_printf("========================================\n\n");

    char line[256];
    while (true) {
        int len = shell_read_line(line, sizeof(line));
        if (len == 0) continue;

        history_add(line);

        int ret;
        esp_err_t err = esp_console_run(line, &ret);
        if (err == ESP_ERR_NOT_FOUND) {
            // Not a built-in command — try to run it as an app from
            // /sdcard/bin/<name>.elf (the launcher: type a name → it runs).
            char split[256];
            strncpy(split, line, sizeof(split) - 1);
            split[sizeof(split) - 1] = '\0';
            char *argv[16];
            int argc = esp_console_split_argv(split, argv, 16);
            if (argc <= 0 || app_run(argv[0], argc, argv) == APP_NOT_FOUND)
                vconsole_printf("Unknown command: %s\n", line);
        } else if (err == ESP_ERR_INVALID_ARG) {
            // empty
        } else if (err != ESP_OK) {
            vconsole_printf("Error: %s\n", esp_err_to_name(err));
        }
    }
}
