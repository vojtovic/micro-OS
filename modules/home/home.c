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
// The todo app stores tasks here ("<0|1> text" per line); the todo widget reads it.
#define TODO_PATH "/sdcard/home/todo.txt"

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
static int  ew, eh, page = PAGE_HOME, sel = 0, scroll = 0, last_min = -1, boot_redraws = 2;

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

// ---- weather icon (drawn from a weather code) ----------------------------
static void draw_sun(int cx, int cy, int r)
{
    gfx_draw_circle(efb, cx, cy, r, 0, 1);
    static const int dx[] = { 1, 1, 0, -1, -1, -1, 0, 1 };
    static const int dy[] = { 0, 1, 1, 1, 0, -1, -1, -1 };
    for (int a = 0; a < 8; a++)
        gfx_draw_line(efb, cx + dx[a] * (r + 3), cy + dy[a] * (r + 3),
                      cx + dx[a] * (r + 7), cy + dy[a] * (r + 7), 1);
}

static void draw_cloud(int cx, int cy)   // solid puffy cloud, ~30 wide
{
    gfx_draw_circle(efb, cx - 9, cy + 2, 7, 1, 1);
    gfx_draw_circle(efb, cx + 9, cy + 2, 7, 1, 1);
    gfx_draw_circle(efb, cx,     cy - 3, 9, 1, 1);
    gfx_fill_rect(efb, cx - 9, cy + 2, 18, 7, 1);
}

// Draw a ~40x40 weather icon whose top-left is (x,y). code<0 = unknown.
static void draw_wicon(int x, int y, int code)
{
    int cx = x + 20, cy = y + 20;
    if (code < 0) { gfx_draw_text_scaled(efb, x + 12, y + 8, "?", font, 1, 3); return; }
    if (code == 0)               { draw_sun(cx, cy, 9); return; }              // clear
    if (code <= 3)               { draw_sun(cx - 6, cy - 6, 6); draw_cloud(cx + 4, cy + 6); return; } // partly
    if (code <= 48) {                                                          // fog
        for (int i = 0; i < 4; i++) gfx_draw_line(efb, x + 4, cy - 6 + i * 6, x + 34, cy - 6 + i * 6, 1);
        return;
    }
    draw_cloud(cx, cy - 4);
    if (code <= 67 || (code >= 80 && code <= 82)) {                            // rain
        for (int i = -1; i <= 1; i++) gfx_draw_line(efb, cx + i * 8, cy + 8, cx + i * 8 - 3, cy + 16, 1);
    } else if (code <= 77 || (code >= 85 && code <= 86)) {                     // snow
        for (int i = -1; i <= 1; i++) {
            int sx = cx + i * 9, sy = cy + 12;
            gfx_draw_line(efb, sx - 2, sy, sx + 2, sy, 1);
            gfx_draw_line(efb, sx, sy - 2, sx, sy + 2, 1);
        }
    } else {                                                                   // storm
        gfx_draw_line(efb, cx + 2, cy + 6, cx - 4, cy + 13, 1);
        gfx_draw_line(efb, cx - 4, cy + 13, cx + 2, cy + 13, 1);
        gfx_draw_line(efb, cx + 2, cy + 13, cx - 3, cy + 20, 1);
    }
}

// ---- individual widgets (draw into a given box) --------------------------
static void w_clock(int x, int y, int w, int h)
{
    bevel(efb, x, y, w, h, 6);
    rtc_time_t t; int have = (get_time(&t) == 0);
    if (have) {
        gfx_draw_text_scaled(efb, x + 12, y + 10, WD[t.wday & 7], font, 1, 2);
        char s[16]; snprintf(s, sizeof(s), "%02d.%02d.%04d", t.day, t.month, t.year);
        gfx_draw_text_scaled(efb, x + 12, y + 32, s, font, 1, 2);
    } else {
        gfx_draw_text(efb, x + 12, y + 22, "no clock", font, 1);
    }
}

