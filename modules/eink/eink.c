#include "module_types.h"
#include "display_types.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>

extern void module_register(module_exports_t *exports);
extern int  display_mux_register(const char *name, const display_driver_ops_t *ops);
extern int  display_mux_unregister(const char *name);
extern int  vconsole_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
extern void *heap_caps_malloc(size_t size, uint32_t caps);
extern void  heap_caps_free(void *ptr);
extern int   gpio_set_level(int gpio, uint32_t level);
extern int   gpio_get_level(int gpio);
extern int   gpio_config(const void *cfg);

#define MALLOC_CAP_SPIRAM  (1 << 10)

#define EPD_WIDTH    200
#define EPD_HEIGHT   200
#define EPD_BUF_SIZE (EPD_WIDTH * EPD_HEIGHT / 8)

#define EPD_ROWS  25
#define EPD_COLS  33

#define PIN_CLK   12
#define PIN_MOSI  11
#define PIN_CS    16
#define PIN_DC    15
#define PIN_RST   17
#define PIN_BUSY  18

#define GHOSTING_THRESHOLD  10

static uint8_t *s_framebuf;
static uint8_t *s_oldbuf;
static int s_partial_count;
static int s_ghosting_limit = GHOSTING_THRESHOLD;

static void epd_gpio_init(void)
{
    uint64_t out_mask = (1ULL << PIN_CS) | (1ULL << PIN_DC) | (1ULL << PIN_RST);
    uint32_t out_cfg[5];
    memset(out_cfg, 0, sizeof(out_cfg));
    out_cfg[0] = out_mask & 0xFFFFFFFF;
    out_cfg[1] = out_mask >> 32;
    out_cfg[2] = 2;
    gpio_config(out_cfg);

    uint64_t in_mask = (1ULL << PIN_BUSY);
    uint32_t in_cfg[5];
    memset(in_cfg, 0, sizeof(in_cfg));
    in_cfg[0] = in_mask & 0xFFFFFFFF;
    in_cfg[1] = in_mask >> 32;
    in_cfg[2] = 1;
    gpio_config(in_cfg);

    gpio_set_level(PIN_CS, 1);
    gpio_set_level(PIN_DC, 0);
    gpio_set_level(PIN_RST, 1);
}

static void epd_spi_write_byte(uint8_t data)
{
    gpio_set_level(PIN_CS, 0);
    for (int i = 7; i >= 0; i--) {
        gpio_set_level(PIN_CLK, 0);
        gpio_set_level(PIN_MOSI, (data >> i) & 1);
        gpio_set_level(PIN_CLK, 1);
    }
    gpio_set_level(PIN_CS, 1);
}

static void epd_cmd(uint8_t cmd)
{
    gpio_set_level(PIN_DC, 0);
    epd_spi_write_byte(cmd);
}

static void epd_data(uint8_t data)
{
    gpio_set_level(PIN_DC, 1);
    epd_spi_write_byte(data);
}

static void epd_wait_busy(void)
{
    int timeout = 50000;
    while (gpio_get_level(PIN_BUSY) == 1 && timeout-- > 0) {
        for (volatile int i = 0; i < 100; i++);
    }
}

static void epd_reset(void)
{
    gpio_set_level(PIN_RST, 1);
    for (volatile int i = 0; i < 50000; i++);
    gpio_set_level(PIN_RST, 0);
    for (volatile int i = 0; i < 50000; i++);
    gpio_set_level(PIN_RST, 1);
    for (volatile int i = 0; i < 50000; i++);
}

static void epd_hw_init_full(void)
{
    epd_reset();
    epd_wait_busy();

    epd_cmd(0x12);
    epd_wait_busy();

    epd_cmd(0x01);
    epd_data((EPD_HEIGHT - 1) & 0xFF);
    epd_data(((EPD_HEIGHT - 1) >> 8) & 0xFF);
    epd_data(0x00);

    epd_cmd(0x11);
    epd_data(0x01);

    epd_cmd(0x44);
    epd_data(0x00);
    epd_data((EPD_WIDTH / 8) - 1);

    epd_cmd(0x45);
    epd_data((EPD_HEIGHT - 1) & 0xFF);
    epd_data(((EPD_HEIGHT - 1) >> 8) & 0xFF);
    epd_data(0x00);
    epd_data(0x00);

    epd_cmd(0x4E);
    epd_data(0x00);
    epd_cmd(0x4F);
    epd_data((EPD_HEIGHT - 1) & 0xFF);
    epd_data(((EPD_HEIGHT - 1) >> 8) & 0xFF);

    epd_cmd(0x3C);
    epd_data(0x05);

    epd_cmd(0x21);
    epd_data(0x00);
    epd_data(0x80);
}

