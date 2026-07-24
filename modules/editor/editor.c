// editor — a dual-display text editor.
//
//   editor <path>      edit (or create) a text file
//
// e-ink = the whole document (line-numbered, an overview that refreshes only on
// structural changes / scroll — the slow panel stays calm). OLED = the current
// line being edited, with a live cursor (fast). Type to edit; ENTER splits a
// line; BACKSPACE deletes / merges; arrow keys move the cursor; ESC saves & quits.
//
// Best with the CardKB (arrow keys). Uses the kernel file + wrapped-text helpers.

#include "gfx_abi.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>

extern int   vconsole_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
extern int   input_get_key(uint32_t timeout_ms);
extern int   snprintf(char *str, size_t size, const char *fmt, ...);
extern int   app_read_file(const char *path, char *buf, int max);
extern int   app_write_file(const char *path, const char *buf, int len);

typedef struct duo duo_t;
extern duo_t *duo_open(void);
extern void   duo_close(duo_t *d);
extern void   duo_message(duo_t *d, const char *text);
extern void  *duo_eink_fb(duo_t *d);
extern void  *duo_oled_fb(duo_t *d);
extern int    duo_eink_w(duo_t *d);
extern int    duo_eink_h(duo_t *d);
extern void   duo_eink_commit(duo_t *d);
extern void   duo_oled_commit(duo_t *d);

#define MAXLINES  128
#define MAXCOL    120
#define OLED_SCALE  2
#define OLED_COLS  (128 / (6 * OLED_SCALE))            // chars visible on the OLED line

static char lines[MAXLINES][MAXCOL];
static int  nlines, cy, cx, scroll, dirty;
static char filepath[128];

static duo_t            *d;
static gfx_fb_t         *efb, *ofb;
static const gfx_font_t *font;
static int  ew, eh, doc_rows;

static int line_len(int i) { return (int)strlen(lines[i]); }

// ---- load / save ---------------------------------------------------------
static void load(void)
{
    static char buf[8192];
    int n = app_read_file(filepath, buf, sizeof(buf) - 1);
    nlines = 0; cx = cy = scroll = 0; dirty = 0;
    if (n <= 0) { lines[0][0] = '\0'; nlines = 1; return; }
    buf[n] = '\0';

    int li = 0, ci = 0;
    for (int i = 0; i < n && li < MAXLINES; i++) {
        char c = buf[i];
        if (c == '\r') continue;
        if (c == '\n') { lines[li][ci] = '\0'; li++; ci = 0; }
        else if (ci < MAXCOL - 1) lines[li][ci++] = c;
    }
    if (li < MAXLINES && (ci > 0 || li == 0)) { lines[li][ci] = '\0'; li++; }
    nlines = li;
}

static void save(void)
{
    static char buf[8192];
    int n = 0;
    for (int i = 0; i < nlines && n < (int)sizeof(buf) - 2; i++) {
        int len = line_len(i);
        if (n + len + 1 >= (int)sizeof(buf)) len = sizeof(buf) - 2 - n;
        memcpy(buf + n, lines[i], len); n += len;
        if (i < nlines - 1) buf[n++] = '\n';
    }
    int r = app_write_file(filepath, buf, n);
    if (r == 0) dirty = 0;
    duo_message(d, r == 0 ? "Saved" : "Save failed");
}

// ---- rendering -----------------------------------------------------------
static void draw_doc(void)
{
    if (!efb) return;
    gfx_fill_rect(efb, 0, 0, ew, eh, 0);
    gfx_draw_rect(efb, 0, 0, ew, eh, 1, 1);

    gfx_fill_rect(efb, 2, 2, ew - 4, 12, 1);           // title bar
    char title[64];
    snprintf(title, sizeof(title), "%s%s", dirty ? "*" : " ", filepath);
    gfx_draw_text(efb, 4, 4, title, font, 0);

    for (int r = 0; r < doc_rows; r++) {
        int i = scroll + r;
        if (i >= nlines) break;
        char row[80];
        snprintf(row, sizeof(row), "%2d %s", i + 1, lines[i]);
        gfx_draw_text(efb, 4, 18 + r * 10, row, font, 1);
    }
    gfx_draw_text(efb, 4, eh - 11, "type edit  arrows move  ESC save+quit", font, 1);
    duo_eink_commit(d);
}

