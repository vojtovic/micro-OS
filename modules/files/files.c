// files — a dual-display file manager (Dolphin-style).
//
// e-ink = the directory listing (path bar + entries with [DIR] markers; redrawn
// only on scroll / dir change / after an operation, so the slow panel isn't
// thrashed). OLED = live details for the selected entry (name, type, size,
// position) and the interactive prompts (new-folder / rename input, delete
// confirm, clipboard status).
//
// Keys:
//   up/down      move            ENTER  open folder / run .elf / edit text
//   ESC          up a dir / quit g      toggle root  (SD <-> /sys)
//   n new folder r rename        d delete
//   c copy       x cut           v paste (into current dir)
//
// Copy/cut/paste works on files (any size — read into a PSRAM buffer). Moving or
// renaming a folder works within one filesystem; copying a folder or moving one
// across filesystems (SD <-> /sys) is not supported yet.

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
extern int   app_write_file(const char *path, const char *buf, int len);
extern int   app_mkdir(const char *path);
extern int   app_delete(const char *path);
extern int   app_rename(const char *from, const char *to);
extern int   app_stat(const char *path, int *is_dir, long *size);
extern void *heap_caps_malloc(size_t size, uint32_t caps);
extern void  heap_caps_free(void *p);

typedef struct duo duo_t;
extern duo_t *duo_open(void);
extern void   duo_close(duo_t *d);
extern void  *duo_eink_fb(duo_t *d);
extern void  *duo_oled_fb(duo_t *d);
extern int    duo_eink_w(duo_t *d);
extern int    duo_eink_h(duo_t *d);
extern void   duo_eink_commit(duo_t *d);
extern void   duo_oled_commit(duo_t *d);

#define MAXENT      96
#define MCAP_SPIRAM (1u << 10)

static const char *const ROOTS[] = { "/sdcard", "/sys" };
#define NROOTS ((int)(sizeof(ROOTS) / sizeof(ROOTS[0])))
static int cur_root = 0;

static char     names[MAXENT][48];
static uint8_t  is_dir[MAXENT];
static int      count, sel, scroll;
static char     path[160];

static char     clip[192];         // clipboard source path
static int      clip_op;           // 0 none, 1 copy, 2 cut
static char     status[40];        // one-line result shown on the OLED

static duo_t            *d;
static const gfx_font_t *font;
static int  ew, eh;

enum { MODE_LIST, MODE_VIEW, MODE_INPUT, MODE_CONFIRM };
static int  mode = MODE_LIST;

// text viewer (fallback when the editor app is missing)
static char viewbuf[2560];
static int  viewlen, view_line;

// text input (new folder / rename)
enum { PA_MKDIR, PA_RENAME };
static int  input_action;
static char input[48];
static int  input_len;

static int ends_with(const char *s, const char *suf)
{
    int ls = (int)strlen(s), lf = (int)strlen(suf);
    return ls >= lf && strcmp(s + ls - lf, suf) == 0;
}

static void join(char *dst, int n, const char *dir, const char *name)
{
    snprintf(dst, n, "%s/%s", dir, name);
}

static void fmt_size(long sz, char *out, int n)
{
    if (sz < 1024)            snprintf(out, n, "%ldB", sz);
    else if (sz < 1024L*1024) snprintf(out, n, "%ld.%ldK", sz / 1024, (sz % 1024) * 10 / 1024);
    else                      snprintf(out, n, "%ld.%ldM", sz / (1024L*1024),
                                       (sz % (1024L*1024)) * 10 / (1024L*1024));
}

static void reload(void)
{
    int c = app_list_dir(path, &names[1], &is_dir[1], MAXENT - 1);
    if (c < 0) c = 0;
    strcpy(names[0], "\x11 up");
    is_dir[0] = 1;
    count = c + 1;
    sel = 0; scroll = 0;
}

static int list_rows(void) { int r = (eh - 32) / 12; return r < 1 ? 1 : r; }

static int at_root(void) { return strcmp(path, ROOTS[cur_root]) == 0; }

