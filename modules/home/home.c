// home — the two-page home screen.
//
// Page 0 (HOME): a wallpaper + widgets only (a big live clock, a weather
// placeholder) — like a phone home screen. Page 1 (APPS): the scrollable grid of
// app tiles with letter shortcuts. Arrow DOWN from HOME opens APPS; arrow UP at
// the top of APPS returns to HOME. ENTER / a letter launches an app; launched
// apps return here. ESC exits.
//
// On HOME the OLED shows the live ticking time; the e-ink widgets refresh once a
// minute. On APPS the OLED shows the highlighted app.

#include "gfx_abi.h"
#include "rtc_service.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>

extern int   vconsole_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
extern int   input_get_key(uint32_t timeout_ms);
extern int   snprintf(char *str, size_t size, const char *fmt, ...);
extern int   app_list(char (*names)[32], int max);
extern int   app_run(const char *name, int argc, char **argv);
extern int   app_read_file(const char *path, char *buf, int max);
extern int   app_write_file(const char *path, const char *buf, int len);
extern void *registry_vtable(const char *name);

#define LAYOUT_PATH "/sys/etc/home.conf"      // widget order, editable on-screen

// The weather app caches its last reading here (temp_tenths;code;wind_tenths;loc).
#define WEATHER_CACHE "/sys/cache/weather.txt"

typedef struct duo duo_t;
extern duo_t *duo_open(void);
extern void   duo_close(duo_t *d);
extern void  *duo_eink_fb(duo_t *d);
extern void  *duo_oled_fb(duo_t *d);
extern int    duo_eink_w(duo_t *d);
extern int    duo_eink_h(duo_t *d);
extern void   duo_eink_commit(duo_t *d);
extern void   duo_oled_commit(duo_t *d);
extern void   duo_message(duo_t *d, const char *text);

enum { PAGE_HOME, PAGE_APPS };
#define COLS 2

static char apps[24][32];
static int  napps;

static duo_t            *d;
static gfx_fb_t         *efb, *ofb;
static const gfx_font_t *font;
static int  ew, eh, page = PAGE_HOME, sel = 0, scroll = 0, last_min = -1;

static const char *const WD[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };

static int get_time(rtc_time_t *t)
{
    const rtc_ops_t *rtc = (const rtc_ops_t *)registry_vtable("rtc");
    return (rtc && rtc->get(t) == 0) ? 0 : -1;
}

static char up_first(const char *s) { char c = s[0]; return (c >= 'a' && c <= 'z') ? c - 32 : c; }

// Read the weather cache written by the weather app ("temp10;code;..."). 1 if ok.
static int read_weather(int *temp10, int *code)
{
    char buf[80];
    int n = app_read_file(WEATHER_CACHE, buf, sizeof(buf) - 1);
    if (n <= 0) return 0;
    buf[n] = '\0';
    const char *p = buf;
    int t = 0, c = 0, sign = 1;
    if (*p == '-') { sign = -1; p++; }
    while (*p >= '0' && *p <= '9') t = t * 10 + (*p++ - '0');
    if (*p == ';') p++;
    while (*p >= '0' && *p <= '9') c = c * 10 + (*p++ - '0');
    *temp10 = t * sign; *code = c;
    return 1;
}

static const char *wcond(int code)
{
    if (code == 0)  return "Clear";
    if (code <= 3)  return "Cloudy";
    if (code <= 48) return "Fog";
    if (code <= 67) return "Rain";
    if (code <= 77) return "Snow";
    if (code <= 82) return "Showers";
    return "Storm";
}

// ---- widgets -------------------------------------------------------------
static void bevel(gfx_fb_t *fb, int x, int y, int w, int h, int cut)
{
    int r = x + w - 1, b = y + h - 1;
    gfx_draw_line(fb, x + cut, y, r - cut, y, 1);
    gfx_draw_line(fb, x + cut, b, r - cut, b, 1);
    gfx_draw_line(fb, x, y + cut, x, b - cut, 1);
    gfx_draw_line(fb, r, y + cut, r, b - cut, 1);
    gfx_draw_line(fb, x, y + cut, x + cut, y, 1);
    gfx_draw_line(fb, r - cut, y, r, y + cut, 1);
    gfx_draw_line(fb, x, b - cut, x + cut, b, 1);
    gfx_draw_line(fb, r - cut, b, r, b - cut, 1);
}

