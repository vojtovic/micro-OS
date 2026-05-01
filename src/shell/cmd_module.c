#include "cmd_module.h"
#include "loader/module_mgr.h"
#include "loader/symtab.h"
#include "services/vconsole.h"
#include "shell/shell.h"
#include "esp_console.h"

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

esp_err_t cmd_module_register(void)
{
    const esp_console_cmd_t cmds[] = {
        { .command = "modload",   .help = "Load ELF module",       .func = cmd_modload },
        { .command = "modstart",  .help = "Start loaded module",   .func = cmd_modstart },
        { .command = "modstop",   .help = "Stop running module",   .func = cmd_modstop },
        { .command = "modunload", .help = "Unload stopped module", .func = cmd_modunload },
        { .command = "lsmod",     .help = "List loaded modules",   .func = cmd_lsmod },
        { .command = "modinfo",   .help = "Module details",        .func = cmd_modinfo },
    };

    for (int i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        esp_err_t ret = esp_console_cmd_register(&cmds[i]);
        if (ret != ESP_OK) return ret;
    }
    return ESP_OK;
}
