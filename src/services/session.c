#include "session.h"
#include "input.h"
#include "vconsole.h"
#include "display_mux.h"
#include "compositor.h"
#include "loader/elf_loader.h"
#include "esp_console.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <sys/stat.h>

static const char *TAG = "session";

#define SESSION_BIN_DIR  "/sdcard/bin"
#define SESSION_STACK    12288        // bytes — GUI apps do gfx + snprintf
#define SESSION_PRIO     4

typedef struct {
    bool              used;
    char              name[SESSION_NAME_LEN];
    elf_load_result_t elf;
    TaskHandle_t      task;
} sess_slot_t;

static sess_slot_t      s_apps[SESSION_MAX_APPS];
static TaskHandle_t     s_shell;
static int              s_focus = -1;         // -1 = shell, else app index
static SemaphoreHandle_t s_lock;

// Strip directory and ".elf" to get a short display name.
static void clean_name(const char *in, char *out, size_t n)
{
    const char *b = strrchr(in, '/');
    b = b ? b + 1 : in;
    size_t i = 0;
    for (; b[i] && i < n - 1 && b[i] != '.'; i++) out[i] = b[i];
    out[i] = '\0';
}

// Point input focus at a slot (-1 = shell). Caller holds s_lock.
static void apply_focus(int slot)
{
    s_focus = slot;
    if (slot < 0 || !s_apps[slot].used) {
        input_set_focus(s_shell);            // NULL early = ungated (shell reads)
        comp_set_focus(NULL);                // release app displays to the console
        display_mux_refresh_fast();          // console onto the OLED
    } else {
        input_flush();                       // drop stale keys (e.g. the Enter that
                                             // launched the app) so they don't leak
        input_set_focus(s_apps[slot].task);
        input_request_repaint(s_apps[slot].task);  // repaint for un-migrated apps
        comp_set_focus(s_apps[slot].task);          // blit compositor windows
    }
}

static void session_focus_cmd(int sel);   // forward decl

esp_err_t session_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;
    input_set_switch_handler(session_focus_cmd);
    ESP_LOGI(TAG, "Session manager ready (max %d apps)", SESSION_MAX_APPS);
    return ESP_OK;
}

void session_set_shell(void *task)
{
    s_shell = (TaskHandle_t)task;
    if (s_focus < 0) input_set_focus(s_shell);   // start focused on the shell
}

// The per-app task: run the app to completion, then clean up and refocus.
static void app_task(void *arg)
{
    sess_slot_t *s = (sess_slot_t *)arg;
    char *av[1] = { s->name };
    elf_loader_call_entry(&s->elf, 1, av);       // blocks until the app's main returns

    xSemaphoreTake(s_lock, portMAX_DELAY);
    elf_loader_unload(&s->elf);
    int idx = (int)(s - s_apps);
    s->used = false;
    s->task = NULL;
    if (s_focus == idx) apply_focus(-1);          // hand focus back to the shell
    xSemaphoreGive(s_lock);

    vTaskDelete(NULL);
}

esp_err_t session_spawn(const char *name)
{
    if (!name || !*name) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_lock, portMAX_DELAY);

    int slot = -1;
    for (int i = 0; i < SESSION_MAX_APPS; i++)
        if (!s_apps[i].used) { slot = i; break; }
    if (slot < 0) {
        xSemaphoreGive(s_lock);
        ESP_LOGW(TAG, "No free app slot (max %d)", SESSION_MAX_APPS);
        return ESP_ERR_NO_MEM;
    }

    sess_slot_t *s = &s_apps[slot];
    char path[160];
    if (strchr(name, '/'))
        snprintf(path, sizeof(path), "%s", name);
    else
        snprintf(path, sizeof(path), "%s/%s.elf", SESSION_BIN_DIR, name);

    struct stat st;
    if (stat(path, &st) != 0) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t err = elf_loader_load(path, &s->elf);
    if (err != ESP_OK) {
        xSemaphoreGive(s_lock);
        ESP_LOGE(TAG, "load failed for %s: %s", path, esp_err_to_name(err));
        return err;
    }

    clean_name(name, s->name, sizeof(s->name));
    s->used = true;

    if (xTaskCreate(app_task, s->name, SESSION_STACK, s, SESSION_PRIO, &s->task) != pdPASS) {
        elf_loader_unload(&s->elf);
        s->used = false;
        xSemaphoreGive(s_lock);
        ESP_LOGE(TAG, "task create failed for %s", s->name);
        return ESP_FAIL;
    }

    apply_focus(slot);
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "spawned '%s' in slot %d", s->name, slot);
    return ESP_OK;
}

