// menu — a graphical application launcher.
//
// The e-ink shows the apps in /sdcard/bin as icons (a placeholder box with the
// app number, plus its name); the OLED is the number input line. Press a digit
// (1-9) to run that app — e.g. "3" runs app #3. The launcher releases both
// displays while the app runs, then re-grabs and redraws when it returns.
// ESC / q quits.
//
// Icons are placeholders (numbered boxes) for now — real per-app bitmaps later.

#include "gfx_abi.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>

extern int vconsole_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
extern int input_get_key(uint32_t timeout_ms);
extern int display_mux_grab(const char *name);
extern int display_mux_release(const char *name);
extern int display_mux_present(const char *name, const gfx_fb_t *fb);
extern int display_mux_render_text(const char *name, const char *text, size_t len);
extern int display_mux_dims(const char *name, int *rows, int *cols);
extern int app_list(char (*names)[32], int max);      // APP_NAME_MAX == 32
extern int app_run(const char *name, int argc, char **argv);
extern int snprintf(char *str, size_t size, const char *fmt, ...);

#define EINK      "eink"
#define OLED      "oled"
#define MAX_APPS   24
#define NAME_LEN   32
#define ICON       28    // icon box size (px)
#define ROW_H      34    // vertical pitch between icons
#define LIST_Y0    16    // first icon row y

static char apps[MAX_APPS][NAME_LEN];
static int  napps;
static int  have_eink, have_oled;

static gfx_fb_t         *efb;     // e-ink graphics framebuffer
static const gfx_font_t *font;
static int  ew, eh;               // e-ink logical pixel size

// A placeholder app icon: an outlined box with the app number, name alongside.
static void draw_icon(int x, int y, char num, const char *name)
{
    gfx_fill_rect(efb, x, y, ICON, 1, 1);              // top
    gfx_fill_rect(efb, x, y + ICON - 1, ICON, 1, 1);   // bottom
    gfx_fill_rect(efb, x, y, 1, ICON, 1);              // left
    gfx_fill_rect(efb, x + ICON - 1, y, 1, ICON, 1);   // right
    gfx_draw_char(efb, x + ICON / 2 - 2, y + ICON / 2 - 3, num, font, 1);
    gfx_draw_text(efb, x + ICON + 6, y + ICON / 2 - 3, name, font, 1);
}

static int visible_rows(void)
{
    int n = (eh - LIST_Y0) / ROW_H;
    if (n > 9) n = 9;              // only 1-9 are digit-selectable
    if (n > napps) n = napps;
    return n;
}

static void draw_eink(void)
{
    if (!efb) return;
    gfx_fill_rect(efb, 0, 0, ew, eh, 0);               // clear to white
    gfx_draw_text(efb, 2, 0, "Applications", font, 1);
    gfx_fill_rect(efb, 0, 10, ew, 1, 1);               // rule

    if (napps == 0) {
        gfx_draw_text(efb, 4, LIST_Y0, "(no apps in /bin)", font, 1);
    } else {
        int rows = visible_rows();
        for (int i = 0; i < rows; i++)
            draw_icon(4, LIST_Y0 + i * ROW_H, (char)('1' + i), apps[i]);
    }
    display_mux_present(EINK, efb);
}

// Text fallback used when there is no e-ink.
static void draw_list_text(void)
{
    char buf[512];
    int n = snprintf(buf, sizeof(buf), "Apps (%d):\n", napps);
    for (int i = 0; i < napps && i < 9 && n < (int)sizeof(buf) - 40; i++)
        n += snprintf(buf + n, sizeof(buf) - n, "%d %s\n", i + 1, apps[i]);
    display_mux_render_text(OLED, buf, (size_t)n);
}

static void show_prompt(int running)
{
    char buf[40];
    int n = running
          ? snprintf(buf, sizeof(buf), "running (%d apps)", napps)
          : snprintf(buf, sizeof(buf), "pick 1-%d:", napps < 9 ? napps : 9);
    display_mux_render_text(OLED, buf, (size_t)n);
}

static void redraw(void)
{
    if (have_eink) draw_eink();
    else if (have_oled) draw_list_text();
    if (have_oled) show_prompt(0);
}

int main(int argc, char *argv[])
{
    have_oled = (display_mux_grab(OLED) == 0);
    have_eink = (display_mux_grab(EINK) == 0);
    if (!have_oled && !have_eink) {
        vconsole_printf("menu: no display — load 'oled'/'eink' first\n");
        return 1;
    }

    font = gfx_font_get("5x7");
    if (have_eink) {
        int rows = 0, cols = 0;
        display_mux_dims(EINK, &rows, &cols);
        ew = cols > 0 ? cols * 6 : 240;
        eh = rows > 0 ? rows * 8 : 360;
        efb = gfx_fb_alloc((uint16_t)ew, (uint16_t)eh, GFX_FMT_MONO_HMSB, 0);
        if (!efb) have_eink = 0;   // fall back to text on OLED
    }

    napps = app_list(apps, MAX_APPS);
    redraw();
    vconsole_printf("menu: press 1-9 to run an app, ESC to quit\n");

    for (;;) {
        int k = input_get_key(1000000);
        if (k < 0) continue;

        if (k == 0x1B || k == 'q') break;              // ESC / q -> quit

        if (k >= '1' && k <= '9') {
            int idx = k - '1';
            if (idx >= napps) continue;

            if (have_oled) show_prompt(1);
            if (have_oled) display_mux_release(OLED);
            if (have_eink) display_mux_release(EINK);

            char *av[1] = { apps[idx] };
            app_run(apps[idx], 1, av);

            if (have_oled) display_mux_grab(OLED);
            if (have_eink) display_mux_grab(EINK);
            napps = app_list(apps, MAX_APPS);
            redraw();
        }
    }

    if (efb) gfx_fb_free(efb);
    if (have_oled) { display_mux_render_text(OLED, "", 0); display_mux_release(OLED); }
    if (have_eink) display_mux_release(EINK);
    vconsole_printf("menu: bye\n");
    return 0;
}
