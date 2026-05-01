# Phase 2: ELF Module Runtime — Design Specification

Date: 2026-04-30
Status: Approved
Phase: 2 of roadmap.md

## Overview

Runtime ELF module loading for ESP32-S3, enabling drivers and apps to load from SD card without reflashing. The kernel stays lean; peripherals (display, Wi-Fi, apps) are loadable `.elf` files.

## Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Code placement | IRAM for hot paths, PSRAM for the rest | Professional performance split; PSRAM has ~200ns latency vs ~10ns IRAM |
| Module declaration | Magic struct in ELF + optional .ini manifest | Self-contained ELF for runtime validation, manifest for fast pre-load checks |
| File format | Bare .elf + .ini files | Package format (.mpk) deferred to Phase 4 |
| Unload strategy | Cooperative with PSRAM allocation safety net | Module does cleanup, loader catches leaks |
| ELF parser | esp_elf component + custom everything else | Espressif handles Xtensa relocations, we own the architecture |
| Test module | Dummy display implementing hal_display_ops_t | Exercises full pipeline without specific hardware |

## Source Layout

```
src/loader/
  elf_loader.c/h   — wraps esp_elf, loads ELF from SD into PSRAM/IRAM, resolves symbols
  symtab.c/h       — kernel symbol export table, sorted array, binary search lookup
  module.c/h       — module lifecycle state machine, memory tracking, loaded module table
  manifest.c/h     — INI manifest parser (reuses config.c), pre-load compatibility checks
  cmd_module.c/h   — shell commands: insmod, rmmod, lsmod, modinfo, modprobe
```

## Kernel Symbol Table

Modules call kernel functions through a symbol table. Undefined symbols in the module ELF are resolved against this table at load time.

```c
typedef struct {
    const char *name;
    void       *addr;
} kernel_sym_t;

#define KSYM(fn) { #fn, (void *)&fn }

static const kernel_sym_t s_symtab[] = {
    KSYM(bus_spi_init),
    KSYM(registry_add),
    KSYM(registry_find),
    KSYM(registry_set_state),
    KSYM(vconsole_printf),
    KSYM(vconsole_write),
    KSYM(vconsole_putchar),
    KSYM(heap_caps_malloc),
    KSYM(heap_caps_free),
    KSYM(xTaskCreate),
    KSYM(vTaskDelete),
    KSYM(vTaskDelay),
    KSYM(xSemaphoreCreateMutex),
    KSYM(xSemaphoreTake),
    KSYM(xSemaphoreGive),
    KSYM(xEventGroupCreate),
    KSYM(xEventGroupSetBits),
    KSYM(xEventGroupWaitBits),
    KSYM(gpio_config),
    KSYM(gpio_set_level),
    KSYM(gpio_get_level),
    KSYM(esp_log_write),
    KSYM(memcpy),
    KSYM(memset),
    KSYM(strlen),
    KSYM(snprintf),
    // ~40-60 symbols total, grows as modules need more APIs
};
```

Lookup is binary search on the sorted array — O(log n) for ~50 entries.

Adding a new exported symbol is a one-line `KSYM()` entry.

## Module Metadata

### Magic struct (in every module ELF)

```c
#define MODULE_ABI_VERSION  1
#define MODULE_MAGIC        0x4D4F4455  // "MODU"

#define MODULE_FLAG_IRAM_HOT  (1 << 0)  // request .iram.text placement in IRAM

typedef struct {
    uint32_t    magic;
    uint32_t    abi_version;
    char        name[32];
    char        version[16];
    char        requires[128];  // comma-separated service names
    uint32_t    flags;
} module_info_t;
```

Every module defines this at symbol `__module_info`.

### Companion manifest (optional .ini)

```ini
# /sdcard/drivers/oled_driver.ini
[module]
name=oled_driver
version=1.0.0
abi=1
description=SH1106 OLED display driver
author=vojtovic

[requires]
services=bus,vconsole

[memory]
iram_hint=0
```

The loader checks the manifest first (if present) for fast rejection before loading the ELF.

## Module Lifecycle

### States

```
UNLOADED -> LOADED -> VALIDATED -> INITIALIZED -> RUNNING -> STOPPING -> UNLOADED
```

### Exported lifecycle functions

Every module must export these three symbols:

```c
int module_init(void);   // allocate resources, register services
int module_start(void);  // begin operation, start tasks
int module_stop(void);   // stop tasks, deregister services, free resources
```

### Loaded module tracking

```c
#define MODULE_MAX  8
#define MODULE_ALLOC_TRACK_MAX  64

typedef struct {
    char             name[32];
    module_state_t   state;
    module_info_t   *info;
    void            *text_base;
    void            *data_base;
    size_t           text_size;
    size_t           data_size;
    int            (*init)(void);
    int            (*start)(void);
    int            (*stop)(void);
    void            *allocs[MODULE_ALLOC_TRACK_MAX];
    int              alloc_count;
} loaded_module_t;
```

### Memory safety net

`module_malloc(size)` and `module_free(ptr)` wrappers record allocations in the module's `allocs[]` array. On unload, after `module_stop()` returns, remaining tracked allocations are freed with a warning log.

Modules may also use raw `heap_caps_malloc` — those are not tracked.

