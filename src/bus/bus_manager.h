#ifndef BUS_MANAGER_H
#define BUS_MANAGER_H

#include "esp_err.h"
#include "driver/spi_master.h"
#include "hal/hal.h"

esp_err_t bus_init(void);

esp_err_t bus_spi_init(spi_host_device_t host, int mosi, int miso,
                       int clk, int max_transfer);

esp_err_t bus_spi_add_device(spi_host_device_t host,
                             const spi_device_interface_config_t *dev_cfg,
                             spi_device_handle_t *handle);

esp_err_t bus_spi_lock(spi_device_handle_t handle);
void      bus_spi_unlock(spi_device_handle_t handle);

const hal_bus_ops_t *bus_get_ops(void);

#endif