void session_switch_to(int ordinal)   // -1 = shell, 0-based index among running apps
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (ordinal < 0) {
        apply_focus(-1);
    } else {
        int seen = 0, target = -1;
        for (int i = 0; i < SESSION_MAX_APPS; i++)
            if (s_apps[i].used) {
                if (seen == ordinal) { target = i; break; }
                seen++;
            }
        if (target >= 0) apply_focus(target);   // else: no such app -> ignore
    }
    xSemaphoreGive(s_lock);
}

// Input focus command from the switch key: -2 = cycle, -1 = shell, 0.. = app.
static void session_focus_cmd(int sel)
{
    if (sel == -2) session_switch_next();
    else           session_switch_to(sel);
}

void session_switch_next(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);

    // Ordered focus ring: shell (-1), then each used app slot.
    int order[SESSION_MAX_APPS + 1];
    int n = 0;
    order[n++] = -1;
    for (int i = 0; i < SESSION_MAX_APPS; i++)
        if (s_apps[i].used) order[n++] = i;

    int cur = 0;
    for (int i = 0; i < n; i++)
        if (order[i] == s_focus) { cur = i; break; }

    apply_focus(order[(cur + 1) % n]);
    xSemaphoreGive(s_lock);
}

int session_list(char names[][SESSION_NAME_LEN], int max)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = 0;
    for (int i = 0; i < SESSION_MAX_APPS && n < max; i++)
        if (s_apps[i].used) {
            strncpy(names[n], s_apps[i].name, SESSION_NAME_LEN - 1);
            names[n][SESSION_NAME_LEN - 1] = '\0';
            n++;
        }
    xSemaphoreGive(s_lock);
    return n;
}

int session_count(void)
{
    int n = 0;
    for (int i = 0; i < SESSION_MAX_APPS; i++)
        if (s_apps[i].used) n++;
    return n;
}

// ---- shell commands ------------------------------------------------------
static int cmd_spawn(int argc, char **argv)
{
    if (argc < 2) {
        vconsole_printf("Usage: spawn <app>   (run an app in the background, TAB to switch)\n");
        return 1;
    }
    esp_err_t err = session_spawn(argv[1]);
    if (err == ESP_ERR_NOT_FOUND)      vconsole_printf("spawn: no such app '%s'\n", argv[1]);
    else if (err == ESP_ERR_NO_MEM)    vconsole_printf("spawn: too many apps running\n");
    else if (err != ESP_OK)            vconsole_printf("spawn: failed (%s)\n", esp_err_to_name(err));
    else                               vconsole_printf("spawn: '%s' running — TAB (or 'sw') to switch\n", argv[1]);
    return 0;
}

static int cmd_apps(int argc, char **argv)
{
    char names[SESSION_MAX_APPS][SESSION_NAME_LEN];
    int n = session_list(names, SESSION_MAX_APPS);
    vconsole_printf("Focus ring: [shell]%s\n", s_focus < 0 ? " <=" : "");
    for (int i = 0; i < n; i++)
        vconsole_printf("  %d: %s\n", i + 1, names[i]);
    if (n == 0) vconsole_printf("  (no apps running)\n");
    return 0;
}

static int cmd_sw(int argc, char **argv)
{
    if (session_count() == 0) {
        vconsole_printf("sw: no apps running\n");
        return 0;
    }
    session_switch_next();
    return 0;
}

esp_err_t cmd_session_register(void)
{
    const esp_console_cmd_t cmds[] = {
        { .command = "spawn", .help = "Run an app in the background (TAB switches focus)", .func = cmd_spawn },
        { .command = "apps",  .help = "List running background apps",                      .func = cmd_apps  },
        { .command = "sw",    .help = "Switch focus to the next app (same as TAB)",         .func = cmd_sw    },
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        esp_err_t err = esp_console_cmd_register(&cmds[i]);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}
