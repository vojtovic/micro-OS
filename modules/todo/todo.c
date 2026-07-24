// todo — a dual-display task list.
//
// e-ink = the task list ([x]/[ ] + text), redrawn only on a change / scroll.
// OLED = the current task (or the "new task" input line), updated live. Tasks
// persist in /sdcard/home/todo.txt as "<0|1> text" lines (1 = done).
//
// Keys: up/down move; ENTER/space toggles done; a = add a task (type on the
// OLED, ENTER adds, ESC cancels); d = delete; ESC = save & quit.

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

#define PATH    "/sdcard/home/todo.txt"
#define MAXT    64
#define MAXLEN  80

static char tasks[MAXT][MAXLEN];
static char done[MAXT];
static int  ntasks, sel, scroll;

static char newtask[MAXLEN];
static int  newlen, adding;

static duo_t            *d;
static gfx_fb_t         *efb, *ofb;
static const gfx_font_t *font;
static int  ew, eh, rows;

static void load(void)
{
    static char buf[MAXT * MAXLEN];
    int n = app_read_file(PATH, buf, sizeof(buf) - 1);
    ntasks = sel = scroll = 0;
    if (n <= 0) return;
    buf[n] = '\0';
    int s = 0;
    for (int i = 0; i <= n && ntasks < MAXT; i++) {
        if (buf[i] == '\n' || buf[i] == '\0') {
            if (i > s) {
                const char *l = buf + s;
                done[ntasks] = (l[0] == '1') ? 1 : 0;
                int off = (l[1] == ' ') ? 2 : 0;
                strncpy(tasks[ntasks], l + off, MAXLEN - 1);
                tasks[ntasks][(i - s > MAXLEN - 1) ? MAXLEN - 1 : i - s - off] = '\0';
                ntasks++;
            }
            s = i + 1;
        }
    }
}

static void save(void)
{
    static char buf[MAXT * MAXLEN];
    int n = 0;
    for (int i = 0; i < ntasks && n < (int)sizeof(buf) - MAXLEN - 4; i++)
        n += snprintf(buf + n, sizeof(buf) - n, "%d %s\n", done[i] ? 1 : 0, tasks[i]);
    app_write_file(PATH, buf, n);
}

static void draw_list(void)
{
    if (!efb) return;
    gfx_fill_rect(efb, 0, 0, ew, eh, 0);
    gfx_draw_rect(efb, 0, 0, ew, eh, 1, 1);
    gfx_fill_rect(efb, 2, 2, ew - 4, 14, 1);
    gfx_draw_text_scaled(efb, 6, 3, "TO-DO", font, 0, 1);
    char cnt[16]; int dn = 0;
    for (int i = 0; i < ntasks; i++) if (done[i]) dn++;
    snprintf(cnt, sizeof(cnt), "%d/%d", dn, ntasks);
    gfx_draw_text(efb, ew - 44, 5, cnt, font, 0);

    for (int r = 0; r < rows; r++) {
        int i = scroll + r;
        if (i >= ntasks) break;
        char line[64];
        snprintf(line, sizeof(line), "[%c] %s", done[i] ? 'x' : ' ', tasks[i]);
        gfx_draw_text(efb, 6, 22 + r * 12, line, font, 1);
    }
    if (ntasks == 0) gfx_draw_text(efb, 6, 24, "(no tasks — press 'a')", font, 1);
    gfx_draw_text(efb, 4, eh - 11, "ENTER done  a add  d del  ESC save", font, 1);
    duo_eink_commit(d);
}

