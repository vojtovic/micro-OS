// home — the graphical home screen ("wall") for the whole system.
//
// e-ink = the wall: a dotted wallpaper, a clock widget (time from the RTC), and
// a scrollable grid of app tiles. Each tile shows a shortcut letter (the app's
// first letter) so you can launch it instantly by pressing that key. Arrow keys
// move the highlight (and scroll to more apps), ENTER launches, R = drivers, Y =
// system, ESC exits. OLED shows the highlighted app.
//
// Launched apps run to completion and return to the wall.

#include "gfx_abi.h"
#include "rtc_service.h"
#include <stdint.h>
#include <stddef.h>

extern int   vconsole_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
extern int   input_get_key(uint32_t timeout_ms);
extern int   snprintf(char *str, size_t size, const char *fmt, ...);
extern int   app_list(char (*names)[32], int max);
extern int   app_run(const char *name, int argc, char **argv);
extern int   shell_exec(const char *line);
extern void *registry_vtable(const char *name);

typedef struct duo duo_t;
extern duo_t *duo_open(void);
extern void   duo_close(duo_t *d);
extern void   duo_menu(duo_t *d, const char *title, const char *const *items, int n,
                       int selected, const char *controls);
extern void   duo_message(duo_t *d, const char *text);
extern void  *duo_eink_fb(duo_t *d);
extern int    duo_eink_w(duo_t *d);
extern int    duo_eink_h(duo_t *d);
extern void   duo_eink_commit(duo_t *d);

enum { SC_WALL, SC_DRIVERS, SC_SYSTEM };

typedef struct { const char *label, *path, *name; } drv_t;
static const drv_t drvs[] = {
    { "OLED display",    "/sdcard/drivers/oled.elf",   "oled"   },
    { "E-ink display",   "/sdcard/drivers/eink.elf",   "eink"   },
    { "CardKB keyboard", "/sdcard/drivers/cardkb.elf", "cardkb" },
    { "RTC clock",       "/sdcard/drivers/rtc.elf",    "rtc"    },
};
#define NDRV 4
static const char *drv_labels[NDRV];

#define COLS 2
static char apps[24][32];
static int  napps;

static duo_t            *d;
static const gfx_font_t *font;
static int  screen = SC_WALL, sel = 0, scroll = 0, ew, eh;

static char up_first(const char *s) { char c = s[0]; return (c >= 'a' && c <= 'z') ? c - 32 : c; }

static void clock_str(char *t, int tn, char *dt, int dn)
{
    const rtc_ops_t *rtc = (const rtc_ops_t *)registry_vtable("rtc");
    rtc_time_t x;
    if (rtc && rtc->get(&x) == 0) {
        snprintf(t,  tn, "%02d:%02d", x.hour, x.min);
        snprintf(dt, dn, "%04d-%02d-%02d", x.year, x.month, x.day);
    } else { snprintf(t, tn, "--:--"); snprintf(dt, dn, "no clock"); }
}

// The wall: wallpaper + clock widget + scrollable app-tile grid.
static void draw_wall(void)
{
    gfx_fb_t *fb = (gfx_fb_t *)duo_eink_fb(d);
    if (!fb) return;
    int w = ew, h = eh;

    gfx_fill_rect(fb, 0, 0, w, h, 0);
    for (int y = 6; y < h; y += 14)                     // wallpaper dot grid
        for (int x = 6; x < w; x += 14)
            gfx_set_pixel(fb, x, y, 1);

    // clock widget
    gfx_fill_rect(fb, 4, 4, w - 8, 42, 0);
    gfx_draw_rect(fb, 4, 4, w - 8, 42, 1, 1);
    char t[8], dt[16];
    clock_str(t, sizeof(t), dt, sizeof(dt));
    gfx_draw_text_scaled(fb, 12, 12, t, font, 1, 3);
    gfx_draw_text(fb, w - 78, 12, dt, font, 1);
    gfx_draw_text(fb, w - 78, 30, "micro_arch", font, 1);

    // app-tile grid
    int gx = 6, gy = 52, gap = 6;
    int tw = (w - gx - gap - gx) / COLS, th = 42;
    int rows_vis = (h - gy - 14) / (th + gap);
    if (rows_vis < 1) rows_vis = 1;

    int selrow = sel / COLS;
    if (selrow < scroll)               scroll = selrow;
    if (selrow >= scroll + rows_vis)   scroll = selrow - rows_vis + 1;

    for (int i = 0; i < napps; i++) {
        int r = i / COLS - scroll, c = i % COLS;
        if (r < 0 || r >= rows_vis) continue;
        int tx = gx + c * (tw + gap), ty = gy + r * (th + gap);
        int on = (i == sel);

        gfx_fill_rect(fb, tx, ty, tw, th, 0);           // opaque tile over wallpaper
        gfx_draw_rect(fb, tx, ty, tw, th, on ? 3 : 1, 1);

        char sc[2] = { up_first(apps[i]), 0 };          // shortcut badge
        gfx_fill_rect(fb, tx + 3, ty + 3, 14, 14, 1);
        gfx_draw_text_scaled(fb, tx + 5, ty + 5, sc, font, 0, 1);

        gfx_draw_text(fb, tx + 4, ty + 24, apps[i], font, 1);
    }

    gfx_draw_text(fb, 6, h - 11, "move ENTER open  R drv  Y sys  ESC", font, 1);
    duo_eink_commit(d);

    duo_message(d, napps ? apps[sel] : "(no apps)");
}

