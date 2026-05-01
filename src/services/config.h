#ifndef CONFIG_H
#define CONFIG_H

#include "esp_err.h"
#include <stdbool.h>

#define CONFIG_MAX_ENTRIES 64
#define CONFIG_MAX_KEYLEN  32
#define CONFIG_MAX_VALLEN  128

typedef struct {
    char section[CONFIG_MAX_KEYLEN];
    char key[CONFIG_MAX_KEYLEN];
    char value[CONFIG_MAX_VALLEN];
} config_entry_t;

typedef struct {
    config_entry_t entries[CONFIG_MAX_ENTRIES];
    int count;
    char path[128];
} config_t;

esp_err_t config_load(config_t *cfg, const char *path);
const char *config_get(const config_t *cfg, const char *section, const char *key);
int config_get_int(const config_t *cfg, const char *section, const char *key, int fallback);
void config_dump(const config_t *cfg);
void config_dump_section(const config_t *cfg, const char *section);

esp_err_t cmd_config_register(void);

#endif