// ---- individual widgets (draw into a given box) --------------------------
static void w_clock(int x, int y, int w, int h)
{
    bevel(efb, x, y, w, h, 8);
    rtc_time_t t; int have = (get_time(&t) == 0);
    char s[32];
    if (have) snprintf(s, sizeof(s), "%s  %04d-%02d-%02d", WD[t.wday & 7], t.year, t.month, t.day);
    else      snprintf(s, sizeof(s), "no clock");
    gfx_draw_text(efb, x + 12, y + 10, s, font, 1);
    if (have) snprintf(s, sizeof(s), "%02d:%02d", t.hour, t.min);
    else      snprintf(s, sizeof(s), "--:--");
    int tw = (int)strlen(s) * 6 * 4;
    gfx_draw_text_scaled(efb, x + (w - tw) / 2, y + 30, s, font, 1, 4);
}

static void w_weather(int x, int y, int w, int h)
{
    bevel(efb, x, y, w, h, 8);
    gfx_draw_text(efb, x + 12, y + 8, "WEATHER", font, 1);
    int sx = x + 28, sy = y + 44;
    gfx_draw_circle(efb, sx, sy, 10, 0, 1);
    for (int a = 0; a < 8; a++) {
        static const int rx[] = { 1, 1, 0, -1, -1, -1, 0, 1 };
        static const int ry[] = { 0, 1, 1, 1, 0, -1, -1, -1 };
        gfx_draw_line(efb, sx + rx[a] * 14, sy + ry[a] * 14, sx + rx[a] * 19, sy + ry[a] * 19, 1);
    }
    int t10, wcode;
    if (read_weather(&t10, &wcode)) {
        char s[16]; int i = t10 / 10, f = t10 % 10; if (f < 0) f = -f;
        snprintf(s, sizeof(s), "%d.%d C", i, f);
        gfx_draw_text_scaled(efb, x + w - 100, y + 26, s, font, 1, 3);
        gfx_draw_text(efb, x + w - 100, y + 54, wcond(wcode), font, 1);
    } else {
        gfx_draw_text_scaled(efb, x + w - 90, y + 30, "-- C", font, 1, 3);
        gfx_draw_text(efb, x + w - 96, y + 56, "run weather", font, 1);
    }
}

typedef void (*wdraw_t)(int, int, int, int);
typedef struct { const char *id; int h; wdraw_t draw; } widget_t;
static const widget_t WIDGETS[] = {
    { "clock",   92, w_clock   },
    { "weather", 76, w_weather },
};
#define NWIDGETS ((int)(sizeof(WIDGETS) / sizeof(WIDGETS[0])))

static int  layout[8], nlayout;    // ordered widget indices (top -> bottom)
static int  editing, pick;         // layout edit mode + picked slot

static int widget_by_id(const char *id, int len)
{
    for (int i = 0; i < NWIDGETS; i++)
        if ((int)strlen(WIDGETS[i].id) == len && memcmp(WIDGETS[i].id, id, len) == 0) return i;
    return -1;
}

static void load_layout(void)
{
    nlayout = 0;
    char buf[80];
    int n = app_read_file(LAYOUT_PATH, buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        int s = 0;
        for (int i = 0; i <= n && nlayout < 8; i++) {
            if (buf[i] == ',' || buf[i] == '\n' || buf[i] == '\0') {
                int wi = widget_by_id(buf + s, i - s);
                if (wi >= 0) layout[nlayout++] = wi;
                s = i + 1;
            }
        }
    }
    if (nlayout == 0)                                  // default: registry order
        for (int i = 0; i < NWIDGETS; i++) layout[nlayout++] = i;
}