// ---- e-ink: the directory listing --------------------------------------
static void draw_list_eink(void)
{
    gfx_fb_t *fb = (gfx_fb_t *)duo_eink_fb(d);
    if (!fb) return;
    gfx_fill_rect(fb, 0, 0, ew, eh, 0);
    gfx_draw_rect(fb, 0, 0, ew, eh, 1, 1);

    gfx_fill_rect(fb, 2, 2, ew - 4, 12, 1);            // path bar (inverted)
    gfx_draw_text(fb, 4, 4, path, font, 0);

    int y0 = 18, rh = 12, rows = list_rows();
    for (int r = 0; r < rows; r++) {
        int i = scroll + r;
        if (i >= count) break;
        char line[56];
        if (is_dir[i]) snprintf(line, sizeof(line), "[%s]", names[i]);
        else           snprintf(line, sizeof(line), " %s", names[i]);
        if (i == sel) {                                // highlight the selection
            gfx_fill_rect(fb, 3, y0 + r * rh - 1, ew - 6, rh, 1);
            gfx_draw_text(fb, 6, y0 + r * rh, line, font, 0);
        } else {
            gfx_draw_text(fb, 6, y0 + r * rh, line, font, 1);
        }
    }
    gfx_draw_text(fb, 4, eh - 11, "ENTER open  n r d c x v  g root  ESC", font, 1);
    duo_eink_commit(d);
}

// ---- OLED: live details / prompts --------------------------------------
static void oled_line(gfx_fb_t *fb, int y, const char *s, int scale)
{
    if (scale > 1) gfx_draw_text_scaled(fb, 4, y, s, font, 1, scale);
    else           gfx_draw_text(fb, 4, y, s, font, 1);
}

static void draw_oled(void)
{
    gfx_fb_t *fb = (gfx_fb_t *)duo_oled_fb(d);
    if (!fb) return;
    gfx_fill_rect(fb, 0, 0, 128, 64, 0);
    gfx_draw_rect(fb, 0, 0, 128, 64, 1, 1);

    if (mode == MODE_INPUT) {
        oled_line(fb, 4, input_action == PA_MKDIR ? "New folder:" : "Rename:", 1);
        char row[24]; int hs = input_len > 18 ? input_len - 18 : 0, k = 0;
        for (int i = hs; i < input_len && k < 19; i++) row[k++] = input[i];
        row[k++] = '_'; row[k] = '\0';
        oled_line(fb, 24, row, 1);
        oled_line(fb, 52, "ENTER ok  ESC cancel", 1);
        duo_oled_commit(d);
        return;
    }
    if (mode == MODE_CONFIRM) {
        oled_line(fb, 4, "Delete?", 1);
        char row[24]; snprintf(row, sizeof(row), "%.18s", names[sel]);
        oled_line(fb, 22, row, 1);
        oled_line(fb, 52, "ENTER yes  ESC no", 1);
        duo_oled_commit(d);
        return;
    }

    // normal: selected entry details
    char nm[22]; snprintf(nm, sizeof(nm), "%.20s", names[sel]);
    oled_line(fb, 3, nm, 1);
    gfx_draw_line(fb, 0, 13, 128, 13, 1);

    const char *type = (sel == 0) ? "PARENT" : is_dir[sel] ? "FOLDER" :
                       ends_with(names[sel], ".elf") ? "APP" : "FILE";
    char info[28];
    if (sel > 0 && !is_dir[sel]) {
        char full[200]; long sz = 0; int dir = 0;
        join(full, sizeof(full), path, names[sel]);
        char sb[16] = "?";
        if (app_stat(full, &dir, &sz) == 0) fmt_size(sz, sb, sizeof(sb));
        snprintf(info, sizeof(info), "%s  %s", type, sb);
    } else {
        snprintf(info, sizeof(info), "%s", type);
    }
    oled_line(fb, 18, info, 1);

    char pos[24]; snprintf(pos, sizeof(pos), "%d / %d", sel + 1, count);
    oled_line(fb, 32, pos, 1);

    if (clip_op) {
        char cb[26];
        const char *b = strrchr(clip, '/'); b = b ? b + 1 : clip;
        snprintf(cb, sizeof(cb), "%s %.16s", clip_op == 1 ? "copy" : "cut", b);
        oled_line(fb, 46, cb, 1);
    } else if (status[0]) {
        oled_line(fb, 46, status, 1);
    }
    duo_oled_commit(d);
}

