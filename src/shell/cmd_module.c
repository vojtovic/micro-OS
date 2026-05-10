#include "cmd_module.h"
#include "loader/module_mgr.h"
#include "loader/symtab.h"
#include "services/vconsole.h"
#include "shell/shell.h"
#include "esp_console.h"
#include "mbedtls/sha256.h"
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>

static const char *state_str(module_state_t s)
{
    switch (s) {
    case MODULE_STATE_LOADED:  return "loaded";
    case MODULE_STATE_STARTED: return "running";
    case MODULE_STATE_STOPPED: return "stopped";
    case MODULE_STATE_ERROR:   return "error";
    default:                   return "unknown";
    }
}

static int cmd_modload(int argc, char **argv)
{
    if (argc < 2) {
        vconsole_printf("Usage: modload <path.elf>\n");
        return 1;
    }
    char path[256];
    shell_resolve_path(argv[1], path, sizeof(path));

    esp_err_t err = module_mgr_load(path);
    if (err != ESP_OK) {
        vconsole_printf("Load failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    vconsole_printf("Module loaded\n");
    return 0;
}

static int cmd_modstart(int argc, char **argv)
{
    if (argc < 2) {
        vconsole_printf("Usage: modstart <name>\n");
        return 1;
    }
    esp_err_t err = module_mgr_start(argv[1]);
    if (err != ESP_OK) {
        vconsole_printf("Start failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    vconsole_printf("Module '%s' started\n", argv[1]);
    return 0;
}

static int cmd_modstop(int argc, char **argv)
{
    if (argc < 2) {
        vconsole_printf("Usage: modstop <name>\n");
        return 1;
    }
    esp_err_t err = module_mgr_stop(argv[1]);
    if (err != ESP_OK) {
        vconsole_printf("Stop failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    vconsole_printf("Module '%s' stopped\n", argv[1]);
    return 0;
}

static int cmd_modunload(int argc, char **argv)
{
    if (argc < 2) {
        vconsole_printf("Usage: modunload <name>\n");
        return 1;
    }
    esp_err_t err = module_mgr_unload(argv[1]);
    if (err != ESP_OK) {
        vconsole_printf("Unload failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    vconsole_printf("Module '%s' unloaded\n", argv[1]);
    return 0;
}

static int cmd_lsmod(int argc, char **argv)
{
    int count = module_mgr_count();
    if (count == 0) {
        vconsole_printf("No modules loaded\n");
        return 0;
    }

    bool verbose = (argc >= 2 && strcmp(argv[1], "-v") == 0);

    if (verbose) {
        vconsole_printf("%-12s %-8s %-7s %-7s %7s %7s %7s %s\n",
                        "NAME", "VERSION", "SOURCE", "STATE",
                        "PSRAM", "IRAM", "DRAM", "FLAGS");
        size_t total_psram = 0, total_iram = 0, total_dram = 0;
        for (int i = 0; i < count; i++) {
            const loaded_module_t *m = module_mgr_get(i);
            if (!m) continue;
            const char *src = (m->source == MOD_SRC_STATIC) ? "STATIC" : "LOADED";
            vconsole_printf("%-12s %-8s %-7s %-7s %5zuKB %5zuKB %5zuKB %s\n",
                            m->name,
                            m->exports->info->version,
                            src,
                            state_str(m->state),
                            m->psram_used / 1024,
                            m->iram_used / 1024,
                            m->dram_used / 1024,
                            m->iram_degraded ? "IRAM_DEGRADED" : "OK");
            total_psram += m->psram_used;
            total_iram  += m->iram_used;
            total_dram  += m->dram_used;
        }
        vconsole_printf("%-12s %-8s %-7s %-7s %5zuKB %5zuKB %5zuKB\n",
                        "totals", "", "", "",
                        total_psram / 1024, total_iram / 1024, total_dram / 1024);
        vconsole_printf("(%d/%d slots)\n", count, MODULE_MAX_LOADED);
        return 0;
    }

    vconsole_printf("%-16s %-8s %-8s %s\n", "NAME", "VERSION", "STATE", "TEXT/DATA");
    for (int i = 0; i < count; i++) {
        const loaded_module_t *m = module_mgr_get(i);
        if (!m) continue;
        vconsole_printf("%-16s %-8s %-8s %zu/%zu\n",
                        m->name,
                        m->exports->info->version,
                        state_str(m->state),
                        m->elf.text_size,
                        m->elf.data_size);
    }
    vconsole_printf("(%d/%d slots)\n", count, MODULE_MAX_LOADED);
    return 0;
}

static int cmd_modinfo(int argc, char **argv)
{
    if (argc < 2) {
        vconsole_printf("Usage: modinfo <name>\n");
        return 1;
    }
    const loaded_module_t *m = module_mgr_find(argv[1]);
    if (!m) {
        vconsole_printf("Module '%s' not found\n", argv[1]);
        return 1;
    }

    vconsole_printf("Name:       %s\n", m->name);
    vconsole_printf("Version:    %s\n", m->exports->info->version);
    vconsole_printf("ABI:        %lu\n", (unsigned long)m->exports->info->abi_version);
    vconsole_printf("State:      %s\n", state_str(m->state));
    vconsole_printf("Path:       %s\n", m->path);
    vconsole_printf("Text:       %zu B\n", m->elf.text_size);
    vconsole_printf("Data:       %zu B\n", m->elf.data_size);
    vconsole_printf("File:       %zu B\n", m->elf.file_size);

    if (m->exports->info->requires[0])
        vconsole_printf("Requires:   %s\n", m->exports->info->requires);
    if (m->manifest.loaded) {
        if (m->manifest.description[0])
            vconsole_printf("Desc:       %s\n", m->manifest.description);
        if (m->manifest.author[0])
            vconsole_printf("Author:     %s\n", m->manifest.author);
    }

    vconsole_printf("Flags:      0x%08lX\n", (unsigned long)m->exports->info->flags);
    vconsole_printf("Start fn:   %s\n", m->exports->start ? "yes" : "no");
    vconsole_printf("Stop fn:    %s\n", m->exports->stop ? "yes" : "no");

    int syms = symtab_count();
    vconsole_printf("Kernel sym: %d exported\n", syms);
    return 0;
}

static int cmd_modrun(int argc, char **argv)
{
    if (argc < 2) {
        vconsole_printf("Usage: modrun <path.elf>\n");
        return 1;
    }
    char path[256];
    shell_resolve_path(argv[1], path, sizeof(path));

    esp_err_t err = module_mgr_load(path);
    if (err != ESP_OK) {
        vconsole_printf("Load failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    const char *name = NULL;
    for (int i = 0; i < module_mgr_count(); i++) {
        const loaded_module_t *m = module_mgr_get(i);
        if (m && strcmp(m->path, path) == 0) {
            name = m->name;
            break;
        }
    }
    if (!name) return 1;

    err = module_mgr_start(name);
    if (err != ESP_OK) {
        vconsole_printf("Start failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    return 0;
}

static int cmd_verify(int argc, char **argv)
{
    if (argc < 2) {
        vconsole_printf("Usage: verify <file>\n");
        return 1;
    }

    char path[256];
    shell_resolve_path(argv[1], path, sizeof(path));

    FILE *f = fopen(path, "rb");
    if (!f) {
        vconsole_printf("Cannot open: %s\n", path);
        return 1;
    }

    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    uint8_t buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        mbedtls_sha256_update(&ctx, buf, n);
    fclose(f);
    uint8_t hash[32];
    mbedtls_sha256_finish(&ctx, hash);
    mbedtls_sha256_free(&ctx);

    char hex[65];
    for (int i = 0; i < 32; i++)
        sprintf(hex + i * 2, "%02x", hash[i]);
    hex[64] = '\0';

    char sha_path[264];
    snprintf(sha_path, sizeof(sha_path), "%s.sha256", path);
    FILE *sf = fopen(sha_path, "r");
    if (sf) {
        char expected[65] = {0};
        fgets(expected, sizeof(expected), sf);
        fclose(sf);
        if (strncmp(expected, hex, 64) == 0) {
            vconsole_printf("PASS  %s\n", hex);
        } else {
            vconsole_printf("FAIL  computed: %s\n", hex);
            vconsole_printf("      expected: %.64s\n", expected);
            return 1;
        }
    } else {
        vconsole_printf("%s  %s\n", hex, path);
    }
    return 0;
}

esp_err_t cmd_module_register(void)
{
    const esp_console_cmd_t cmds[] = {
        { .command = "modload",   .help = "Load ELF module",       .func = cmd_modload },
        { .command = "modstart",  .help = "Start loaded module",   .func = cmd_modstart },
        { .command = "modstop",   .help = "Stop running module",   .func = cmd_modstop },
        { .command = "modunload", .help = "Unload stopped module", .func = cmd_modunload },
        { .command = "modrun",    .help = "Load and start module",  .func = cmd_modrun },
        { .command = "lsmod",     .help = "List modules (-v for memory)", .func = cmd_lsmod },
        { .command = "modinfo",   .help = "Module details",        .func = cmd_modinfo },
        { .command = "verify",    .help = "Verify file hash: verify <file>", .func = cmd_verify },
    };

    for (int i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        esp_err_t ret = esp_console_cmd_register(&cmds[i]);
        if (ret != ESP_OK) return ret;
    }
    return ESP_OK;
}
