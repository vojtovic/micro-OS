# High-Performance Loadable Modules — Design Plan

**Date:** 2026-05-10
**Status:** Proposed (Phase 5 candidate)
**Goal:** Make loadable ELF modules run at near-native speed for graphics, audio, and DMA-heavy workloads — without giving up dynamic loading.

---

## Executive Summary

Today every loaded module's `.text` lives entirely in PSRAM. PSRAM-resident code is functional for shell commands, drivers, and network apps but pays a 20–40% penalty for tight inner loops (graphics blits, audio processing, image scaling). For real interactive applications — 60fps OLED UIs, audio playback with effects, animated menus — that penalty is visible.

This plan eliminates the penalty through four coordinated changes:

1. **Kernel ABI** — export 26 new symbols (`gfx_*`, `display_*`, `audio_*`, `dma_*`, `cache_*`) so modules delegate hot-path work to fast kernel code that already runs from internal flash/IRAM.
2. **IRAM placement for modules** — extend the ELF loader to honor `IRAM_ATTR` so module hot loops run from internal SRAM at full CPU speed.
3. **DMA-everywhere** — generic async DMA service so the CPU sleeps while peripherals work.
4. **Static-link escape hatch** — same source compiles in for performance-critical first-party modules; loadable for everything else.

**Achievable after implementation:**

| Use case                    | Today (PSRAM) | After plan       |
| --------------------------- | ------------- | ---------------- |
| 60fps OLED animation        | 30–40 fps     | 60+ fps, <1% CPU |
| MP3 / Opus decode           | Choppy        | Smooth, async    |
| Image scaling / blit        | 30–50% slow   | Near-native      |
| Game framebuffer composition | Sluggish      | Real-time        |

---

## 1. Kernel ABI Exports

### Header outline: `include/abi/kernel_abi.h`

