// files — a dual-display file browser.
//
// e-ink = the file list (a page of entries; only redrawn when the page scrolls
// or you change directory, so the slow panel isn't thrashed). OLED = the active
// selection (name, type, position) updated live as you move. Folders show as
// [name], "‹up›" goes to the parent. ENTER: open a folder, run a .elf, or view a
// text file. ESC: leave the viewer / go up a directory / quit at the root.

#include "gfx_abi.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>

extern int   vconsole_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
extern int   input_get_key(uint32_t timeout_ms);
extern int   snprintf(char *str, size_t size, const char *fmt, ...);
extern int   app_list_dir(const char *path, char (*names)[48], uint8_t *is_dir, int max);
extern int   app_run(const char *name, int argc, char **argv);
extern int   app_read_file(const char *path, char *buf, int max);

typedef struct duo duo_t;
extern duo_t *duo_open(void);
extern void   duo_close(duo_t *d);
extern void   duo_message(duo_t *d, const char *text);
extern void  *duo_eink_fb(duo_t *d);
extern int    duo_eink_w(duo_t *d);
extern int    duo_eink_h(duo_t *d);
extern void   duo_eink_commit(duo_t *d);

#define MAXENT   64
#define ROOT     "/sdcard"

static char     names[MAXENT][48];
static uint8_t  is_dir[MAXENT];
static int      count, sel, scroll;
static char     path[128] = ROOT;

static duo_t            *d;
static const gfx_font_t *font;
static int  ew, eh;

enum { MODE_LIST, MODE_VIEW };
static int  mode = MODE_LIST;
static char viewbuf[2560];
static int  viewlen, view_line;

static int ends_with(const char *s, const char *suf)
{
    int ls = (int)strlen(s), lf = (int)strlen(suf);
    return ls >= lf && strcmp(s + ls - lf, suf) == 0;
}

static void reload(void)
{
    int c = app_list_dir(path, &names[1], &is_dir[1], MAXENT - 1);
    if (c < 0) c = 0;
    strcpy(names[0], "\x11 up");      // 0x11 as a little "up" glyph fallback
    is_dir[0] = 1;
    count = c + 1;
    sel = 0; scroll = 0;
}

static int list_rows(void) { int r = (eh - 32) / 12; return r < 1 ? 1 : r; }

// e-ink: current page of the list (redrawn only on scroll / dir change).
static void draw_list_eink(void)
{
    gfx_fb_t *fb = (gfx_fb_t *)duo_eink_fb(d);
    if (!fb) return;
    gfx_fill_rect(fb, 0, 0, ew, eh, 0);
    gfx_draw_rect(fb, 0, 0, ew, eh, 1, 1);

    gfx_fill_rect(fb, 2, 2, ew - 4, 12, 1);           // path bar
    gfx_draw_text(fb, 4, 4, path, font, 0);

    int y0 = 18, rh = 12, rows = list_rows();
    for (int r = 0; r < rows; r++) {
        int i = scroll + r;
        if (i >= count) break;
        char line[56];
        if (is_dir[i]) snprintf(line, sizeof(line), "[%s]", names[i]);
        else           snprintf(line, sizeof(line), " %s", names[i]);
        gfx_draw_text(fb, 6, y0 + r * rh, line, font, 1);
    }
    gfx_draw_text(fb, 4, eh - 11, "ENTER open   ESC up/quit", font, 1);
    duo_eink_commit(d);
}

// OLED: the live selection.
static void draw_oled(void)
{
    char buf[40];
    const char *type = (sel == 0) ? "PARENT" : is_dir[sel] ? "FOLDER" :
                       ends_with(names[sel], ".elf") ? "APP" : "FILE";
    snprintf(buf, sizeof(buf), "%s  %d/%d", type, sel + 1, count);
    duo_message(d, names[sel]);
    // second line via the console (OLED message is single big line); keep name big
    (void)buf;
}

static void refresh(int redraw_eink)
{
    int rows = list_rows(), old = scroll;
    if (sel < scroll)          scroll = sel;
    if (sel >= scroll + rows)  scroll = sel - rows + 1;
    if (redraw_eink || scroll != old) draw_list_eink();
    draw_oled();
}

static void go_up(void)
{
    if (strcmp(path, ROOT) == 0) return;
    int len = (int)strlen(path);
    while (len > 0 && path[len - 1] != '/') len--;
    if (len > 1) path[len - 1] = '\0'; else path[0] = '\0';
    if ((int)strlen(path) < (int)strlen(ROOT)) strcpy(path, ROOT);
    reload();
    refresh(1);
}

static void go_into(const char *name)
{
    char np[128];
    snprintf(np, sizeof(np), "%s/%s", path, name);
    if (strlen(np) < sizeof(path)) { strcpy(path, np); reload(); refresh(1); }
}

static void draw_view(void)
{
    gfx_fb_t *fb = (gfx_fb_t *)duo_eink_fb(d);
    if (!fb) return;
    gfx_fill_rect(fb, 0, 0, ew, eh, 0);
    int off = 0, line = 0;
    while (line < view_line && off < viewlen) { if (viewbuf[off] == '\n') line++; off++; }
    gfx_draw_text(fb, 3, 3, viewbuf + off, font, 1);   // wraps on \n, clips to fb
    duo_eink_commit(d);
    duo_message(d, "VIEW  ESC=back");
}

static void open_sel(void)
{
    if (sel == 0) { go_up(); return; }
    if (is_dir[sel]) { go_into(names[sel]); return; }

    char full[160];
    snprintf(full, sizeof(full), "%s/%s", path, names[sel]);
    if (ends_with(names[sel], ".elf")) {
        char *av[1] = { full };
        app_run(full, 1, av);                          // returns here on exit
        refresh(1);
    } else {
        // Text file: open in the editor; fall back to the inline viewer if the
        // editor app isn't installed.
        char *av[2] = { "editor", full };
        if (app_run("editor", 2, av) == -1000) {       // APP_NOT_FOUND
            viewlen = app_read_file(full, viewbuf, sizeof(viewbuf) - 1);
            if (viewlen < 0) viewlen = 0;
            viewbuf[viewlen] = '\0';
            view_line = 0;
            mode = MODE_VIEW;
            draw_view();
        } else {
            refresh(1);
        }
    }
}

int main(int argc, char *argv[])
{
    d = duo_open();
    if (!d) { vconsole_printf("files: display init failed\n"); return 1; }
    font = gfx_font_get("5x7");
    ew = duo_eink_w(d); eh = duo_eink_h(d);

    reload();
    refresh(1);
    vconsole_printf("files: browse %s. up/down move, ENTER open, ESC up/quit.\n", ROOT);

    for (;;) {
        int k = input_get_key(1000000);
        if (k < 0) continue;

        if (mode == MODE_VIEW) {
            if (k == 0x1B) { mode = MODE_LIST; refresh(1); }
            else if (k == 's' || k == 0xB6) { view_line++; draw_view(); }
            else if ((k == 'w' || k == 0xB5) && view_line > 0) { view_line--; draw_view(); }
            continue;
        }

        if (k == 0x1B) {                               // ESC: up a dir, or quit at root
            if (strcmp(path, ROOT) == 0) break;
            go_up();
        }
        else if (k == 'w' || k == 0xB5) { if (sel > 0) sel--; refresh(0); }
        else if (k == 's' || k == 0xB6) { if (sel < count - 1) sel++; refresh(0); }
        else if (k == 0x0D || k == 0x0A) open_sel();
    }

    duo_close(d);
    vconsole_printf("files: bye\n");
    return 0;
}
