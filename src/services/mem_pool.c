#include "services/mem_pool.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

static const char *TAG = "mem_pool";

static size_t s_iram_budget = MEM_POOL_DEFAULT_IRAM_KB * 1024;
static size_t s_iram_used   = 0;
static size_t s_dram_used   = 0;

esp_err_t mem_pool_init(size_t iram_kb)
{
    if (iram_kb == 0) iram_kb = MEM_POOL_DEFAULT_IRAM_KB;
    s_iram_budget = iram_kb * 1024;
    s_iram_used   = 0;
    s_dram_used   = 0;

    size_t internal_free = heap_caps_get_free_size(
        MALLOC_CAP_INTERNAL | MALLOC_CAP_EXEC | MALLOC_CAP_32BIT);

    ESP_LOGI(TAG, "module IRAM budget: %u KB (internal exec free: %u KB)",
             (unsigned)iram_kb, (unsigned)(internal_free / 1024));

    if (s_iram_budget > internal_free) {
        ESP_LOGW(TAG, "budget exceeds available internal exec heap — "
                      "modules will hit OOM before budget");
    }
    return ESP_OK;
}

void *mem_pool_alloc_iram(size_t size, size_t align)
{
    if (size == 0) return NULL;
    if (align < 4) align = 4;

    if (s_iram_used + size > s_iram_budget) {
        ESP_LOGW(TAG, "IRAM budget exceeded: need %u B, used %u B, budget %u B",
                 (unsigned)size, (unsigned)s_iram_used, (unsigned)s_iram_budget);
        return NULL;
    }

    uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_EXEC | MALLOC_CAP_32BIT;
    void *p = heap_caps_aligned_alloc(align, size, caps);
    if (!p) {
        ESP_LOGE(TAG, "internal exec heap OOM (need %u B, free %u B)",
                 (unsigned)size,
                 (unsigned)heap_caps_get_free_size(caps));
        return NULL;
    }
    s_iram_used += size;
    return p;
}

void *mem_pool_alloc_dram(size_t size, size_t align)
{
    if (size == 0) return NULL;
    if (align < 4) align = 4;

    uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT;
    void *p = heap_caps_aligned_alloc(align, size, caps);
    if (!p) {
        ESP_LOGE(TAG, "internal DMA heap OOM (need %u B, free %u B)",
                 (unsigned)size,
                 (unsigned)heap_caps_get_free_size(caps));
        return NULL;
    }
    s_dram_used += size;
    return p;
}

void mem_pool_free_iram(void *ptr, size_t size)
{
    if (!ptr) return;
    heap_caps_free(ptr);
    if (s_iram_used >= size) s_iram_used -= size;
    else                     s_iram_used  = 0;
}

void mem_pool_free_dram(void *ptr, size_t size)
{
    if (!ptr) return;
    heap_caps_free(ptr);
    if (s_dram_used >= size) s_dram_used -= size;
    else                     s_dram_used  = 0;
}

size_t mem_pool_iram_budget(void) { return s_iram_budget; }
size_t mem_pool_iram_used(void)   { return s_iram_used; }
size_t mem_pool_dram_used(void)   { return s_dram_used; }