```c
/* ============================================================
 * Framebuffer & graphics primitives  (prefix: gfx_, display_)
 * ============================================================ */

typedef enum { GFX_FMT_MONO_VLSB = 0, GFX_FMT_RGB565 = 1, GFX_FMT_GRAY4 = 2 } gfx_fmt_t;

typedef struct {
    uint8_t  *pixels;       /* PSRAM, 32-byte aligned, DMA-capable */
    uint16_t  w, h;
    uint16_t  stride;
    gfx_fmt_t fmt;
    uint32_t  flags;        /* GFX_FB_DOUBLE | GFX_FB_DIRTY_TRACK */
} gfx_fb_t;

/* Allocation — PSRAM-backed, DMA-aligned. NOT IRAM. ~5 us. */
gfx_fb_t *gfx_fb_alloc(uint16_t w, uint16_t h, gfx_fmt_t fmt, uint32_t flags);
void      gfx_fb_free (gfx_fb_t *fb);

/* Pixel ops — IRAM_ATTR, inlined fast path. ~6-12 cycles each. */
void     IRAM_ATTR gfx_set_pixel (gfx_fb_t *fb, int x, int y, uint32_t color);
uint32_t IRAM_ATTR gfx_get_pixel (const gfx_fb_t *fb, int x, int y);

/* Region ops — IRAM_ATTR, hand-tuned 32-bit word writes.
 * fill_rect: ~0.4 cyc/pixel; copy/blit: memcpy-bound. */
void IRAM_ATTR gfx_fill_rect (gfx_fb_t *fb, int x, int y, int w, int h, uint32_t color);
void IRAM_ATTR gfx_copy_rect (gfx_fb_t *dst, int dx, int dy,
                              const gfx_fb_t *src, int sx, int sy, int w, int h);
void IRAM_ATTR gfx_blit_masked(gfx_fb_t *dst, int dx, int dy,
                               const gfx_fb_t *src, int sx, int sy, int w, int h,
                               uint32_t transparent_color);

/* Text — bitmap font, glyph cache in IRAM. ~80 cycles/glyph for 6x8 mono. */
typedef struct gfx_font gfx_font_t;
const gfx_font_t *gfx_font_get(const char *name);   /* "6x8", "8x16" */
int IRAM_ATTR gfx_draw_char(gfx_fb_t *fb, int x, int y, char c,
                            const gfx_font_t *f, uint32_t color);
int IRAM_ATTR gfx_draw_text(gfx_fb_t *fb, int x, int y, const char *s,
                            const gfx_font_t *f, uint32_t color);

/* Display push — async SPI DMA. Callback runs in DMA ISR (must be IRAM_ATTR). */
typedef void (*display_done_cb_t)(void *user) IRAM_ATTR;

esp_err_t display_blit_async(int dev_id, const gfx_fb_t *fb,
                             int x, int y, int w, int h,
                             display_done_cb_t cb, void *user);
esp_err_t display_blit_sync (int dev_id, const gfx_fb_t *fb,
                             int x, int y, int w, int h);
esp_err_t display_wait_idle (int dev_id, TickType_t timeout);

/* ============================================================
 * Audio primitives  (prefix: audio_)
 * ============================================================ */

typedef struct audio_stream audio_stream_t;

typedef struct {
    uint32_t sample_rate;     /* Hz */
    uint8_t  bits_per_sample; /* 16, 24, 32 */
    uint8_t  channels;        /* 1 or 2 */
    uint16_t dma_buf_count;   /* default 4 */
    uint16_t dma_buf_len;     /* frames per buffer */
} audio_cfg_t;

audio_stream_t *audio_open (int dev_id, const audio_cfg_t *cfg);
void            audio_close(audio_stream_t *s);

/* Blocking write — copies into next free DMA buf. Memcpy-bound. */
size_t audio_write(audio_stream_t *s, const void *pcm, size_t bytes, TickType_t timeout);

/* Async submit — zero-copy if buf is DMA-capable. Callback in I2S ISR. */
typedef void (*audio_done_cb_t)(void *user, size_t bytes_played) IRAM_ATTR;
esp_err_t audio_submit_async(audio_stream_t *s, const void *pcm, size_t bytes,
                             audio_done_cb_t cb, void *user);

/* ESP-DSP offload — kernel wraps esp-dsp, kept in flash (cold path). */
esp_err_t audio_fft_r2_f32 (float *data, int n);
esp_err_t audio_fir_f32    (const float *in, float *out, int n,
                            const float *coeffs, int taps);
esp_err_t audio_biquad_f32 (const float *in, float *out, int n,
                            float *state, const float *coeffs);

/* ============================================================
 * Generic DMA service  (prefix: dma_, cache_)
 * ============================================================ */

typedef enum { DMA_CH_SPI2, DMA_CH_SPI3, DMA_CH_I2S0, DMA_CH_PARLIO } dma_ch_t;

typedef struct {
    dma_ch_t   channel;
    const void *src;
    void       *dst;
    size_t      len;
    uint32_t    flags;        /* DMA_F_TX | DMA_F_RX | DMA_F_INVALIDATE_AFTER */
} dma_xfer_t;

typedef void (*dma_cb_t)(void *user, esp_err_t result) IRAM_ATTR;

esp_err_t dma_submit     (const dma_xfer_t *x, dma_cb_t cb, void *user);
esp_err_t dma_submit_sync(const dma_xfer_t *x, TickType_t timeout);

/* Cache helpers — IRAM_ATTR, mandatory before/after PSRAM<->DMA transfers.
 * ~30 cycles fixed + ~2 cyc per cache line (32 B). */
void IRAM_ATTR cache_flush_range     (const void *addr, size_t len);
void IRAM_ATTR cache_invalidate_range(const void *addr, size_t len);
```

### Symbol table delta

| Group       | New symbols |
| ----------- | ----------: |
| `gfx_*`     |          11 |
| `display_*` |           3 |
| `audio_*`   |           8 |
| `dma_*`     |           2 |
| `cache_*`   |           2 |
| **Total**   |      **26** |

Existing 31 + 26 = **57 exported kernel symbols.** Hash table sized at 128 — lookup stays O(1).

### IRAM cost (hot-path subset)

| Function group                                 |  Bytes |
| ---------------------------------------------- | -----: |
| `gfx_set_pixel` / `gfx_get_pixel`              |    220 |
| `gfx_fill_rect` (word-write inner loop)        |    410 |
| `gfx_copy_rect` + `gfx_blit_masked`            |    680 |
| `gfx_draw_char` / `gfx_draw_text`              |    520 |
| `display_blit_async` ISR trampoline            |    180 |
| `audio_submit_async` ISR trampoline            |    160 |
| `dma_submit` + ISR dispatch                    |    340 |
| `cache_flush_range` / `cache_invalidate_range` |    190 |
| **Total IRAM**                                 | **~2.7 KB** |

Cold-path symbols (alloc/free, font lookup, ESP-DSP wrappers, audio_open) live in flash and add no IRAM pressure.

