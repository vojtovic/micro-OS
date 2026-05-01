# ELF Module Runtime Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Enable runtime loading of ELF modules from SD card on ESP32-S3, with lifecycle management, symbol resolution, and memory safety.

**Architecture:** Kernel exports a sorted symbol table. `esp_elf` component handles Xtensa ELF relocation. Our `module.c` orchestrates the lifecycle (load → validate → init → start → stop → unload) with PSRAM allocation tracking. Modules declare themselves via a magic struct and optional INI manifest. Five shell commands (`insmod`, `rmmod`, `lsmod`, `modinfo`, `modprobe`) expose the system.

**Tech Stack:** C11, ESP-IDF v5.x, PlatformIO, `esp_elf` component, FreeRTOS, PSRAM/IRAM

**Spec:** `docs/2026-04-30-elf-module-runtime-design.md`

---

## File Map

| Action | Path | Responsibility |
|--------|------|----------------|
| Create | `src/loader/symtab.h` | `kernel_sym_t` type, `KSYM` macro, lookup function declaration |
| Create | `src/loader/symtab.c` | Sorted kernel symbol array, binary search `symtab_resolve()` |
| Create | `src/loader/module_sdk.h` | Module-side header: `module_info_t`, `MODULE_ABI_VERSION`, `MODULE_MAGIC`, lifecycle function declarations, `module_malloc`/`module_free` declarations, kernel symbol `extern` declarations |
| Create | `src/loader/elf_loader.h` | `elf_load_result_t` type, `elf_loader_load()`/`elf_loader_unload()` declarations |
| Create | `src/loader/elf_loader.c` | Wraps `esp_elf`: opens file, validates header, loads sections, resolves symbols, handles IRAM split |
| Create | `src/loader/manifest.h` | `manifest_t` type, `manifest_load()`/`manifest_check_compat()` declarations |
| Create | `src/loader/manifest.c` | INI manifest parser using `config_load()`, ABI + service dependency pre-check |
| Create | `src/loader/module.h` | `loaded_module_t`, `module_state_t`, lifecycle API: `module_load()`/`module_unload()`/`module_find()`, `module_malloc()`/`module_free()` |
| Create | `src/loader/module.c` | Module lifecycle state machine, loaded module table, allocation tracker, FreeRTOS task wait on unload |
| Create | `src/loader/cmd_module.h` | `cmd_module_register()` declaration |
| Create | `src/loader/cmd_module.c` | Shell commands: `insmod`, `rmmod`, `lsmod`, `modinfo`, `modprobe` |
| Create | `modules/dummy_display/platformio.ini` | Module build config — Xtensa toolchain, relocatable ELF output |
| Create | `modules/dummy_display/src/dummy_display.c` | Dummy display module — implements `hal_display_ops_t`, registers service |
| Create | `modules/dummy_display/dummy_display.ini` | Module manifest |
| Modify | `platformio.ini` | Add `esp_elf` to `lib_deps` |
| Modify | `src/main.c` | No changes needed (modules load at runtime, not boot) |
| Modify | `src/shell/shell.c` | Add `#include "loader/cmd_module.h"` and `cmd_module_register()` call |
| Modify | `src/shell/cmd_system.c` | Remove `cmd_lsmod` and `cmd_modinfo` (lines 217-272, 379-380) |

---

### Task 1: Add esp_elf dependency and verify build

**Files:**
- Modify: `platformio.ini`

This task adds the `esp_elf` component and confirms the kernel still builds.

- [ ] **Step 1: Add esp_elf to platformio.ini**

Add `lib_deps` to the existing `[env:freenove_esp32_s3_wroom]` section:

```ini
lib_deps =
    espressif/esp_elf@^0.2.0
```

If `esp_elf` is not available via PlatformIO registry, use the ESP Component Registry instead by adding to `platformio.ini`:

```ini
lib_deps =
    espressif/esp_elf@^0.2.0
```

Or if that fails, add it as an IDF component. Create `idf_component.yml` in the project root:

```yaml
dependencies:
  espressif/esp_elf:
    version: ">=0.2.0"
```

- [ ] **Step 2: Build to verify**

Run: `pio run 2>&1 | tail -20`

Expected: `[SUCCESS]` with no new errors. RAM/Flash may increase slightly due to the new library.

If `esp_elf` is not found, check the ESP Component Registry for the correct package name and version. The component may be named `esp_elf_loader` or similar. Adjust the dependency accordingly.

- [ ] **Step 3: Check esp_elf API**

After the library is fetched, find the main header:

```bash
find .pio -name "*.h" -path "*elf*" | head -10
```

Read the header to understand the API. Key things to look for:
- Function to load an ELF from a buffer or file
- Symbol resolution callback type
- Relocation handling API
- Memory allocation hooks

Record the actual function signatures — later tasks reference them. If the API differs from what this plan assumes, adapt accordingly.

- [ ] **Step 4: Commit**

```bash
git add platformio.ini idf_component.yml 2>/dev/null
git commit -m "feat: add esp_elf dependency for module loader"
```

---

### Task 2: Kernel symbol table

**Files:**
- Create: `src/loader/symtab.h`
- Create: `src/loader/symtab.c`

The symbol table is a sorted array of `{name, address}` pairs. Modules resolve undefined symbols against it at load time via binary search.

- [ ] **Step 1: Create symtab.h**

Create `src/loader/symtab.h`:

```c
#ifndef SYMTAB_H
#define SYMTAB_H

#include <stddef.h>

typedef struct {
    const char *name;
    void       *addr;
} kernel_sym_t;

#define KSYM(fn) { #fn, (void *)&(fn) }

void *symtab_resolve(const char *name);
int   symtab_count(void);

#endif
```

