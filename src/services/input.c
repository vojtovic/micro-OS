#include "input.h"
#include "vconsole.h"
#include "esp_console.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "input";

#define INPUT_QUEUE_DEPTH 32

static QueueHandle_t s_queue = NULL;

// Focus gating for cooperative multitasking: only the focused task's
// input_get_key() drains the queue; others poll and return nothing. NULL means
// ungated (the shell reads normally when no app has focus). The TAB switch key
// is handled globally via s_switch_cb and never delivered to an app.
static TaskHandle_t  s_focus = NULL;
static void        (*s_switch_cb)(int) = NULL;

// After TAB we open a short window during which a digit re-targets the focus
// directly (TAB then 1-9 -> app N, TAB then 0 -> shell). TAB itself cycles
// instantly, so plain switching stays snappy.
static int64_t s_jump_until = 0;   // esp_timer_get_time() us; 0 = window closed

// A newly focused app gets exactly one INPUT_KEY_REPAINT the first time it reads
// after gaining focus. Using a flag (not a queued key) makes delivery reliable —
// it can't be flushed or raced away.
static TaskHandle_t s_repaint_target = NULL;

void input_set_focus(void *task)             { s_focus = (TaskHandle_t)task; }
void input_request_repaint(void *task)       { s_repaint_target = (TaskHandle_t)task; }
void input_set_switch_handler(void (*cb)(int)) { s_switch_cb = cb; }

// Bridge the UART console (stdin) into the input service, so keys typed in the
// serial monitor reach apps too — not just the CardKB. Both sources push here,
// so the shell and apps read a single unified stream via input_get_key().
static void uart_bridge_task(void *arg)
{
    (void)arg;
    for (;;) {
        int c = getchar();
        if (c == EOF) {
            vTaskDelay(pdMS_TO_TICKS(10));   // stdin is non-blocking → poll
            continue;
        }
        input_push_key(c);
    }
}

esp_err_t input_init(void)
{
    s_queue = xQueueCreate(INPUT_QUEUE_DEPTH, sizeof(int));
    if (!s_queue) {
        ESP_LOGE(TAG, "Failed to create input queue");
        return ESP_ERR_NO_MEM;
    }

    xTaskCreate(uart_bridge_task, "uart_in", 3072, NULL, 3, NULL);

    ESP_LOGI(TAG, "Input service ready (queue depth %d, UART bridged)",
             INPUT_QUEUE_DEPTH);
    return ESP_OK;
}

int input_push_key(int key)
{
    if (!s_queue) return -1;
    return (xQueueSend(s_queue, &key, 0) == pdTRUE) ? 0 : -1;
}

int input_get_key(uint32_t timeout_ms)
{
    if (!s_queue) return -1;

    TaskHandle_t self = xTaskGetCurrentTaskHandle();
    int remaining = (int)timeout_ms;
    const int slice = 50;   // ms — re-check focus / count down in small steps

    for (;;) {
        // Not the focused task: block here until this task is focused. We do NOT
        // count down or return -1, so a backgrounded app pauses completely
        // (its draw loop stops) instead of spinning and fighting for the
        // display. It resumes reading once switched back to the foreground.
        if (s_focus != NULL && s_focus != self) {
            vTaskDelay(pdMS_TO_TICKS(slice));
            continue;
        }

        // Just gained focus? Deliver a one-shot repaint so passive apps (which
        // only draw on events) refresh their screen immediately on switch-in.
        if (self == s_repaint_target) {
            s_repaint_target = NULL;
            return INPUT_KEY_REPAINT;
        }

        int step = (remaining < 0 || remaining > slice) ? slice : remaining;
        int key;
        if (xQueueReceive(s_queue, &key, pdMS_TO_TICKS(step)) == pdTRUE) {
            if (key == INPUT_KEY_SWITCH) {      // TAB: cycle now, open jump window
                if (s_switch_cb) s_switch_cb(-2);
                s_jump_until = esp_timer_get_time() + 300000;   // 300 ms
                continue;
            }
            // A digit right after TAB re-targets focus directly (never delivered).
            if (s_jump_until && esp_timer_get_time() < s_jump_until) {
                if (key >= '1' && key <= '9') { s_jump_until = 0; if (s_switch_cb) s_switch_cb(key - '1'); continue; }
                if (key == '0')               { s_jump_until = 0; if (s_switch_cb) s_switch_cb(-1);        continue; }
            }
            s_jump_until = 0;                   // any other key closes the window
            return key;
        }
        if (remaining >= 0) {
            remaining -= step;
            if (remaining <= 0) return -1;
        }
        // remaining < 0 would mean block forever; callers pass finite timeouts.
    }
}

void input_flush(void)
{
    if (s_queue) xQueueReset(s_queue);
}

// input <push <text> | read | status> — exercises the service without hardware.
static int cmd_input(int argc, char **argv)
{
    if (argc < 2) {
        vconsole_printf("Usage: input <push <text>|read|status>\n");
        return 0;
    }

    if (strcmp(argv[1], "push") == 0) {
        if (argc < 3) {
            vconsole_printf("Usage: input push <text>\n");
            return 0;
        }
        int n = 0;
        for (const char *p = argv[2]; *p; p++)
            if (input_push_key((int)(unsigned char)*p) == 0) n++;
        vconsole_printf("Pushed %d key(s)\n", n);
        return 0;
    }

    if (strcmp(argv[1], "read") == 0) {
        char buf[INPUT_QUEUE_DEPTH + 1];
        int key, n = 0;
        while (n < INPUT_QUEUE_DEPTH && (key = input_get_key(0)) >= 0)
            buf[n++] = (char)key;
        buf[n] = '\0';
        vconsole_printf("Read %d key(s): \"%s\"\n", n, buf);
        return 0;
    }

    if (strcmp(argv[1], "status") == 0) {
        unsigned waiting = s_queue ? (unsigned)uxQueueMessagesWaiting(s_queue) : 0;
        vconsole_printf("Input queue: %u/%d waiting\n", waiting, INPUT_QUEUE_DEPTH);
        return 0;
    }

    vconsole_printf("Unknown subcommand: %s\n", argv[1]);
    return 0;
}

esp_err_t cmd_input_register(void)
{
    const esp_console_cmd_t cmd = {
        .command = "input",
        .help = "Input service: input <push <text>|read|status>",
        .func = cmd_input,
    };
    return esp_console_cmd_register(&cmd);
}