### Worked example — `bouncing_ball.c` at 60 fps on 128×64 OLED

```c
#include "abi/kernel_abi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#define W 128
#define H 64
#define R 6
#define OLED_DEV 0   /* registered by oled_sh1106 module */

static SemaphoreHandle_t s_blit_done;

static void IRAM_ATTR on_blit_done(void *user) {
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR(s_blit_done, &hp);
    if (hp) portYIELD_FROM_ISR();
}

void module_main(void) {
    s_blit_done = xSemaphoreCreateBinary();
    gfx_fb_t *fb = gfx_fb_alloc(W, H, GFX_FMT_MONO_VLSB, GFX_FB_DOUBLE);
    const gfx_font_t *f6 = gfx_font_get("6x8");

    int x = 20, y = 20, vx = 2, vy = 1;
    TickType_t next = xTaskGetTickCount();
    char hud[16]; uint32_t frame = 0;

    for (;;) {
        gfx_fill_rect(fb, 0, 0, W, H, 0);
        x += vx; y += vy;
        if (x < R || x > W-R-1) vx = -vx;
        if (y < R || y > H-R-1) vy = -vy;
        gfx_fill_rect(fb, x-R, y-R, 2*R, 2*R, 1);
        snprintf(hud, sizeof hud, "f%lu", (unsigned long)frame++);
        gfx_draw_text(fb, 0, 0, hud, f6, 1);
        cache_flush_range(fb->pixels, fb->stride * H);
        display_blit_async(OLED_DEV, fb, 0, 0, W, H, on_blit_done, NULL);
        xSemaphoreTake(s_blit_done, pdMS_TO_TICKS(20));
        vTaskDelayUntil(&next, pdMS_TO_TICKS(16));
    }
}
```

**Frame budget at 60 Hz (16.67 ms):**

| Stage                          |    Cost |
| ------------------------------ | ------: |
| `gfx_fill_rect` clear (1 KB)   |  ~25 µs |
| Ball + HUD draw                |  ~40 µs |
| `cache_flush_range` (1 KB)     |  ~12 µs |
| `display_blit_async` setup     |  ~20 µs |
| SPI DMA push @ 40 MHz          | ~205 µs |
| ISR + semaphore wake           |   ~8 µs |
| **CPU active per frame**       | **~105 µs (0.6%)** |
| **Wall clock per frame**       | ~310 µs |

Module logic stays under 1% CPU; DMA owns the wire.

### Design notes

- **Dual-buffer ownership.** `GFX_FB_DOUBLE` allocates two PSRAM planes; `display_blit_async` flips internally. Modules never touch the in-flight buffer.
- **Cache discipline is the module's job.** ABI exposes `cache_flush_range`/`cache_invalidate_range` rather than hiding them, so modules batch flushes (one per frame, not one per primitive).
- **ISR callbacks must be `IRAM_ATTR`.** The loader rejects modules whose registered callbacks resolve to flash/PSRAM addresses; checked at `module_load()` against the ELF symbol section.
- **No floating point in hot path.** Integer-only graphics. ESP-DSP wrappers are the only float entry points; cold-path.

Header lives at `include/abi/kernel_abi.h`; implementations split across `src/services/gfx_core.c` (IRAM), `src/services/display_mux.c` (extended), `src/services/audio_i2s.c` (new), `src/services/dma_service.c` (new).

---

## 2. Memory Architecture

### 2.1 IRAM Budget — total ~180 KB usable

| Consumer                                | Bytes | Justification |
| --------------------------------------- | ----: | ------------- |
| Kernel hot code (ISRs, gfx primitives, bus_lock, vconsole writev) | 48 KB | Currently ~32 KB; reserve headroom for SIMD blit primitives. |
| FreeRTOS internals + esp_event + panic handler | 56 KB | Measured via `idf.py size` — non-negotiable. |
| **Module IRAM pool** (carved at boot via `heap_caps_register`) | **64 KB** | Sized for ~4 concurrent modules each contributing ~16 KB of `IRAM_ATTR` (e-ink ISR + line blitter, OLED frame pump, Wi-Fi RX deferred handler). |
| DMA descriptor arena (`lldesc_t` ring)  |  8 KB | ~512 descriptors at 12B + 4B alignment slack. Enough for 3 concurrent SPI DMA chains. |
| Reserve / fragmentation slack           |  4 KB | Internal SRAM heap fragments under EXEC + DMA mixed allocs. |
| **Total**                               | **180 KB** |  |

