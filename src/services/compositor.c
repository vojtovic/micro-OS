#include "compositor.h"
#include "display_mux.h"
#include "loader/gfx_abi.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "compositor";

#define COMP_TEXT_CAP    1280    // per text window
#define COMP_SLOW_DEBOUNCE 250   // ms — settle time before redrawing a slow (e-ink) display

typedef enum { WIN_GFX, WIN_TEXT } win_type_t;

typedef struct {
    bool         used;
    win_type_t   type;
    TaskHandle_t owner;
    char         display[DISPLAY_NAME_LEN];
    gfx_fb_t    *fb;        // WIN_GFX: draw target / stored surface
    char        *text;      // WIN_TEXT: stored text
    int          text_len;
} comp_win_t;

static comp_win_t        s_wins[COMP_MAX_WINDOWS];
static TaskHandle_t      s_focus;
static SemaphoreHandle_t s_lock;
static TaskHandle_t      s_worker;        // debounced slow-display (e-ink) redraws
static volatile int64_t  s_slow_due;      // esp_timer us; 0 = nothing pending

static void blit(comp_win_t *w);          // fwd

// Redraw the focused task's SLOW-display windows once switching has settled.
static void slow_worker(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);   // wait for a slow-blit request
        for (;;) {
            int64_t due = s_slow_due;
            if (due == 0) break;
            int64_t now = esp_timer_get_time();
            if (now >= due) {
                s_slow_due = 0;
                xSemaphoreTake(s_lock, portMAX_DELAY);
                for (int i = 0; i < COMP_MAX_WINDOWS; i++) {
                    comp_win_t *w = &s_wins[i];
                    if (w->used && w->owner == s_focus && !display_mux_is_fast(w->display))
                        blit(w);
                }
                xSemaphoreGive(s_lock);
                break;
            }
            vTaskDelay(pdMS_TO_TICKS((int)((due - now) / 1000) + 1));
        }
    }
}

esp_err_t compositor_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;
    xTaskCreate(slow_worker, "comp_slow", 4096, NULL, 4, &s_worker);
    ESP_LOGI(TAG, "Compositor ready (max %d windows)", COMP_MAX_WINDOWS);
    return ESP_OK;
}

static int alloc_slot(void)
{
    for (int i = 0; i < COMP_MAX_WINDOWS; i++)
        if (!s_wins[i].used) return i;
    return -1;
}

// Push a window's stored content to its physical display. Caller holds s_lock.
static void blit(comp_win_t *w)
{
    display_mux_grab(w->display);        // suppress console mirror while shown
    if (w->type == WIN_GFX) {
        if (w->fb) display_mux_present(w->display, w->fb);
    } else {
        display_mux_render_text(w->display, w->text, (size_t)w->text_len);
    }
}

int comp_open_gfx(const char *display, uint16_t w, uint16_t h, int fmt)
{
    if (!display) return -1;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int id = alloc_slot();
    if (id < 0) { xSemaphoreGive(s_lock); return -1; }

    comp_win_t *win = &s_wins[id];
    memset(win, 0, sizeof(*win));
    win->fb = gfx_fb_alloc(w, h, (gfx_fmt_t)fmt, 0);
    if (!win->fb) { xSemaphoreGive(s_lock); ESP_LOGE(TAG, "fb alloc failed"); return -1; }
    win->type  = WIN_GFX;
    win->owner = xTaskGetCurrentTaskHandle();
    strncpy(win->display, display, DISPLAY_NAME_LEN - 1);
    win->used = true;

    xSemaphoreGive(s_lock);
    return id;
}

int comp_open_text(const char *display)
{
    if (!display) return -1;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int id = alloc_slot();
    if (id < 0) { xSemaphoreGive(s_lock); return -1; }

    comp_win_t *win = &s_wins[id];
    memset(win, 0, sizeof(*win));
    win->text = heap_caps_malloc(COMP_TEXT_CAP, MALLOC_CAP_SPIRAM);
    if (!win->text) { xSemaphoreGive(s_lock); ESP_LOGE(TAG, "text alloc failed"); return -1; }
    win->text[0] = '\0';
    win->type  = WIN_TEXT;
    win->owner = xTaskGetCurrentTaskHandle();
    strncpy(win->display, display, DISPLAY_NAME_LEN - 1);
    win->used = true;

    xSemaphoreGive(s_lock);
    return id;
}

void *comp_window_fb(int win)
{
    if (win < 0 || win >= COMP_MAX_WINDOWS) return NULL;
    return s_wins[win].used ? s_wins[win].fb : NULL;
}

