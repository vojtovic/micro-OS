#ifndef UI_H
#define UI_H

#include <stdint.h>

// ui — a minimal keyboard-driven widget toolkit.
//
// A UI context is bound to a gfx framebuffer (a compositor window) and a font.
// You add widgets top-to-bottom; the toolkit lays them out vertically, tracks
// focus, scrolls to keep the focused widget visible, and draws a highlight bar.
// Feed keys with ui_handle_key(); render with ui_render() into the framebuffer,
// then comp_commit() the window.
//
// Navigation: up/down (or w/s) move focus between focusable widgets; left/right
// (or a/d) cycle a focused CHOICE; ENTER activates a BUTTON (returns its id) or
// cycles a CHOICE. Designed for small screens + a keyboard (no pointer).

#define UI_MAX_WIDGETS  16

typedef struct ui_ctx ui_ctx;

// fb is a gfx_fb_t*, font a gfx_font_t* (kept as void* so this header stays
// free of the gfx ABI). Returns NULL on failure.
ui_ctx *ui_create(void *fb, const void *font);
void    ui_destroy(ui_ctx *u);

// Add widgets (drawn top-to-bottom). Strings are borrowed (kept alive by the
// caller). Return a widget id (>= 0), or -1 if full.
int  ui_add_label (ui_ctx *u, const char *text);
int  ui_add_choice(ui_ctx *u, const char *label,
                   const char *const *opts, int nopts, int initial);
int  ui_add_button(ui_ctx *u, const char *text);

// Current selected option index of a CHOICE widget (0 if not a choice).
int  ui_choice_get(ui_ctx *u, int id);

// Draw all (visible) widgets into the framebuffer.
void ui_render(ui_ctx *u);

// Feed one key. Returns the id of a BUTTON activated with ENTER, else -1.
int  ui_handle_key(ui_ctx *u, int key);

#endif // UI_H