static void w_weather(int x, int y, int w, int h)
{
    bevel(efb, x, y, w, h, 6);
    int t10, code, have = read_weather(&t10, &code);
    draw_wicon(x + 6, y + 8, have ? code : -1);
    if (have) {
        char s[16]; int i = t10 / 10, f = t10 % 10; if (f < 0) f = -f;
        snprintf(s, sizeof(s), "%d.%d", i, f);
        gfx_draw_text_scaled(efb, x + 52, y + 10, s, font, 1, 3);
        gfx_draw_text(efb, x + 52, y + 38, wcond(code), font, 1);
    } else {
        gfx_draw_text(efb, x + 52, y + 16, "no data", font, 1);
        gfx_draw_text(efb, x + 52, y + 32, "run weather", font, 1);
    }
}

// TO-DO widget: a peek at the open tasks from the todo app.
static void w_todo(int x, int y, int w, int h)
{
    bevel(efb, x, y, w, h, 6);
    gfx_draw_text_scaled(efb, x + 10, y + 6, "TO-DO", font, 1, 2);

    static char buf[1024];
    int n = app_read_file(TODO_PATH, buf, sizeof(buf) - 1);
    if (n <= 0) { gfx_draw_text(efb, x + 12, y + 30, "(no tasks)", font, 1); return; }
    buf[n] = '\0';

    int maxrows = (h - 28) / 12; if (maxrows < 1) maxrows = 1;
    int shown = 0, open = 0, total = 0, s = 0;
    for (int i = 0; i <= n; i++) {
        if (buf[i] == '\n' || buf[i] == '\0') {
            if (i > s) {
                buf[i] = '\0';
                const char *l = buf + s;
                int done = (l[0] == '1'), off = (l[1] == ' ') ? 2 : 0;
                total++;
                if (!done) {
                    open++;
                    if (shown < maxrows) {
                        int maxch = (w - 20) / 6 - 4; if (maxch < 4) maxch = 4;
                        char line[44];
                        snprintf(line, sizeof(line), "[ ] %.*s", maxch, l + off);
                        gfx_draw_text(efb, x + 10, y + 26 + shown * 12, line, font, 1);
                        shown++;
                    }
                }
            }
            s = i + 1;
        }
    }
    char hdr[16]; snprintf(hdr, sizeof(hdr), "%d/%d", total - open, total);
    gfx_draw_text(efb, x + w - 44, y + 8, hdr, font, 1);
    if (open == 0) gfx_draw_text(efb, x + 12, y + 26, "all done!", font, 1);
    else if (open > shown) {
        char more[16]; snprintf(more, sizeof(more), "+%d more", open - shown);
        gfx_draw_text(efb, x + 10, y + 26 + shown * 12, more, font, 1);
    }
}

typedef void (*wdraw_t)(int, int, int, int);
typedef struct { const char *id; int cols; int h; wdraw_t draw; } widget_t;
static const widget_t WIDGETS[] = {
    { "clock",   1, 62, w_clock   },
    { "weather", 1, 62, w_weather },
    { "todo",    2, 86, w_todo    },
};
#define NWIDGETS ((int)(sizeof(WIDGETS) / sizeof(WIDGETS[0])))

static int  layout[8], nlayout;    // ordered widget indices (top -> bottom)
static int  lcols[8];              // per-slot width in columns (1 = half, 2 = full)
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
                if (i > s) {                           // entry is "id" or "id:cols"
                    int idlen = i - s, c = 0;
                    for (int j = s; j < i; j++)
                        if (buf[j] == ':') {
                            idlen = j - s;
                            for (int k = j + 1; k < i; k++)
                                if (buf[k] >= '0' && buf[k] <= '9') c = c * 10 + (buf[k] - '0');
                            break;
                        }
                    int wi = widget_by_id(buf + s, idlen);
                    if (wi >= 0) {
                        layout[nlayout] = wi;
                        lcols[nlayout] = (c == 1 || c == 2) ? c : WIDGETS[wi].cols;
                        nlayout++;
                    }
                }
                s = i + 1;
            }
        }
    }
    // Append any registered widgets not already in the saved layout, so new
    // widgets (e.g. after an update) appear automatically instead of staying
    // hidden behind an old home.conf.
    for (int i = 0; i < NWIDGETS && nlayout < 8; i++) {
        int found = 0;
        for (int j = 0; j < nlayout; j++) if (layout[j] == i) { found = 1; break; }
        if (!found) { layout[nlayout] = i; lcols[nlayout] = WIDGETS[i].cols; nlayout++; }
    }
}

