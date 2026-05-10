#include "manifest.h"
#include "services/config.h"
#include "services/registry.h"
#include "symtab.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <string.h>

#define MODULE_ABI_VERSION  1

static const char *TAG = "manifest";

esp_err_t manifest_load(manifest_t *m, const char *ini_path)
{
    memset(m, 0, sizeof(*m));

    config_t *cfg = heap_caps_malloc(sizeof(config_t), MALLOC_CAP_SPIRAM);
    if (!cfg) return ESP_ERR_NO_MEM;

    esp_err_t err = config_load(cfg, ini_path);
    if (err != ESP_OK) {
        heap_caps_free(cfg);
        return err;
    }

    const char *v;
    if ((v = config_get(cfg, "module", "name")))
        strncpy(m->name, v, sizeof(m->name) - 1);
    if ((v = config_get(cfg, "module", "version")))
        strncpy(m->version, v, sizeof(m->version) - 1);
    m->abi = config_get_int(cfg, "module", "abi", 0);
    if ((v = config_get(cfg, "module", "description")))
        strncpy(m->description, v, sizeof(m->description) - 1);
    if ((v = config_get(cfg, "module", "author")))
        strncpy(m->author, v, sizeof(m->author) - 1);
    if ((v = config_get(cfg, "requires", "services")))
        strncpy(m->requires, v, sizeof(m->requires) - 1);
    m->iram_hint = config_get_int(cfg, "memory", "iram_hint", 0);

    // Phase 5.6 manifest extensions
    m->iram_pages = config_get_int(cfg, "memory", "iram_pages", 0);
    m->dram_pages = config_get_int(cfg, "memory", "dram_pages", 0);
    if ((v = config_get(cfg, "requires", "kernel_deps")))
        strncpy(m->kernel_deps, v, sizeof(m->kernel_deps) - 1);

    // static_link defaults to true (allow); only "no"/"false"/"0" disables it
    m->static_link_allowed = true;
    if ((v = config_get(cfg, "module", "static_link"))) {
        if (strcmp(v, "no") == 0 || strcmp(v, "false") == 0 || strcmp(v, "0") == 0)
            m->static_link_allowed = false;
    }

    m->loaded = true;
    heap_caps_free(cfg);
    return ESP_OK;
}

esp_err_t manifest_check_compat(const manifest_t *m)
{
    if (!m->loaded) return ESP_OK;

    if (m->abi != 0 && m->abi != MODULE_ABI_VERSION) {
        ESP_LOGE(TAG, "%s: ABI version %d, kernel expects %d",
                 m->name, m->abi, MODULE_ABI_VERSION);
        return ESP_ERR_INVALID_VERSION;
    }

    if (m->requires[0] != '\0') {
        char buf[128];
        strncpy(buf, m->requires, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        char *tok = strtok(buf, ",");
        while (tok) {
            while (*tok == ' ') tok++;
            if (registry_find(tok) == NULL) {
                ESP_LOGE(TAG, "%s: requires service '%s' which is not registered",
                         m->name, tok);
                return ESP_ERR_NOT_FOUND;
            }
            tok = strtok(NULL, ",");
        }
    }

    // Phase 5.6: validate kernel_deps prefixes against the symbol table.
    // Missing prefix is non-fatal (warn) — module may still work if it
    // doesn't actually call the missing symbols at runtime.
    if (m->kernel_deps[0] != '\0') {
        char buf[128];
        strncpy(buf, m->kernel_deps, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        char *tok = strtok(buf, ",");
        while (tok) {
            while (*tok == ' ') tok++;
            if (!symtab_has_prefix(tok)) {
                ESP_LOGW(TAG, "%s: kernel_dep prefix '%s' has no matching exports",
                         m->name, tok);
            }
            tok = strtok(NULL, ",");
        }
    }

    return ESP_OK;
}
