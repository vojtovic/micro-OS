#ifndef COMPOSITOR_H
#define COMPOSITOR_H

#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

// compositor — focus-driven display output for apps.
//
// Instead of grabbing a display and pushing pixels straight to hardware, an app
// opens a WINDOW (an off-screen surface bound to a named display) and draws into
// it. The compositor keeps every window's latest content and blits only the
// FOCUSED app's windows to the physical displays. On a focus switch it re-blits
// the stored surface itself, so apps never repaint on switch-in and a paused
// app's screen is preserved without any per-app grab/present/repaint logic.
//
// Fullscreen model: one app owns each display at a time (the focused one).

#define COMP_MAX_WINDOWS  8

esp_err_t compositor_init(void);

// Open a graphics window (an allocated framebuffer you draw into) on `display`,
// owned by the calling task. `fmt` is a gfx_fmt_t. Returns a window id >= 0, or
// -1 on failure.
int   comp_open_gfx(const char *display, uint16_t w, uint16_t h, int fmt);

// Open a text window on `display` (content pushed via comp_set_text).
int   comp_open_text(const char *display);

// The framebuffer of a GFX window (a gfx_fb_t*), or NULL. Draw into this, then
// comp_commit().
void *comp_window_fb(int win);

// GFX window: show the current framebuffer now if the owner is focused (always
// kept as the stored surface for the next focus-in).
void  comp_commit(int win);

// TEXT window: store `text` and show it if the owner is focused.
void  comp_set_text(int win, const char *text, int len);

// Close a window and free its surface.
void  comp_close(int win);

// Called by the session on focus change: show `task`'s windows (grab+blit) and
// release the displays it does not use. NULL = no app (shell).
void  comp_set_focus(void *task);

#endif // COMPOSITOR_H
