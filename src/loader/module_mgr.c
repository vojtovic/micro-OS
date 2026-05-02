#include "module_mgr.h"
#include "symtab.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "module_mgr";

static loaded_module_t s_modules[MODULE_MAX_LOADED];
static module_exports_t *s_pending_exports = NULL;

void module_register(module_exports_t *exports)
{
    s_pending_exports = exports;
}

esp_err_t module_mgr_init(void)
{
    memset(s_modules, 0, sizeof(s_modules));
    ESP_LOGI(TAG, "Module manager ready (max %d modules)", MODULE_MAX_LOADED);
    return ESP_OK;
}

static loaded_module_t *find_slot(void)
{
    for (int i = 0; i < MODULE_MAX_LOADED; i++) {
        if (!s_modules[i].active) return &s_modules[i];
    }
    return NULL;
}

static loaded_module_t *find_name(const char *name)
{
    for (int i = 0; i < MODULE_MAX_LOADED; i++) {
        if (s_modules[i].active && strcmp(s_modules[i].name, name) == 0)
            return &s_modules[i];
    }
    return NULL;
}

static void derive_manifest_path(const char *elf_path, char *ini_path, size_t size)
{
    strncpy(ini_path, elf_path, size - 1);
    ini_path[size - 1] = '\0';
    char *dot = strrchr(ini_path, '.');
    if (dot && strcmp(dot, ".elf") == 0)
        strcpy(dot, ".ini");
    else
        strncat(ini_path, ".ini", size - strlen(ini_path) - 1);
}