static void epd_hw_init_partial(void)
{
    epd_reset();
    epd_wait_busy();

    epd_cmd(0x3C);
    epd_data(0x80);

    epd_cmd(0x01);
    epd_data((EPD_HEIGHT - 1) & 0xFF);
    epd_data(((EPD_HEIGHT - 1) >> 8) & 0xFF);
    epd_data(0x00);

    epd_cmd(0x11);
    epd_data(0x01);

    epd_cmd(0x44);
    epd_data(0x00);
    epd_data((EPD_WIDTH / 8) - 1);

    epd_cmd(0x45);
    epd_data((EPD_HEIGHT - 1) & 0xFF);
    epd_data(((EPD_HEIGHT - 1) >> 8) & 0xFF);
    epd_data(0x00);
    epd_data(0x00);

    epd_cmd(0x4E);
    epd_data(0x00);
    epd_cmd(0x4F);
    epd_data((EPD_HEIGHT - 1) & 0xFF);
    epd_data(((EPD_HEIGHT - 1) >> 8) & 0xFF);

    epd_cmd(0x21);
    epd_data(0x00);
    epd_data(0x80);
}

static void epd_display_full(const uint8_t *buf)
{
    epd_hw_init_full();

    epd_cmd(0x24);
    for (int i = 0; i < EPD_BUF_SIZE; i++) {
        epd_data(buf[i]);
    }

    epd_cmd(0x26);
    for (int i = 0; i < EPD_BUF_SIZE; i++) {
        epd_data(buf[i]);
    }

    epd_cmd(0x22);
    epd_data(0xF7);
    epd_cmd(0x20);
    epd_wait_busy();
}

static void epd_display_partial(const uint8_t *buf)
{
    epd_hw_init_partial();

    epd_cmd(0x26);
    for (int i = 0; i < EPD_BUF_SIZE; i++) {
        epd_data(s_oldbuf[i]);
    }

    epd_cmd(0x24);
    for (int i = 0; i < EPD_BUF_SIZE; i++) {
        epd_data(buf[i]);
    }

    epd_cmd(0x22);
    epd_data(0xFF);
    epd_cmd(0x20);
    epd_wait_busy();
}

