// gfxdemo — a graphical foreground app.
//
// Unlike a text app, gfxdemo takes exclusive ownership of a display via
// display_mux_grab(): while grabbed, the kernel stops mirroring the console
// onto that screen, so the app fully owns the pixels. It allocates its own
// framebuffer, draws an animated scene each frame, and pushes it with
// display_mux_present(). The frame loop uses input_get_key() both as the
// ~30ms frame timer AND the exit check — any key quits.
//
// Usage:  gfxdemo [display]      (default display: "oled")
//
// This is the reference pattern for graphical apps: grab -> alloc fb ->
// loop{draw, present, poll input} -> free fb -> release.

#include "gfx_abi.h"
#include <stdint.h>

extern int vconsole_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
extern int input_get_key(uint32_t timeout_ms);                  // -1 on timeout
extern int display_mux_grab(const char *name);                  // 0 == ESP_OK
extern int display_mux_release(const char *name);
extern int display_mux_present(const char *name, const gfx_fb_t *fb);

// The OLED is 128x64 MONO_VLSB — its native gfx format, so present is a
// straight copy (no conversion). Match those dimensions for a 1:1 blit.
#define GD_W  128
#define GD_H  64

int main(int argc, char *argv[])
{
    const char *disp = (argc >= 2 && argv[1]) ? argv[1] : "oled";

    if (display_mux_grab(disp) != 0) {
        vconsole_printf("gfxdemo: no display '%s'\n", disp);
        return 1;
    }

    gfx_fb_t *fb = gfx_fb_alloc(GD_W, GD_H, GFX_FMT_MONO_VLSB, 0);
    if (!fb) {
        vconsole_printf("gfxdemo: framebuffer alloc failed\n");
        display_mux_release(disp);
        return 1;
    }

    const gfx_font_t *font = gfx_font_get("5x7");

    vconsole_printf("gfxdemo on '%s' — press any key to exit\n", disp);

    // Bouncing box + title. Box origin (bx,by), velocity (vx,vy).
    int  bx = 8, by = 20, vx = 3, vy = 2;
    const int box = 12;
    uint32_t frame = 0;

    for (;;) {
        // ---- draw one frame ----
        gfx_fill_rect(fb, 0, 0, GD_W, GD_H, 0);              // clear to black
        gfx_draw_text(fb, 2, 0, "micro_arch gfx", font, 1);  // static title

        gfx_fill_rect(fb, bx, by, box, box, 1);              // moving box
        bx += vx;
        by += vy;
        if (bx <= 0)            { bx = 0;            vx = -vx; }
        if (bx >= GD_W - box)   { bx = GD_W - box;   vx = -vx; }
        if (by <= 10)           { by = 10;           vy = -vy; }
        if (by >= GD_H - box)   { by = GD_H - box;   vy = -vy; }

        // frame counter, bottom-left
        char buf[24];
        int n = 0;
        buf[n++] = 'f'; buf[n++] = ':';
        // manual uint -> decimal (no snprintf dependency needed)
        char tmp[12]; int t = 0;
        uint32_t v = frame;
        do { tmp[t++] = (char)('0' + (v % 10)); v /= 10; } while (v && t < 11);
        while (t > 0) buf[n++] = tmp[--t];
        buf[n] = '\0';
        gfx_draw_text(fb, 2, GD_H - 8, buf, font, 1);

        display_mux_present(disp, fb);

        // ---- frame timer + exit poll: ~30ms per frame, any key quits ----
        if (input_get_key(30) >= 0)
            break;
        frame++;
    }

    gfx_fb_free(fb);
    display_mux_release(disp);
    vconsole_printf("gfxdemo: bye\n");
    return 0;
}
