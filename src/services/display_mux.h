#ifndef DISPLAY_MUX_H
#define DISPLAY_MUX_H

#include "esp_err.h"
#include "display_types.h"
#include <stdbool.h>

#define DISPLAY_MUX_MAX_DRIVERS  4
#define DISPLAY_NAME_LEN         32

typedef struct {
    char                       name[DISPLAY_NAME_LEN];
    const display_driver_ops_t *ops;
    bool                       active;
} display_slot_t;

esp_err_t display_mux_init(void);
esp_err_t display_mux_register(const char *name, const display_driver_ops_t *ops);
esp_err_t display_mux_unregister(const char *name);
int       display_mux_count(void);
const display_slot_t *display_mux_get(int index);

// Push the current console to one named display (NULL = all active displays).
// For slow, on-demand displays (e-ink) that are not auto-mirrored live.
esp_err_t display_mux_refresh(const char *name);

esp_err_t cmd_display_register(void);

#endif
