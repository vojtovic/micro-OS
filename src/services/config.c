#include "config.h"
#include "vconsole.h"
#include "shell/shell.h"
#include "esp_console.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "config";

esp_err_t config_load(config_t *cfg, const char *path)
{
    memset(cfg, 0, sizeof(*cfg));
    strncpy(cfg->path, path, sizeof(cfg->path) - 1);

    FILE *f = fopen(path, "r");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open: %s", path);
        return ESP_ERR_NOT_FOUND;
    }

    char section[CONFIG_MAX_KEYLEN] = "";
    char line[256];

    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r' || line[len - 1] == ' '))
            line[--len] = '\0';

        if (len == 0 || line[0] == '#' || line[0] == ';')
            continue;

        if (line[0] == '[') {
            char *end = strchr(line, ']');
            if (end) {
                *end = '\0';
                strncpy(section, line + 1, CONFIG_MAX_KEYLEN - 1);
                section[CONFIG_MAX_KEYLEN - 1] = '\0';
            }
            continue;
        }

        char *eq = strchr(line, '=');
        if (!eq) continue;

        if (cfg->count >= CONFIG_MAX_ENTRIES) {
            ESP_LOGW(TAG, "Config full (%d entries), skipping rest", CONFIG_MAX_ENTRIES);
            break;
        }

        *eq = '\0';
        config_entry_t *e = &cfg->entries[cfg->count];
        strncpy(e->section, section, CONFIG_MAX_KEYLEN - 1);
        strncpy(e->key, line, CONFIG_MAX_KEYLEN - 1);
        strncpy(e->value, eq + 1, CONFIG_MAX_VALLEN - 1);

        size_t klen = strlen(e->key);
        while (klen > 0 && e->key[klen - 1] == ' ') e->key[--klen] = '\0';
        char *vstart = e->value;
        while (*vstart == ' ') vstart++;
        if (vstart != e->value) memmove(e->value, vstart, strlen(vstart) + 1);

        cfg->count++;
    }
    fclose(f);

    ESP_LOGI(TAG, "Loaded %s: %d entries", path, cfg->count);
    return ESP_OK;
}

const char *config_get(const config_t *cfg, const char *section, const char *key)
{
    for (int i = 0; i < cfg->count; i++) {
        if (strcmp(cfg->entries[i].section, section) == 0 &&
            strcmp(cfg->entries[i].key, key) == 0) {
            return cfg->entries[i].value;
        }
    }
    return NULL;
}

int config_get_int(const config_t *cfg, const char *section, const char *key, int fallback)
{
    const char *val = config_get(cfg, section, key);
    if (!val) return fallback;
    return (int)strtol(val, NULL, 0);
}

void config_dump(const config_t *cfg)
{
    vconsole_printf("# %s (%d entries)\n", cfg->path, cfg->count);
    const char *last_section = "";
    for (int i = 0; i < cfg->count; i++) {
        if (strcmp(cfg->entries[i].section, last_section) != 0) {
            vconsole_printf("\n[%s]\n", cfg->entries[i].section);
            last_section = cfg->entries[i].section;
        }
        vconsole_printf("%s=%s\n", cfg->entries[i].key, cfg->entries[i].value);
    }
}

void config_dump_section(const config_t *cfg, const char *section)
{
    vconsole_printf("[%s]\n", section);
    int found = 0;
    for (int i = 0; i < cfg->count; i++) {
        if (strcmp(cfg->entries[i].section, section) == 0) {
            vconsole_printf("%s=%s\n", cfg->entries[i].key, cfg->entries[i].value);
            found++;
        }
    }
    if (found == 0) vconsole_printf("(no entries)\n");
}

static int cmd_config(int argc, char **argv)
{
    if (argc < 2) {
        vconsole_printf("Usage: config <file>                  (dump all)\n");
        vconsole_printf("       config <file> <section>        (dump section)\n");
        vconsole_printf("       config <file> <section.key>    (get value)\n");
        return 1;
    }

    char path[256];
    shell_resolve_path(argv[1], path, sizeof(path));

    config_t *cfg = malloc(sizeof(config_t));
    if (!cfg) {
        vconsole_printf("Out of memory\n");
        return 1;
    }

    esp_err_t ret = config_load(cfg, path);
    if (ret != ESP_OK) {
        vconsole_printf("Cannot load: %s\n", path);
        free(cfg);
        return 1;
    }

    if (argc == 2) {
        config_dump(cfg);
    } else {
        char *dot = strchr(argv[2], '.');
        if (dot) {
            *dot = '\0';
            const char *val = config_get(cfg, argv[2], dot + 1);
            if (val) {
                vconsole_printf("%s\n", val);
            } else {
                vconsole_printf("Not found: [%s] %s\n", argv[2], dot + 1);
                free(cfg);
                return 1;
            }
        } else {
            config_dump_section(cfg, argv[2]);
        }
    }

    free(cfg);
    return 0;
}

esp_err_t cmd_config_register(void)
{
    const esp_console_cmd_t cmd = {
        .command = "config",
        .help = "Read INI config: config <file> [section[.key]]",
        .func = cmd_config,
    };
    return esp_console_cmd_register(&cmd);
}
