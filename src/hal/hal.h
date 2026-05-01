#ifndef HAL_H
#define HAL_H

#include "esp_err.h"
#include "driver/spi_master.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct hal_storage_ops {
    esp_err_t (*mount)(void);
    esp_err_t (*unmount)(void);
    bool      (*is_mounted)(void);
    void      (*print_info)(void);
} hal_storage_ops_t;

typedef struct hal_bus_ops {
    esp_err_t (*spi_init)(spi_host_device_t host, int mosi, int miso,
                          int clk, int max_transfer);
    esp_err_t (*spi_add_device)(spi_host_device_t host,
                                const spi_device_interface_config_t *cfg,
                                spi_device_handle_t *handle);
    esp_err_t (*spi_lock)(spi_device_handle_t handle);
    void      (*spi_unlock)(spi_device_handle_t handle);
} hal_bus_ops_t;

typedef struct hal_klog_ops {
    void (*dump)(void);
} hal_klog_ops_t;

typedef struct hal_vconsole_ops hal_vconsole_ops_t;

typedef struct hal_display_ops {
    esp_err_t (*init)(void);
    void      (*shutdown)(void);
    void      (*clear)(void);
    void      (*flush)(void);
    void      (*set_pixel)(int x, int y, uint8_t color);
    void      (*draw_text)(int x, int y, const char *text);
    int       (*get_width)(void);
    int       (*get_height)(void);
} hal_display_ops_t;

typedef struct hal_input_ops {
    esp_err_t (*init)(void);
    void      (*shutdown)(void);
    int       (*read_key)(void);
    bool      (*key_available)(void);
} hal_input_ops_t;

typedef struct hal_timer_ops {
    esp_err_t (*init)(void);
    uint64_t  (*get_time_us)(void);
    esp_err_t (*set_alarm)(uint64_t us, void (*cb)(void *arg), void *arg);
    esp_err_t (*cancel_alarm)(void);
} hal_timer_ops_t;

typedef struct hal_net_ops {
    esp_err_t (*init)(void);
    void      (*shutdown)(void);
    esp_err_t (*connect)(const char *ssid, const char *pass);
    esp_err_t (*disconnect)(void);
    bool      (*is_connected)(void);
    esp_err_t (*get_ip)(char *buf, size_t buf_size);
} hal_net_ops_t;

#endif
