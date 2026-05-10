#include "services/dma_service.h"
#include "esp_heap_caps.h"
#include "esp_cache.h"
#include "esp_log.h"

static const char *TAG = "dma_service";

#define CACHE_LINE_BYTES 64  // ESP32-S3 with our sdkconfig (64B lines)

void *dma_buf_alloc(size_t size, dma_buf_kind_t kind)
{
    if (size == 0) return NULL;

    uint32_t caps;
    size_t   align;

    switch (kind) {
    case DMA_BUF_DESC:
    case DMA_BUF_PAYLOAD:
        caps  = MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT;
        align = 4;
        break;
    case DMA_BUF_FRAME:
        caps  = MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA;
        align = CACHE_LINE_BYTES;
        break;
    default:
        ESP_LOGE(TAG, "unknown buffer kind %d", (int)kind);
        return NULL;
    }

    void *p = heap_caps_aligned_alloc(align, size, caps);
    if (!p) {
        ESP_LOGE(TAG, "alloc failed (kind=%d size=%u free=%u)",
                 (int)kind, (unsigned)size,
                 (unsigned)heap_caps_get_free_size(caps));
    }
    return p;
}

esp_err_t dma_buf_sync_for_device(void *buf, size_t size)
{
    if (!buf || size == 0) return ESP_ERR_INVALID_ARG;
    // Writeback dirty CPU cache lines so DMA sees latest writes.
    return esp_cache_msync(buf, size,
                           ESP_CACHE_MSYNC_FLAG_DIR_C2M |
                           ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

esp_err_t dma_buf_sync_for_cpu(void *buf, size_t size)
{
    if (!buf || size == 0) return ESP_ERR_INVALID_ARG;
    // Invalidate CPU cache lines so next read pulls DMA's writes from PSRAM.
    return esp_cache_msync(buf, size,
                           ESP_CACHE_MSYNC_FLAG_DIR_M2C |
                           ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

void dma_buf_free(void *buf)
{
    if (buf) heap_caps_free(buf);
}

// ─── kernel_abi.h aliases (Phase 5.3a) ─────────────────────────────────
// These forward to the dma_buf_sync_* helpers so the kernel ABI surface
// stays semantic ("cache_*") while the implementation stays in one place.

esp_err_t cache_flush_range(void *addr, size_t len)
{
    return dma_buf_sync_for_device(addr, len);
}

esp_err_t cache_invalidate_range(void *addr, size_t len)
{
    return dma_buf_sync_for_cpu(addr, len);
}