static void save_layout(void)
{
    char buf[96]; int n = 0;
    for (int i = 0; i < nlayout && n < (int)sizeof(buf) - 16; i++)
        n += snprintf(buf + n, sizeof(buf) - n, "%s%s:%d",
                      i ? "," : "", WIDGETS[layout[i]].id, lcols[i]);
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

    // 2-column flow layout: cols=1 widgets pack two-per-row (side by side),
    // cols=2 widgets take a full row.
    const int m = 10, gap = 8, top = 24;
    const int fullw = ew - 2 * m, halfw = (fullw - gap) / 2;
    int yy = top, slot = 0, rowh = 0;                  // slot 0 = left free, 1 = right free
    for (int i = 0; i < nlayout; i++) {
        const widget_t *wg = &WIDGETS[layout[i]];
        int wx, ww, wy;
        if (lcols[i] >= 2) {
            if (slot == 1) { yy += rowh + gap; slot = 0; rowh = 0; }   // close a half-row
            wx = m; ww = fullw; wy = yy;
            yy += wg->h + gap;
        } else if (slot == 0) {
            wx = m; ww = halfw; wy = yy; slot = 1; rowh = wg->h;
        } else {
            wx = m + halfw + gap; ww = halfw; wy = yy;
            if (wg->h > rowh) rowh = wg->h;
            yy += rowh + gap; slot = 0; rowh = 0;
        }
        gfx_fill_rect(efb, wx, wy, ww, wg->h, 0);      // opaque over wallpaper
        wg->draw(wx, wy, ww, wg->h);
        if (editing && i == pick)
            gfx_draw_rect(efb, wx - 2, wy - 2, ww + 4, wg->h + 4, 2, 1);
    }

    if (editing) gfx_draw_text(efb, (ew - 34 * 6) / 2, eh - 12,
                               "EDIT: L/R pick U/D move w width ESC", font, 1);
    else         gfx_draw_text(efb, (ew - 24 * 6) / 2, eh - 12,
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
                int have = (get_time(&t) == 0);
                // Force the wallpaper a few times right after start (heals a
                // boot-time race where the first e-ink commits didn't reach the
                // panel — the eink driver re-sends the full frame during its boot
                // heal window); after that, redraw only when the minute changes.
                if (boot_redraws > 0 || (have && t.min != last_min)) {
                    if (boot_redraws > 0) boot_redraws--;
                    last_min = have ? t.min : -1;
                    draw_home_eink();
                }
            }
            continue;
        }

        if (page == PAGE_HOME) {
            if (editing) {                              // rearrange widgets
                if (k == 0x1B) { editing = 0; save_layout(); render(); }
                else if (k == 0xB4) { if (pick > 0) pick--; render(); }              // select prev
                else if (k == 0xB7) { if (pick < nlayout - 1) pick++; render(); }    // select next
                else if (k == 'w' || k == 'W') {                                     // toggle width
                    lcols[pick] = (lcols[pick] == 1) ? 2 : 1; render();
                }
                else if (k == 0xB5 && pick > 0) {                                    // move up
                    int t = layout[pick]; layout[pick] = layout[pick - 1]; layout[pick - 1] = t;
                    int c = lcols[pick]; lcols[pick] = lcols[pick - 1]; lcols[pick - 1] = c;
                    pick--; render();
                }
                else if (k == 0xB6 && pick < nlayout - 1) {                          // move down
                    int t = layout[pick]; layout[pick] = layout[pick + 1]; layout[pick + 1] = t;
                    int c = lcols[pick]; lcols[pick] = lcols[pick + 1]; lcols[pick + 1] = c;
                    pick++; render();
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
