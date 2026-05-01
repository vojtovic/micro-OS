#ifndef MODULE_TYPES_H
#define MODULE_TYPES_H

#include <stdint.h>

#define MODULE_ABI_VERSION  1
#define MODULE_MAGIC        0x4D4F4455

#define MODULE_FLAG_IRAM_HOT  (1 << 0)

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
