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

// I2C (I2C_NUM_0) read/write helpers — exported to modules so input-source
// drivers (e.g. CardKB at 0x5F) can talk to the kernel I2C bus.
esp_err_t bus_i2c_read (uint8_t addr, uint8_t *data, size_t len);
esp_err_t bus_i2c_write(uint8_t addr, const uint8_t *data, size_t len);

// Display SPI (hardware SPI2 + DMA) — exported to display driver modules to
// replace bit-banging. disp_spi_add() lazily initialises the shared SPI2 bus
// with the given CLK/MOSI pins and adds a device at clk_hz (no hardware CS —
// the driver keeps managing CS/DC via GPIO). disp_spi_write() clocks bytes out
// via DMA (through an internal bounce buffer). out_handle is a void* cookie.
esp_err_t disp_spi_add  (int clk, int mosi, int clk_hz, void **out_handle);
esp_err_t disp_spi_write(void *handle, const uint8_t *data, size_t len);

const hal_bus_ops_t *bus_get_ops(void);

#endif
