#ifndef DMA_SERVICE_H
#define DMA_SERVICE_H

#include "esp_err.h"
#include <stddef.h>

// DMA buffer allocator — picks correct heap caps based on intended use, and
// exposes cache flush / invalidate helpers so PSRAM-backed DMA buffers stay
// coherent. Required pattern for PSRAM-resident frame buffers:
//
//   uint8_t *fb = dma_buf_alloc(FB_BYTES, DMA_BUF_FRAME);
//   render_into(fb);
//   dma_buf_sync_for_device(fb, FB_BYTES);     // before SPI/I2S DMA reads
//   ... DMA runs ...
//   dma_buf_sync_for_cpu(fb, FB_BYTES);        // before CPU re-reads
//
// Forgetting the flush is the #1 PSRAM-DMA bug; using this helper makes it
// the obvious path.

typedef enum {
    DMA_BUF_DESC,    // lldesc_t / GDMA descriptor ring — must be in DRAM
    DMA_BUF_PAYLOAD, // small (< 4 KB) source/sink — DRAM
    DMA_BUF_FRAME,   // large frame buffer — PSRAM with manual flush
} dma_buf_kind_t;

void     *dma_buf_alloc           (size_t size, dma_buf_kind_t kind);
esp_err_t dma_buf_sync_for_device (void *buf, size_t size);
esp_err_t dma_buf_sync_for_cpu    (void *buf, size_t size);
void      dma_buf_free            (void *buf);

#endif
