// nerv — a NERV / MAGI (Neon Genesis Evangelion) themed demo.
//
// Proves the monochrome + geometric gfx stack can carry the Evangelion terminal
// aesthetic, using the dual-display idea: the E-INK holds the big static MAGI
// panel (hazard stripes, bevelled boxes, status), the OLED shows the live
// readout (a reticle + a ticking sync ratio + a blinking WARNING). ESC quits.
//
// Font is ASCII-only, so labels are the canon romaji (NERV, MAGI, A.T. FIELD,
// PATTERN BLUE) — a bold condensed / katakana font is future work.

#include "gfx_abi.h"
#include <stdint.h>
#include <stddef.h>

extern int   vconsole_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
extern int   input_get_key(uint32_t timeout_ms);
extern int   snprintf(char *str, size_t size, const char *fmt, ...);

typedef struct duo duo_t;
extern duo_t *duo_open(void);
extern void   duo_close(duo_t *d);
extern void  *duo_eink_fb(duo_t *d);
extern void  *duo_oled_fb(duo_t *d);
extern int    duo_eink_w(duo_t *d);
extern int    duo_eink_h(duo_t *d);
extern void   duo_eink_commit(duo_t *d);
extern void   duo_oled_commit(duo_t *d);

static const gfx_font_t *font;
static duo_t            *d;

// ---- NERV theme primitives ----------------------------------------------

// Diagonal hazard stripes filling a horizontal band.
static void hazard(gfx_fb_t *fb, int x, int y, int w, int h, uint32_t c)
{
    for (int off = -h; off < w; off += 12)
        for (int t = 0; t < 6; t++)
            gfx_draw_line(fb, x + off + t, y + h - 1, x + off + t + h, y, c);
}

// Angular NERV panel: a rectangle with cut (bevelled) corners.
static void bevel(gfx_fb_t *fb, int x, int y, int w, int h, int cut, uint32_t c)
{
    int r = x + w - 1, b = y + h - 1;
    gfx_draw_line(fb, x + cut, y, r - cut, y, c);
    gfx_draw_line(fb, x + cut, b, r - cut, b, c);
    gfx_draw_line(fb, x, y + cut, x, b - cut, c);
    gfx_draw_line(fb, r, y + cut, r, b - cut, c);
    gfx_draw_line(fb, x, y + cut, x + cut, y, c);
    gfx_draw_line(fb, r - cut, y, r, y + cut, c);
    gfx_draw_line(fb, x, b - cut, x + cut, b, c);
    gfx_draw_line(fb, r - cut, b, r, b - cut, c);
}

// A MAGI unit box: bevelled frame + name + a status line.
static void magi_box(gfx_fb_t *fb, int x, int y, int w, int h,
                     const char *name, const char *status)
{
    bevel(fb, x, y, w, h, 6, 1);
    gfx_draw_text(fb, x + 8, y + 6, name, font, 1);
    gfx_draw_line(fb, x + 6, y + 16, x + w - 7, y + 16, 1);
    gfx_draw_circle(fb, x + 12, y + 26, 3, 1, 1);          // status LED
    gfx_draw_text(fb, x + 20, y + 22, status, font, 1);
}

// ---- e-ink: the static MAGI panel ----------------------------------------
static void draw_eink(gfx_fb_t *fb, int ew, int eh)
{
    gfx_fill_rect(fb, 0, 0, ew, eh, 0);
    bevel(fb, 1, 1, ew - 2, eh - 2, 10, 1);

    // header: inverted title bar with hazard flanks
    gfx_fill_rect(fb, 8, 8, ew - 16, 30, 1);
    hazard(fb, 12, 12, 34, 22, 0);
    hazard(fb, ew - 46, 12, 34, 22, 0);
    int tw = 4 * 6 * 3;                                    // "NERV" @3x
    gfx_draw_text_scaled(fb, (ew - tw) / 2, 12, "NERV", font, 0, 3);
    gfx_draw_text(fb, (ew - 11 * 6) / 2, 42, "MAGI SYSTEM", font, 1);

    // three MAGI units
    int by = 58, bh = 46, gap = 8;
    magi_box(fb, 10, by,             ew - 20, bh, "MELCHIOR-1", "STATUS: ONLINE");
    magi_box(fb, 10, by + bh + gap,  ew - 20, bh, "BALTHASAR-2", "STATUS: ONLINE");
    magi_box(fb, 10, by + 2*(bh+gap),ew - 20, bh, "CASPER-3",   "STATUS: ONLINE");

    // central readout
    int cy = by + 3*(bh+gap) + 6;
    bevel(fb, 10, cy, ew - 20, 40, 6, 1);
    gfx_draw_text(fb, 18, cy + 6, "PATTERN ANALYSIS", font, 1);
    gfx_draw_text_scaled(fb, 18, cy + 18, "BLUE", font, 1, 2);
    gfx_draw_text(fb, ew - 74, cy + 22, "CONFIRMED", font, 1);

    // footer hazard band + alert
    int fy = eh - 34;
    hazard(fb, 8, fy, ew - 16, 12, 1);
    gfx_draw_text_scaled(fb, (ew - 9 * 6 * 2) / 2, fy + 16, "A.T.FIELD", font, 1, 2);

    duo_eink_commit(d);
}

// ---- OLED: the live readout ----------------------------------------------
static void draw_oled(gfx_fb_t *fb, int frame)
{
    gfx_fill_rect(fb, 0, 0, 128, 64, 0);
    gfx_draw_rect(fb, 0, 0, 128, 64, 1, 1);

    // targeting reticle on the left
    int rx = 30, ry = 32;
    gfx_draw_circle(fb, rx, ry, 18, 0, 1);
    gfx_draw_circle(fb, rx, ry, 10, 0, 1);
    gfx_draw_line(fb, rx - 24, ry, rx + 24, ry, 1);
    gfx_draw_line(fb, rx, ry - 24, rx, ry + 24, 1);

    // live sync-ratio readout on the right
    int ratio = 400 + (frame * 7) % 60;                   // 40.0 .. 45.9 %
    char buf[16];
    snprintf(buf, sizeof(buf), "%d.%d", ratio / 10, ratio % 10);
    gfx_draw_text(fb, 64, 8, "SYNC RATIO", font, 1);
    gfx_draw_text_scaled(fb, 64, 20, buf, font, 1, 2);
    gfx_draw_text(fb, 64, 40, "LCL: NOMINAL", font, 1);

    if ((frame / 8) & 1) {                                // blinking WARNING
        gfx_fill_rect(fb, 64, 52, 60, 10, 1);
        gfx_draw_text(fb, 68, 53, "! WARNING", font, 0);
    }
    duo_oled_commit(d);
}

int main(int argc, char *argv[])
{
    d = duo_open();
    if (!d) { vconsole_printf("nerv: display init failed\n"); return 1; }

    gfx_fb_t *efb = (gfx_fb_t *)duo_eink_fb(d);
    gfx_fb_t *ofb = (gfx_fb_t *)duo_oled_fb(d);
    int ew = duo_eink_w(d), eh = duo_eink_h(d);
    font = gfx_font_get("5x7");

    vconsole_printf("nerv: MAGI online. ESC to quit.\n");

    if (efb) draw_eink(efb, ew, eh);       // static MAGI panel on the e-ink

    // live readout loop on the OLED
    int frame = 0;
    for (;;) {
        if (ofb) draw_oled(ofb, frame);
        int k = input_get_key(120);        // ~8 fps blink/tick
        if (k == 0x1B) break;
        frame++;
    }

    duo_close(d);
    vconsole_printf("nerv: bye\n");
    return 0;
}
