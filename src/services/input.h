#ifndef INPUT_H
#define INPUT_H

#include "esp_err.h"
#include <stdint.h>

// Key-event input service.
//
// Decouples input *sources* (CardKB driver, UART) from *consumers* (shell now,
// GUI apps later). Sources call input_push_key(); consumers call
// input_get_key(). Backed by a thread-safe FreeRTOS queue.

esp_err_t input_init(void);

// Push a key into the queue — called by input-source drivers (e.g. CardKB).
// Non-blocking; returns 0 on success, -1 if the queue is full or uninitialised.
int input_push_key(int key);

// Pop the next key, waiting up to timeout_ms (0 = non-blocking). Returns the
// key code (>=0), or -1 if none arrived within the timeout.
int input_get_key(uint32_t timeout_ms);

// Discard all queued keys. Called before launching an app so the keystroke
// that launched it (e.g. the Enter after typing its name) does not leak into
// the app's own input loop.
void input_flush(void);

// Registers the `input` diagnostic shell command (push/read/status).
esp_err_t cmd_input_register(void);

#endif
