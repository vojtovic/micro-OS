#include "loader/kernel_abi.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "gfx_core";

#define DMA_ALIGN 64

static size_t fmt_stride(uint16_t w, gfx_fmt_t fmt)
{
    switch (fmt) {
    case GFX_FMT_MONO_VLSB: return ((w + 7) / 8);
    case GFX_FMT_RGB565:    return w * 2;
    case GFX_FMT_GRAY4:     return (w + 1) / 2;
    default:                return 0;
    }
}

static size_t fmt_bytes(uint16_t w, uint16_t h, gfx_fmt_t fmt)
{
    switch (fmt) {
    case GFX_FMT_MONO_VLSB: return ((h + 7) / 8) * w;  /* page-stride */
    case GFX_FMT_RGB565:    return (size_t)w * h * 2;
    case GFX_FMT_GRAY4:     return ((w + 1) / 2) * h;
    default:                return 0;
    }
}

gfx_fb_t *gfx_fb_alloc(uint16_t w, uint16_t h, gfx_fmt_t fmt, uint32_t flags)
{
    if (w == 0 || h == 0) return NULL;

    size_t bytes  = fmt_bytes(w, h, fmt);
    size_t stride = fmt_stride(w, fmt);
    if (bytes == 0) {
        ESP_LOGE(TAG, "unknown fmt %d", (int)fmt);
        return NULL;
    }

    gfx_fb_t *fb = heap_caps_malloc(sizeof(*fb), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!fb) return NULL;

    fb->pixels = heap_caps_aligned_alloc(DMA_ALIGN, bytes,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    if (!fb->pixels) {
        heap_caps_free(fb);
        ESP_LOGE(TAG, "OOM for %u-byte framebuffer", (unsigned)bytes);
        return NULL;
    }
    memset(fb->pixels, 0, bytes);

    fb->w = w; fb->h = h;
    fb->stride = stride;
    fb->fmt = fmt;
    fb->flags = flags;
    fb->bytes = bytes;
    return fb;
}

void gfx_fb_free(gfx_fb_t *fb)
{
    if (!fb) return;
    if (fb->pixels) heap_caps_free(fb->pixels);
    heap_caps_free(fb);
}

/* ─────────────── pixel ops ─────────────── */

static inline bool in_bounds(const gfx_fb_t *fb, int x, int y)
{
    return (unsigned)x < fb->w && (unsigned)y < fb->h;
}

void gfx_set_pixel(gfx_fb_t *fb, int x, int y, uint32_t color)
{
    if (!fb || !in_bounds(fb, x, y)) return;

    switch (fb->fmt) {
    case GFX_FMT_MONO_VLSB: {
        uint8_t *p = &fb->pixels[(y / 8) * fb->w + x];
        uint8_t  m = 1u << (y & 7);
        if (color) *p |= m; else *p &= ~m;
        break;
    }
    case GFX_FMT_RGB565: {
        uint16_t *p = (uint16_t *)(fb->pixels + y * fb->stride + x * 2);
        *p = (uint16_t)color;
        break;
    }
    case GFX_FMT_GRAY4: {
        uint8_t *p = &fb->pixels[y * fb->stride + (x >> 1)];
        uint8_t  shift = (x & 1) ? 0 : 4;
        *p = (*p & ~(0xF << shift)) | ((color & 0xF) << shift);
        break;
    }
    }
}

uint32_t gfx_get_pixel(const gfx_fb_t *fb, int x, int y)
{
    if (!fb || !in_bounds(fb, x, y)) return 0;

    switch (fb->fmt) {
    case GFX_FMT_MONO_VLSB:
        return (fb->pixels[(y / 8) * fb->w + x] >> (y & 7)) & 1;
    case GFX_FMT_RGB565:
        return *(uint16_t *)(fb->pixels + y * fb->stride + x * 2);
    case GFX_FMT_GRAY4: {
        uint8_t v = fb->pixels[y * fb->stride + (x >> 1)];
        return (x & 1) ? (v & 0xF) : ((v >> 4) & 0xF);
    }
    }
    return 0;
}

/* ─────────────── region ops ─────────────── */

static void clip(int *x, int *y, int *w, int *h, int max_w, int max_h)
{
    if (*x < 0)        { *w += *x; *x = 0; }
    if (*y < 0)        { *h += *y; *y = 0; }
    if (*x + *w > max_w) *w = max_w - *x;
    if (*y + *h > max_h) *h = max_h - *y;
}

void gfx_fill_rect(gfx_fb_t *fb, int x, int y, int w, int h, uint32_t color)
{
    if (!fb) return;
    clip(&x, &y, &w, &h, fb->w, fb->h);
    if (w <= 0 || h <= 0) return;

    if (fb->fmt == GFX_FMT_RGB565) {
        uint16_t c = (uint16_t)color;
        for (int row = y; row < y + h; row++) {
            uint16_t *p = (uint16_t *)(fb->pixels + row * fb->stride) + x;
            for (int col = 0; col < w; col++) p[col] = c;
        }
        return;
    }
    /* Generic per-pixel for mono/gray4 — slower but bit-correct. */
    for (int row = y; row < y + h; row++)
        for (int col = x; col < x + w; col++)
            gfx_set_pixel(fb, col, row, color);
}

void gfx_copy_rect(gfx_fb_t *dst, int dx, int dy,
                   const gfx_fb_t *src, int sx, int sy, int w, int h)
{
    if (!dst || !src || dst->fmt != src->fmt) return;
    /* Clip both surfaces. */
    if (sx < 0) { dx -= sx; w += sx; sx = 0; }
    if (sy < 0) { dy -= sy; h += sy; sy = 0; }
    if (sx + w > src->w) w = src->w - sx;
    if (sy + h > src->h) h = src->h - sy;
    if (dx < 0) { sx -= dx; w += dx; dx = 0; }
    if (dy < 0) { sy -= dy; h += dy; dy = 0; }
    if (dx + w > dst->w) w = dst->w - dx;
    if (dy + h > dst->h) h = dst->h - dy;
    if (w <= 0 || h <= 0) return;

    if (dst->fmt == GFX_FMT_RGB565) {
        for (int row = 0; row < h; row++) {
            const uint16_t *s = (const uint16_t *)(src->pixels + (sy + row) * src->stride) + sx;
            uint16_t       *d = (uint16_t *)(dst->pixels + (dy + row) * dst->stride) + dx;
            memcpy(d, s, (size_t)w * 2);
        }
        return;
    }
    for (int row = 0; row < h; row++)
        for (int col = 0; col < w; col++)
            gfx_set_pixel(dst, dx + col, dy + row,
                          gfx_get_pixel(src, sx + col, sy + row));
}

void gfx_blit_masked(gfx_fb_t *dst, int dx, int dy,
                     const gfx_fb_t *src, int sx, int sy, int w, int h,
                     uint32_t transparent_color)
{
    if (!dst || !src || dst->fmt != src->fmt) return;
    /* Same clipping as copy_rect. */
    if (sx < 0) { dx -= sx; w += sx; sx = 0; }
    if (sy < 0) { dy -= sy; h += sy; sy = 0; }
    if (sx + w > src->w) w = src->w - sx;
    if (sy + h > src->h) h = src->h - sy;
    if (dx < 0) { sx -= dx; w += dx; dx = 0; }
    if (dy < 0) { sy -= dy; h += dy; dy = 0; }
    if (dx + w > dst->w) w = dst->w - dx;
    if (dy + h > dst->h) h = dst->h - dy;
    if (w <= 0 || h <= 0) return;

    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) {
            uint32_t c = gfx_get_pixel(src, sx + col, sy + row);
            if (c != transparent_color)
                gfx_set_pixel(dst, dx + col, dy + row, c);
        }
    }
}
