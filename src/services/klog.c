#include "klog.h"
#include "hal/hal.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#define KLOG_BUF_SIZE 8192

static char s_buf[KLOG_BUF_SIZE];
static int s_head = 0;
static int s_len = 0;
static vprintf_like_t s_orig_vprintf = NULL;

static void klog_append(const char *str, int n)
{
    for (int i = 0; i < n; i++) {
        s_buf[s_head] = str[i];
        s_head = (s_head + 1) % KLOG_BUF_SIZE;
        if (s_len < KLOG_BUF_SIZE) s_len++;
    }
}

static int klog_vprintf(const char *fmt, va_list args)
{
    char line[256];
    int n = vsnprintf(line, sizeof(line), fmt, args);
    if (n > 0) {
        int store = (n < (int)sizeof(line)) ? n : (int)sizeof(line) - 1;
        klog_append(line, store);
    }
    if (s_orig_vprintf) {
        return s_orig_vprintf(fmt, args);
    }
    return n;
}

esp_err_t klog_init(void)
{
    s_orig_vprintf = esp_log_set_vprintf(klog_vprintf);
    return ESP_OK;
}

void klog_dump(void)
{
    if (s_len == 0) {
        printf("(empty)\n");
        return;
    }
    int start = (s_len < KLOG_BUF_SIZE) ? 0 : s_head;
    for (int i = 0; i < s_len; i++) {
        putchar(s_buf[(start + i) % KLOG_BUF_SIZE]);
    }
}

static const hal_klog_ops_t s_klog_ops = {
    .dump = klog_dump,
};

const hal_klog_ops_t *klog_get_ops(void) { return &s_klog_ops; }