static void refresh(int redraw_eink)
{
    int rows = list_rows(), old = scroll;
    if (sel < scroll)          scroll = sel;
    if (sel >= scroll + rows)  scroll = sel - rows + 1;
    if (redraw_eink || scroll != old) draw_list_eink();
    draw_oled();
}

// ---- navigation --------------------------------------------------------
static void go_up(void)
{
    if (at_root()) return;
    int len = (int)strlen(path);
    while (len > 0 && path[len - 1] != '/') len--;
    if (len > 1) path[len - 1] = '\0'; else path[0] = '\0';
    if ((int)strlen(path) < (int)strlen(ROOTS[cur_root])) strcpy(path, ROOTS[cur_root]);
    status[0] = '\0';
    reload(); refresh(1);
}

static void go_into(const char *name)
{
    char np[160];
    join(np, sizeof(np), path, name);
    if (strlen(np) < sizeof(path)) { strcpy(path, np); status[0] = '\0'; reload(); refresh(1); }
}

static void toggle_root(void)
{
    cur_root = (cur_root + 1) % NROOTS;
    strcpy(path, ROOTS[cur_root]);
    status[0] = '\0';
    reload(); refresh(1);
}

// ---- text viewer (editor fallback) -------------------------------------
static void draw_view(void)
{
    gfx_fb_t *fb = (gfx_fb_t *)duo_eink_fb(d);
    if (!fb) return;
    gfx_fill_rect(fb, 0, 0, ew, eh, 0);
    int off = 0, line = 0;
    while (line < view_line && off < viewlen) { if (viewbuf[off] == '\n') line++; off++; }
    gfx_draw_text(fb, 3, 3, viewbuf + off, font, 1);
    duo_eink_commit(d);
    gfx_fb_t *o = (gfx_fb_t *)duo_oled_fb(d);
    if (o) { gfx_fill_rect(o, 0, 0, 128, 64, 0); oled_line(o, 24, "VIEW  ESC=back", 1); duo_oled_commit(d); }
}

static void open_sel(void)
{
    if (sel == 0) { go_up(); return; }
    if (is_dir[sel]) { go_into(names[sel]); return; }

    char full[200];
    join(full, sizeof(full), path, names[sel]);
    if (ends_with(names[sel], ".elf")) {
        char *av[1] = { full };
        app_run(full, 1, av);
        refresh(1);
    } else {
        char *av[2] = { "editor", full };
        if (app_run("editor", 2, av) == -1000) {
            viewlen = app_read_file(full, viewbuf, sizeof(viewbuf) - 1);
            if (viewlen < 0) viewlen = 0;
            viewbuf[viewlen] = '\0';
            view_line = 0; mode = MODE_VIEW;
            draw_view();
        } else {
            refresh(1);
        }
    }
}

// ---- operations --------------------------------------------------------
static void begin_input(int action)
{
    if (action == PA_RENAME && sel == 0) return;       // can't rename "up"
    input_action = action;
    input_len = 0;
    input[0] = '\0';
    if (action == PA_RENAME) {
        snprintf(input, sizeof(input), "%s", names[sel]);
        input_len = (int)strlen(input);
    }
    mode = MODE_INPUT;
    draw_oled();
}

static void commit_input(void)
{
    input[input_len] = '\0';
    mode = MODE_LIST;
    if (input_len == 0) { refresh(0); return; }

    char dst[200];
    join(dst, sizeof(dst), path, input);
    if (input_action == PA_MKDIR) {
        snprintf(status, sizeof(status), app_mkdir(dst) == 0 ? "made %s" : "mkdir failed", input);
    } else {  // rename
        char src[200];
        join(src, sizeof(src), path, names[sel]);
        snprintf(status, sizeof(status), app_rename(src, dst) == 0 ? "renamed" : "rename failed");
    }
    reload(); refresh(1);
}

static void do_delete(void)
{
    char full[200];
    join(full, sizeof(full), path, names[sel]);
    snprintf(status, sizeof(status), app_delete(full) == 0 ? "deleted" : "delete failed");
    mode = MODE_LIST;
    reload(); refresh(1);
}

