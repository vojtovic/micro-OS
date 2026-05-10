#ifndef MEM_POOL_H
#define MEM_POOL_H

#include "esp_err.h"
#include <stddef.h>

// Module memory pool — tracks IRAM/DRAM bytes allocated by the ELF loader for
// loaded modules' .iram_text / .dram_data sections. Uses standard
// MALLOC_CAP_INTERNAL heaps; the "pool" is a soft budget enforced in software
// so a single module can't eat all of internal SRAM.
//
// Tunable via /sys/etc/loader.conf:
//     module.iram_pool_kb = 96
// (Default 64 KB. Honored on next boot.)

#define MEM_POOL_DEFAULT_IRAM_KB 64

esp_err_t mem_pool_init(size_t iram_kb);

// Allocate from internal SRAM with EXEC caps for module IRAM_ATTR sections.
// Returns NULL if the soft budget would be exceeded OR the system is out of
// internal RAM. Caller frees via mem_pool_free_iram.
void *mem_pool_alloc_iram(size_t size, size_t align);

// Allocate DMA-capable internal RAM for module .dram_data sections
// (DMA descriptors, ISR-touched data). No soft budget; bounded only by
// available internal SRAM.
void *mem_pool_alloc_dram(size_t size, size_t align);

void mem_pool_free_iram(void *ptr, size_t size);
void mem_pool_free_dram(void *ptr, size_t size);

size_t mem_pool_iram_budget(void);
size_t mem_pool_iram_used(void);
size_t mem_pool_dram_used(void);

#endif
