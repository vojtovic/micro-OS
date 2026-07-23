#ifndef DUO_H
#define DUO_H

// duo — the dual-display cooperation toolkit.
//
// The core idea of the whole system: the two displays cooperate to cover each
// other's weaknesses. The E-INK is big but slow → it holds the main view (a
// list, a document, a canvas) that changes rarely. The OLED is small but fast →
// it holds the active, live element (the value you're editing, a cursor, live
// feedback). duo gives every app a consistent way to drive both.
//
// It opens one compositor window per display and offers ready-made pieces:
// duo_menu() for the e-ink (titled numbered list + a controls bar), duo_value()/
// duo_message() for the OLED, plus raw framebuffer access for custom drawing.
// Refresh the e-ink only on structural changes; refresh the OLED freely.

typedef struct duo duo_t;

// Open the two windows (e-ink main + OLED active). NULL on failure.
duo_t *duo_open(void);
void   duo_close(duo_t *d);

// E-INK: draw a titled, numbered list (`items`, 1-based labels) with `selected`
// highlighted, and `controls` as a bar along the bottom. Any of title/controls
// may be NULL. Refreshes the slow e-ink — call on selection/structure changes.
void duo_menu(duo_t *d, const char *title, const char *const *items, int n,
              int selected, const char *controls);

// OLED: a label above a "<  value  >" editor (fast — refresh on every edit).
void duo_value(duo_t *d, const char *label, const char *value);
// OLED: a short centered message.
void duo_message(duo_t *d, const char *text);

// Raw access for custom drawing (framebuffers are gfx_fb_t*).
void *duo_eink_fb(duo_t *d);
void *duo_oled_fb(duo_t *d);
int   duo_eink_w(duo_t *d);
int   duo_eink_h(duo_t *d);
void  duo_eink_commit(duo_t *d);
void  duo_oled_commit(duo_t *d);

#endif // DUO_H
