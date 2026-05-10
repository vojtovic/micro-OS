#ifndef KERNEL_ABI_H
#define KERNEL_ABI_H

// Kernel ABI exported to loadable modules.
//
// Phase 5.3a: framebuffer + pixel/region primitives + cache coherency helpers.
//   - gfx_*   : framebuffer alloc + integer pixel/region ops (IRAM-resident
//               in the kernel; fast even when called from a PSRAM module)
//   - cache_* : explicit cache flush/invalidate; required around PSRAM
//               framebuffer <-> SPI/I2S DMA hand-offs
//   - dma_buf_*: typed DMA buffer alloc; see services/dma_service.h
//
// Deferred to 5.3b: gfx_draw_text + font system, display_blit_async, generic
// dma_submit. The DMA descriptor service and font/text rendering are larger
// efforts that need the display drivers ported first to validate the API.

#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * Framebuffer & graphics primitives
 * ============================================================ */

typedef enum {
    GFX_FMT_MONO_VLSB = 0,  /* SH1106/SSD1306 native: 1-bit, vertical LSB */
    GFX_FMT_RGB565    = 1,  /* 16-bit color */
    GFX_FMT_GRAY4     = 2,  /* 4-bit grayscale (e-ink) */
} gfx_fmt_t;

#define GFX_FB_DOUBLE        (1u << 0)  /* future: ping-pong buffers */
#define GFX_FB_DIRTY_TRACK   (1u << 1)  /* future: dirty-rect tracking */

typedef struct {
    uint8_t  *pixels;   /* PSRAM, cache-line aligned, DMA-capable */
    uint16_t  w, h;
    uint16_t  stride;   /* bytes per row */
    gfx_fmt_t fmt;
    uint32_t  flags;
    size_t    bytes;    /* total pixels buffer size */
} gfx_fb_t;

/* Allocate a framebuffer with DMA-capable PSRAM backing. Returns NULL on OOM. */
gfx_fb_t *gfx_fb_alloc(uint16_t w, uint16_t h, gfx_fmt_t fmt, uint32_t flags);
void      gfx_fb_free (gfx_fb_t *fb);

/* Pixel-level — bounds-checked, kernel-side. */
void     gfx_set_pixel(gfx_fb_t *fb, int x, int y, uint32_t color);
uint32_t gfx_get_pixel(const gfx_fb_t *fb, int x, int y);

/* Region — clipped to fb bounds. Word-aligned inner loops where format permits. */
void gfx_fill_rect (gfx_fb_t *fb, int x, int y, int w, int h, uint32_t color);
void gfx_copy_rect (gfx_fb_t *dst, int dx, int dy,
                    const gfx_fb_t *src, int sx, int sy, int w, int h);
void gfx_blit_masked(gfx_fb_t *dst, int dx, int dy,
                     const gfx_fb_t *src, int sx, int sy, int w, int h,
                     uint32_t transparent_color);

/* ============================================================
 * Cache coherency — required for PSRAM <-> DMA hand-offs
 * ============================================================ */

/* Writeback dirty CPU cache lines so DMA reads see latest CPU writes. */
esp_err_t cache_flush_range     (void *addr, size_t len);
/* Invalidate cache lines so next CPU read pulls from PSRAM (post-DMA). */
esp_err_t cache_invalidate_range(void *addr, size_t len);

#ifdef __cplusplus
}
#endif

#endif
