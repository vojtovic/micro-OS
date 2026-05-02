#ifndef MODULE_SDK_H
#define MODULE_SDK_H

#include "module_types.h"
#include <stddef.h>
#include <stdbool.h>

extern void module_register(module_exports_t *exports);

extern int  vconsole_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

typedef struct {
    int       (*init)(void);
    void      (*shutdown)(void);
    void      (*render)(const char *text, size_t len);
    void      (*clear)(void);
    int       (*get_rows)(void);
    int       (*get_cols)(void);
    uint32_t  (*get_caps)(void);
} display_driver_ops_t;

extern int  display_mux_register(const char *name, const display_driver_ops_t *ops);
extern int  display_mux_unregister(const char *name);
extern int  vconsole_write(const char *data, size_t len);
extern int  vconsole_putchar(char c);

extern int   registry_add(const char *name, uint16_t version, void *vtable, void *ctx);
extern void *registry_find(const char *name);
extern int   registry_set_state(const char *name, int state);

extern void *heap_caps_malloc(size_t size, uint32_t caps);
extern void  heap_caps_free(void *ptr);

#ifndef MALLOC_CAP_SPIRAM
#define MALLOC_CAP_SPIRAM    (1 << 10)
#endif
#ifndef MALLOC_CAP_INTERNAL
#define MALLOC_CAP_INTERNAL  (1 << 11)
#endif

#endif
