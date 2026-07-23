// menu — a graphical application launcher, on the compositor.
//
// Draws the apps in /sdcard/bin as icons into a compositor window on the e-ink
// (placeholder = numbered box + name); the OLED is a text window showing the
// number prompt. Press a digit (1-9) to launch that app as a background task
// (session_spawn) — it takes focus, menu goes to the background (TAB returns to
// it). ESC / q quits.

#include "gfx_abi.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>

extern int vconsole_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
extern int input_get_key(uint32_t timeout_ms);
extern int display_mux_dims(const char *name, int *rows, int *cols);
extern int app_list(char (*names)[32], int max);
extern int session_spawn(const char *name);         // launch app as a task
extern int snprintf(char *str, size_t size, const char *fmt, ...);
// Compositor window API.
extern int   comp_open_gfx(const char *display, uint16_t w, uint16_t h, int fmt);
extern int   comp_open_text(const char *display);
extern void *comp_window_fb(int win);
extern void  comp_commit(int win);
extern void  comp_set_text(int win, const char *text, int len);
extern void  comp_close(int win);

#define EINK      "eink"
#define OLED      "oled"
#define MAX_APPS   24
#define NAME_LEN   32
#define ICON       28
#define ROW_H      34
#define LIST_Y0    16

static char apps[MAX_APPS][NAME_LEN];
static int  napps;

static int   eink_win = -1, oled_win = -1;
static gfx_fb_t         *efb;
static const gfx_font_t *font;
static int  ew, eh;

static void draw_icon(int x, int y, char num, const char *name)
{
    gfx_fill_rect(efb, x, y, ICON, 1, 1);
    gfx_fill_rect(efb, x, y + ICON - 1, ICON, 1, 1);
    gfx_fill_rect(efb, x, y, 1, ICON, 1);
    gfx_fill_rect(efb, x + ICON - 1, y, 1, ICON, 1);
    gfx_draw_char(efb, x + ICON / 2 - 2, y + ICON / 2 - 3, num, font, 1);
    gfx_draw_text(efb, x + ICON + 6, y + ICON / 2 - 3, name, font, 1);
}

static int visible_rows(void)
{
    int n = (eh - LIST_Y0) / ROW_H;
    if (n > 9)     n = 9;
    if (n > napps) n = napps;
    return n;
}

static void draw_eink(void)
{
    if (eink_win < 0 || !efb) return;
    gfx_fill_rect(efb, 0, 0, ew, eh, 0);
    gfx_draw_text(efb, 2, 0, "Applications", font, 1);
    gfx_fill_rect(efb, 0, 10, ew, 1, 1);
    if (napps == 0) {
        gfx_draw_text(efb, 4, LIST_Y0, "(no apps in /bin)", font, 1);
    } else {
        int rows = visible_rows();
        for (int i = 0; i < rows; i++)
            draw_icon(4, LIST_Y0 + i * ROW_H, (char)('1' + i), apps[i]);
    }
    comp_commit(eink_win);
}

static void show_prompt(char echo)
{
    char buf[40];
    int n;
    if (echo) n = snprintf(buf, sizeof(buf), "launch app #%c ...", echo);
    else      n = snprintf(buf, sizeof(buf), "pick 1-%d:", napps < 9 ? napps : 9);
    if (oled_win >= 0) comp_set_text(oled_win, buf, n);
}

int main(int argc, char *argv[])
{
    font = gfx_font_get("5x7");

    int rows = 0, cols = 0;
    display_mux_dims(EINK, &rows, &cols);
    ew = cols > 0 ? cols * 6 : 240;
    eh = rows > 0 ? rows * 8 : 360;

    eink_win = comp_open_gfx(EINK, (uint16_t)ew, (uint16_t)eh, GFX_FMT_MONO_HMSB);
    oled_win = comp_open_text(OLED);
    efb = (gfx_fb_t *)comp_window_fb(eink_win);

    napps = app_list(apps, MAX_APPS);
    draw_eink();
    show_prompt(0);
    vconsole_printf("menu: press 1-9 to launch an app, ESC to quit\n");

    for (;;) {
        int k = input_get_key(1000000);
        if (k < 0) continue;
        if (k == 0x1B || k == 'q') break;

        if (k >= '1' && k <= '9') {
            int idx = k - '1';
            if (idx >= napps) continue;
            show_prompt((char)k);
            if (session_spawn(apps[idx]) != 0)
                vconsole_printf("menu: failed to launch %s\n", apps[idx]);
            // The launched app now has focus; menu is backgrounded (TAB returns).
            show_prompt(0);
        }
    }

    comp_close(eink_win);
    comp_close(oled_win);
    vconsole_printf("menu: bye\n");
    return 0;
}