The module IRAM pool gets a private cap bit `MALLOC_CAP_MODULE_IRAM`. Pool size is reported by `mem` and tunable via `module.iram_pool_kb` in `/sys/etc/loader.conf` (kernel re-carves on boot, no rebuild required).

### 2.2 ELF Section Placement Table

| Section                                    | Region        | `heap_caps` flags                                       |
| ------------------------------------------ | ------------- | ------------------------------------------------------- |
| `.text`, `.literal`                        | PSRAM         | `MALLOC_CAP_SPIRAM \| MALLOC_CAP_EXEC`                  |
| `.iram0.text`, `.iram1.text`, `.iram_text` | Module IRAM pool | `MALLOC_CAP_MODULE_IRAM \| MALLOC_CAP_EXEC`          |
| `.dram0.data`, `.dram_data`                | Internal DRAM | `MALLOC_CAP_INTERNAL \| MALLOC_CAP_DMA \| MALLOC_CAP_8BIT` |
| `.data`                                    | PSRAM         | `MALLOC_CAP_SPIRAM \| MALLOC_CAP_8BIT`                  |
| `.bss`, `.noinit`                          | PSRAM         | `MALLOC_CAP_SPIRAM \| MALLOC_CAP_8BIT`                  |
| `.rodata`, `.rodata.str1.*`                | PSRAM         | `MALLOC_CAP_SPIRAM`                                     |
| `.flash.text` (XIP marker, future)         | Flash (XIP)   | —                                                       |

Detection: walk `Elf32_Shdr` entries, decide by `(sh_flags & SHF_EXECINSTR) << 1 | (sh_flags & SHF_WRITE)` plus a string-prefix match on `.iram` / `.dram`. Unknown allocatable sections default to PSRAM.

### 2.3 Cache Configuration (sdkconfig.defaults)

```ini
CONFIG_ESP32S3_INSTRUCTION_CACHE_SIZE_32KB=y
CONFIG_ESP32S3_INSTRUCTION_CACHE_LINE_64B=y
CONFIG_ESP32S3_INSTRUCTION_CACHE_8WAYS=y
CONFIG_ESP32S3_DATA_CACHE_SIZE_64KB=y
CONFIG_ESP32S3_DATA_CACHE_LINE_64B=y
CONFIG_ESP32S3_DATA_CACHE_8WAYS=y
CONFIG_SPIRAM_FETCH_INSTRUCTIONS=y
CONFIG_SPIRAM_RODATA=y
CONFIG_SPIRAM_SPEED_80M=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_XIP_FROM_PSRAM=y
```

**Why these:** 32 KB I-cache + 64 KB D-cache is the maximum pairing the S3 supports without starving SRAM. 64-byte lines amortize the ~50–100 cycle PSRAM miss across 16 instructions / 16 words. 8-way associativity matters because module `.text` lives in disjoint PSRAM chunks per module — 4-way thrashes when 4+ modules execute concurrently. `XIP_FROM_PSRAM` is required for code execution from PSRAM at all.

### 2.4 DMA Buffer Helper

```c
typedef enum {
    DMA_BUF_DESC,      /* lldesc_t ring — must be in DRAM */
    DMA_BUF_PAYLOAD,   /* small (<4 KB) source/sink — DRAM */
    DMA_BUF_FRAME,     /* large frame buffer — PSRAM with manual flush */
} dma_buf_kind_t;

void *dma_buf_alloc(size_t size, dma_buf_kind_t kind);
esp_err_t dma_buf_sync_for_device(void *buf, size_t size);  /* M2C cache flush */
esp_err_t dma_buf_sync_for_cpu   (void *buf, size_t size);  /* C2M invalidate */
void dma_buf_free(void *buf);
```

`DESC` and `PAYLOAD` use `MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT`. `FRAME` uses `MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA` aligned to 64 B via `heap_caps_aligned_alloc`. The helper exists specifically to make cache flush the obvious path — forgetting it is the #1 PSRAM-DMA bug.

### 2.5 Allocation Policy: Degrade-with-warning (with opt-out)

When `MALLOC_CAP_MODULE_IRAM` is exhausted, the loader places `.iram*` sections into PSRAM-EXEC and emits:
```
W (loader): module 'wifi' iram fallback: 12 KB → PSRAM (expect 5-10x slower ISR)
```
The module's `lsmod` entry is flagged `degraded`. Authors needing hard IRAM (real-time ISRs) annotate manifest with `iram_required=true`; the loader then refuses load and names the LRU candidate to unload.