static void draw_oled(void)
{
    if (!ofb) return;
    gfx_fill_rect(ofb, 0, 0, 128, 64, 0);
    gfx_draw_rect(ofb, 0, 0, 128, 64, 1, 1);

    if (adding) {
        gfx_draw_text(ofb, 4, 4, "New task:", font, 1);
        int hs = newlen > 18 ? newlen - 18 : 0;
        char win[20]; int k = 0;
        for (int i = hs; i < newlen && k < 19; i++) win[k++] = newtask[i];
        win[k++] = '_'; win[k] = '\0';
        gfx_draw_text(ofb, 4, 26, win, font, 1);
        gfx_draw_text(ofb, 4, 52, "ENTER add  ESC cancel", font, 1);
        duo_oled_commit(d);
        return;
    }
    if (ntasks == 0) { gfx_draw_text_scaled(ofb, 10, 24, "empty", font, 1, 2); duo_oled_commit(d); return; }

    char hdr[24];
    snprintf(hdr, sizeof(hdr), "%d/%d  [%c]", sel + 1, ntasks, done[sel] ? 'x' : ' ');
    gfx_draw_text(ofb, 4, 4, hdr, font, 1);
    gfx_draw_line(ofb, 0, 14, 128, 14, 1);
    // task text, wrapped-ish (two lines of ~20 chars)
    char l1[21], l2[21];
    int len = (int)strlen(tasks[sel]);
    int c = len < 20 ? len : 20; memcpy(l1, tasks[sel], c); l1[c] = '\0';
    gfx_draw_text_scaled(ofb, 4, 22, l1, font, 1, 1);
    if (len > 20) { int c2 = len - 20 < 20 ? len - 20 : 20; memcpy(l2, tasks[sel] + 20, c2); l2[c2] = '\0';
                    gfx_draw_text(ofb, 4, 34, l2, font, 1); }
    duo_oled_commit(d);
}

static void refresh(int structural)
{
    int old = scroll;
    if (sel < scroll)          scroll = sel;
    if (sel >= scroll + rows)  scroll = sel - rows + 1;
    if (structural || scroll != old) draw_list();
    draw_oled();
}

int main(int argc, char *argv[])
{
    d = duo_open();
    if (!d) { vconsole_printf("todo: display init failed\n"); return 1; }
    efb = (gfx_fb_t *)duo_eink_fb(d);
    ofb = (gfx_fb_t *)duo_oled_fb(d);
    ew = duo_eink_w(d); eh = duo_eink_h(d);
    rows = (eh - 34) / 12; if (rows < 1) rows = 1;
    font = gfx_font_get("5x7");

    load();
    draw_list();
    draw_oled();
    vconsole_printf("todo: %d tasks. up/down, ENTER toggle, a add, d del, ESC save.\n", ntasks);

    for (;;) {
        int k = input_get_key(1000000);
        if (k < 0) continue;

        if (adding) {                                  // typing a new task
            if (k == 0x1B) { adding = 0; draw_oled(); }                 // cancel
            else if (k == 0x0D || k == 0x0A) {
                if (newlen > 0 && ntasks < MAXT) {
                    newtask[newlen] = '\0';
                    strcpy(tasks[ntasks], newtask);
                    done[ntasks] = 0; ntasks++;
                    sel = ntasks - 1;
                }
                adding = 0; refresh(1);
            }
            else if ((k == 0x08 || k == 0x7F) && newlen > 0) { newlen--; draw_oled(); }
            else if (k >= 0x20 && k < 0x7F && newlen < MAXLEN - 1) { newtask[newlen++] = (char)k; draw_oled(); }
            continue;
        }

        if (k == 0x1B) { save(); break; }                              // save & quit
        else if (k == 0xB5) { if (sel > 0) sel--; refresh(0); }        // up
        else if (k == 0xB6) { if (sel < ntasks - 1) sel++; refresh(0); } // down
        else if (k == 0x0D || k == 0x0A || k == ' ') {                 // toggle done
            if (ntasks) { done[sel] = !done[sel]; refresh(1); }
        }
        else if (k == 'a' || k == 'A') { adding = 1; newlen = 0; draw_oled(); }  // add
        else if (k == 'd' || k == 'D') {                               // delete
            if (ntasks) {
                for (int i = sel; i < ntasks - 1; i++) { strcpy(tasks[i], tasks[i + 1]); done[i] = done[i + 1]; }
                ntasks--;
                if (sel >= ntasks && sel > 0) sel--;
                refresh(1);
            }
        }
    }

    duo_close(d);
    vconsole_printf("todo: saved & bye\n");
    return 0;
}
