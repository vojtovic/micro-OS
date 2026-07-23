#ifndef SESSION_H
#define SESSION_H

#include "esp_err.h"

// session — cooperative multitasking for apps.
//
// Each spawned app runs in its own FreeRTOS task; the input service delivers
// keys only to the FOCUSED task. The focus ring is [shell, app0, app1, ...];
// the TAB key (or the `sw` command) rotates it. A newly focused app is sent an
// INPUT_KEY_REPAINT so it can redraw. When an app's main() returns, its task
// unloads the ELF and hands focus back.

#define SESSION_MAX_APPS   4
#define SESSION_NAME_LEN   32

esp_err_t session_init(void);

// Register the shell's task as focus slot "shell" (call from the shell task).
void session_set_shell(void *task);

// Load /sdcard/bin/<name>.elf and run it as a focused background task.
esp_err_t session_spawn(const char *name);

// Rotate focus to the next target in [shell, running apps].
void session_switch_next(void);

// Jump focus directly: -1 = shell, 0-based index among the running apps.
void session_switch_to(int ordinal);

// Copy running app names into `names`; returns the count.
int session_list(char names[][SESSION_NAME_LEN], int max);
int session_count(void);

// Registers the `spawn`, `apps` and `sw` shell commands.
esp_err_t cmd_session_register(void);

#endif // SESSION_H
