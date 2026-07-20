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

// Module-safe graphics/text ABI (framebuffer, pixel/region ops, font/text).
// Split out so loadable modules can include it without pulling in esp_err.h.
#include "gfx_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

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
