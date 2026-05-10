#ifndef MODULE_TYPES_H
#define MODULE_TYPES_H

#include <stdint.h>

#define MODULE_ABI_VERSION  1
#define MODULE_MAGIC        0x4D4F4455

#define MODULE_FLAG_IRAM_HOT  (1 << 0)

// Phase 5.2 module-mgr error codes (above the ESP_ERR user range)
#define ESP_ERR_NO_MEM_IRAM        0x10101  // module IRAM pool budget exhausted
#define ESP_ERR_MOD_BAD_SECTION    0x10110  // unknown alloc-flagged ELF section
#define ESP_ERR_MOD_CACHE_FLUSH    0x10111  // Cache_Invalidate_ICache failed
#define ESP_ERR_MOD_STATIC_DUP     0x10120  // static module name collision
#define ESP_ERR_MOD_STATIC_DENIED  0x10121  // .ini static_link = no

typedef struct {
    uint32_t    magic;
    uint32_t    abi_version;
    char        name[32];
    char        version[16];
    char        requires[128];
    uint32_t    flags;
} module_info_t;

typedef struct {
    module_info_t  *info;
    int           (*start)(void);
    int           (*stop)(void);
} module_exports_t;

#endif
