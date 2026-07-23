#include "ui.h"
#include "loader/gfx_abi.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <stdio.h>

#define UI_ROW_H  8    // one 5x7 text line per widget row

typedef enum { W_LABEL, W_CHOICE, W_BUTTON } wtype_t;

typedef struct {
    wtype_t            type;
    const char        *label;
    const char *const *opts;   // CHOICE
    int                nopts;
    int                sel;
} widget_t;

struct ui_ctx {
    gfx_fb_t         *fb;
    const gfx_font_t *font;
    widget_t          w[UI_MAX_WIDGETS];
    int               count;
    int               focus;   // index of focused widget (-1 = none yet)
    int               scroll;  // first visible widget row
};

static int is_focusable(const widget_t *w)
{
    return w->type == W_CHOICE || w->type == W_BUTTON;
}

ui_ctx *ui_create(void *fb, const void *font)
{
    if (!fb) return NULL;
    ui_ctx *u = heap_caps_malloc(sizeof(*u), MALLOC_CAP_SPIRAM);
    if (!u) return NULL;
    memset(u, 0, sizeof(*u));
    u->fb    = (gfx_fb_t *)fb;
    u->font  = (const gfx_font_t *)font;
    u->focus = -1;
    return u;
}

void ui_destroy(ui_ctx *u)
{
    if (u) heap_caps_free(u);
}

static int add(ui_ctx *u, wtype_t type, const char *label)
{
    if (!u || u->count >= UI_MAX_WIDGETS) return -1;
    int id = u->count++;
    widget_t *w = &u->w[id];
    memset(w, 0, sizeof(*w));
    w->type  = type;
    w->label = label;
    if (u->focus < 0 && is_focusable(w)) u->focus = id;   // first focusable
    return id;
}

int ui_add_label(ui_ctx *u, const char *text) { return add(u, W_LABEL, text); }
int ui_add_button(ui_ctx *u, const char *text) { return add(u, W_BUTTON, text); }

int ui_add_choice(ui_ctx *u, const char *label,
                  const char *const *opts, int nopts, int initial)
{
    int id = add(u, W_CHOICE, label);
    if (id < 0) return -1;
    widget_t *w = &u->w[id];
    w->opts  = opts;
    w->nopts = nopts;
    w->sel   = (initial >= 0 && initial < nopts) ? initial : 0;
    return id;
}

int ui_choice_get(ui_ctx *u, int id)
{
    if (!u || id < 0 || id >= u->count || u->w[id].type != W_CHOICE) return 0;
    return u->w[id].sel;
}

static void scroll_to_focus(ui_ctx *u)
{
    int rows = u->fb->h / UI_ROW_H;
    if (rows < 1) rows = 1;
    if (u->focus < u->scroll)            u->scroll = u->focus;
    if (u->focus >= u->scroll + rows)    u->scroll = u->focus - rows + 1;
    if (u->scroll < 0) u->scroll = 0;
}

void ui_render(ui_ctx *u)
{
    if (!u) return;
    gfx_fill_rect(u->fb, 0, 0, u->fb->w, u->fb->h, 0);   // clear
    scroll_to_focus(u);

    int rows = u->fb->h / UI_ROW_H;
    for (int r = 0; r < rows; r++) {
        int i = u->scroll + r;
        if (i >= u->count) break;
        widget_t *w = &u->w[i];
        int y = r * UI_ROW_H;
        int focused = (i == u->focus);

        if (focused) gfx_fill_rect(u->fb, 0, y, u->fb->w, UI_ROW_H, 1);
        uint32_t col = focused ? 0 : 1;

        char line[48];
        if (w->type == W_CHOICE) {
            const char *val = (w->opts && w->sel < w->nopts) ? w->opts[w->sel] : "";
            snprintf(line, sizeof(line), "%s: %s", w->label ? w->label : "", val);
        } else if (w->type == W_BUTTON) {
            snprintf(line, sizeof(line), "[ %s ]", w->label ? w->label : "");
        } else {
            snprintf(line, sizeof(line), "%s", w->label ? w->label : "");
        }
        gfx_draw_text(u->fb, 1, y, line, u->font, col);
    }
}

static void move_focus(ui_ctx *u, int dir)
{
    for (int i = u->focus + dir; i >= 0 && i < u->count; i += dir)
        if (is_focusable(&u->w[i])) { u->focus = i; return; }
}

int ui_handle_key(ui_ctx *u, int key)
{
    if (!u || u->focus < 0) return -1;
    widget_t *w = &u->w[u->focus];

    switch (key) {
    case 'w': case 0xB5:          // up
        move_focus(u, -1);
        break;
    case 's': case 0xB6:          // down
        move_focus(u, +1);
        break;
    case 'a': case 0xB4:          // left — cycle choice down
        if (w->type == W_CHOICE && w->nopts > 0)
            w->sel = (w->sel + w->nopts - 1) % w->nopts;
        break;
    case 'd': case 0xB7:          // right — cycle choice up
        if (w->type == W_CHOICE && w->nopts > 0)
            w->sel = (w->sel + 1) % w->nopts;
        break;
    case 0x0D: case 0x0A:         // ENTER
        if (w->type == W_BUTTON) return u->focus;
        if (w->type == W_CHOICE && w->nopts > 0)
            w->sel = (w->sel + 1) % w->nopts;
        break;
    default:
        break;
    }
    return -1;
}
