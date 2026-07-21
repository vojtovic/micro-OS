#ifndef DISPLAY_TYPES_H
#define DISPLAY_TYPES_H

#include <stdint.h>
#include <stddef.h>

#define DISPLAY_CAP_PARTIAL_REFRESH  (1 << 0)
#define DISPLAY_CAP_FAST_REFRESH     (1 << 1)
#define DISPLAY_CAP_COLOR            (1 << 2)

// Forward declaration — avoids pulling gfx_abi.h into this shared header
// (kernel and module builds reach it via different include paths). Drivers
// that implement present() include gfx_abi.h and get the full type.
struct gfx_fb;

typedef struct {
    int       (*init)(void);
    void      (*shutdown)(void);
    void      (*render)(const char *text, size_t len);
    void      (*clear)(void);
    int       (*get_rows)(void);
    int       (*get_cols)(void);
    uint32_t  (*get_caps)(void);
    // Optional: present an arbitrary framebuffer (GUI). NULL if unsupported.
    // The fb's format is expected to match the display's native format.
    int       (*present)(const struct gfx_fb *fb);
} display_driver_ops_t;

#endif