// A window is "live" (should draw) if its owner is the focused app, OR no app is
// focused (s_focus == NULL) — the latter is the blocking app_run / shell case,
// where the running app is the de-facto foreground.
static bool win_live(const comp_win_t *w)
{
    return w->used && (w->owner == s_focus || s_focus == NULL);
}

void comp_commit(int win)
{
    if (win < 0 || win >= COMP_MAX_WINDOWS) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    comp_win_t *w = &s_wins[win];
    if (w->type == WIN_GFX && win_live(w))
        blit(w);
    xSemaphoreGive(s_lock);
}

void comp_set_text(int win, const char *text, int len)
{
    if (win < 0 || win >= COMP_MAX_WINDOWS || !text || len < 0) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    comp_win_t *w = &s_wins[win];
    if (w->used && w->type == WIN_TEXT) {
        if (len > COMP_TEXT_CAP - 1) len = COMP_TEXT_CAP - 1;
        memcpy(w->text, text, (size_t)len);
        w->text[len] = '\0';
        w->text_len = len;
        if (win_live(w)) blit(w);
    }
    xSemaphoreGive(s_lock);
}

void comp_close(int win)
{
    if (win < 0 || win >= COMP_MAX_WINDOWS) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    comp_win_t *w = &s_wins[win];
    if (w->used) {
        char disp[DISPLAY_NAME_LEN];
        strncpy(disp, w->display, sizeof(disp));
        if (w->fb)   gfx_fb_free(w->fb);
        if (w->text) heap_caps_free(w->text);
        memset(w, 0, sizeof(*w));

        // Release the display back to the console unless another window still
        // targets it.
        bool still_used = false;
        for (int i = 0; i < COMP_MAX_WINDOWS; i++)
            if (s_wins[i].used && strcmp(s_wins[i].display, disp) == 0) { still_used = true; break; }
        if (!still_used) display_mux_release(disp);
    }
    xSemaphoreGive(s_lock);
}

void comp_set_focus(void *task)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_focus = (TaskHandle_t)task;

    // Show every window owned by the focused task; remember which displays that
    // covered so the rest can be released back to the console. FAST displays
    // (OLED) blit now; SLOW displays (e-ink) are deferred to the worker so rapid
    // switching doesn't thrash the panel — only the settled app is drawn.
    char shown[COMP_MAX_WINDOWS][DISPLAY_NAME_LEN];
    int  nshown = 0;
    bool need_slow = false;
    for (int i = 0; i < COMP_MAX_WINDOWS; i++) {
        comp_win_t *w = &s_wins[i];
        if (!w->used || w->owner != s_focus) continue;
        if (display_mux_is_fast(w->display)) blit(w);
        else                                 need_slow = true;
        strncpy(shown[nshown++], w->display, DISPLAY_NAME_LEN - 1);
    }

    // Release displays that have windows but aren't shown by the focused task.
    // Collect released SLOW displays (e-ink) so we can paint the console onto
    // them afterwards — the auto-mirror only covers fast displays, so without
    // this a slow display would keep showing the previous app's stale frame
    // when focus moves to the shell (or to an app that doesn't use it).
    char rel_slow[COMP_MAX_WINDOWS][DISPLAY_NAME_LEN];
    int  nrel = 0;
    for (int i = 0; i < COMP_MAX_WINDOWS; i++) {
        comp_win_t *w = &s_wins[i];
        if (!w->used) continue;
        bool is_shown = false;
        for (int j = 0; j < nshown; j++)
            if (strcmp(shown[j], w->display) == 0) { is_shown = true; break; }
        if (is_shown) continue;
        display_mux_release(w->display);
        if (!display_mux_is_fast(w->display)) {
            bool dup = false;
            for (int j = 0; j < nrel; j++)
                if (strcmp(rel_slow[j], w->display) == 0) { dup = true; break; }
            if (!dup) strncpy(rel_slow[nrel++], w->display, DISPLAY_NAME_LEN - 1);
        }
    }

    xSemaphoreGive(s_lock);

    // Paint the console onto each freed slow display (done outside the lock —
    // an e-ink refresh takes ~2 s).
    for (int i = 0; i < nrel; i++)
        display_mux_refresh(rel_slow[i]);

    // Schedule the deferred slow (e-ink) redraw; re-arming pushes it back so it
    // fires only once switching settles.
    if (need_slow && s_worker) {
        s_slow_due = esp_timer_get_time() + COMP_SLOW_DEBOUNCE * 1000;
        xTaskNotifyGive(s_worker);
    }
}