// Copy one file via a PSRAM buffer (whole-file read/write).
static int copy_file(const char *src, const char *dst)
{
    long sz = 0; int dir = 0;
    if (app_stat(src, &dir, &sz) != 0 || dir) return -1;
    char *buf = heap_caps_malloc((size_t)sz + 1, MCAP_SPIRAM);
    if (!buf) return -1;
    int n = app_read_file(src, buf, (int)sz);
    int ok = (n >= 0) && (app_write_file(dst, buf, n) == 0);
    heap_caps_free(buf);
    return ok ? 0 : -1;
}

static void do_paste(void)
{
    if (!clip_op) return;
    const char *base = strrchr(clip, '/'); base = base ? base + 1 : clip;
    char dst[200];
    join(dst, sizeof(dst), path, base);

    int is_d = 0; long sz = 0;
    app_stat(clip, &is_d, &sz);

    if (clip_op == 2) {                                // cut = move
        if (app_rename(clip, dst) == 0) { snprintf(status, sizeof(status), "moved"); clip_op = 0; }
        else if (!is_d && copy_file(clip, dst) == 0) { app_delete(clip); snprintf(status, sizeof(status), "moved"); clip_op = 0; }
        else snprintf(status, sizeof(status), is_d ? "move folder: same FS only" : "move failed");
    } else {                                           // copy
        if (is_d) snprintf(status, sizeof(status), "folder copy unsupported");
        else snprintf(status, sizeof(status), copy_file(clip, dst) == 0 ? "copied" : "copy failed");
    }
    reload(); refresh(1);
}

static void mark_clip(int op)
{
    if (sel == 0) return;
    join(clip, sizeof(clip), path, names[sel]);
    clip_op = op;
    draw_oled();
}

int main(int argc, char *argv[])
{
    d = duo_open();
    if (!d) { vconsole_printf("files: display init failed\n"); return 1; }
    font = gfx_font_get("5x7");
    ew = duo_eink_w(d); eh = duo_eink_h(d);

    strcpy(path, ROOTS[cur_root]);
    reload();
    refresh(1);
    vconsole_printf("files: %s  (n new, r rename, d del, c/x/v clip, g root, ESC up/quit)\n", path);

    for (;;) {
        int k = input_get_key(1000000);
        if (k < 0) continue;

        if (mode == MODE_VIEW) {
            if (k == 0x1B) { mode = MODE_LIST; refresh(1); }
            else if (k == 's' || k == 0xB6) { view_line++; draw_view(); }
            else if ((k == 'w' || k == 0xB5) && view_line > 0) { view_line--; draw_view(); }
            continue;
        }
        if (mode == MODE_INPUT) {
            if (k == 0x1B) { mode = MODE_LIST; refresh(0); }
            else if (k == 0x0D || k == 0x0A) commit_input();
            else if ((k == 0x08 || k == 0x7F) && input_len > 0) { input_len--; draw_oled(); }
            else if (k >= 0x20 && k < 0x7F && input_len < (int)sizeof(input) - 1) {
                input[input_len++] = (char)k; draw_oled();
            }
            continue;
        }
        if (mode == MODE_CONFIRM) {
            if (k == 0x0D || k == 0x0A) do_delete();
            else if (k == 0x1B) { mode = MODE_LIST; refresh(0); }
            continue;
        }

        // MODE_LIST
        switch (k) {
            case 0x1B: if (at_root()) goto done; go_up(); break;
            case 'w': case 0xB5: if (sel > 0) sel--; refresh(0); break;
            case 's': case 0xB6: if (sel < count - 1) sel++; refresh(0); break;
            case 0x0D: case 0x0A: open_sel(); break;
            case 'g': case 'G': toggle_root(); break;
            case 'n': case 'N': begin_input(PA_MKDIR); break;
            case 'r': case 'R': begin_input(PA_RENAME); break;
            case 'd': case 'D': if (sel > 0) { mode = MODE_CONFIRM; draw_oled(); } break;
            case 'c': case 'C': mark_clip(1); break;
            case 'x': case 'X': mark_clip(2); break;
            case 'v': case 'V': do_paste(); break;
            default: break;
        }
    }
done:
    duo_close(d);
    vconsole_printf("files: bye\n");
    return 0;
}
