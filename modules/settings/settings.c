// settings — a *styled* dual-display settings screen. Also a graphics stress
// test: framed cards, a title bar, numbered badges, chevrons and corner accents
// on the e-ink; a bordered value box with arrow chevrons on the OLED. Proves the
// gfx stack can do rich UI (and, by extension, games / wallpapers).
//
// Uses the duo toolkit for window management but draws custom graphics into its
// framebuffers with the gfx primitives.
//
// Keys: 1-9 pick; a/d (or left/right) change; S save; ESC quit.

#include "gfx_abi.h"
#include <stdint.h>
#include <stddef.h>

extern int   vconsole_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
extern int   input_get_key(uint32_t timeout_ms);
extern int   hwconf_get_int(const char *section, const char *key, int def);
extern int   app_write_file(const char *path, const char *buf, int len);
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

#define CONF_PATH "/sys/etc/hardware.conf"

typedef struct {
    const char        *name;
    const char *const *opts;
    const int         *vals;
    int                nopts;
    int                sel;
} setting_t;

static const char *const oled_opts[] = { "0", "180" };
static const int         oled_vals[] = { 0, 180 };
static const char *const eink_opts[] = { "0", "90", "180", "270" };
static const int         eink_vals[] = { 0, 90, 180, 270 };

static setting_t s_set[] = {
    { "OLED rotation", oled_opts, oled_vals, 2, 0 },
    { "Eink rotation", eink_opts, eink_vals, 4, 0 },
};
#define NSET  ((int)(sizeof(s_set) / sizeof(s_set[0])))

static duo_t            *d;
static gfx_fb_t         *efb, *ofb;
static const gfx_font_t *font;
static int ew, eh;

// ">" / "<" chevrons from two lines.
static void chevron_r(gfx_fb_t *fb, int x, int y, int s, uint32_t c)
{ gfx_draw_line(fb, x, y - s, x + s, y, c); gfx_draw_line(fb, x + s, y, x, y + s, c); }
static void chevron_l(gfx_fb_t *fb, int x, int y, int s, uint32_t c)
{ gfx_draw_line(fb, x + s, y - s, x, y, c); gfx_draw_line(fb, x, y, x + s, y + s, c); }

// ---- e-ink: framed cards + title bar + footer ----------------------------
static void draw_eink(int selected)
{
    if (!efb) return;
    gfx_fill_rect(efb, 0, 0, ew, eh, 0);

    // double outer frame + corner accents
    gfx_draw_rect(efb, 0, 0, ew, eh, 1, 1);
    gfx_draw_rect(efb, 3, 3, ew - 6, eh - 6, 1, 1);
    for (int i = 0; i < 8; i++) {
        gfx_draw_line(efb, 6 + i, 6, 6, 6 + i, 1);                 // TL
        gfx_draw_line(efb, ew - 7 - i, 6, ew - 7, 6 + i, 1);       // TR
    }

    // title bar
    gfx_fill_rect(efb, 7, 7, ew - 14, 26, 1);
    int tw = 8 * 6 * 2;                                            // "SETTINGS" @2x
    gfx_draw_text_scaled(efb, (ew - tw) / 2, 11, "SETTINGS", font, 0, 2);

    // setting cards
    int cy = 44, ch = 46;
    for (int i = 0; i < NSET; i++, cy += ch + 8) {
        int on = (i == selected);
        gfx_draw_rect(efb, 12, cy, ew - 24, ch, on ? 3 : 1, 1);

        // numbered badge (filled disc, number reversed)
        int bcx = 34, bcy = cy + ch / 2;
        gfx_draw_circle(efb, bcx, bcy, 13, 1, 1);
        char num[2] = { (char)('1' + i), 0 };
        gfx_draw_text_scaled(efb, bcx - 5, bcy - 7, num, font, 0, 2);

        gfx_draw_text_scaled(efb, 56, cy + 8, s_set[i].name, font, 1, 2);
        char now[24];
        snprintf(now, sizeof(now), "now: %s", s_set[i].opts[s_set[i].sel]);
        gfx_draw_text(efb, 56, cy + 30, now, font, 1);

        if (on) chevron_r(efb, ew - 26, bcy, 7, 1);               // selection marker
    }

    // footer controls bar
    int fy = eh - 26;
    gfx_draw_rect(efb, 12, fy, ew - 24, 18, 1, 1);
    gfx_draw_text(efb, 18, fy + 5, "1-2 pick  a/d change  S save  ESC", font, 1);

    duo_eink_commit(d);
}

