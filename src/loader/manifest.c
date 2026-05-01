#include "manifest.h"
#include "services/config.h"
#include "services/registry.h"
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

    return ESP_OK;
}