static void save_layout(void)
{
    char buf[80]; int n = 0;
    for (int i = 0; i < nlayout && n < (int)sizeof(buf) - 12; i++)
        n += snprintf(buf + n, sizeof(buf) - n, "%s%s", i ? "," : "", WIDGETS[layout[i]].id);
    buf[n++] = '\n';
    app_write_file(LAYOUT_PATH, buf, n);
}

static void draw_home_eink(void)
{
    if (!efb) return;
    gfx_fill_rect(efb, 0, 0, ew, eh, 0);
    for (int y = 8; y < eh; y += 16)                   // wallpaper: small crosses
        for (int x = 8; x < ew; x += 16) {
            gfx_set_pixel(efb, x, y - 1, 1); gfx_set_pixel(efb, x, y + 1, 1);
            gfx_set_pixel(efb, x - 1, y, 1); gfx_set_pixel(efb, x + 1, y, 1);
        }
    gfx_draw_rect(efb, 1, 1, ew - 2, eh - 2, 1, 1);

    int wx = 10, ww = ew - 20, yy = 24;
    for (int i = 0; i < nlayout; i++) {
        const widget_t *wg = &WIDGETS[layout[i]];
        gfx_fill_rect(efb, wx, yy, ww, wg->h, 0);      // opaque over wallpaper
        wg->draw(wx, yy, ww, wg->h);
        if (editing && i == pick)                      // highlight the picked widget
            gfx_draw_rect(efb, wx - 2, yy - 2, ww + 4, wg->h + 4, 2, 1);
        yy += wg->h + 12;
    }

    if (editing) gfx_draw_text(efb, (ew - 30 * 6) / 2, eh - 14,
                               "EDIT: up/down move  ESC save+exit", font, 1);
    else         gfx_draw_text(efb, (ew - 24 * 6) / 2, eh - 14,
                               "v apps   e edit layout", font, 1);
    duo_eink_commit(d);
}

// e-ink: the app-tile grid (page APPS)
static void draw_apps_eink(void)
{
    if (!efb) return;
    gfx_fill_rect(efb, 0, 0, ew, eh, 0);
    gfx_draw_rect(efb, 0, 0, ew, eh, 1, 1);
    gfx_fill_rect(efb, 2, 2, ew - 4, 14, 1);
    gfx_draw_text_scaled(efb, 6, 3, "APPS", font, 0, 1);
    gfx_draw_text(efb, ew - 80, 5, "^ up = home", font, 0);

    int gx = 6, gy = 22, gap = 6;
    int tw = (ew - gx - gap - gx) / COLS, th = 42;
    int rows_vis = (eh - gy - 14) / (th + gap); if (rows_vis < 1) rows_vis = 1;
    int selrow = sel / COLS;
    if (selrow < scroll)             scroll = selrow;
    if (selrow >= scroll + rows_vis) scroll = selrow - rows_vis + 1;

    for (int i = 0; i < napps; i++) {
        int r = i / COLS - scroll, c = i % COLS;
        if (r < 0 || r >= rows_vis) continue;
        int tx = gx + c * (tw + gap), ty = gy + r * (th + gap);
        gfx_fill_rect(efb, tx, ty, tw, th, 0);
        gfx_draw_rect(efb, tx, ty, tw, th, (i == sel) ? 3 : 1, 1);
        char sc[2] = { up_first(apps[i]), 0 };
        gfx_fill_rect(efb, tx + 3, ty + 3, 14, 14, 1);
        gfx_draw_text_scaled(efb, tx + 5, ty + 5, sc, font, 0, 1);
        gfx_draw_text(efb, tx + 4, ty + 24, apps[i], font, 1);
    }
    gfx_draw_text(efb, 6, eh - 11, "move ENTER open  letter=shortcut  ESC", font, 1);
    duo_eink_commit(d);
}