- [ ] **Step 2: Create symtab.c**

Create `src/loader/symtab.c`:

```c
#include "symtab.h"

#include "services/vconsole.h"
#include "services/registry.h"
#include "bus/bus_manager.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "driver/gpio.h"
#include <string.h>
#include <stdio.h>

static const kernel_sym_t s_symtab[] = {
    KSYM(gpio_config),
    KSYM(gpio_get_level),
    KSYM(gpio_set_level),
    KSYM(heap_caps_free),
    KSYM(heap_caps_malloc),
    KSYM(memcpy),
    KSYM(memset),
    KSYM(registry_add),
    KSYM(registry_find),
    KSYM(registry_set_state),
    KSYM(snprintf),
    KSYM(strlen),
    KSYM(vTaskDelay),
    KSYM(vTaskDelete),
    KSYM(vconsole_printf),
    KSYM(vconsole_putchar),
    KSYM(vconsole_write),
    KSYM(xEventGroupCreate),
    KSYM(xEventGroupSetBits),
    KSYM(xEventGroupWaitBits),
    KSYM(xSemaphoreCreateMutex),
    KSYM(xTaskCreate),
};

int symtab_count(void)
{
    return sizeof(s_symtab) / sizeof(s_symtab[0]);
}

void *symtab_resolve(const char *name)
{
    int lo = 0, hi = symtab_count() - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int cmp = strcmp(name, s_symtab[mid].name);
        if (cmp == 0) return s_symtab[mid].addr;
        if (cmp < 0) hi = mid - 1;
        else         lo = mid + 1;
    }
    return NULL;
}
```

The array MUST be sorted alphabetically by name for binary search to work. The entries above are already sorted. When adding new symbols, insert them in sorted order.

Note: `xSemaphoreTake` and `xSemaphoreGive` are macros in FreeRTOS, not functions — they cannot be exported via `KSYM()`. If modules need them, we wrap them in real functions later. `xSemaphoreCreateMutex` is also a macro but resolves to `xQueueCreateMutex` — test during build and adjust if needed.

- [ ] **Step 3: Build to verify**

Run: `pio run 2>&1 | tail -20`

Expected: `[SUCCESS]`. Some FreeRTOS symbols may be macros — if build fails with "initializer element is not constant", remove the offending `KSYM()` entries and add wrapper functions later.

Fix any macro-vs-function issues by either:
1. Removing the entry if not needed yet
2. Creating a thin wrapper: `static void wrap_vSemaphoreTake(...) { xSemaphoreTake(...); }` and exporting that instead

- [ ] **Step 4: Commit**

```bash
git add src/loader/symtab.h src/loader/symtab.c
git commit -m "feat: kernel symbol table with binary search resolver"
```

---

### Task 3: Module SDK header

**Files:**
- Create: `src/loader/module_sdk.h`

This header is included by every loadable module. It defines the module metadata struct and declares kernel symbols so the module can compile.

- [ ] **Step 1: Create module_sdk.h**

Create `src/loader/module_sdk.h`:

```c
#ifndef MODULE_SDK_H
#define MODULE_SDK_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define MODULE_ABI_VERSION  1
#define MODULE_MAGIC        0x4D4F4455

#define MODULE_FLAG_IRAM_HOT  (1 << 0)

typedef struct {
    uint32_t    magic;
    uint32_t    abi_version;
    char        name[32];
    char        version[16];
    char        requires[128];
    uint32_t    flags;
} module_info_t;

typedef int (*module_entry_fn)(void);

extern void *module_malloc(size_t size);
extern void  module_free(void *ptr);

typedef int   (*esp_err_t_fn)(void);

extern int  vconsole_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
extern int  vconsole_write(const char *data, size_t len);
extern int  vconsole_putchar(char c);

typedef struct {
    char     name[32];
    uint16_t version;
    int      state;
    void    *vtable;
    void    *ctx;
} service_entry_t;

extern int   registry_add(const char *name, uint16_t version, void *vtable, void *ctx);
extern void *registry_find(const char *name);
extern int   registry_set_state(const char *name, int state);

extern void *heap_caps_malloc(size_t size, uint32_t caps);
extern void  heap_caps_free(void *ptr);

#define MALLOC_CAP_SPIRAM    (1 << 10)
#define MALLOC_CAP_INTERNAL  (1 << 11)

#endif
```

Note: This header uses simplified types (`int` instead of `esp_err_t`) so modules don't need the full ESP-IDF headers. The actual types match at the ABI level (both are `int` on Xtensa). The `service_entry_t` here is a simplified mirror — modules only need it for `registry_find()` return type.

- [ ] **Step 2: Build kernel to verify it doesn't break anything**

Run: `pio run 2>&1 | tail -10`

Expected: `[SUCCESS]`. The SDK header is not included by any kernel code — it's only for modules.

- [ ] **Step 3: Commit**

```bash
git add src/loader/module_sdk.h
git commit -m "feat: module SDK header for loadable ELF modules"
```

---

### Task 4: Manifest parser

**Files:**
- Create: `src/loader/manifest.h`
- Create: `src/loader/manifest.c`

Parses the optional `.ini` companion file for pre-load compatibility checks. Reuses the existing `config_load()` INI parser from `services/config.c`.

- [ ] **Step 1: Create manifest.h**

Create `src/loader/manifest.h`:

```c
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
```

- [ ] **Step 2: Create manifest.c**

Create `src/loader/manifest.c`:

```c
#include "manifest.h"
#include "module_sdk.h"
#include "services/config.h"
#include "services/registry.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

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
```

