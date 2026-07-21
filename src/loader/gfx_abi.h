#ifndef GFX_ABI_H
#define GFX_ABI_H

// Module-safe graphics/text ABI.
//
// This header is deliberately free of ESP-IDF dependencies (only <stdint.h>/
// <stddef.h>) so loadable modules can include it directly to draw text/graphics
// via the kernel. Kernel-only pieces that need esp_err_t (cache_*, dma_*) stay
// in kernel_abi.h, which includes this file.

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * Framebuffer & graphics primitives
 * ============================================================ */

typedef enum {
    GFX_FMT_MONO_VLSB = 0,  /* SH1106/SSD1306 native: 1-bit, vertical LSB (page) */
    GFX_FMT_RGB565    = 1,  /* 16-bit color */
    GFX_FMT_GRAY4     = 2,  /* 4-bit grayscale (e-ink) */
    GFX_FMT_MONO_HMSB = 3,  /* 1-bit, horizontal MSB-first (e-ink 3.52" native) */
} gfx_fmt_t;

#define GFX_FB_DOUBLE        (1u << 0)  /* future: ping-pong buffers */
#define GFX_FB_DIRTY_TRACK   (1u << 1)  /* future: dirty-rect tracking */

typedef struct gfx_fb {
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
 * Bitmap text — format-agnostic (draws via gfx_set_pixel)
 * ============================================================ */

typedef struct gfx_font gfx_font_t;   /* opaque */

/* Look up a built-in font by name ("5x7"/"6x8"); never NULL (falls back). */
const gfx_font_t *gfx_font_get(const char *name);

/* Draw one glyph; returns the horizontal advance in pixels. */
int gfx_draw_char(gfx_fb_t *fb, int x, int y, char c,
                  const gfx_font_t *f, uint32_t color);

/* Draw a string ('\n' starts a new line); returns width of the last line. */
int gfx_draw_text(gfx_fb_t *fb, int x, int y, const char *s,
                  const gfx_font_t *f, uint32_t color);

#ifdef __cplusplus
}
#endif

#endif /* GFX_ABI_H */