static const uint8_t font_5x7[] = {
    0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x5F,0x00,0x00,
    0x00,0x07,0x00,0x07,0x00,
    0x14,0x7F,0x14,0x7F,0x14,
    0x24,0x2A,0x7F,0x2A,0x12,
    0x23,0x13,0x08,0x64,0x62,
    0x36,0x49,0x55,0x22,0x50,
    0x00,0x05,0x03,0x00,0x00,
    0x00,0x1C,0x22,0x41,0x00,
    0x00,0x41,0x22,0x1C,0x00,
    0x14,0x08,0x3E,0x08,0x14,
    0x08,0x08,0x3E,0x08,0x08,
    0x00,0x50,0x30,0x00,0x00,
    0x08,0x08,0x08,0x08,0x08,
    0x00,0x60,0x60,0x00,0x00,
    0x20,0x10,0x08,0x04,0x02,
    0x3E,0x51,0x49,0x45,0x3E,
    0x00,0x42,0x7F,0x40,0x00,
    0x42,0x61,0x51,0x49,0x46,
    0x21,0x41,0x45,0x4B,0x31,
    0x18,0x14,0x12,0x7F,0x10,
    0x27,0x45,0x45,0x45,0x39,
    0x3C,0x4A,0x49,0x49,0x30,
    0x01,0x71,0x09,0x05,0x03,
    0x36,0x49,0x49,0x49,0x36,
    0x06,0x49,0x49,0x29,0x1E,
    0x00,0x36,0x36,0x00,0x00,
    0x00,0x56,0x36,0x00,0x00,
    0x08,0x14,0x22,0x41,0x00,
    0x14,0x14,0x14,0x14,0x14,
    0x00,0x41,0x22,0x14,0x08,
    0x02,0x01,0x51,0x09,0x06,
    0x32,0x49,0x79,0x41,0x3E,
    0x7E,0x11,0x11,0x11,0x7E,
    0x7F,0x49,0x49,0x49,0x36,
    0x3E,0x41,0x41,0x41,0x22,
    0x7F,0x41,0x41,0x22,0x1C,
    0x7F,0x49,0x49,0x49,0x41,
    0x7F,0x09,0x09,0x09,0x01,
    0x3E,0x41,0x49,0x49,0x7A,
    0x7F,0x08,0x08,0x08,0x7F,
    0x00,0x41,0x7F,0x41,0x00,
    0x20,0x40,0x41,0x3F,0x01,
    0x7F,0x08,0x14,0x22,0x41,
    0x7F,0x40,0x40,0x40,0x40,
    0x7F,0x02,0x0C,0x02,0x7F,
    0x7F,0x04,0x08,0x10,0x7F,
    0x3E,0x41,0x41,0x41,0x3E,
    0x7F,0x09,0x09,0x09,0x06,
    0x3E,0x41,0x51,0x21,0x5E,
    0x7F,0x09,0x19,0x29,0x46,
    0x46,0x49,0x49,0x49,0x31,
    0x01,0x01,0x7F,0x01,0x01,
    0x3F,0x40,0x40,0x40,0x3F,
    0x1F,0x20,0x40,0x20,0x1F,
    0x3F,0x40,0x38,0x40,0x3F,
    0x63,0x14,0x08,0x14,0x63,
    0x07,0x08,0x70,0x08,0x07,
    0x61,0x51,0x49,0x45,0x43,
    0x00,0x7F,0x41,0x41,0x00,
    0x02,0x04,0x08,0x10,0x20,
    0x00,0x41,0x41,0x7F,0x00,
    0x04,0x02,0x01,0x02,0x04,
    0x40,0x40,0x40,0x40,0x40,
    0x00,0x01,0x02,0x04,0x00,
    0x20,0x54,0x54,0x54,0x78,
    0x7F,0x48,0x44,0x44,0x38,
    0x38,0x44,0x44,0x44,0x20,
    0x38,0x44,0x44,0x48,0x7F,
    0x38,0x54,0x54,0x54,0x18,
    0x08,0x7E,0x09,0x01,0x02,
    0x0C,0x52,0x52,0x52,0x3E,
    0x7F,0x08,0x04,0x04,0x78,
    0x00,0x44,0x7D,0x40,0x00,
    0x20,0x40,0x44,0x3D,0x00,
    0x7F,0x10,0x28,0x44,0x00,
    0x00,0x41,0x7F,0x40,0x00,
    0x7C,0x04,0x18,0x04,0x78,
    0x7C,0x08,0x04,0x04,0x78,
    0x38,0x44,0x44,0x44,0x38,
    0x7C,0x14,0x14,0x14,0x08,
    0x08,0x14,0x14,0x18,0x7C,
    0x7C,0x08,0x04,0x04,0x08,
    0x48,0x54,0x54,0x54,0x20,
    0x04,0x3F,0x44,0x40,0x20,
    0x3C,0x40,0x40,0x20,0x7C,
    0x1C,0x20,0x40,0x20,0x1C,
    0x3C,0x40,0x30,0x40,0x3C,
    0x44,0x28,0x10,0x28,0x44,
    0x0C,0x50,0x50,0x50,0x3C,
    0x44,0x64,0x54,0x4C,0x44,
    0x00,0x08,0x36,0x41,0x00,
    0x00,0x00,0x7F,0x00,0x00,
    0x00,0x41,0x36,0x08,0x00,
    0x10,0x08,0x08,0x10,0x08,
    0x00,0x00,0x00,0x00,0x00,
};

static void epd_set_pixel(int x, int y, int color)
{
    if (x < 0 || x >= EPD_WIDTH || y < 0 || y >= EPD_HEIGHT) return;
    int byte_idx = (y * EPD_WIDTH + x) / 8;
    int bit_idx = 7 - (x % 8);
    if (color)
        s_framebuf[byte_idx] |= (1 << bit_idx);
    else
        s_framebuf[byte_idx] &= ~(1 << bit_idx);
}