- [ ] **Step 3: Build to verify**

Run: `pio run 2>&1 | tail -20`

Expected: `[SUCCESS]`. The manifest parser uses `config_load()` from `services/config.c` which is already built and tested. The `MALLOC_CAP_SPIRAM` constant comes from `esp_heap_caps.h` via the ESP-IDF.

- [ ] **Step 4: Commit**

```bash
git add src/loader/manifest.h src/loader/manifest.c
git commit -m "feat: INI manifest parser for module pre-load checks"
```

---

### Task 5: ELF loader wrapper

**Files:**
- Create: `src/loader/elf_loader.h`
- Create: `src/loader/elf_loader.c`

Wraps `esp_elf` to load an ELF file from SD, resolve symbols via `symtab_resolve()`, and return base addresses.

- [ ] **Step 1: Create elf_loader.h**

Create `src/loader/elf_loader.h`:

```c
#ifndef ELF_LOADER_H
#define ELF_LOADER_H

#include "esp_err.h"
#include <stddef.h>

typedef struct {
    void   *text_base;
    void   *data_base;
    size_t  text_size;
    size_t  data_size;
    void   *elf_ctx;
} elf_load_result_t;

esp_err_t elf_loader_load(const char *path, elf_load_result_t *result);
void      elf_loader_unload(elf_load_result_t *result);
void     *elf_loader_find_sym(elf_load_result_t *result, const char *name);

#endif
```

- [ ] **Step 2: Create elf_loader.c**

Create `src/loader/elf_loader.c`:

```c
#include "elf_loader.h"
#include "symtab.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>

static const char *TAG = "elf_loader";
```

**Important:** The exact implementation of this file depends on the `esp_elf` API discovered in Task 1, Step 3. The structure below shows the expected flow. Adapt function calls to match the actual `esp_elf` API.

```c
// After reading the esp_elf header, include it:
// #include "esp_elf.h"  (or whatever the actual header is)

esp_err_t elf_loader_load(const char *path, elf_load_result_t *result)
{
    memset(result, 0, sizeof(*result));

    // 1. Check file exists and get size
    struct stat st;
    if (stat(path, &st) != 0) {
        ESP_LOGE(TAG, "File not found: %s", path);
        return ESP_ERR_NOT_FOUND;
    }

    // 2. Read ELF file into PSRAM buffer
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open: %s", path);
        return ESP_FAIL;
    }

    uint8_t *buf = heap_caps_malloc(st.st_size, MALLOC_CAP_SPIRAM);
    if (!buf) {
        fclose(f);
        ESP_LOGE(TAG, "Cannot allocate %ld bytes for ELF", (long)st.st_size);
        return ESP_ERR_NO_MEM;
    }

    size_t read = fread(buf, 1, st.st_size, f);
    fclose(f);
    if (read != (size_t)st.st_size) {
        heap_caps_free(buf);
        ESP_LOGE(TAG, "Short read: %zu/%ld", read, (long)st.st_size);
        return ESP_FAIL;
    }

    // 3. Validate ELF magic
    if (buf[0] != 0x7F || buf[1] != 'E' || buf[2] != 'L' || buf[3] != 'F') {
        heap_caps_free(buf);
        ESP_LOGE(TAG, "Not an ELF file: %s", path);
        return ESP_ERR_INVALID_ARG;
    }

    // 4. Use esp_elf to load and relocate
    // ADAPT THIS SECTION to match the actual esp_elf API from Task 1 Step 3.
    // The general pattern:
    //   - Initialize esp_elf context
    //   - Provide the buffer and symbol resolution callback
    //   - esp_elf parses sections, allocates memory, applies Xtensa relocations
    //   - Extract entry points and section bases from the context
    //
    // Pseudocode (replace with real API):
    //
    //   esp_elf_t *elf = esp_elf_init(buf, st.st_size);
    //   esp_elf_set_sym_resolver(elf, symtab_resolve);
    //   esp_err_t err = esp_elf_load(elf);
    //   if (err != ESP_OK) { cleanup; return err; }
    //   result->text_base = esp_elf_get_text(elf);
    //   result->data_base = esp_elf_get_data(elf);
    //   result->elf_ctx = elf;

    // 5. Free the file read buffer (esp_elf copies what it needs)
    heap_caps_free(buf);

    ESP_LOGI(TAG, "Loaded %s: text=%p (%zuB), data=%p (%zuB)",
             path, result->text_base, result->text_size,
             result->data_base, result->data_size);
    return ESP_OK;
}

void elf_loader_unload(elf_load_result_t *result)
{
    if (!result) return;

    // ADAPT: call esp_elf cleanup function
    // esp_elf_deinit(result->elf_ctx);

    memset(result, 0, sizeof(*result));
    ESP_LOGI(TAG, "ELF unloaded");
}

void *elf_loader_find_sym(elf_load_result_t *result, const char *name)
{
    if (!result || !result->elf_ctx) return NULL;

    // ADAPT: use esp_elf symbol lookup
    // return esp_elf_find_sym(result->elf_ctx, name);
    return NULL;
}
```

The `// ADAPT` sections must be filled in based on the actual `esp_elf` API discovered in Task 1. This is the one file where the implementation depends on an external API we haven't pinned down yet.

- [ ] **Step 3: Build to verify**

Run: `pio run 2>&1 | tail -20`

Expected: `[SUCCESS]`. The pseudocode sections will need to be replaced with real `esp_elf` calls. If the build fails due to missing `esp_elf` functions, that's expected — fill them in from the API header.

- [ ] **Step 4: Commit**