static void draw_line(void)
{
    if (!ofb) return;
    gfx_fill_rect(ofb, 0, 0, 128, 64, 0);

    char hdr[24];
    snprintf(hdr, sizeof(hdr), "L%d/%d  c%d", cy + 1, nlines, cx + 1);
    gfx_draw_text(ofb, 2, 2, hdr, font, 1);
    gfx_draw_line(ofb, 0, 14, 128, 14, 1);

    int hs = 0;                                        // horizontal scroll to show cursor
    if (cx >= OLED_COLS) hs = cx - OLED_COLS + 1;
    char win[OLED_COLS + 1];
    int len = line_len(cy), k = 0;
    for (int i = hs; i < len && k < OLED_COLS; i++) win[k++] = lines[cy][i];
    win[k] = '\0';
    gfx_draw_text_scaled(ofb, 2, 24, win, font, 1, OLED_SCALE);

    int caret = (cx - hs) * (6 * OLED_SCALE) + 2;      // cursor bar
    gfx_draw_line(ofb, caret, 22, caret, 24 + 7 * OLED_SCALE, 1);
    duo_oled_commit(d);
}

// Keep the cursor line on-screen; returns 1 if the e-ink page scrolled.
static int fix_scroll(void)
{
    int old = scroll;
    if (cy < scroll)               scroll = cy;
    if (cy >= scroll + doc_rows)   scroll = cy - doc_rows + 1;
    return scroll != old;
}

static void refresh(int structural)
{
    if (structural | fix_scroll()) draw_doc();          // e-ink only when needed
    draw_line();                                        // OLED every time (fast)
}

// ---- editing operations --------------------------------------------------
static void insert_char(char c)
{
    int len = line_len(cy);
    if (len >= MAXCOL - 1) return;
    memmove(&lines[cy][cx + 1], &lines[cy][cx], len - cx + 1);
    lines[cy][cx] = c;
    cx++; dirty = 1;
    refresh(0);
}

static void newline(void)
{
    if (nlines >= MAXLINES) return;
    memmove(&lines[cy + 2], &lines[cy + 1], (nlines - cy - 1) * MAXCOL);
    strcpy(lines[cy + 1], &lines[cy][cx]);              // tail -> new line
    lines[cy][cx] = '\0';
    nlines++; cy++; cx = 0; dirty = 1;
    refresh(1);
}

static void backspace(void)
{
    if (cx > 0) {
        int len = line_len(cy);
        memmove(&lines[cy][cx - 1], &lines[cy][cx], len - cx + 1);
        cx--; dirty = 1;
        refresh(0);
    } else if (cy > 0) {                                // merge into previous line
        int plen = line_len(cy - 1);
        if (plen + line_len(cy) < MAXCOL - 1) {
            strcpy(&lines[cy - 1][plen], lines[cy]);
            memmove(&lines[cy], &lines[cy + 1], (nlines - cy - 1) * MAXCOL);
            nlines--; cy--; cx = plen; dirty = 1;
            refresh(1);
        }
    }
}

int main(int argc, char *argv[])
{
    snprintf(filepath, sizeof(filepath), "%s",
             (argc >= 2 && argv[1]) ? argv[1] : "/sdcard/home/untitled.txt");

    d = duo_open();
    if (!d) { vconsole_printf("editor: display init failed\n"); return 1; }
    efb = (gfx_fb_t *)duo_eink_fb(d);
    ofb = (gfx_fb_t *)duo_oled_fb(d);
    ew = duo_eink_w(d); eh = duo_eink_h(d);
    doc_rows = (eh - 30) / 10; if (doc_rows < 1) doc_rows = 1;
    font = gfx_font_get("5x7");

    load();
    draw_doc();
    draw_line();
    vconsole_printf("editor: %s  (type, ENTER, BKSP, arrows, ESC=save+quit)\n", filepath);

    for (;;) {
        int k = input_get_key(1000000);
        if (k < 0) continue;

        if (k == 0x1B) { if (dirty) save(); break; }    // ESC = save & quit
        else if (k == 0x0D || k == 0x0A) newline();
        else if (k == 0x08 || k == 0x7F) backspace();
        else if (k == 0xB4) { if (cx > 0) cx--; else if (cy > 0) { cy--; cx = line_len(cy); } refresh(0); }  // left
        else if (k == 0xB7) { if (cx < line_len(cy)) cx++; else if (cy < nlines - 1) { cy++; cx = 0; } refresh(0); } // right
        else if (k == 0xB5) { if (cy > 0) { cy--; if (cx > line_len(cy)) cx = line_len(cy); } refresh(0); }  // up
        else if (k == 0xB6) { if (cy < nlines - 1) { cy++; if (cx > line_len(cy)) cx = line_len(cy); } refresh(0); } // down
        else if (k >= 0x20 && k < 0x7F) insert_char((char)k);
    }

    duo_close(d);
    vconsole_printf("editor: bye\n");
    return 0;
}