static void render(void)
{
    if (screen == SC_WALL) { draw_wall(); return; }
    if (screen == SC_DRIVERS) {
        duo_menu(d, "Drivers", drv_labels, NDRV, sel, "ENTER start  ESC back");
        duo_message(d, drvs[sel].label);
        return;
    }
    // SC_SYSTEM
    gfx_fb_t *fb = (gfx_fb_t *)duo_eink_fb(d);
    if (fb) {
        gfx_fill_rect(fb, 0, 0, ew, eh, 0);
        gfx_draw_rect(fb, 2, 2, ew - 4, eh - 4, 1, 1);
        gfx_draw_text_scaled(fb, 10, 8, "SYSTEM", font, 1, 2);
        gfx_draw_line(fb, 6, 30, ew - 6, 30, 1);
        char t[8], dt[16];
        clock_str(t, sizeof(t), dt, sizeof(dt));
        char line[48];
        snprintf(line, sizeof(line), "Time: %s  %s", dt, t);
        gfx_draw_text(fb, 10, 42, line, font, 1);
        snprintf(line, sizeof(line), "Apps in /bin: %d", napps);
        gfx_draw_text(fb, 10, 56, line, font, 1);
        gfx_draw_text(fb, 10, 70, "micro_arch  v0.2.0", font, 1);
        gfx_draw_text(fb, 10, eh - 14, "ESC to go back", font, 1);
        duo_eink_commit(d);
    }
    duo_message(d, "SYSTEM");
}

static void launch(const char *name) { char *av[1] = { (char *)name }; app_run(name, 1, av); }

int main(int argc, char *argv[])
{
    for (int i = 0; i < NDRV; i++) drv_labels[i] = drvs[i].label;

    d = duo_open();
    if (!d) { vconsole_printf("home: display init failed\n"); return 1; }
    font = gfx_font_get("5x7");
    ew = duo_eink_w(d); eh = duo_eink_h(d);
    napps = app_list(apps, 24);

    vconsole_printf("home: wall ready. arrows move, ENTER open, letter=shortcut, ESC.\n");
    render();

    for (;;) {
        int k = input_get_key(1000000);
        if (k < 0) continue;

        if (screen != SC_WALL) {                        // sub-screens
            if (k == 0x1B) { screen = SC_WALL; render(); }
            else if (screen == SC_DRIVERS) {
                if (k == 'w' || k == 0xB5) { sel = (sel + NDRV - 1) % NDRV; render(); }
                else if (k == 's' || k == 0xB6) { sel = (sel + 1) % NDRV; render(); }
                else if (k == 0x0D || k == 0x0A) {
                    char cmd[96];
                    snprintf(cmd, sizeof(cmd), "modload %s", drvs[sel].path);  shell_exec(cmd);
                    snprintf(cmd, sizeof(cmd), "modstart %s", drvs[sel].name);
                    duo_message(d, shell_exec(cmd) == 0 ? "OK started" : "start failed");
                }
            }
            continue;
        }

        // --- the wall ---
        if (k == 0x1B) break;                            // exit
        if (k == 'R' || k == 'r') { screen = SC_DRIVERS; sel = 0; render(); continue; }
        if (k == 'Y' || k == 'y') { screen = SC_SYSTEM;  render(); continue; }
        if (napps == 0) continue;

        if (k == 0xB4 || k == 'a')      { if (sel > 0) sel--; render(); }          // left
        else if (k == 0xB7 || k == 'd') { if (sel < napps - 1) sel++; render(); }  // right
        else if (k == 0xB5)             { if (sel >= COLS) sel -= COLS; render(); }// up
        else if (k == 0xB6)             { if (sel + COLS < napps) sel += COLS; render(); } // down
        else if (k == 0x0D || k == 0x0A) { launch(apps[sel]); render(); }          // ENTER
        else if ((k >= 'a' && k <= 'z') || (k >= 'A' && k <= 'Z')) {
            char up = (k >= 'a') ? k - 32 : k;           // first-letter shortcut
            for (int i = 0; i < napps; i++)
                if (up_first(apps[i]) == up) { sel = i; launch(apps[i]); render(); break; }
        }
    }

    duo_close(d);
    vconsole_printf("home: bye\n");
    return 0;
}