static void epd_draw_char(int col, int row, char c)
{
    if (!s_framebuf) return;
    if (c < 0x20 || c > 0x7E) c = ' ';
    int idx = (c - 0x20) * 5;

    int x0 = col * 6;
    int y0 = row * 8;

    for (int dx = 0; dx < 5; dx++) {
        uint8_t bits = font_5x7[idx + dx];
        for (int dy = 0; dy < 8; dy++) {
            epd_set_pixel(x0 + dx, y0 + dy, (bits >> dy) & 1 ? 0 : 1);
        }
    }
    for (int dy = 0; dy < 8; dy++) {
        epd_set_pixel(x0 + 5, y0 + dy, 1);
    }
}

static void eink_render(const char *text, size_t len)
{
    if (!s_framebuf || !s_oldbuf) return;

    memset(s_framebuf, 0xFF, EPD_BUF_SIZE);

    int row = 0, col = 0;
    for (size_t i = 0; i < len && row < EPD_ROWS; i++) {
        if (text[i] == '\n') {
            row++;
            col = 0;
            continue;
        }
        if (col < EPD_COLS) {
            epd_draw_char(col, row, text[i]);
            col++;
        }
    }

    if (s_partial_count >= s_ghosting_limit) {
        epd_display_full(s_framebuf);
        s_partial_count = 0;
    } else {
        epd_display_partial(s_framebuf);
        s_partial_count++;
    }

    memcpy(s_oldbuf, s_framebuf, EPD_BUF_SIZE);
}

static void eink_clear(void)
{
    if (!s_framebuf) return;
    memset(s_framebuf, 0xFF, EPD_BUF_SIZE);
    epd_display_full(s_framebuf);
    if (s_oldbuf) memcpy(s_oldbuf, s_framebuf, EPD_BUF_SIZE);
    s_partial_count = 0;
}

static int eink_get_rows(void) { return EPD_ROWS; }
static int eink_get_cols(void) { return EPD_COLS; }
static uint32_t eink_get_caps(void) { return (1 << 0); }

static const display_driver_ops_t s_display_ops = {
    .init     = NULL,
    .shutdown = NULL,
    .render   = eink_render,
    .clear    = eink_clear,
    .get_rows = eink_get_rows,
    .get_cols = eink_get_cols,
    .get_caps = eink_get_caps,
};

static int eink_start(void)
{
    s_framebuf = (uint8_t *)heap_caps_malloc(EPD_BUF_SIZE, MALLOC_CAP_SPIRAM);
    s_oldbuf   = (uint8_t *)heap_caps_malloc(EPD_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!s_framebuf || !s_oldbuf) {
        if (s_framebuf) heap_caps_free(s_framebuf);
        if (s_oldbuf)   heap_caps_free(s_oldbuf);
        s_framebuf = NULL;
        s_oldbuf = NULL;
        return -1;
    }

    memset(s_framebuf, 0xFF, EPD_BUF_SIZE);
    memset(s_oldbuf,   0xFF, EPD_BUF_SIZE);
    s_partial_count = 0;

    epd_gpio_init();
    eink_clear();

    display_mux_register("eink", &s_display_ops);
    vconsole_printf("[eink] Waveshare 200x200 e-paper ready (ghosting limit: %d)\n",
                    s_ghosting_limit);
    return 0;
}

static int eink_stop(void)
{
    display_mux_unregister("eink");

    epd_cmd(0x10);
    epd_data(0x01);
    epd_wait_busy();

    if (s_framebuf) { heap_caps_free(s_framebuf); s_framebuf = NULL; }
    if (s_oldbuf)   { heap_caps_free(s_oldbuf);   s_oldbuf = NULL; }

    vconsole_printf("[eink] Display driver stopped\n");
    return 0;
}

static module_info_t info = {
    .magic       = MODULE_MAGIC,
    .abi_version = MODULE_ABI_VERSION,
    .name        = "eink",
    .version     = "1.0.0",
    .requires    = "vconsole",
    .flags       = 0,
};

static module_exports_t exports = {
    .info  = &info,
    .start = eink_start,
    .stop  = eink_stop,
};

int main(int argc, char *argv[])
{
    module_register(&exports);
    return 0;
}
