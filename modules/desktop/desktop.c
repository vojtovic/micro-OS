// desktop — a minimal dual-display environment.
//
// The two displays play different roles, matching their physics:
//   * OLED  = the live input line — low latency, shows the word being typed.
//   * E-ink = the document canvas — slow, "things that don't change": every
//             committed word/line accumulates here into a growing note.
//
// Usage:  desktop [name]      opens /sdcard/home/<name>.txt (default: notes)
//
// Keys:
//   printable  -> append to the current word (shown on OLED)
//   SPACE      -> commit the word to the e-ink document (+ a space)
//   ENTER      -> commit the word and start a new line on the e-ink
//   BACKSPACE  -> edit the current word; on an empty word, pull the last
//                 committed word back off the e-ink (undo-word)
//   ESC        -> save the document and quit
//
// The e-ink shows a static status header ("<host> : <name>") above the growing
// document. It grabs both displays so the console mirror leaves them alone,
// then drives them directly via display_mux_render_text().

#include <stdint.h>
#include <stddef.h>
#include <string.h>

extern int vconsole_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
extern int input_get_key(uint32_t timeout_ms);                       // -1 on timeout
// File I/O via the kernel (modules can't reach libc fopen directly).
extern int app_read_file(const char *path, char *buf, int max);      // bytes, or -1
extern int app_write_file(const char *path, const char *buf, int len); // 0 == ok
extern int snprintf(char *str, size_t size, const char *fmt, ...);
// Compositor text windows (the compositor blits stored text on focus, so this
// app needs no grab/present/repaint).
extern int  comp_open_text(const char *display);
extern void comp_set_text(int win, const char *text, int len);
extern void comp_close(int win);

#define OLED       "oled"
#define EINK       "eink"
#define DOC_MAX    1024   // e-ink document ring (whole lines dropped from front)
#define WORD_MAX     64   // current OLED word
#define HDR_MAX      80   // e-ink status header
#define HOST_MAX     32
#define HOST_PATH  "/sdcard/etc/hostname"
#define NAME_MAX     24

static char doc[DOC_MAX];
static int  doc_len;
static char word[WORD_MAX];
static int  word_len;
static int  eink_win = -1, oled_win = -1;

static char note_name[NAME_MAX + 1] = "notes";       // which document
static char notes_path[64] = "/sdcard/home/notes.txt";

static char header[HDR_MAX];             // static status bar shown atop the doc
static int  header_len;
static char compose[HDR_MAX + DOC_MAX];  // header + doc, sent to the e-ink

// Drop whole lines from the front until there is headroom for one more word.
static void doc_trim(void)
{
    while (doc_len > DOC_MAX - WORD_MAX - 2) {
        int i = 0;
        while (i < doc_len && doc[i] != '\n') i++;
        if (i < doc_len) i++;          // consume the newline too
        else             i = doc_len;  // no newline: drop everything
        memmove(doc, doc + i, doc_len - i);
        doc_len -= i;
    }
}

static void doc_append(const char *s, int n)
{
    if (doc_len + n > DOC_MAX - 1) doc_trim();
    if (doc_len + n > DOC_MAX - 1) doc_len = 0;   // pathological: hard reset
    memcpy(doc + doc_len, s, n);
    doc_len += n;
}

// Build the static status header once: "<hostname> : notes" + a separator rule.
static void build_header(void)
{
    char host[HOST_MAX];
    int hn = app_read_file(HOST_PATH, host, HOST_MAX - 1);
    if (hn < 0) hn = 0;
    while (hn > 0 && (host[hn - 1] == '\n' || host[hn - 1] == '\r' || host[hn - 1] == ' '))
        hn--;
    host[hn] = '\0';
    if (hn == 0) { host[0] = '?'; host[1] = '\0'; }

    int r = snprintf(header, sizeof(header),
                     "%s : %s\n------------------------------\n", host, note_name);
    header_len = (r < 0) ? 0 : (r < (int)sizeof(header) ? r : (int)sizeof(header) - 1);
}

// Accept only a safe file-stem (alnum, '_', '-') to avoid path traversal.
static int valid_name(const char *s)
{
    if (!s || !*s) return 0;
    int n = 0;
    for (const char *p = s; *p; p++, n++) {
        char c = *p;
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok || n >= NAME_MAX) return 0;
    }
    return 1;
}

