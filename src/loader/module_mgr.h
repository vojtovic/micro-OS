#ifndef MODULE_MGR_H
#define MODULE_MGR_H

#include "esp_err.h"
#include "elf_loader.h"
#include "manifest.h"
#include "module_types.h"
#include <stdbool.h>

#define MODULE_MAX_LOADED  8

typedef enum {
    MODULE_STATE_LOADED = 0,
    MODULE_STATE_STARTED,
    MODULE_STATE_STOPPED,
    MODULE_STATE_ERROR,
} module_state_t;

typedef struct {
    char              name[32];
    char              path[128];
    manifest_t        manifest;
    elf_load_result_t elf;
    module_exports_t *exports;
    module_state_t    state;
    size_t            psram_before;
    bool              active;
} loaded_module_t;

esp_err_t module_mgr_init(void);
esp_err_t module_mgr_load(const char *elf_path);
esp_err_t module_mgr_start(const char *name);
esp_err_t module_mgr_stop(const char *name);
esp_err_t module_mgr_unload(const char *name);

const loaded_module_t *module_mgr_find(const char *name);
int                    module_mgr_count(void);
const loaded_module_t *module_mgr_get(int index);

void module_register(module_exports_t *exports);

#endif