// ---- OLED: bordered value box with arrow chevrons ------------------------
static void draw_oled(int selected)
{
    if (!ofb) return;
    gfx_fill_rect(ofb, 0, 0, 128, 64, 0);
    gfx_draw_rect(ofb, 0, 0, 128, 64, 1, 1);

    setting_t *s = &s_set[selected];
    gfx_draw_text(ofb, 6, 5, s->name, font, 1);
    gfx_draw_line(ofb, 4, 15, 124, 15, 1);

    gfx_draw_rect(ofb, 22, 22, 84, 34, 1, 1);
    chevron_l(ofb, 8,  39, 6, 1);
    chevron_r(ofb, 114, 39, 6, 1);

    const char *v = s->opts[s->sel];
    int vw = 0; for (const char *p = v; *p; p++) vw += 18;         // scale-3 advance
    gfx_draw_text_scaled(ofb, 64 - vw / 2, 30, v, font, 1, 3);

    duo_oled_commit(d);
}

static void save_conf(void)
{
    char buf[512];
    int n = snprintf(buf, sizeof(buf),
        "# Hardware pin configuration\n"
        "[display_spi]\nclk=12\nmosi=11\n\n"
        "[oled]\ncs=10\ndc=21\nrst=47\nwidth=128\nheight=64\nrotation=%d\n\n"
        "[eink]\ncs=16\ndc=15\nrst=17\nbusy=18\nrotation=%d\n\n"
        "[buzzer]\npin=4\n",
        s_set[0].vals[s_set[0].sel], s_set[1].vals[s_set[1].sel]);
    if (app_write_file(CONF_PATH, buf, n) == 0)
        vconsole_printf("settings: saved (reboot/restart drivers to apply)\n");
    else
        vconsole_printf("settings: FAILED to write %s\n", CONF_PATH);
}

int main(int argc, char *argv[])
{
    d = duo_open();
    if (!d) { vconsole_printf("settings: display init failed\n"); return 1; }
    efb = (gfx_fb_t *)duo_eink_fb(d);
    ofb = (gfx_fb_t *)duo_oled_fb(d);
    ew  = duo_eink_w(d);
    eh  = duo_eink_h(d);
    font = gfx_font_get("5x7");

    int oled_rot = hwconf_get_int("oled", "rotation", 0);
    int eink_rot = hwconf_get_int("eink", "rotation", 0);
    for (int i = 0; i < s_set[0].nopts; i++) if (oled_vals[i] == oled_rot) s_set[0].sel = i;
    for (int i = 0; i < s_set[1].nopts; i++) if (eink_vals[i] == eink_rot) s_set[1].sel = i;

    int selected = 0;
    draw_eink(selected);
    draw_oled(selected);
    vconsole_printf("settings: 1-2 pick, a/d change, S save, ESC quit\n");

    for (;;) {
        int k = input_get_key(1000000);
        if (k < 0) continue;
        if (k == 0x1B) break;

        setting_t *s = &s_set[selected];
        if (k >= '1' && k <= '9') {
            int idx = k - '1';
            if (idx < NSET) { selected = idx; draw_eink(selected); draw_oled(selected); }
        } else if (k == 'a' || k == 0xB4) {
            s->sel = (s->sel + s->nopts - 1) % s->nopts;
            draw_oled(selected);                        // OLED live (fast)
        } else if (k == 'd' || k == 0xB7) {
            s->sel = (s->sel + 1) % s->nopts;
            draw_oled(selected);
        } else if (k == 's' || k == 'S') {
            save_conf();
            draw_eink(selected);                        // refresh "now:" values
        }
    }

    duo_close(d);
    vconsole_printf("settings: bye\n");
    return 0;
}