static void oled_clock(void)
{
    if (!ofb) return;
    gfx_fill_rect(ofb, 0, 0, 128, 64, 0);
    gfx_draw_rect(ofb, 0, 0, 128, 64, 1, 1);
    rtc_time_t t; char s[16];
    if (get_time(&t) == 0) snprintf(s, sizeof(s), "%02d:%02d:%02d", t.hour, t.min, t.sec);
    else                   snprintf(s, sizeof(s), "--:--:--");
    int w = (int)strlen(s) * 6 * 2;
    gfx_draw_text_scaled(ofb, (128 - w) / 2, 24, s, font, 1, 2);
    duo_oled_commit(d);
}

static void render(void)
{
    if (page == PAGE_HOME) { draw_home_eink(); oled_clock(); }
    else { draw_apps_eink(); duo_message(d, napps ? apps[sel] : "(no apps)"); }
}

static void launch(const char *name) { char *av[1] = { (char *)name }; app_run(name, 1, av); }

int main(int argc, char *argv[])
{
    d = duo_open();
    if (!d) { vconsole_printf("home: display init failed\n"); return 1; }
    efb = (gfx_fb_t *)duo_eink_fb(d);
    ofb = (gfx_fb_t *)duo_oled_fb(d);
    ew = duo_eink_w(d); eh = duo_eink_h(d);
    font = gfx_font_get("5x7");
    napps = app_list(apps, 24);
    load_layout();

    vconsole_printf("home: DOWN apps, e edit-layout, ENTER/letter launch, ESC exit.\n");
    render();

    for (;;) {
        int k = input_get_key(page == PAGE_HOME ? 1000 : 1000000);
        if (k < 0) {                                    // timeout — tick the clock
            if (page == PAGE_HOME) {
                oled_clock();                           // live seconds
                rtc_time_t t;
                if (get_time(&t) == 0 && t.min != last_min) { last_min = t.min; draw_home_eink(); }
            }
            continue;
        }

        if (page == PAGE_HOME) {
            if (editing) {                              // rearrange widgets
                if (k == 0x1B) { editing = 0; save_layout(); render(); }
                else if (k == 0xB4) { if (pick > 0) pick--; render(); }              // select prev
                else if (k == 0xB7) { if (pick < nlayout - 1) pick++; render(); }    // select next
                else if (k == 0xB5 && pick > 0) {                                    // move up
                    int t = layout[pick]; layout[pick] = layout[pick - 1]; layout[pick - 1] = t; pick--; render();
                }
                else if (k == 0xB6 && pick < nlayout - 1) {                          // move down
                    int t = layout[pick]; layout[pick] = layout[pick + 1]; layout[pick + 1] = t; pick++; render();
                }
                continue;
            }
            if (k == 0x1B) break;                       // ESC exit
            if (k == 'e' || k == 'E') { editing = 1; pick = 0; render(); }           // edit layout
            else if (k == 0xB6 || k == 's') { page = PAGE_APPS; sel = 0; scroll = 0; render(); }  // down -> apps
            continue;
        }

        // --- PAGE_APPS ---
        if (k == 0x1B) { page = PAGE_HOME; last_min = -1; render(); continue; }
        if (napps == 0) { if (k == 0xB5) { page = PAGE_HOME; last_min = -1; render(); } continue; }

        if (k == 0xB5 || k == 'w') {                    // up: home if on top row, else move
            if (sel < COLS) { page = PAGE_HOME; last_min = -1; render(); }
            else { sel -= COLS; render(); }
        }
        else if (k == 0xB6) { if (sel + COLS < napps) sel += COLS; render(); }          // down
        else if (k == 0xB4) { if (sel > 0) sel--; render(); }                           // left
        else if (k == 0xB7) { if (sel < napps - 1) sel++; render(); }                   // right
        else if (k == 0x0D || k == 0x0A) { launch(apps[sel]); render(); }               // ENTER
        else if ((k >= 'a' && k <= 'z') || (k >= 'A' && k <= 'Z')) {
            char up = (k >= 'a') ? k - 32 : k;
            for (int i = 0; i < napps; i++)
                if (up_first(apps[i]) == up) { sel = i; launch(apps[i]); render(); break; }
        }
    }

    duo_close(d);
    vconsole_printf("home: bye\n");
    return 0;
}