```bash
git add src/loader/elf_loader.h src/loader/elf_loader.c
git commit -m "feat: ELF loader wrapper around esp_elf"
```

---

### Task 6: Module lifecycle manager

**Files:**
- Create: `src/loader/module.h`
- Create: `src/loader/module.c`

The core of Phase 2: manages the loaded module table, state machine, allocation tracking, and unload safety net.

- [ ] **Step 1: Create module.h**

Create `src/loader/module.h`:

```c
#ifndef MODULE_H
#define MODULE_H

#include "module_sdk.h"
#include "elf_loader.h"
#include "esp_err.h"

typedef enum {
    MOD_UNLOADED = 0,
    MOD_LOADED,
    MOD_VALIDATED,
    MOD_INITIALIZED,
    MOD_RUNNING,
    MOD_STOPPING,
} module_state_t;

#define MODULE_MAX             8
#define MODULE_ALLOC_TRACK_MAX 64

typedef struct {
    char              name[32];
    module_state_t    state;
    module_info_t    *info;
    elf_load_result_t elf;
    int             (*init)(void);
    int             (*start)(void);
    int             (*stop)(void);
    void             *allocs[MODULE_ALLOC_TRACK_MAX];
    int               alloc_count;
} loaded_module_t;

esp_err_t module_load(const char *elf_path);
esp_err_t module_unload(const char *name);

loaded_module_t       *module_find(const char *name);
const loaded_module_t *module_get(int index);
int                    module_count(void);

void *module_malloc(size_t size);
void  module_free(void *ptr);

const char *module_state_str(module_state_t s);

#endif
```

- [ ] **Step 2: Create module.c**

Create `src/loader/module.c`:

