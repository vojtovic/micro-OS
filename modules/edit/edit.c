// edit — a simple line-based text editor app (ed-style).
//
// Practical for a keyboard + scrolling console (no cursor addressing needed).
// Loads a file into an in-memory line array, lets you list/append/insert/
// delete/replace lines, and writes it back. Uses the app file helpers
// (app_read_file/app_write_file) since modules can't reach libc fopen.
//
//   edit <file>
//   commands: l=list  a=append  i N=insert  d N=delete  r N=replace
//             w=write  wq=write+quit  q=quit  q!=quit-discard  h=help

#include <stdint.h>

extern int vconsole_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
extern int vconsole_putchar(int c);
extern int input_get_key(uint32_t timeout_ms);
extern int app_read_file (const char *path, char *buf, int max);
extern int app_write_file(const char *path, const char *buf, int len);

#define MAX_LINES 128
#define MAX_LINE  128
#define SCRATCH   (MAX_LINES * MAX_LINE)

static char s_lines[MAX_LINES][MAX_LINE];
static int  s_nlines;
static int  s_dirty;
static char s_scratch[SCRATCH];

// Read one echoed line of text from the keyboard. Returns length, or -1 timeout.
static int read_line(char *buf, int size)
{
    int i = 0;
    for (;;) {
        int c = input_get_key(300000);          // 5-minute idle timeout
        if (c < 0) return -1;
        if (c == '\r' || c == '\n') { vconsole_putchar('\n'); break; }
        if (c == 0x08 || c == 0x7F) {            // backspace
            if (i > 0) { i--; vconsole_printf("\b \b"); }
            continue;
        }
        if (c >= 0x20 && c < 0x7F && i < size - 1) {
            buf[i++] = (char)c;
            vconsole_putchar(c);
        }
    }
    buf[i] = '\0';
    return i;
}

static int parse_int(const char *s)
{
    int v = 0;
    while (*s == ' ') s++;
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return v;
}

static void set_line(int idx, const char *src)
{
    int i = 0;
    while (src[i] && i < MAX_LINE - 1) { s_lines[idx][i] = src[i]; i++; }
    s_lines[idx][i] = '\0';
}

static void load_file(const char *path)
{
    int n = app_read_file(path, s_scratch, SCRATCH - 1);
    s_nlines = 0;
    if (n < 0) { vconsole_printf("(new file '%s')\n", path); return; }

    int col = 0;
    for (int i = 0; i < n && s_nlines < MAX_LINES; i++) {
        char ch = s_scratch[i];
        if (ch == '\n')      { s_lines[s_nlines][col] = '\0'; s_nlines++; col = 0; }
        else if (ch != '\r') { if (col < MAX_LINE - 1) s_lines[s_nlines][col++] = ch; }
    }
    if (col > 0 && s_nlines < MAX_LINES) { s_lines[s_nlines][col] = '\0'; s_nlines++; }
    vconsole_printf("Loaded '%s' (%d lines)\n", path, s_nlines);
}

static void save_file(const char *path)
{
    int len = 0;
    for (int i = 0; i < s_nlines; i++) {
        int l = 0;
        while (s_lines[i][l]) l++;
        if (len + l + 1 > SCRATCH) break;
        for (int j = 0; j < l; j++) s_scratch[len++] = s_lines[i][j];
        s_scratch[len++] = '\n';
    }
    if (app_write_file(path, s_scratch, len) == 0) {
        s_dirty = 0;
        vconsole_printf("Wrote '%s' (%d lines, %d bytes)\n", path, s_nlines, len);
    } else {
        vconsole_printf("Write failed\n");
    }
}

static void list_lines(void)
{
    if (s_nlines == 0) { vconsole_printf("(empty)\n"); return; }
    for (int i = 0; i < s_nlines; i++)
        vconsole_printf("%3d| %s\n", i + 1, s_lines[i]);
}

// Read lines from the keyboard until a lone "." — inserting starting at `at`.
static void input_lines(int at)
{
    vconsole_printf("-- enter text, '.' alone on a line to finish --\n");
    char line[MAX_LINE];
    for (;;) {
        if (read_line(line, sizeof(line)) < 0) return;
        if (line[0] == '.' && line[1] == '\0') return;
        if (s_nlines >= MAX_LINES) { vconsole_printf("(full)\n"); return; }
        for (int k = s_nlines; k > at; k--) set_line(k, s_lines[k - 1]);  // shift down
        set_line(at, line);
        s_nlines++;
        at++;
        s_dirty = 1;
    }
}

int main(int argc, char *argv[])
{
    if (argc < 2) { vconsole_printf("Usage: edit <file>\n"); return 1; }
    const char *path = argv[1];

    s_dirty = 0;
    load_file(path);
    vconsole_printf("edit: l list, a append, i N insert, d N del, r N replace, "
                    "w write, wq, q quit, h help\n");
    list_lines();

    char cmd[MAX_LINE];
    for (;;) {
        vconsole_printf("edit> ");
        int len = read_line(cmd, sizeof(cmd));
        if (len < 0) { vconsole_printf("\n(timeout)\n"); break; }
        if (len == 0) continue;

        char c = cmd[0];
        int arg = parse_int(cmd + 1);

        if (c == 'q') {
            if (cmd[1] == '!' || !s_dirty) break;
            vconsole_printf("Unsaved changes — 'w' to save, or 'q!' to discard\n");
        } else if (c == 'w') {
            save_file(path);
            if (cmd[1] == 'q') break;                        // wq
        } else if (c == 'l') {
            list_lines();
        } else if (c == 'h') {
            vconsole_printf("l=list a=append i N=insert d N=delete r N=replace "
                            "w=write wq q q! h\n");
        } else if (c == 'a') {
            input_lines(s_nlines);                           // append at end
        } else if (c == 'i') {
            if (arg < 1 || arg > s_nlines + 1) { vconsole_printf("bad line\n"); continue; }
            input_lines(arg - 1);
        } else if (c == 'd') {
            if (arg < 1 || arg > s_nlines) { vconsole_printf("bad line\n"); continue; }
            for (int k = arg - 1; k < s_nlines - 1; k++) set_line(k, s_lines[k + 1]);
            s_nlines--;
            s_dirty = 1;
            vconsole_printf("deleted %d\n", arg);
        } else if (c == 'r') {
            if (arg < 1 || arg > s_nlines) { vconsole_printf("bad line\n"); continue; }
            vconsole_printf("%3d| ", arg);
            char line[MAX_LINE];
            if (read_line(line, sizeof(line)) >= 0) { set_line(arg - 1, line); s_dirty = 1; }
        } else {
            vconsole_printf("? (h for help)\n");
        }
    }

    if (s_dirty) vconsole_printf("(exited with unsaved changes)\n");
    return 0;
}