**Why this over alternatives:** **refuse** breaks user workflows on a system whose whole point is dynamic loading; **LRU eviction** is hostile because evicting a driver mid-frame corrupts ongoing DMA. Degrade-with-warning preserves liveness, surfaces the perf cost loudly, lets safety-critical modules opt into hard-fail.

### 2.6 Diagnostics

```
> mem
Region          Total      Free       Used    Largest
INTERNAL DRAM   200 KB    142 KB     58 KB    96 KB
INTERNAL IRAM   180 KB    116 KB     64 KB    52 KB
  ├ kernel              104 KB (locked)
  ├ module pool          64 KB    52 KB free
  └ dma desc arena        8 KB     6 KB free
PSRAM         8192 KB   7104 KB   1088 KB   6912 KB
Caches: I=32K/8w/64B  D=64K/8w/64B  PSRAM=oct@80MHz
```

```
> lsmod -v
NAME      STATE     PSRAM     IRAM    DRAM   FLAGS
eink      running   42.1 KB   8.2 KB  1.8 KB IRAM_OK
oled      running   18.4 KB   3.6 KB  0.6 KB IRAM_OK
wifi      running  612.0 KB  12.0 KB  4.4 KB IRAM_DEGRADED (PSRAM fallback)
cardkb    running    6.2 KB   0.0 KB  0.2 KB
                   ────────  ───────  ──────
totals             678.7 KB  23.8 KB  7.0 KB
```

`IRAM_DEGRADED` is sticky until unload — one-glance signal that perf is below spec.

---

## 3. Loader & Build System

### 3.1 ELF Loader Patch — IRAM placement

**Functions to patch:**
- `elf_loader.c` — add `static void *placement_cb(esp_elf_t*, const Elf32_Shdr*, const char *name, size_t size)`, register before `esp_elf_relocate()`. Add `elf_loader_iram_used()` accessor.
- `elf_loader.h` — extend `elf_load_result_t` with `iram_text`, `iram_text_size`, `dram_data`, `dram_data_size`, free-list so `elf_loader_unload()` releases internal-SRAM blocks via `heap_caps_free()`.
- `module_mgr.c::module_mgr_load()` — measure `MALLOC_CAP_INTERNAL|MALLOC_CAP_EXEC` free pool before/after, copy into new `loaded_module_t` fields, short-circuit with `ESP_ERR_NO_MEM_IRAM` when allocation fails.
- `cmd_module.c::cmd_lsmod_v()` — print new fields.

**Section-placement loop, after:**

```c
for (shdr in elf->shdrs) {
    if (!(shdr.flags & SHF_ALLOC)) continue;
    const char *nm = strtab + shdr.name;
    uint32_t caps; void **track;

    if (strcmp(nm, ".iram_text") == 0 || strcmp(nm, ".iram0.text") == 0) {
        caps  = MALLOC_CAP_INTERNAL | MALLOC_CAP_EXEC | MALLOC_CAP_32BIT;
        track = &result->iram_text; result->iram_text_size += shdr.size;
    } else if (strcmp(nm, ".dram_data") == 0) {
        caps  = MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT;
        track = &result->dram_data; result->dram_data_size += shdr.size;
    } else {
        caps  = MALLOC_CAP_SPIRAM;  track = NULL;
    }

    shdr.addr = heap_caps_aligned_alloc(4, shdr.size, caps);
    if (!shdr.addr) {
        ESP_LOGE(TAG, "out of %s for section %s (need %u, free %u)",
                 (caps & MALLOC_CAP_INTERNAL) ? "IRAM/DRAM" : "PSRAM",
                 nm, shdr.size, heap_caps_get_free_size(caps));
        elf_loader_unload(result);
        return (caps & MALLOC_CAP_INTERNAL) ? ESP_ERR_NO_MEM_IRAM : ESP_ERR_NO_MEM;
    }
    if (track) *track = shdr.addr;
    memcpy(shdr.addr, file_buf + shdr.offset, shdr.size);
}

/* Cache invalidate before first execution */
if (result->iram_text_size) {
    Cache_Invalidate_ICache_Items((uint32_t)result->iram_text,
                                  (result->iram_text_size + 31) / 32);
}
```

When `ESP_ERR_NO_MEM_IRAM` propagates: `module_mgr_load()` logs `"IRAM exhausted (need %u, free %u). Drop IRAM_ATTR, mark module static_link=yes, or rmmod another IRAM module."`.