```c
#include "module.h"
#include "manifest.h"
#include "symtab.h"
#include "services/registry.h"
#include "services/vconsole.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "module";

static loaded_module_t s_modules[MODULE_MAX];
static int s_module_count = 0;

static loaded_module_t *s_current_loading = NULL;

const char *module_state_str(module_state_t s)
{
    switch (s) {
        case MOD_UNLOADED:    return "unloaded";
        case MOD_LOADED:      return "loaded";
        case MOD_VALIDATED:   return "validated";
        case MOD_INITIALIZED: return "initialized";
        case MOD_RUNNING:     return "running";
        case MOD_STOPPING:    return "stopping";
        default:              return "unknown";
    }
}

loaded_module_t *module_find(const char *name)
{
    for (int i = 0; i < s_module_count; i++) {
        if (s_modules[i].state != MOD_UNLOADED &&
            strcmp(s_modules[i].name, name) == 0)
            return &s_modules[i];
    }
    return NULL;
}

const loaded_module_t *module_get(int index)
{
    if (index < 0 || index >= s_module_count) return NULL;
    return &s_modules[index];
}

int module_count(void)
{
    return s_module_count;
}

static loaded_module_t *alloc_slot(void)
{
    for (int i = 0; i < MODULE_MAX; i++) {
        if (s_modules[i].state == MOD_UNLOADED) {
            if (i >= s_module_count) s_module_count = i + 1;
            return &s_modules[i];
        }
    }
    return NULL;
}

void *module_malloc(size_t size)
{
    void *ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (ptr && s_current_loading &&
        s_current_loading->alloc_count < MODULE_ALLOC_TRACK_MAX) {
        s_current_loading->allocs[s_current_loading->alloc_count++] = ptr;
    }
    return ptr;
}

void module_free(void *ptr)
{
    if (!ptr) return;
    if (s_current_loading) {
        for (int i = 0; i < s_current_loading->alloc_count; i++) {
            if (s_current_loading->allocs[i] == ptr) {
                s_current_loading->allocs[i] =
                    s_current_loading->allocs[--s_current_loading->alloc_count];
                break;
            }
        }
    }
    heap_caps_free(ptr);
}

static void free_tracked_allocs(loaded_module_t *mod)
{
    for (int i = 0; i < mod->alloc_count; i++) {
        if (mod->allocs[i]) {
            ESP_LOGW(TAG, "%s: leaked allocation at %p", mod->name, mod->allocs[i]);
            heap_caps_free(mod->allocs[i]);
            mod->allocs[i] = NULL;
        }
    }
    if (mod->alloc_count > 0)
        ESP_LOGW(TAG, "%s: freed %d leaked allocations", mod->name, mod->alloc_count);
    mod->alloc_count = 0;
}

static esp_err_t validate_module_info(loaded_module_t *mod)
{
    module_info_t *info = (module_info_t *)elf_loader_find_sym(&mod->elf, "__module_info");
    if (!info) {
        ESP_LOGE(TAG, "No __module_info symbol found");
        return ESP_ERR_NOT_FOUND;
    }

    if (info->magic != MODULE_MAGIC) {
        ESP_LOGE(TAG, "Bad magic: 0x%08lX (expected 0x%08X)",
                 (unsigned long)info->magic, MODULE_MAGIC);
        return ESP_ERR_INVALID_ARG;
    }

    if (info->abi_version != MODULE_ABI_VERSION) {
        ESP_LOGE(TAG, "%s: ABI version %lu, kernel expects %d",
                 info->name, (unsigned long)info->abi_version, MODULE_ABI_VERSION);
        return ESP_ERR_INVALID_VERSION;
    }

    if (info->requires[0] != '\0') {
        char buf[128];
        strncpy(buf, info->requires, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        char *tok = strtok(buf, ",");
        while (tok) {
            while (*tok == ' ') tok++;
            if (registry_find(tok) == NULL) {
                ESP_LOGE(TAG, "%s: requires service '%s' which is not registered",
                         info->name, tok);
                return ESP_ERR_NOT_FOUND;
            }
            tok = strtok(NULL, ",");
        }
    }

    mod->info = info;
    strncpy(mod->name, info->name, sizeof(mod->name) - 1);
    return ESP_OK;
}

static esp_err_t resolve_lifecycle(loaded_module_t *mod)
{
    mod->init  = (int (*)(void))elf_loader_find_sym(&mod->elf, "module_init");
    mod->start = (int (*)(void))elf_loader_find_sym(&mod->elf, "module_start");
    mod->stop  = (int (*)(void))elf_loader_find_sym(&mod->elf, "module_stop");

    if (!mod->init || !mod->start || !mod->stop) {
        ESP_LOGE(TAG, "%s: missing lifecycle functions (init=%p start=%p stop=%p)",
                 mod->name, mod->init, mod->start, mod->stop);
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}

esp_err_t module_load(const char *elf_path)
{
    loaded_module_t *mod = alloc_slot();
    if (!mod) {
        ESP_LOGE(TAG, "Module table full (%d max)", MODULE_MAX);
        return ESP_ERR_NO_MEM;
    }

    memset(mod, 0, sizeof(*mod));

    // Try manifest pre-check
    char ini_path[256];
    strncpy(ini_path, elf_path, sizeof(ini_path) - 5);
    ini_path[sizeof(ini_path) - 5] = '\0';
    char *dot = strrchr(ini_path, '.');
    if (dot) strcpy(dot, ".ini");

    manifest_t manifest;
    if (manifest_load(&manifest, ini_path) == ESP_OK) {
        esp_err_t compat = manifest_check_compat(&manifest);
        if (compat != ESP_OK) {
            vconsole_printf("%s: manifest check failed\n", manifest.name);
            return compat;
        }
    }

    // Load ELF
    esp_err_t err = elf_loader_load(elf_path, &mod->elf);
    if (err != ESP_OK) {
        vconsole_printf("Failed to load ELF: %s\n", elf_path);
        return err;
    }
    mod->state = MOD_LOADED;

    // Validate __module_info
    err = validate_module_info(mod);
    if (err != ESP_OK) {
        vconsole_printf("%s: validation failed\n", mod->name[0] ? mod->name : elf_path);
        elf_loader_unload(&mod->elf);
        mod->state = MOD_UNLOADED;
        return err;
    }
    mod->state = MOD_VALIDATED;

    // Check not already loaded
    for (int i = 0; i < s_module_count; i++) {
        if (&s_modules[i] != mod &&
            s_modules[i].state != MOD_UNLOADED &&
            strcmp(s_modules[i].name, mod->name) == 0) {
            ESP_LOGE(TAG, "%s: already loaded", mod->name);
            elf_loader_unload(&mod->elf);
            mod->state = MOD_UNLOADED;
            return ESP_ERR_INVALID_STATE;
        }
    }

    // Resolve lifecycle functions
    err = resolve_lifecycle(mod);
    if (err != ESP_OK) {
        elf_loader_unload(&mod->elf);
        mod->state = MOD_UNLOADED;
        return err;
    }

    // Run init
    s_current_loading = mod;
    int ret = mod->init();
    s_current_loading = NULL;
    if (ret != 0) {
        ESP_LOGE(TAG, "%s: module_init() returned %d", mod->name, ret);
        free_tracked_allocs(mod);
        elf_loader_unload(&mod->elf);
        mod->state = MOD_UNLOADED;
        return ESP_FAIL;
    }
    mod->state = MOD_INITIALIZED;

    // Run start
    s_current_loading = mod;
    ret = mod->start();
    s_current_loading = NULL;
    if (ret != 0) {
        ESP_LOGE(TAG, "%s: module_start() returned %d", mod->name, ret);
        mod->stop();
        free_tracked_allocs(mod);
        elf_loader_unload(&mod->elf);
        mod->state = MOD_UNLOADED;
        return ESP_FAIL;
    }
    mod->state = MOD_RUNNING;

    vconsole_printf("%s v%s loaded (%zuKB PSRAM)\n",
                    mod->name, mod->info->version,
                    (mod->elf.text_size + mod->elf.data_size) / 1024);
    return ESP_OK;
}

esp_err_t module_unload(const char *name)
{
    loaded_module_t *mod = module_find(name);
    if (!mod) {
        vconsole_printf("Module not found: %s\n", name);
        return ESP_ERR_NOT_FOUND;
    }

    if (mod->state == MOD_RUNNING || mod->state == MOD_INITIALIZED) {
        mod->state = MOD_STOPPING;
        vconsole_printf("Stopping %s...\n", mod->name);

        s_current_loading = mod;
        mod->stop();
        s_current_loading = NULL;

        vTaskDelay(pdMS_TO_TICKS(100));
    }

    size_t freed = mod->elf.text_size + mod->elf.data_size;

    free_tracked_allocs(mod);
    elf_loader_unload(&mod->elf);

    mod->state = MOD_UNLOADED;
    vconsole_printf("%s unloaded, freed %zuKB PSRAM\n", mod->name, freed / 1024);
    return ESP_OK;
}
```

- [ ] **Step 3: Build to verify**

Run: `pio run 2>&1 | tail -20`

Expected: `[SUCCESS]`.

- [ ] **Step 4: Commit**

```bash
git add src/loader/module.h src/loader/module.c
git commit -m "feat: module lifecycle manager with allocation tracking"
```

---

### Task 7: Shell commands (insmod, rmmod, lsmod, modinfo, modprobe)

