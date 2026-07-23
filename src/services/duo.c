#include "duo.h"
#include "display_mux.h"
#include "compositor.h"
#include "loader/gfx_abi.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <stdio.h>

#define DUO_TITLE_SCALE  2
#define DUO_ITEM_SCALE   2
#define DUO_ITEM_H       20   // e-ink list row pitch (scale-2 glyph is 14px tall)
#define DUO_VAL_SCALE    3    // big OLED value
#define DUO_CHAR_ADV     6    // 5x7 font advance at scale 1

struct duo {
    int               eink_win, oled_win;
    gfx_fb_t         *efb, *ofb;
    const gfx_font_t *font;
    int               ew, eh;
};

duo_t *duo_open(void)
{
    duo_t *d = heap_caps_malloc(sizeof(*d), MALLOC_CAP_SPIRAM);
    if (!d) return NULL;
    memset(d, 0, sizeof(*d));

    d->font = gfx_font_get("5x7");

    int rows = 0, cols = 0;
    display_mux_dims("eink", &rows, &cols);
    d->ew = cols > 0 ? cols * 6 : 240;
    d->eh = rows > 0 ? rows * 8 : 360;

    d->eink_win = comp_open_gfx("eink", (uint16_t)d->ew, (uint16_t)d->eh, GFX_FMT_MONO_HMSB);
    d->oled_win = comp_open_gfx("oled", 128, 64, GFX_FMT_MONO_VLSB);
    d->efb = comp_window_fb(d->eink_win);
    d->ofb = comp_window_fb(d->oled_win);
    return d;
}

void duo_close(duo_t *d)
{
    if (!d) return;
    comp_close(d->eink_win);
    comp_close(d->oled_win);
    heap_caps_free(d);
}

void duo_menu(duo_t *d, const char *title, const char *const *items, int n,
              int selected, const char *controls)
{
    if (!d || !d->efb) return;
    gfx_fb_t *fb = d->efb;
    gfx_fill_rect(fb, 0, 0, d->ew, d->eh, 0);

    int y0 = 2;
    if (title) {
        gfx_draw_text_scaled(fb, 2, 0, title, d->font, 1, DUO_TITLE_SCALE);
        int sep = 8 * DUO_TITLE_SCALE;
        gfx_fill_rect(fb, 0, sep, d->ew, 1, 1);
        y0 = sep + 6;
    }

    for (int i = 0; i < n; i++) {
        int y = y0 + i * DUO_ITEM_H;
        char line[48];
        snprintf(line, sizeof(line), "%d %s", i + 1, items[i] ? items[i] : "");
        if (i == selected) {
            gfx_fill_rect(fb, 0, y - 2, d->ew, DUO_ITEM_H - 2, 1);
            gfx_draw_text_scaled(fb, 2, y, line, d->font, 0, DUO_ITEM_SCALE);
        } else {
            gfx_draw_text_scaled(fb, 2, y, line, d->font, 1, DUO_ITEM_SCALE);
        }
    }

    if (controls) {
        int by = d->eh - 9;                          // small hint bar
        gfx_fill_rect(fb, 0, by - 2, d->ew, 1, 1);
        gfx_draw_text(fb, 2, by, controls, d->font, 1);
    }

    comp_commit(d->eink_win);
}

void duo_value(duo_t *d, const char *label, const char *value)
{
    if (!d || !d->ofb) return;
    gfx_fill_rect(d->ofb, 0, 0, 128, 64, 0);
    if (label) gfx_draw_text(d->ofb, 2, 2, label, d->font, 1);   // small caption

    char v[24];
    int len = snprintf(v, sizeof(v), "< %s >", value ? value : "");
    int w = len * DUO_CHAR_ADV * DUO_VAL_SCALE;                  // big, centered
    int x = (128 - w) / 2;
    if (x < 0) x = 0;
    gfx_draw_text_scaled(d->ofb, x, 26, v, d->font, 1, DUO_VAL_SCALE);
    comp_commit(d->oled_win);
}

void duo_message(duo_t *d, const char *text)
{
    if (!d || !d->ofb) return;
    gfx_fill_rect(d->ofb, 0, 0, 128, 64, 0);
    const char *t = text ? text : "";
    int w = (int)strlen(t) * DUO_CHAR_ADV * 2;
    int x = (128 - w) / 2; if (x < 0) x = 0;
    gfx_draw_text_scaled(d->ofb, x, 24, t, d->font, 1, 2);
    comp_commit(d->oled_win);
}

void *duo_eink_fb(duo_t *d)   { return d ? d->efb : NULL; }
void *duo_oled_fb(duo_t *d)   { return d ? d->ofb : NULL; }
int   duo_eink_w(duo_t *d)    { return d ? d->ew : 0; }
int   duo_eink_h(duo_t *d)    { return d ? d->eh : 0; }
void  duo_eink_commit(duo_t *d) { if (d) comp_commit(d->eink_win); }
void  duo_oled_commit(duo_t *d) { if (d) comp_commit(d->oled_win); }