### 3.2 New error codes

```c
#define ESP_ERR_NO_MEM_IRAM        (ESP_ERR_NO_MEM | 0x100)   // 0x10101
#define ESP_ERR_MOD_BAD_SECTION    0x10110  // unknown alloc-flagged section
#define ESP_ERR_MOD_CACHE_FLUSH    0x10111
#define ESP_ERR_MOD_STATIC_DUP     0x10120  // static module name collision at boot
#define ESP_ERR_MOD_STATIC_DENIED  0x10121  // .ini static_link = no
```

### 3.3 New `loaded_module_t` fields

```c
typedef enum { MOD_SRC_LOADED, MOD_SRC_STATIC } module_source_t;

typedef struct {
    /* ...existing fields... */
    module_source_t source;
    size_t          iram_used;
    size_t          dram_used;
    size_t          psram_used;
    uint64_t        load_time_us;  // 0 for STATIC
} loaded_module_t;
```

### 3.4 CMakeLists.txt — static-link wiring

```cmake
FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.*)

set(MODULES_STATIC_LIST $ENV{MODULES_STATIC})
separate_arguments(MODULES_STATIC_LIST)

set(static_module_srcs "")
set(static_module_defs "")
foreach(mod ${MODULES_STATIC_LIST})
    set(mod_dir ${CMAKE_SOURCE_DIR}/modules/${mod})
    if(NOT EXISTS ${mod_dir}/${mod}.c)
        message(FATAL_ERROR "static module '${mod}' not found at ${mod_dir}")
    endif()
    list(APPEND static_module_srcs ${mod_dir}/${mod}.c)
    list(APPEND static_module_defs "MODULE_BUILD_STATIC=1")
    message(STATUS "Static-linking module: ${mod}")
endforeach()

idf_component_register(
    SRCS         ${app_sources} ${static_module_srcs}
    INCLUDE_DIRS "loader" "services" "modules"
    REQUIRES     espressif__elf_loader)

target_compile_definitions(${COMPONENT_LIB} PRIVATE ${static_module_defs})
target_link_options(${COMPONENT_LIB} PRIVATE
    "-Wl,--undefined=__start_module_ctors"
    "-Wl,--require-defined=__start_module_ctors")
```

`platformio.ini`:

```ini
[env:static-display]
extends      = env:freenove_esp32_s3_wroom
extra_scripts = pre:scripts/export_modules_static.py   ; sets ENV{MODULES_STATIC}="oled"
```

### 3.5 `MODULE_STATIC_REGISTER` macro

```c
// modules/include/module_sdk.h
typedef void (*module_ctor_fn)(void);

#if MODULE_BUILD_STATIC
  #define MODULE_STATIC_REGISTER(NAME, EXPORTS_PTR)                        \
      static void __mod_ctor_##NAME(void) {                                \
          extern void module_register_static(const char *src,              \
                                             module_exports_t *e);        \
          module_register_static("STATIC", (EXPORTS_PTR));                 \
      }                                                                    \
      static const module_ctor_fn __attribute__((used,                     \
          section("module_ctors"))) __mod_ctor_p_##NAME = __mod_ctor_##NAME;
#else
  #define MODULE_STATIC_REGISTER(NAME, EXPORTS_PTR)  /* loaded path: noop */
#endif
```

Linker fragment (`src/linker.lf`) collects the section:

```
SECTIONS {
    .module_ctors : ALIGN(4) {
        __start_module_ctors = .;
        KEEP(*(module_ctors))
        __stop_module_ctors  = .;
    } > drom0_0_seg
}
```

Boot enumeration in `module_mgr_init()`:

```c
extern module_ctor_fn __start_module_ctors[], __stop_module_ctors[];

void module_mgr_init(void) {
    memset(s_modules, 0, sizeof(s_modules));
    for (module_ctor_fn *p = __start_module_ctors; p < __stop_module_ctors; p++) {
        s_pending_exports = NULL;
        (*p)();
        if (!s_pending_exports) continue;
        loaded_module_t *m = find_slot();
        m->exports = s_pending_exports;
        m->source  = MOD_SRC_STATIC;
        m->load_time_us = 0;
        strncpy(m->name, m->exports->info->name, sizeof(m->name)-1);
        strcpy(m->path, "<built-in>");
        m->state  = MODULE_STATE_LOADED;
        m->active = true;
    }
}
```