**Files:**
- Create: `src/loader/cmd_module.h`
- Create: `src/loader/cmd_module.c`
- Modify: `src/shell/shell.c` — add include and registration call
- Modify: `src/shell/cmd_system.c` — remove old `cmd_lsmod` and `cmd_modinfo`

- [ ] **Step 1: Create cmd_module.h**

Create `src/loader/cmd_module.h`:

```c
#ifndef CMD_MODULE_H
#define CMD_MODULE_H

#include "esp_err.h"

esp_err_t cmd_module_register(void);

#endif
```

- [ ] **Step 2: Create cmd_module.c**

Create `src/loader/cmd_module.c`:

```c
#include "cmd_module.h"
#include "module.h"
#include "manifest.h"
#include "services/vconsole.h"
#include "esp_console.h"
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <dirent.h>

static int cmd_insmod(int argc, char **argv)
{
    if (argc < 2) {
        vconsole_printf("Usage: insmod <path.elf>\n");
        return 1;
    }
    esp_err_t err = module_load(argv[1]);
    return (err == ESP_OK) ? 0 : 1;
}

static int cmd_rmmod(int argc, char **argv)
{
    if (argc < 2) {
        vconsole_printf("Usage: rmmod <name>\n");
        return 1;
    }
    esp_err_t err = module_unload(argv[1]);
    return (err == ESP_OK) ? 0 : 1;
}

static int cmd_lsmod(int argc, char **argv)
{
    int n = module_count();
    int active = 0;

    vconsole_printf("MODULE           VERSION  STATE        TEXT    DATA   ALLOCS\n");
    for (int i = 0; i < n; i++) {
        const loaded_module_t *m = module_get(i);
        if (!m || m->state == MOD_UNLOADED) continue;
        vconsole_printf("%-16s %-8s %-12s %4zuKB  %4zuKB  %d\n",
                        m->name,
                        m->info ? m->info->version : "?",
                        module_state_str(m->state),
                        m->elf.text_size / 1024,
                        m->elf.data_size / 1024,
                        m->alloc_count);
        active++;
    }
    if (active == 0) vconsole_printf("(no modules loaded)\n");
    return 0;
}

static int cmd_modinfo(int argc, char **argv)
{
    if (argc < 2) {
        vconsole_printf("Usage: modinfo <path.elf | name>\n");
        return 1;
    }

    // Check if it's a loaded module name first
    loaded_module_t *mod = module_find(argv[1]);
    if (mod && mod->info) {
        vconsole_printf("Name:     %s\n", mod->info->name);
        vconsole_printf("Version:  %s\n", mod->info->version);
        vconsole_printf("ABI:      %lu\n", (unsigned long)mod->info->abi_version);
        vconsole_printf("Requires: %s\n",
                        mod->info->requires[0] ? mod->info->requires : "(none)");
        vconsole_printf("State:    %s\n", module_state_str(mod->state));
        vconsole_printf("Text:     %zuKB\n", mod->elf.text_size / 1024);
        vconsole_printf("Data:     %zuKB\n", mod->elf.data_size / 1024);
        vconsole_printf("Allocs:   %d\n", mod->alloc_count);
        return 0;
    }

    // Try as a file path — read manifest
    char ini_path[256];
    strncpy(ini_path, argv[1], sizeof(ini_path) - 5);
    ini_path[sizeof(ini_path) - 5] = '\0';
    char *dot = strrchr(ini_path, '.');
    if (dot) strcpy(dot, ".ini");

    manifest_t m;
    if (manifest_load(&m, ini_path) == ESP_OK) {
        vconsole_printf("Name:     %s\n", m.name);
        vconsole_printf("Version:  %s\n", m.version);
        vconsole_printf("ABI:      %d\n", m.abi);
        vconsole_printf("Requires: %s\n", m.requires[0] ? m.requires : "(none)");
        vconsole_printf("Desc:     %s\n", m.description);
        vconsole_printf("Author:   %s\n", m.author);
        return 0;
    }

    // Try as just a file stat
    struct stat st;
    if (stat(argv[1], &st) == 0) {
        vconsole_printf("File:     %s\n", argv[1]);
        vconsole_printf("Size:     %ld bytes\n", (long)st.st_size);
        vconsole_printf("(no manifest found)\n");
        return 0;
    }

    vconsole_printf("Module not found: %s\n", argv[1]);
    return 1;
}

static int cmd_modprobe(int argc, char **argv)
{
    if (argc < 2) {
        vconsole_printf("Usage: modprobe <name>\n");
        return 1;
    }

    // Already loaded?
    if (module_find(argv[1])) {
        vconsole_printf("%s: already loaded\n", argv[1]);
        return 0;
    }

    // Search /sdcard/drivers/<name>.elf
    char path[256];
    snprintf(path, sizeof(path), "/sdcard/drivers/%s.elf", argv[1]);
    struct stat st;
    if (stat(path, &st) != 0) {
        vconsole_printf("Module not found: %s\n", path);
        return 1;
    }

    esp_err_t err = module_load(path);
    return (err == ESP_OK) ? 0 : 1;
}

esp_err_t cmd_module_register(void)
{
    static const esp_console_cmd_t cmds[] = {
        { .command = "insmod",   .help = "Load ELF module: insmod <path.elf>",     .func = cmd_insmod },
        { .command = "rmmod",    .help = "Unload module: rmmod <name>",             .func = cmd_rmmod },
        { .command = "lsmod",    .help = "List loaded modules",                     .func = cmd_lsmod },
        { .command = "modinfo",  .help = "Module info: modinfo <path.elf | name>",  .func = cmd_modinfo },
        { .command = "modprobe", .help = "Load by name from /sdcard/drivers/",      .func = cmd_modprobe },
    };

    for (int i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        esp_err_t err = esp_console_cmd_register(&cmds[i]);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}
```

