#ifndef MANIFEST_H
#define MANIFEST_H

#include "esp_err.h"
#include <stdbool.h>

typedef struct {
    char name[32];
    char version[16];
    int  abi;
    char description[64];
    char author[32];
    char requires[128];
    int  iram_hint;
    bool loaded;
} manifest_t;

esp_err_t manifest_load(manifest_t *m, const char *ini_path);
esp_err_t manifest_check_compat(const manifest_t *m);

#endif
