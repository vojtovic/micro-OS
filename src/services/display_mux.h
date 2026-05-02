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

esp_err_t cmd_display_register(void);

#endif