- [ ] **Step 3: Remove old lsmod and modinfo from cmd_system.c**

In `src/shell/cmd_system.c`:

1. Delete the `cmd_lsmod` function (lines 217-250).
2. Delete the `cmd_modinfo` function (lines 252-272).
3. Remove their entries from the command registration array (the lines containing `"lsmod"` and `"modinfo"`).
4. Remove any `#include <dirent.h>` or `#include <sys/stat.h>` if they become unused (check if other functions in cmd_system.c still use them — they probably do, so leave them).

- [ ] **Step 4: Wire cmd_module_register into shell.c**

In `src/shell/shell.c`:

1. Add `#include "loader/cmd_module.h"` after the existing includes.
2. Add this block after the `cmd_fstab_register()` call:

```c
    // Module loader commands
    ret = cmd_module_register();
    if (ret != ESP_OK) return ret;
```

- [ ] **Step 5: Build to verify**

Run: `pio run 2>&1 | tail -20`

Expected: `[SUCCESS]`. The command count should be 67 (was 64, removed 2 old, added 5 new: 64 - 2 + 5 = 67).

- [ ] **Step 6: Commit**

```bash
git add src/loader/cmd_module.h src/loader/cmd_module.c src/shell/shell.c src/shell/cmd_system.c
git commit -m "feat: shell commands for module management (insmod, rmmod, lsmod, modinfo, modprobe)"
```

---

### Task 8: Dummy display test module

**Files:**
- Create: `modules/dummy_display/platformio.ini`
- Create: `modules/dummy_display/src/dummy_display.c`
- Create: `modules/dummy_display/dummy_display.ini`

This is a separate PlatformIO project that produces a relocatable ELF to test the full loader pipeline.

- [ ] **Step 1: Create module directory**

```bash
mkdir -p modules/dummy_display/src
```

- [ ] **Step 2: Create platformio.ini for the module**

Create `modules/dummy_display/platformio.ini`:

```ini
[env:dummy_display]
platform = espressif32
board = freenove_esp32_s3_wroom
framework = espidf

; Build a relocatable object, not a full firmware
build_flags =
    -nostdlib
    -r
    -I../../src/loader
    -I../../src/hal

; Don't link as firmware — output is a .o / .elf relocatable
; This may need custom extra_scripts to produce the right output.
; See Step 5 for adjustments.
```

**Important:** PlatformIO is designed to produce firmware images, not relocatable objects. This config may need adjustments. The key requirement is producing an ELF with unresolved externals (symbols that the kernel loader resolves at runtime). Options:

A. Use a custom linker script that doesn't place sections at fixed addresses
B. Use `extra_scripts` to override the link step with `xtensa-esp32s3-elf-gcc -nostdlib -r -o module.elf src/*.o`
C. Use the Xtensa toolchain directly via a Makefile instead of PlatformIO

Try option A first. If it doesn't work, fall back to a simple Makefile:

```makefile
# modules/dummy_display/Makefile
TOOLCHAIN = ~/.platformio/packages/toolchain-xtensa-esp-elf/bin/xtensa-esp32s3-elf
CC = $(TOOLCHAIN)-gcc
CFLAGS = -c -Os -ffunction-sections -fdata-sections -I../../src/loader -I../../src/hal
OUTPUT = dummy_display.elf

all: $(OUTPUT)

$(OUTPUT): src/dummy_display.o
	$(TOOLCHAIN)-ld -r -o $@ $<

src/dummy_display.o: src/dummy_display.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f src/*.o $(OUTPUT)
```

- [ ] **Step 3: Create dummy_display.c**

Create `modules/dummy_display/src/dummy_display.c`:

```c
#include "module_sdk.h"

const module_info_t __module_info = {
    .magic       = MODULE_MAGIC,
    .abi_version = MODULE_ABI_VERSION,
    .name        = "dummy_display",
    .version     = "1.0.0",
    .requires    = "vconsole",
    .flags       = 0,
};

typedef struct {
    int (*init)(void);
    void (*shutdown)(void);
    void (*clear)(void);
    void (*flush)(void);
    void (*set_pixel)(int x, int y, unsigned char color);
    void (*draw_text)(int x, int y, const char *text);
    int  (*get_width)(void);
    int  (*get_height)(void);
} hal_display_ops_t;

static int  disp_init(void)    { vconsole_printf("dummy_display: init\n"); return 0; }
static void disp_shutdown(void){ vconsole_printf("dummy_display: shutdown\n"); }
static void disp_clear(void)   { vconsole_printf("dummy_display: clear\n"); }
static void disp_flush(void)   { vconsole_printf("dummy_display: flush\n"); }

static void disp_set_pixel(int x, int y, unsigned char c)
{
    vconsole_printf("dummy_display: pixel(%d,%d)=%d\n", x, y, c);
}

static void disp_draw_text(int x, int y, const char *text)
{
    vconsole_printf("dummy_display: text(%d,%d)=\"%s\"\n", x, y, text);
}

static int disp_get_width(void)  { return 128; }
static int disp_get_height(void) { return 64; }

static const hal_display_ops_t s_ops = {
    .init      = disp_init,
    .shutdown  = disp_shutdown,
    .clear     = disp_clear,
    .flush     = disp_flush,
    .set_pixel = disp_set_pixel,
    .draw_text = disp_draw_text,
    .get_width = disp_get_width,
    .get_height= disp_get_height,
};

int module_init(void)
{
    vconsole_printf("dummy_display: module_init\n");
    registry_add("display", 1, (void *)&s_ops, 0);
    return 0;
}

int module_start(void)
{
    vconsole_printf("dummy_display: started\n");
    return 0;
}

int module_stop(void)
{
    vconsole_printf("dummy_display: stopped\n");
    registry_set_state("display", 0);
    return 0;
}
```

