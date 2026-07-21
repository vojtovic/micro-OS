#include "input.h"
#include "vconsole.h"
#include "esp_console.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "input";

#define INPUT_QUEUE_DEPTH 32

static QueueHandle_t s_queue = NULL;

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
    int key;
    if (xQueueReceive(s_queue, &key, pdMS_TO_TICKS(timeout_ms)) == pdTRUE)
        return key;
    return -1;
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
