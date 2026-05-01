#ifndef VCONSOLE_H
#define VCONSOLE_H

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <stddef.h>

#define VCONSOLE_NEW_DATA_BIT  (1 << 0)
#define VCONSOLE_CLEARED_BIT   (1 << 1)

typedef struct hal_vconsole_ops {
    int    (*write)(const char *data, size_t len);
    size_t (*read_tail)(char *buf, size_t buf_size);
    size_t (*get_lines)(int count, char *buf, size_t buf_size);
    void   (*clear)(void);
    size_t (*used)(void);
} hal_vconsole_ops_t;

esp_err_t vconsole_init(size_t buf_size);

int vconsole_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int vconsole_putchar(char c);
int vconsole_write(const char *data, size_t len);

size_t vconsole_read_tail(char *buf, size_t buf_size);
size_t vconsole_get_lines(int count, char *buf, size_t buf_size);

size_t vconsole_used(void);
size_t vconsole_capacity(void);
void   vconsole_clear(void);

EventGroupHandle_t vconsole_get_event_group(void);

void vconsole_attach_stdout(void);
void vconsole_detach_stdout(void);

const hal_vconsole_ops_t *vconsole_get_ops(void);
esp_err_t cmd_vconsole_register(void);

#endif