- [ ] **Step 4: Create manifest**

Create `modules/dummy_display/dummy_display.ini`:

```ini
[module]
name=dummy_display
version=1.0.0
abi=1
description=Dummy display driver for testing module loader
author=vojtovic

[requires]
services=vconsole

[memory]
iram_hint=0
```

- [ ] **Step 5: Build the module**

Try building with the Makefile approach:

```bash
cd modules/dummy_display && make
```

If the toolchain path is wrong, find it:

```bash
find ~/.platformio -name "xtensa-esp32s3-elf-gcc" 2>/dev/null
```

And update the Makefile accordingly.

Expected output: `dummy_display.elf` file in `modules/dummy_display/`.

Verify it's a relocatable ELF:

```bash
file modules/dummy_display/dummy_display.elf
```

Expected: `ELF 32-bit LSB relocatable, Tensilica Xtensa`

- [ ] **Step 6: Commit**

```bash
git add modules/dummy_display/
git commit -m "feat: dummy display test module for loader pipeline"
```

---

### Task 9: Integration test on hardware

**Files:** No new files — this is a test-and-fix task.

- [ ] **Step 1: Build the kernel**

```bash
pio run 2>&1 | tail -20
```

Expected: `[SUCCESS]`.

- [ ] **Step 2: Flash and connect**

```bash
pio run -t upload -t monitor
```

- [ ] **Step 3: Copy test module to SD card**

Copy `modules/dummy_display/dummy_display.elf` and `modules/dummy_display/dummy_display.ini` to the SD card at `/sdcard/drivers/`.

If the ESP32 is connected and SD is mounted, you can use the shell:

```
uarch> # (copy files via card reader to SD, then reboot)
```

Or use a card reader to copy the files before inserting the SD.

- [ ] **Step 4: Test modinfo (pre-load)**

```
uarch> modinfo /sdcard/drivers/dummy_display.elf
```

Expected:
```
Name:     dummy_display
Version:  1.0.0
ABI:      1
Requires: vconsole
Desc:     Dummy display driver for testing module loader
Author:   vojtovic
```

- [ ] **Step 5: Test insmod**

```
uarch> insmod /sdcard/drivers/dummy_display.elf
```

Expected:
```
dummy_display: module_init
dummy_display: started
dummy_display v1.0.0 loaded (NKB PSRAM)
```

- [ ] **Step 6: Test lsmod**

```
uarch> lsmod
```

Expected:
```
MODULE           VERSION  STATE        TEXT    DATA   ALLOCS
dummy_display    1.0.0    running       NKB    NKB   0
```

- [ ] **Step 7: Test service registration**

```
uarch> service info display
```

Expected:
```
Name:    display
Version: 1
State:   running
VTable:  0x3FCXXXXX
```

- [ ] **Step 8: Test rmmod**

```
uarch> rmmod dummy_display
```

Expected:
```
Stopping dummy_display...
dummy_display: stopped
dummy_display unloaded, freed NKB PSRAM
```

- [ ] **Step 9: Test reload (exit criteria)**

```
uarch> insmod /sdcard/drivers/dummy_display.elf
uarch> rmmod dummy_display
uarch> insmod /sdcard/drivers/dummy_display.elf
uarch> rmmod dummy_display
```

Expected: Loads and unloads repeatedly without reboot, no memory leaks, no crashes.

- [ ] **Step 10: Test ABI rejection**

Create a bad module (or hex-edit the ABI version in dummy_display.elf) and try loading it.

Expected: Clear error message like `bad_module: ABI version X, kernel expects 1 -- rejected`.

- [ ] **Step 11: Test modprobe**

```
uarch> modprobe dummy_display
```

Expected: Searches `/sdcard/drivers/dummy_display.elf`, checks manifest, loads module. Same result as `insmod` but by name.

- [ ] **Step 12: Fix any issues found**

If any test fails, debug and fix. Common issues:
- Symbol resolution failures: add missing symbols to `s_symtab[]` in `symtab.c`
- Cache coherency: ensure `esp_psram_extram_writeback_cache()` is called after loading code to PSRAM
- Relocation errors: check `esp_elf` logs for details
- Stack overflow in module functions: increase the calling task's stack size

- [ ] **Step 13: Commit fixes**

```bash
git add -A
git commit -m "fix: integration test fixes for module loader"
```

---

### Task 10: Update roadmap and memory

**Files:**
- Modify: `roadmap.md` — check Phase 2 boxes
- Update: project memory files

- [ ] **Step 1: Update roadmap.md**

Mark all Phase 2 items as complete:

```markdown
- [x] ELF loader integration (load from SD to RAM/PSRAM).
- [x] Kernel symbol export table and resolver.
- [x] Standard module lifecycle API (`module_init`, `module_start`, `module_stop`).
- [x] ABI/version checks at load time.
- [x] Memory boundaries and relocation safety checks.
```

- [ ] **Step 2: Update project memory**

Update the memory files to reflect Phase 2 completion:
- `project_phase0_status.md` — update "Next" to Phase 3
- Create or update Phase 2 memory with implementation details
- Update `MEMORY.md` index

- [ ] **Step 3: Final build stats**

Run `pio run` and record final RAM/Flash usage for the memory files.

- [ ] **Step 4: Commit**

```bash
git add roadmap.md
git commit -m "docs: mark Phase 2 complete in roadmap"
```