### 3.6 Manifest .ini extension

```ini
[module]
name        = oled
version     = 1.0.0
abi         = 1
description = SH1106 OLED driver over SPI
static_link = yes              ; allow inclusion via MODULES_STATIC

[memory]
iram_pages  = 2                ; advisory: 2 * 4 KB IRAM expected
dram_pages  = 1                ; for DMA framebuffer

[requires]
services    = vconsole, bus_spi
kernel_deps = bus_, gpio_, esp_log_   ; symbol-prefix list, checked vs symtab
```

`manifest_check_compat()` rejects modules whose `kernel_deps` prefixes aren't present in `symtab.c`; `module_mgr_load()` refuses `static_link=no` modules when `MODULES_STATIC` includes them.

### 3.7 Workflows

**Dev — loaded module (fast iteration):**
```bash
cd modules/ && make oled.elf
sha256sum oled.elf | cut -d' ' -f1 > oled.elf.sha256
cp oled.elf oled.elf.sha256 oled.ini /run/media/$USER/SDCARD/lib/modules/
# device:
> insmod /sdcard/lib/modules/oled.elf
> modstart oled
> lsmod -v       # oled  LOADED  iram=4096  dram=1024  psram=12288
```

**Production — static module (zero loader cost):**
```bash
MODULES_STATIC="oled audio_pcm" pio run -e static-display
pio run -e static-display -t upload
# device:
> lsmod
oled       STATIC  v1.0.0  0us
audio_pcm  STATIC  v0.3.1  0us
hello      LOADED  v1.0.0  812us
```

Same `oled.c` source compiles in both modes.

---

## 4. Implementation Phases

Strictly ordered — each phase enables the next. Estimated effort per developer-day.

### Phase 5.1 — Memory plumbing (foundation, ~3 days)
1. Add `MALLOC_CAP_MODULE_IRAM` private cap bit, register pool at boot
2. Update sdkconfig.defaults with cache config
3. Implement `dma_buf_alloc` / `dma_buf_sync_*` in new `src/services/dma_service.c`
4. Extend `cmd_mem` to show region breakdown
5. **Exit gate:** `mem` shows new layout; build clean.

### Phase 5.2 — ELF loader IRAM placement (~3 days)
1. Add placement callback to `elf_loader.c`
2. Extend `elf_load_result_t`, `loaded_module_t`
3. New error codes; degrade-with-warning fallback
4. Extend `cmd_lsmod_v` with IRAM/DRAM/flags columns
5. Test: build a hello module with one `IRAM_ATTR` function, verify it lands in IRAM via `lsmod -v` and disassembly
6. **Exit gate:** loader places sections correctly; degraded-fallback log appears when pool is full.

### Phase 5.3 — Kernel ABI: graphics + DMA (~5 days)
1. New `include/abi/kernel_abi.h`
2. Implement `gfx_*` primitives in `src/services/gfx_core.c` (IRAM)
3. Implement `display_blit_async` in `display_mux.c` (extends existing)
4. Implement generic `dma_submit` / `dma_submit_sync`
5. Add 26 new symbols to symbol table
6. Port the existing `oled` and `eink` modules to use the new ABI
7. **Exit gate:** `bouncing_ball` example module hits 60 fps on real hardware, CPU < 5%.

### Phase 5.4 — Kernel ABI: audio (~4 days)
1. Implement `audio_open` / `audio_write` / `audio_submit_async` (I2S DMA)
2. Wrap ESP-DSP for `audio_fft_r2_f32`, `audio_fir_f32`, `audio_biquad_f32`
3. Build a sample `tone_gen` module that streams a sine wave
4. **Exit gate:** PCM playback at 44.1 kHz, no glitches over 5-minute test.

### Phase 5.5 — Static-link build system (~3 days)
1. Add `MODULE_STATIC_REGISTER` macro + linker fragment
2. CMakeLists.txt loop over `MODULES_STATIC`
3. New `[env:static-*]` PlatformIO envs
4. Extend `module_mgr_init` to enumerate `__start_module_ctors`
5. **Exit gate:** `MODULES_STATIC="oled" pio run` produces a single firmware where `lsmod` shows oled as `STATIC`.

### Phase 5.6 — Manifest extensions + diagnostics polish (~2 days)
1. Parse new `[memory]` and `[requires]` sections in manifest
2. `manifest_check_compat` validates `kernel_deps` against symbol table
3. Polish `mem` and `lsmod -v` output
4. Update package manager (`pkg`) to read new manifest fields, print IRAM warnings on install
5. **Exit gate:** `pkg install` of an IRAM-heavy module shows the warning before downloading.

