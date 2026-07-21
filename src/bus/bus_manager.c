#include "bus_manager.h"
#include "config/pin_config.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "bus";

// ---- Display SPI (hardware SPI2 + DMA), replaces per-module bit-banging -----
// The display bus (SPI2) is shared by OLED + e-ink. Pins are module-owned
// (architecture: modules read pin config), so disp_spi_add() takes them and
// lazily initialises the bus on first use. CS/DC stay module-managed via GPIO
// (spics_io_num=-1), so a driver can hold CS across a whole command+data burst.
#define DISPLAY_SPI_HOST   SPI2_HOST
#define DISP_SPI_CHUNK     4096

static bool             s_disp_bus_ready = false;
static uint8_t         *s_disp_bounce = NULL;   // internal DMA-capable bounce
static SemaphoreHandle_t s_disp_lock = NULL;

esp_err_t bus_spi_init(spi_host_device_t host, int mosi, int miso,
                       int clk, int max_transfer)
{
    spi_bus_config_t cfg = {
        .mosi_io_num   = mosi,
        .miso_io_num   = miso,
        .sclk_io_num   = clk,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = max_transfer,
    };

    esp_err_t ret = spi_bus_initialize(host, &cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI host %d init failed: %s", host, esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t bus_init(void)
{
    // SD card SPI bus
    esp_err_t ret = bus_spi_init(SD_SPI_HOST, PIN_SD_MOSI, PIN_SD_MISO,
                                 PIN_SD_CLK, 4096);
    if (ret != ESP_OK) return ret;
    ESP_LOGI(TAG, "SD SPI ready (CLK=%d MOSI=%d MISO=%d CS=%d)",
             PIN_SD_CLK, PIN_SD_MOSI, PIN_SD_MISO, PIN_SD_CS);

    // I2C bus
    i2c_config_t i2c_conf = {
        .mode             = I2C_MODE_MASTER,
        .sda_io_num       = PIN_I2C_SDA,
        .scl_io_num       = PIN_I2C_SCL,
        .sda_pullup_en    = GPIO_PULLUP_ENABLE,
        .scl_pullup_en    = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000,
    };

    ret = i2c_param_config(I2C_NUM_0, &i2c_conf);
    if (ret != ESP_OK) return ret;

    ret = i2c_driver_install(I2C_NUM_0, i2c_conf.mode, 0, 0, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "I2C ready (SDA=%d SCL=%d)", PIN_I2C_SDA, PIN_I2C_SCL);

    return ESP_OK;
}

esp_err_t bus_spi_add_device(spi_host_device_t host,
                             const spi_device_interface_config_t *dev_cfg,
                             spi_device_handle_t *handle)
{
    return spi_bus_add_device(host, dev_cfg, handle);
}

esp_err_t bus_spi_lock(spi_device_handle_t handle)
{
    return spi_device_acquire_bus(handle, portMAX_DELAY);
}

void bus_spi_unlock(spi_device_handle_t handle)
{
    spi_device_release_bus(handle);
}

esp_err_t bus_i2c_read(uint8_t addr, uint8_t *data, size_t len)
{
    if (!data || len == 0) return ESP_ERR_INVALID_ARG;
    return i2c_master_read_from_device(I2C_NUM_0, addr, data, len,
                                       pdMS_TO_TICKS(100));
}

esp_err_t bus_i2c_write(uint8_t addr, const uint8_t *data, size_t len)
{
    if (!data || len == 0) return ESP_ERR_INVALID_ARG;
    return i2c_master_write_to_device(I2C_NUM_0, addr, data, len,
                                      pdMS_TO_TICKS(100));
}

esp_err_t disp_spi_add(int clk, int mosi, int clk_hz, void **out_handle)
{
    if (!out_handle) return ESP_ERR_INVALID_ARG;

    if (!s_disp_bus_ready) {
        esp_err_t ret = bus_spi_init(DISPLAY_SPI_HOST, mosi, -1, clk, DISP_SPI_CHUNK);
        if (ret != ESP_OK) return ret;

        s_disp_bounce = heap_caps_malloc(DISP_SPI_CHUNK,
                                         MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        s_disp_lock = xSemaphoreCreateMutex();
        if (!s_disp_bounce || !s_disp_lock) {
            ESP_LOGE(TAG, "display SPI bounce/lock alloc failed");
            return ESP_ERR_NO_MEM;
        }
        s_disp_bus_ready = true;
        ESP_LOGI(TAG, "Display SPI ready (SPI2, CLK=%d MOSI=%d)", clk, mosi);
    }

    // No hardware CS: the driver toggles CS/DC via GPIO, so it can hold CS low
    // across a whole command+data burst.
    spi_device_interface_config_t dev = {
        .clock_speed_hz = clk_hz,
        .mode           = 0,
        .spics_io_num   = -1,
        .queue_size     = 1,
    };
    spi_device_handle_t h;
    esp_err_t ret = spi_bus_add_device(DISPLAY_SPI_HOST, &dev, &h);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "display SPI add device failed: %s", esp_err_to_name(ret));
        return ret;
    }
    *out_handle = h;
    return ESP_OK;
}

esp_err_t disp_spi_write(void *handle, const uint8_t *data, size_t len)
{
    if (!handle || !data || len == 0) return ESP_ERR_INVALID_ARG;
    if (!s_disp_bounce) return ESP_ERR_INVALID_STATE;

    spi_device_handle_t h = (spi_device_handle_t)handle;
    esp_err_t ret = ESP_OK;

    // Copy through an internal DMA-capable bounce buffer in chunks — robust for
    // PSRAM-resident framebuffers without relying on SPI-DMA-from-PSRAM.
    xSemaphoreTake(s_disp_lock, portMAX_DELAY);
    for (size_t off = 0; off < len && ret == ESP_OK; ) {
        size_t n = len - off;
        if (n > DISP_SPI_CHUNK) n = DISP_SPI_CHUNK;
        memcpy(s_disp_bounce, data + off, n);

        spi_transaction_t t = { .length = n * 8, .tx_buffer = s_disp_bounce };
        ret = spi_device_transmit(h, &t);
        off += n;
    }
    xSemaphoreGive(s_disp_lock);
    return ret;
}

static const hal_bus_ops_t s_bus_ops = {
    .spi_init       = bus_spi_init,
    .spi_add_device = bus_spi_add_device,
    .spi_lock       = bus_spi_lock,
    .spi_unlock     = bus_spi_unlock,
};

const hal_bus_ops_t *bus_get_ops(void) { return &s_bus_ops; }
