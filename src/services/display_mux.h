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
    bool                       grabbed;   // owned by a GUI app — no console mirror
} display_slot_t;

esp_err_t display_mux_init(void);
esp_err_t display_mux_register(const char *name, const display_driver_ops_t *ops);
esp_err_t display_mux_unregister(const char *name);
int       display_mux_count(void);
const display_slot_t *display_mux_get(int index);

// Push the current console to one named display (NULL = all active displays).
// For slow, on-demand displays (e-ink) that are not auto-mirrored live.
esp_err_t display_mux_refresh(const char *name);

// Push the console onto FAST displays (OLED) even when grabbed — used to clear a
// paused GUI app's frozen frame when focus returns to the shell.
esp_err_t display_mux_refresh_fast(void);

// True if the named display advertises DISPLAY_CAP_FAST_REFRESH (OLED). Slow
// displays (e-ink) return false — the compositor debounces their redraws.
bool display_mux_is_fast(const char *name);

// Present an arbitrary framebuffer to a named display (GUI path). Returns
// ESP_ERR_NOT_FOUND if no such display, ESP_ERR_NOT_SUPPORTED if the driver
// has no present op.
esp_err_t display_mux_present(const char *name, const struct gfx_fb *fb);

// Push a text string to a named display's text render op (the GUI-text path,
// used by a grabbed display since the console mirror is off). Returns
// ESP_ERR_NOT_FOUND / ESP_ERR_NOT_SUPPORTED like present.
esp_err_t display_mux_render_text(const char *name, const char *text, size_t len);

// Query a named display's text grid size (rows x cols). Either pointer may be
// NULL. Returns ESP_ERR_NOT_FOUND if there is no such display.
esp_err_t display_mux_dims(const char *name, int *rows, int *cols);

// Grab/release a display for exclusive GUI use — while grabbed, the console
// auto-mirror skips it, so a graphical app owns the screen. Release restores
// normal console mirroring.
esp_err_t display_mux_grab(const char *name);
esp_err_t display_mux_release(const char *name);

esp_err_t cmd_display_register(void);

#endif
