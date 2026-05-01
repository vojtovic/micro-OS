#include "vconsole.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_console.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "vconsole";

/* ── ring buffer state ────────────────────────────────────── */

static char *s_buf = NULL;
static size_t s_cap = 0;
static size_t s_head = 0;
static size_t s_len = 0;
static SemaphoreHandle_t s_mutex = NULL;
static EventGroupHandle_t s_event_group = NULL;

/* ── line index (circular array of offsets) ───────────────── */

#define LINE_INDEX_MAX 1024

static uint16_t *s_line_idx = NULL;
static int s_line_head = 0;
static int s_line_count = 0;

static void line_index_push(size_t ring_offset)
{
    s_line_idx[s_line_head] = (uint16_t)(ring_offset % s_cap);
    s_line_head = (s_line_head + 1) % LINE_INDEX_MAX;
    if (s_line_count < LINE_INDEX_MAX) s_line_count++;
}

/* ── stdout hook state ────────────────────────────────────── */

static FILE *s_orig_stdout = NULL;

/* ── ring buffer core ─────────────────────────────────────── */

static void ring_write(const char *data, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        s_buf[s_head] = data[i];
        s_head = (s_head + 1) % s_cap;
        if (s_len < s_cap) s_len++;

        if (data[i] == '\n') {
            line_index_push(s_head);
        }
    }
}

/* ── when ring wraps, line index entries pointing to overwritten
     data must be discarded ───────────────────────────────── */

static void line_index_gc(void)
{
    if (s_line_count == 0 || s_len < s_cap) return;

    size_t oldest_valid = (s_head + s_cap - s_len) % s_cap;
    while (s_line_count > 0) {
        int oldest_idx = (s_line_head + LINE_INDEX_MAX - s_line_count) % LINE_INDEX_MAX;
        uint16_t off = s_line_idx[oldest_idx];

        size_t dist;
        if (off >= oldest_valid)
            dist = off - oldest_valid;
        else
            dist = s_cap - oldest_valid + off;

        if (dist < s_len) break;
        s_line_count--;
    }
}

/* ── public API ───────────────────────────────────────────── */

