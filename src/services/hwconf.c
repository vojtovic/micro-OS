#include "hwconf.h"
#include "services/vconsole.h"
#include "esp_console.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>

static const char *TAG = "hwconf";

#define HWCONF_MAX_ENTRIES  48
#define HWCONF_SECT_LEN     24
#define HWCONF_KEY_LEN      24
#define HWCONF_VAL_LEN      48

typedef struct {
    char section[HWCONF_SECT_LEN];
    char key[HWCONF_KEY_LEN];
    char value[HWCONF_VAL_LEN];
} hwconf_entry_t;

static hwconf_entry_t s_entries[HWCONF_MAX_ENTRIES];
static int            s_count  = 0;
static bool           s_loaded = false;

// Trim leading/trailing whitespace in place, return the trimmed start.
static char *trim(char *s)
{
    while (*s && isspace((unsigned char)*s)) s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) *--end = '\0';
    return s;
}

esp_err_t hwconf_load(void)
{
    s_count  = 0;
    s_loaded = true;   // mark loaded even if the file is missing (defaults apply)

    FILE *f = fopen(HWCONF_PATH, "r");
    if (!f) {
        ESP_LOGW(TAG, "No config at %s — modules will use defaults", HWCONF_PATH);
        return ESP_OK;
    }

    char line[128];
    char section[HWCONF_SECT_LEN] = "";
    while (fgets(line, sizeof(line), f) && s_count < HWCONF_MAX_ENTRIES) {
        // strip comment (# to end of line) and surrounding whitespace
        char *hash = strchr(line, '#');
        if (hash) *hash = '\0';
        char *p = trim(line);
        if (*p == '\0') continue;

        if (*p == '[') {
            char *close = strchr(p, ']');
            if (!close) continue;
            *close = '\0';
            char *name = trim(p + 1);
            strncpy(section, name, sizeof(section) - 1);
            section[sizeof(section) - 1] = '\0';
            continue;
        }

        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = trim(p);
        char *val = trim(eq + 1);
        if (*key == '\0') continue;

        hwconf_entry_t *e = &s_entries[s_count++];
        strncpy(e->section, section, sizeof(e->section) - 1);
        e->section[sizeof(e->section) - 1] = '\0';
        strncpy(e->key, key, sizeof(e->key) - 1);
        e->key[sizeof(e->key) - 1] = '\0';
        strncpy(e->value, val, sizeof(e->value) - 1);
        e->value[sizeof(e->value) - 1] = '\0';
    }
    fclose(f);

    ESP_LOGI(TAG, "Loaded %d entries from %s", s_count, HWCONF_PATH);
    return ESP_OK;
}

static const char *lookup(const char *section, const char *key)
{
    if (!s_loaded) hwconf_load();
    if (!section || !key) return NULL;
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_entries[i].section, section) == 0 &&
            strcmp(s_entries[i].key, key) == 0)
            return s_entries[i].value;
    }
    return NULL;
}

int hwconf_get_int(const char *section, const char *key, int def)
{
    const char *v = lookup(section, key);
    if (!v || *v == '\0') return def;
    char *end;
    long n = strtol(v, &end, 0);   // base 0: decimal or 0x hex
    if (end == v) return def;      // no digits parsed
    return (int)n;
}

int hwconf_get_str(const char *section, const char *key,
                   char *buf, int len, const char *def)
{
    const char *v = lookup(section, key);
    if (!v) {
        if (buf && len > 0) {
            if (def) { strncpy(buf, def, len - 1); buf[len - 1] = '\0'; }
            else     { buf[0] = '\0'; }
        }
        return 0;
    }
    if (buf && len > 0) {
        strncpy(buf, v, len - 1);
        buf[len - 1] = '\0';
    }
    return 1;
}

static int cmd_hwconf(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "reload") == 0) {
        hwconf_load();
        vconsole_printf("Reloaded %s (%d entries)\n", HWCONF_PATH, s_count);
        return 0;
    }
    if (argc >= 4 && strcmp(argv[1], "get") == 0) {
        char buf[HWCONF_VAL_LEN];
        if (hwconf_get_str(argv[2], argv[3], buf, sizeof(buf), NULL))
            vconsole_printf("[%s] %s = %s\n", argv[2], argv[3], buf);
        else
            vconsole_printf("[%s] %s = (unset)\n", argv[2], argv[3]);
        return 0;
    }

    // default: dump the whole table
    if (!s_loaded) hwconf_load();
    vconsole_printf("%s — %d entries\n", HWCONF_PATH, s_count);
    char last[HWCONF_SECT_LEN] = "";
    for (int i = 0; i < s_count; i++) {
        if (strcmp(last, s_entries[i].section) != 0) {
            vconsole_printf("[%s]\n", s_entries[i].section);
            strncpy(last, s_entries[i].section, sizeof(last) - 1);
        }
        vconsole_printf("  %s = %s\n", s_entries[i].key, s_entries[i].value);
    }
    vconsole_printf("\nUsage: hwconf [get <section> <key> | reload]\n");
    return 0;
}

esp_err_t cmd_hwconf_register(void)
{
    const esp_console_cmd_t cmd = {
        .command = "hwconf",
        .help = "Show/query hardware.conf (hwconf | hwconf get <sect> <key> | hwconf reload)",
        .func = cmd_hwconf,
    };
    return esp_console_cmd_register(&cmd);
}