**Total estimate: ~20 developer-days.** Phases 5.1 and 5.2 are blocking; 5.3/5.4/5.5 can partially overlap if multiple developers; 5.6 is final polish.

---

## 5. Performance Targets / Validation

Build a `bench` shell command that runs micro-benchmarks and prints CPU cycles. Target results:

| Benchmark                                  | Today (PSRAM) | After plan | Target ratio |
| ------------------------------------------ | ------------: | ---------: | -----------: |
| `gfx_fill_rect` 128×64 mono                | ~50 µs        | ~25 µs     | ≥ 1.8× |
| `gfx_draw_text` 16-char string             | ~180 µs       | ~80 µs     | ≥ 2.0× |
| OLED full-frame redraw, async              | 25–30 fps     | 60+ fps    | ≥ 2.0× |
| Audio 44.1 kHz 16-bit stereo, no underrun  | inconsistent  | sustained  | pass/fail |
| Module load time (10 KB ELF)               | ~250 ms       | ~270 ms    | ≤ 1.1× (small regression OK) |
| `lsmod -v` accuracy (IRAM bytes vs `.elf`) | n/a           | exact      | exact match |

Benchmark must run in the existing shell with `bench gfx`, `bench audio`, `bench loader`. Results dumped via `vconsole_printf` to keep it scriptable.

---

## 6. Risks & Mitigations

| Risk                                                 | Likelihood | Mitigation                                                       |
| ---------------------------------------------------- | ---------- | ---------------------------------------------------------------- |
| Cache-config change destabilizes existing modules    | Medium     | Phase 5.1 includes regression run of all existing module tests; revert cache changes if any module breaks. |
| IRAM pool exhaustion makes apps fail confusingly     | High       | Degrade-with-warning + `lsmod -v` `IRAM_DEGRADED` flag + `pkg install` warning before install. |
| Static-link constructor section gets GC'd by linker  | Medium     | `--require-defined=__start_module_ctors` linker option; CI smoke test that builds `static-display` env. |
| ESP-IDF version bump invalidates `lldesc_t` layout   | Low        | Wrap all DMA descriptor mgmt in `dma_service.c`; one place to fix. |
| Symbol table growth (31 → 57) bloats kernel size     | Low        | Hash table already sized for 128; symbols are pointers (4 B each) + 32-byte name. ~2 KB added to .rodata. |
| `IRAM_ATTR` callbacks landing in PSRAM cause crash   | High       | Loader rejects at load-time by checking ELF symbol section addresses against IRAM/flash range. |

---

## 7. Out of Scope

Explicitly deferred — listed so they don't get re-litigated:

- Real-time video (>10 fps full-frame on color displays) — needs dedicated MIPI-DSI hardware we don't have.
- Hardware JPEG decoder integration — possible later phase, not required for current targets.
- Module signing / authenticated execution — separate security phase.
- Hot-swap of static modules — the whole point of static is they ship with the kernel; swap = reflash.
- Cross-architecture modules (Xtensa LX6 vs LX7) — single-target by design.

---

## Files Touched (Summary)

**New:**
- `include/abi/kernel_abi.h`
- `src/services/gfx_core.c` + `.h`
- `src/services/audio_i2s.c` + `.h`
- `src/services/dma_service.c` + `.h`
- `modules/include/module_sdk.h`
- `src/linker.lf`
- `scripts/export_modules_static.py`
- `modules/bouncing_ball/` (example)

**Modified:**
- `src/loader/elf_loader.c` + `.h` — placement callback, free-list
- `src/loader/module_mgr.c` + `.h` — new fields, static enumeration
- `src/loader/symtab.c` — add 26 new symbols
- `src/loader/module_types.h` — new error codes
- `src/loader/manifest.h` + `.c` — `[memory]`, `[requires]` sections
- `src/services/display_mux.c` — `display_blit_async`
- `src/shell/cmd_module.c` — extended `lsmod -v`
- `src/shell/shell.c` (`cmd_mem`) — region breakdown
- `src/services/pkg_manager.c` — IRAM-warning on install
- `src/CMakeLists.txt` — `MODULES_STATIC` loop
- `platformio.ini` — `[env:static-*]`
- `sdkconfig.defaults` — cache config
- `roadmap.md` — Phase 5 entry