esp_err_t vconsole_init(size_t buf_size)
{
    if (buf_size > UINT16_MAX) {
        ESP_LOGE(TAG, "Buffer size exceeds uint16_t line index range");
        return ESP_ERR_INVALID_ARG;
    }

    s_buf = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    if (!s_buf) {
        ESP_LOGE(TAG, "Failed to allocate %u bytes from PSRAM", (unsigned)buf_size);
        return ESP_ERR_NO_MEM;
    }

    s_line_idx = heap_caps_malloc(LINE_INDEX_MAX * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if (!s_line_idx) {
        heap_caps_free(s_buf);
        s_buf = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        heap_caps_free(s_line_idx);
        heap_caps_free(s_buf);
        s_buf = NULL;
        s_line_idx = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_event_group = xEventGroupCreate();
    if (!s_event_group) {
        vSemaphoreDelete(s_mutex);
        heap_caps_free(s_line_idx);
        heap_caps_free(s_buf);
        s_buf = NULL;
        s_line_idx = NULL;
        s_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_cap = buf_size;
    s_head = 0;
    s_len = 0;
    s_line_head = 0;
    s_line_count = 0;

    ESP_LOGI(TAG, "Virtual console: %u KB buffer, %d-line index (PSRAM)",
             (unsigned)(buf_size / 1024), LINE_INDEX_MAX);
    return ESP_OK;
}

int vconsole_write(const char *data, size_t len)
{
    if (!s_buf || !data || len == 0) return 0;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    ring_write(data, len);
    line_index_gc();
    xSemaphoreGive(s_mutex);

    xEventGroupSetBits(s_event_group, VCONSOLE_NEW_DATA_BIT);

    FILE *out = s_orig_stdout ? s_orig_stdout : stdout;
    fwrite(data, 1, len, out);
    fflush(out);
    return (int)len;
}

int vconsole_printf(const char *fmt, ...)
{
    char line[512];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);

    if (n <= 0) return n;
    size_t w = (n < (int)sizeof(line)) ? (size_t)n : sizeof(line) - 1;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    ring_write(line, w);
    line_index_gc();
    xSemaphoreGive(s_mutex);

    xEventGroupSetBits(s_event_group, VCONSOLE_NEW_DATA_BIT);

    FILE *out = s_orig_stdout ? s_orig_stdout : stdout;
    fwrite(line, 1, w, out);
    fflush(out);
    return n;
}

int vconsole_putchar(char c)
{
    if (!s_buf) {
        putchar(c);
        return c;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    ring_write(&c, 1);
    line_index_gc();
    xSemaphoreGive(s_mutex);

    xEventGroupSetBits(s_event_group, VCONSOLE_NEW_DATA_BIT);

    if (s_orig_stdout)
        fputc(c, s_orig_stdout);
    else
        putchar(c);
    return c;
}

size_t vconsole_read_tail(char *buf, size_t buf_size)
{
    if (!s_buf || !buf || buf_size == 0) return 0;
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    size_t to_read = (s_len < buf_size) ? s_len : buf_size;
    size_t start = (s_head + s_cap - to_read) % s_cap;

    for (size_t i = 0; i < to_read; i++) {
        buf[i] = s_buf[(start + i) % s_cap];
    }

    xSemaphoreGive(s_mutex);
    return to_read;
}

size_t vconsole_get_lines(int count, char *buf, size_t buf_size)
{
    if (!s_buf || !buf || buf_size == 0 || count <= 0) return 0;
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    if (s_len == 0) {
        xSemaphoreGive(s_mutex);
        buf[0] = '\0';
        return 0;
    }

    size_t copy_from;

    if (s_line_count > 0 && count <= s_line_count) {
        int idx = (s_line_head + LINE_INDEX_MAX - count) % LINE_INDEX_MAX;
        copy_from = s_line_idx[idx];
    } else {
        size_t ring_start = (s_head + s_cap - s_len) % s_cap;
        int newlines = 0;
        copy_from = 0;
        for (size_t i = s_len; i > 0; i--) {
            char c = s_buf[(ring_start + i - 1) % s_cap];
            if (c == '\n') {
                newlines++;
                if (newlines == count + 1) {
                    copy_from = i;
                    break;
                }
            }
        }
        copy_from = (ring_start + copy_from) % s_cap;
    }

    size_t data_len;
    if (s_head >= copy_from)
        data_len = s_head - copy_from;
    else
        data_len = s_cap - copy_from + s_head;

    size_t to_copy = (data_len < buf_size - 1) ? data_len : buf_size - 1;

    for (size_t i = 0; i < to_copy; i++) {
        buf[i] = s_buf[(copy_from + i) % s_cap];
    }
    buf[to_copy] = '\0';

    xSemaphoreGive(s_mutex);
    return to_copy;
}

size_t vconsole_used(void)
{
    return s_len;
}

size_t vconsole_capacity(void)
{
    return s_cap;
}

void vconsole_clear(void)
{
    if (!s_buf) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_head = 0;
    s_len = 0;
    s_line_head = 0;
    s_line_count = 0;
    xSemaphoreGive(s_mutex);

    xEventGroupSetBits(s_event_group, VCONSOLE_CLEARED_BIT);
}

EventGroupHandle_t vconsole_get_event_group(void)
{
    return s_event_group;
}

/* ── stdout hook (optional) ───────────────────────────────── */

static ssize_t vconsole_cookie_write(void *cookie, const char *data, size_t size)
{
    (void)cookie;
    if (!s_buf || !data || size == 0) return (ssize_t)size;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    ring_write(data, size);
    line_index_gc();
    xSemaphoreGive(s_mutex);

    xEventGroupSetBits(s_event_group, VCONSOLE_NEW_DATA_BIT);

    if (s_orig_stdout) {
        fwrite(data, 1, size, s_orig_stdout);
        fflush(s_orig_stdout);
    }
    return (ssize_t)size;
}

void vconsole_attach_stdout(void)
{
    if (s_orig_stdout) return;
    s_orig_stdout = stdout;

    cookie_io_functions_t funcs = {
        .read  = NULL,
        .write = vconsole_cookie_write,
        .seek  = NULL,
        .close = NULL,
    };
    FILE *new_stdout = fopencookie(NULL, "w", funcs);
    if (new_stdout) {
        setvbuf(new_stdout, NULL, _IONBF, 0);
        stdout = new_stdout;
        ESP_LOGI(TAG, "stdout attached to vconsole");
    } else {
        ESP_LOGW(TAG, "fopencookie failed — stdout not captured");
        s_orig_stdout = NULL;
    }
}

void vconsole_detach_stdout(void)
{
    if (!s_orig_stdout) return;
    fclose(stdout);
    stdout = s_orig_stdout;
    s_orig_stdout = NULL;
    ESP_LOGI(TAG, "stdout detached from vconsole");
}

/* ── HAL vtable ───────────────────────────────────────────── */

static const hal_vconsole_ops_t s_vconsole_ops = {
    .write     = vconsole_write,
    .read_tail = vconsole_read_tail,
    .get_lines = vconsole_get_lines,
    .clear     = vconsole_clear,
    .used      = vconsole_used,
};

const hal_vconsole_ops_t *vconsole_get_ops(void)
{
    return &s_vconsole_ops;
}

/* ── shell commands ───────────────────────────────────────── */

static int cmd_vconsole_fn(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: vconsole <status|clear|tail [n]>\n");
        return 1;
    }

    if (strcmp(argv[1], "status") == 0) {
        printf("Virtual console:\n");
        printf("  buffer:  %u KB (PSRAM)\n", (unsigned)(s_cap / 1024));
        printf("  used:    %u bytes (%.1f%%)\n",
               (unsigned)s_len, s_cap ? (s_len * 100.0 / s_cap) : 0.0);
        printf("  lines:   %d (max %d tracked)\n", s_line_count, LINE_INDEX_MAX);
        printf("  stdout:  %s\n", s_orig_stdout ? "attached" : "direct");
        return 0;
    }

    if (strcmp(argv[1], "clear") == 0) {
        vconsole_clear();
        printf("Virtual console cleared\n");
        return 0;
    }

    if (strcmp(argv[1], "tail") == 0) {
        int n = 20;
        if (argc >= 3) n = atoi(argv[2]);
        if (n <= 0) n = 20;

        char *tmp = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
        if (!tmp) {
            printf("Out of memory\n");
            return 1;
        }
        size_t got = vconsole_get_lines(n, tmp, 4096);
        if (got > 0) {
            fwrite(tmp, 1, got, s_orig_stdout ? s_orig_stdout : stdout);
            FILE *out = s_orig_stdout ? s_orig_stdout : stdout;
            if (tmp[got - 1] != '\n') fputc('\n', out);
            fflush(out);
        } else {
            printf("(empty)\n");
        }
        heap_caps_free(tmp);
        return 0;
    }

    printf("Unknown subcommand: %s\n", argv[1]);
    return 1;
}

esp_err_t cmd_vconsole_register(void)
{
    const esp_console_cmd_t cmd = {
        .command = "vconsole",
        .help    = "Virtual console: status|clear|tail [n]",
        .func    = cmd_vconsole_fn,
    };
    return esp_console_cmd_register(&cmd);
}