esp_err_t module_mgr_load(const char *elf_path)
{
    loaded_module_t *mod = find_slot();
    if (!mod) {
        ESP_LOGE(TAG, "All %d module slots occupied", MODULE_MAX_LOADED);
        return ESP_ERR_NO_MEM;
    }

    memset(mod, 0, sizeof(*mod));

    char ini_path[128];
    derive_manifest_path(elf_path, ini_path, sizeof(ini_path));

    struct stat st;
    bool has_manifest = (stat(ini_path, &st) == 0);
    if (has_manifest) {
        esp_err_t err = manifest_load(&mod->manifest, ini_path);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to parse manifest %s", ini_path);
            has_manifest = false;
        }
    }

    if (has_manifest) {
        esp_err_t err = manifest_check_compat(&mod->manifest);
        if (err != ESP_OK) return err;

        if (mod->manifest.name[0] && find_name(mod->manifest.name)) {
            ESP_LOGE(TAG, "Module '%s' already loaded", mod->manifest.name);
            return ESP_ERR_INVALID_STATE;
        }
    }

    mod->psram_before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    if (mod->psram_before < 4096) {
        ESP_LOGE(TAG, "Insufficient PSRAM: %zu bytes free", mod->psram_before);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = elf_loader_load(elf_path, &mod->elf);
    if (err != ESP_OK) return err;

    s_pending_exports = NULL;
    int ret = elf_loader_call_entry(&mod->elf, 0, NULL);
    if (ret != 0) {
        ESP_LOGE(TAG, "Entry point returned %d", ret);
        elf_loader_unload(&mod->elf);
        return ESP_FAIL;
    }

    if (!s_pending_exports || !s_pending_exports->info) {
        ESP_LOGE(TAG, "Module did not call module_register()");
        elf_loader_unload(&mod->elf);
        s_pending_exports = NULL;
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (s_pending_exports->info->magic != MODULE_MAGIC) {
        ESP_LOGE(TAG, "Bad module magic: 0x%08lX (expected 0x%08lX)",
                 (unsigned long)s_pending_exports->info->magic,
                 (unsigned long)MODULE_MAGIC);
        elf_loader_unload(&mod->elf);
        s_pending_exports = NULL;
        return ESP_ERR_INVALID_ARG;
    }

    mod->exports = s_pending_exports;
    s_pending_exports = NULL;
    strncpy(mod->name, mod->exports->info->name, sizeof(mod->name) - 1);

    if (find_name(mod->name)) {
        ESP_LOGE(TAG, "Module '%s' already loaded", mod->name);
        elf_loader_unload(&mod->elf);
        mod->exports = NULL;
        return ESP_ERR_INVALID_STATE;
    }

    strncpy(mod->path, elf_path, sizeof(mod->path) - 1);
    mod->state = MODULE_STATE_LOADED;
    mod->active = true;

    size_t psram_after = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t used = (mod->psram_before > psram_after)
                  ? (mod->psram_before - psram_after) : 0;

    ESP_LOGI(TAG, "Loaded '%s' v%s (%zu B text, %zu B data, %zu B PSRAM)",
             mod->name, mod->exports->info->version,
             mod->elf.text_size, mod->elf.data_size, used);
    return ESP_OK;
}

esp_err_t module_mgr_start(const char *name)
{
    loaded_module_t *mod = find_name(name);
    if (!mod) {
        ESP_LOGE(TAG, "Module '%s' not found", name);
        return ESP_ERR_NOT_FOUND;
    }
    if (mod->state == MODULE_STATE_STARTED) {
        ESP_LOGW(TAG, "'%s' already running", name);
        return ESP_ERR_INVALID_STATE;
    }
    if (!mod->exports->start) {
        ESP_LOGE(TAG, "'%s' has no start function", name);
        return ESP_ERR_NOT_SUPPORTED;
    }

    int ret = mod->exports->start();
    if (ret != 0) {
        ESP_LOGE(TAG, "'%s' start() failed: %d", name, ret);
        mod->state = MODULE_STATE_ERROR;
        return ESP_FAIL;
    }

    mod->state = MODULE_STATE_STARTED;
    ESP_LOGI(TAG, "Started '%s'", name);
    return ESP_OK;
}

esp_err_t module_mgr_stop(const char *name)
{
    loaded_module_t *mod = find_name(name);
    if (!mod) {
        ESP_LOGE(TAG, "Module '%s' not found", name);
        return ESP_ERR_NOT_FOUND;
    }
    if (mod->state != MODULE_STATE_STARTED) {
        ESP_LOGW(TAG, "'%s' is not running", name);
        return ESP_ERR_INVALID_STATE;
    }

    if (mod->exports->stop) {
        int ret = mod->exports->stop();
        if (ret != 0)
            ESP_LOGW(TAG, "'%s' stop() returned %d", name, ret);
    }

    mod->state = MODULE_STATE_STOPPED;
    ESP_LOGI(TAG, "Stopped '%s'", name);
    return ESP_OK;
}

esp_err_t module_mgr_unload(const char *name)
{
    loaded_module_t *mod = find_name(name);
    if (!mod) {
        ESP_LOGE(TAG, "Module '%s' not found", name);
        return ESP_ERR_NOT_FOUND;
    }
    if (mod->state == MODULE_STATE_STARTED) {
        ESP_LOGE(TAG, "'%s' still running — stop first", name);
        return ESP_ERR_INVALID_STATE;
    }

    size_t psram_pre = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    elf_loader_unload(&mod->elf);
    size_t psram_post = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    size_t recovered = (psram_post > psram_pre) ? (psram_post - psram_pre) : 0;
    size_t leak = (psram_post < mod->psram_before)
                  ? (mod->psram_before - psram_post) : 0;

    ESP_LOGI(TAG, "Unloaded '%s' (recovered %zu B)", name, recovered);
    if (leak > 256)
        ESP_LOGW(TAG, "Possible leak: %zu B not returned", leak);

    memset(mod, 0, sizeof(*mod));
    return ESP_OK;
}

const loaded_module_t *module_mgr_find(const char *name)
{
    return find_name(name);
}

int module_mgr_count(void)
{
    int n = 0;
    for (int i = 0; i < MODULE_MAX_LOADED; i++)
        if (s_modules[i].active) n++;
    return n;
}

const loaded_module_t *module_mgr_get(int index)
{
    int n = 0;
    for (int i = 0; i < MODULE_MAX_LOADED; i++) {
        if (s_modules[i].active) {
            if (n == index) return &s_modules[i];
            n++;
        }
    }
    return NULL;
}
