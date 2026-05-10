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
    int  iram_hint;            // legacy; bytes hint for IRAM_ATTR code

    // Phase 5.6 manifest extensions:
    int  iram_pages;           // [memory] iram_pages — 4 KB units
    int  dram_pages;           // [memory] dram_pages — 4 KB units of DMA-able DRAM
    char kernel_deps[128];     // [requires] kernel_deps — comma-list of symbol prefixes
    bool static_link_allowed;  // [module] static_link — default true; "no" disallows MODULES_STATIC

    bool loaded;
} manifest_t;

esp_err_t manifest_load(manifest_t *m, const char *ini_path);
esp_err_t manifest_check_compat(const manifest_t *m);

#endif