static void show_eink(void)
{
    int n = 0;
    memcpy(compose, header, (size_t)header_len);  n += header_len;
    memcpy(compose + n, doc, (size_t)doc_len);     n += doc_len;
    comp_set_text(eink_win, compose, n);
}

// OLED shows a prompt with the in-progress word and a cursor: "> word_".
static void show_oled(void)
{
    char line[WORD_MAX + 4];
    line[0] = '>';
    line[1] = ' ';
    memcpy(line + 2, word, (size_t)word_len);
    int n = 2 + word_len;
    line[n++] = '_';                 // cursor
    comp_set_text(oled_win, line, n);
}

static void commit_word(int with_newline)
{
    if (word_len > 0) doc_append(word, word_len);
    doc_append(with_newline ? "\n" : " ", 1);
    word_len = 0;
    show_eink();
    show_oled();
}

int main(int argc, char *argv[])
{
    doc_len = 0;
    word_len = 0;

    // Optional note name: `desktop <name>` opens /sdcard/home/<name>.txt.
    if (argc >= 2 && valid_name(argv[1])) {
        int i = 0;
        for (; argv[1][i] && i < NAME_MAX; i++) note_name[i] = argv[1][i];
        note_name[i] = '\0';
    }
    snprintf(notes_path, sizeof(notes_path), "/sdcard/home/%s.txt", note_name);

    // Compositor text windows. These succeed even if a display isn't present —
    // its commits simply become no-ops — so no availability juggling is needed.
    oled_win = comp_open_text(OLED);
    eink_win = comp_open_text(EINK);

    build_header();

    // Load the persisted document so notes accumulate across sessions.
    int r = app_read_file(notes_path, doc, DOC_MAX - 1);
    doc_len = (r > 0) ? r : 0;
    vconsole_printf("desktop: %d bytes loaded from %s\n", doc_len, notes_path);
    vconsole_printf("desktop: SPACE=word->eink  ENTER=newline  BKSP=edit/undo-word  ESC=save+quit\n");

    show_eink();
    show_oled();

    for (;;) {
        int k = input_get_key(1000000);   // long block; loop on timeout
        if (k < 0) continue;

        if (k == 0x1B) break;                              // ESC (compositor repaints on focus)
        if (k == 0x20) { commit_word(0); continue; }       // SPACE
        if (k == 0x0D || k == 0x0A) { commit_word(1); continue; }  // ENTER
        if (k == 0x08 || k == 0x7F) {                      // BACKSPACE / DEL
            if (word_len > 0) {
                // Editing the in-progress word on the OLED.
                word_len--;
                show_oled();
            } else if (doc_len > 0) {
                // Word buffer empty: pull the last committed word off the
                // e-ink back onto the OLED for editing (an "undo last word").
                if (doc[doc_len - 1] == ' ' || doc[doc_len - 1] == '\n')
                    doc_len--;                    // drop the trailing separator
                int start = doc_len;
                while (start > 0 && doc[start - 1] != ' ' && doc[start - 1] != '\n')
                    start--;
                int wl = doc_len - start;
                if (wl > WORD_MAX - 1) {          // clamp overly long words
                    start = doc_len - (WORD_MAX - 1);
                    wl = WORD_MAX - 1;
                }
                memcpy(word, doc + start, (size_t)wl);
                word_len = wl;
                doc_len = start;
                show_eink();
                show_oled();
            }
            continue;
        }
        if (k >= 0x20 && k < 0x7F) {                       // printable
            if (word_len < WORD_MAX - 1) { word[word_len++] = (char)k; show_oled(); }
            continue;
        }
        // ignore everything else (arrows, function keys, ...)
    }

    // Persist the document (one SD write per session — cheap and crash-safe
    // enough for notes). Any word still in progress is committed first.
    if (word_len > 0) doc_append(word, word_len);
    int saved = app_write_file(notes_path, doc, doc_len);

    // Close the windows; the compositor releases the displays back to the
    // console (the e-ink keeps its last frame until something else draws).
    comp_close(oled_win);
    comp_close(eink_win);
    vconsole_printf("desktop: %s %s (%d bytes)\n",
                    saved == 0 ? "saved" : "FAILED to save", notes_path, doc_len);
    return 0;
}