## ELF Loader (esp_elf Integration)

### Load flow

1. Open `.elf` from SD via `fopen()`.
2. Read ELF header, validate ELF32 / Xtensa / little-endian.
3. Pass to `esp_elf` for section loading and relocation, providing `symtab_resolve()` as the symbol lookup callback.
4. `esp_elf` loads `.text` into executable memory, `.data`/`.bss`/`.rodata` into PSRAM.
5. Call `esp_psram_extram_writeback_cache()` to flush cache and make PSRAM-loaded code executable.
6. Return load result with base addresses and function pointers.

### IRAM hot path support

- If `module_info_t.flags` has `MODULE_FLAG_IRAM_HOT`, the loader looks for a `.iram.text` section.
- That section is allocated in IRAM: `heap_caps_malloc(size, MALLOC_CAP_EXEC | MALLOC_CAP_INTERNAL)`.
- Remaining code stays in PSRAM.
- If IRAM allocation fails, fall back to all-PSRAM with a warning log.
- Modules mark hot functions: `__attribute__((section(".iram.text")))`.

### Symbol resolution callback

```c
void *symtab_resolve(const char *name) {
    // binary search kernel symbol table
    // returns address or NULL (triggers "unresolved symbol: <name>" error)
}
```

## Unload Sequence

1. Call `module_stop()`.
2. Wait up to 2 seconds for module's FreeRTOS tasks to exit.
3. Free remaining tracked PSRAM allocations (safety net, log warnings for each leak).
4. Free ELF text/data memory.
5. Remove from loaded modules table.
6. Log: "module_name unloaded, freed NKB PSRAM".

## Shell Commands

| Command | Usage | Description |
|---------|-------|-------------|
| `insmod` | `insmod <path.elf>` | Load, validate, init, and start a module |
| `rmmod` | `rmmod <name>` | Stop and unload a module by name |
| `lsmod` | `lsmod` | List loaded modules with state and memory usage |
| `modinfo` | `modinfo <path.elf>` | Show module metadata without loading |
| `modprobe` | `modprobe <name>` | Search /sdcard/drivers/ by name, check manifest, load |

### Example output

```
uarch> lsmod
MODULE           VERSION  STATE     TEXT    DATA   ALLOCS
oled_driver      1.0.0    running   12KB   4KB    3

uarch> modinfo /sdcard/drivers/oled_driver.elf
Name:     oled_driver
Version:  1.0.0
ABI:      1
Requires: bus, vconsole
Desc:     SH1106 OLED display driver
Author:   vojtovic

uarch> rmmod oled_driver
Stopping oled_driver...
oled_driver: deregistered service 'display_oled'
oled_driver unloaded, freed 16KB PSRAM
```

### Error examples

```
uarch> insmod /sdcard/drivers/bad_module.elf
bad_module: ABI version 3, kernel expects 1 -- rejected

uarch> insmod /sdcard/drivers/wifi.elf
wifi: requires service 'net_stack' which is not registered -- rejected

uarch> rmmod oled_driver
oled_driver: WARNING: leaked 2048 bytes at 0x3FC80000
oled_driver unloaded, freed 18KB PSRAM
```

## Test Module: Dummy Display

Built as a separate PlatformIO project under `modules/dummy_display/`.

Implements all 8 functions in `hal_display_ops_t`, logging each call to vconsole instead of driving hardware. Registers as service `"display"` in the registry. Tests the full pipeline: symbol resolution, HAL vtable, service registration, load/unload cycle.

```
modules/
  dummy_display/
    platformio.ini
    src/dummy_display.c
    dummy_display.ini
```

### Module SDK header

`src/loader/module_sdk.h` — included by all modules. Provides `module_info_t`, `MODULE_ABI_VERSION`, `module_malloc`/`module_free`, and declares all kernel-exported symbols.

## Build Impact Estimate

- Kernel: +15-20KB flash (esp_elf component + loader code), +1KB RAM (loaded module table)
- Per module: varies, dummy display ~8-12KB PSRAM

## Exit Criteria (from roadmap)

- A sample module loads, runs, and unloads repeatedly without reboot.
- Incompatible ABI modules are rejected with clear error messages.

## Existing Command Conflicts

The current `modinfo` and `lsmod` in `cmd_system.c` show hardcoded built-in info. During Phase 2 implementation:
- Remove `cmd_modinfo` and `cmd_lsmod` from `cmd_system.c`.
- Replace with dynamic versions in `cmd_module.c` that show loaded modules.
- Built-in services are visible via `service list` (already exists).

## Module Build Setup

Modules are built as separate PlatformIO projects targeting the same ESP32-S3 board. Key linker requirements:
- Output must be a relocatable ELF (not a fully linked firmware image).
- Use `-nostdlib -r` or equivalent to produce a relocatable object with unresolved externals.
- The module's `platformio.ini` must use the same Xtensa toolchain as the kernel.
- The `module_sdk.h` header is shared via a symlink or include path pointing to the kernel's `src/loader/module_sdk.h`.
- Standard C library functions (memcpy, strlen, etc.) resolve through the kernel symbol table at load time, not through static linking.

Exact `platformio.ini` and linker script for modules will be defined during implementation.
