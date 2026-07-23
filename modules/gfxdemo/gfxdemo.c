// gfxdemo — a graphical demo, on the compositor.
//
// Opens a compositor WINDOW on the OLED and draws an animated scene into it,
// committing each frame. The compositor shows the window only while gfxdemo is
// the focused app and preserves the last frame across focus switches — so this
// app needs no grab/present/release and no repaint-on-focus handling. ESC quits.

#include "gfx_abi.h"
#include <stdint.h>
#include <stddef.h>

extern int   vconsole_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
extern int   input_get_key(uint32_t timeout_ms);
// Compositor window API.
extern int   comp_open_gfx(const char *display, uint16_t w, uint16_t h, int fmt);
extern void *comp_window_fb(int win);
extern void  comp_commit(int win);
extern void  comp_close(int win);

#define GD_W  128
#define GD_H  64

int main(int argc, char *argv[])
{
    const char *disp = (argc >= 2 && argv[1]) ? argv[1] : "oled";

    int win = comp_open_gfx(disp, GD_W, GD_H, GFX_FMT_MONO_VLSB);
    if (win < 0) {
        vconsole_printf("gfxdemo: could not open a window on '%s'\n", disp);
        return 1;
    }
    gfx_fb_t *fb = (gfx_fb_t *)comp_window_fb(win);
    const gfx_font_t *font = gfx_font_get("5x7");

    vconsole_printf("gfxdemo on '%s' — ESC to quit\n", disp);

    int  bx = 8, by = 20, vx = 3, vy = 2;
    const int box = 12;
    uint32_t frame = 0;

    for (;;) {
        gfx_fill_rect(fb, 0, 0, GD_W, GD_H, 0);
        gfx_draw_text(fb, 2, 0, "micro_arch gfx", font, 1);

        gfx_fill_rect(fb, bx, by, box, box, 1);
        bx += vx; by += vy;
        if (bx <= 0)          { bx = 0;          vx = -vx; }
        if (bx >= GD_W - box) { bx = GD_W - box; vx = -vx; }
        if (by <= 10)         { by = 10;         vy = -vy; }
        if (by >= GD_H - box) { by = GD_H - box; vy = -vy; }

        char buf[24];
        int n = 0;
        buf[n++] = 'f'; buf[n++] = ':';
        char tmp[12]; int t = 0;
        uint32_t v = frame;
        do { tmp[t++] = (char)('0' + (v % 10)); v /= 10; } while (v && t < 11);
        while (t > 0) buf[n++] = tmp[--t];
        buf[n] = '\0';
        gfx_draw_text(fb, 2, GD_H - 8, buf, font, 1);

        comp_commit(win);

        int k = input_get_key(30);      // ~30 ms/frame; ESC quits, ignore the rest
        if (k == 0x1B) break;
        frame++;
    }

    comp_close(win);
    vconsole_printf("gfxdemo: bye\n");
    return 0;
}
