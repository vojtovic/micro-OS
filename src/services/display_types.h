#ifndef DISPLAY_TYPES_H
#define DISPLAY_TYPES_H

#include <stdint.h>
#include <stddef.h>

#define DISPLAY_CAP_PARTIAL_REFRESH  (1 << 0)
#define DISPLAY_CAP_FAST_REFRESH     (1 << 1)
#define DISPLAY_CAP_COLOR            (1 << 2)

typedef struct {
    int       (*init)(void);
    void      (*shutdown)(void);
    void      (*render)(const char *text, size_t len);
    void      (*clear)(void);
    int       (*get_rows)(void);
    int       (*get_cols)(void);
    uint32_t  (*get_caps)(void);
} display_driver_ops_t;

#endif
