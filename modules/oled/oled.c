#include "module_types.h"
#include "display_types.h"
#include "gfx_abi.h"          // kernel graphics/text ABI (module-safe)
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
extern int   gpio_config(const void *cfg);
extern int   snprintf(char *str, size_t size, const char *fmt, ...);
// Hardware display SPI (SPI2 + DMA) — replaces bit-banging.
extern int   disp_spi_add(int clk, int mosi, int clk_hz, void **out_handle);
extern int   disp_spi_write(void *handle, const uint8_t *data, size_t len);

#define OLED_SPI_HZ  (10 * 1000 * 1000)   // 10 MHz

#define MALLOC_CAP_SPIRAM  (1 << 10)

#define SH1106_WIDTH   128
#define SH1106_HEIGHT   64
#define SH1106_PAGES     8

#define OLED_ROWS  8
#define OLED_COLS  21

#define PIN_CLK   12
#define PIN_MOSI  11
#define PIN_CS    10
#define PIN_DC    21
#define PIN_RST   47

static uint8_t          *s_framebuf;   // == s_fb->pixels (raw bytes for SH1106 flush)
static gfx_fb_t         *s_fb;          // kernel framebuffer (MONO_VLSB, SH1106 native)
static const gfx_font_t *s_font;        // kernel 5x7 font
static void             *s_spi;         // hardware SPI device handle

static void oled_gpio_init(void)
{
    // Only CS/DC/RST are GPIO — CLK/MOSI are driven by the hardware SPI
    // peripheral (claimed by disp_spi_add), so they must NOT be configured
    // here. gpio_config_t in ESP-IDF v5.x is 24 bytes; use 8 uint32_t.
    uint64_t pin_mask = (1ULL << PIN_CS) | (1ULL << PIN_DC) | (1ULL << PIN_RST);
    uint32_t cfg[8];
    memset(cfg, 0, sizeof(cfg));
    cfg[0] = pin_mask & 0xFFFFFFFF;       // pin_bit_mask low 32
    cfg[1] = pin_mask >> 32;               // pin_bit_mask high 32
    cfg[2] = 2;                            // mode = GPIO_MODE_OUTPUT
    gpio_config(cfg);

    gpio_set_level(PIN_CS,  1);
    gpio_set_level(PIN_DC,  0);
    gpio_set_level(PIN_RST, 1);
}

// Send a command byte (DC low) over hardware SPI, CS asserted for the byte.
static void oled_cmd(uint8_t cmd)
{
    gpio_set_level(PIN_DC, 0);
    gpio_set_level(PIN_CS, 0);
    disp_spi_write(s_spi, &cmd, 1);
    gpio_set_level(PIN_CS, 1);
}

// Send a data buffer (DC high) in one hardware-SPI (DMA) transfer.
static void oled_data_buf(const uint8_t *data, size_t len)
{
    gpio_set_level(PIN_DC, 1);
    gpio_set_level(PIN_CS, 0);
    disp_spi_write(s_spi, data, len);
    gpio_set_level(PIN_CS, 1);
}

static void oled_data(uint8_t data)
{
    oled_data_buf(&data, 1);
}

static void oled_reset(void)
{
    gpio_set_level(PIN_RST, 0);
    for (volatile int i = 0; i < 100000; i++);
    gpio_set_level(PIN_RST, 1);
    for (volatile int i = 0; i < 100000; i++);
}

static void oled_hw_init(void)
{
    oled_reset();

    oled_cmd(0xAE);
    oled_cmd(0xD5); oled_cmd(0x80);
    oled_cmd(0xA8); oled_cmd(0x3F);
    oled_cmd(0xD3); oled_cmd(0x00);
    oled_cmd(0x40);
    oled_cmd(0xAD); oled_cmd(0x8B);
    oled_cmd(0xA1);
    oled_cmd(0xC8);
    oled_cmd(0xDA); oled_cmd(0x12);
    oled_cmd(0x81); oled_cmd(0xCF);
    oled_cmd(0xD9); oled_cmd(0x1F);
    oled_cmd(0xDB); oled_cmd(0x40);
    oled_cmd(0x33);
    oled_cmd(0xA6);
    oled_cmd(0xAF);
}

static void oled_set_pos(uint8_t col, uint8_t page)
{
    col += 2;
    oled_cmd(0xB0 | page);
    oled_cmd(col & 0x0F);
    oled_cmd(0x10 | (col >> 4));
}

static void oled_clear_display(void)
{
    if (!s_framebuf) return;
    memset(s_framebuf, 0, SH1106_WIDTH * SH1106_PAGES);
    for (int page = 0; page < SH1106_PAGES; page++) {
        oled_set_pos(0, page);
        oled_data_buf(&s_framebuf[page * SH1106_WIDTH], SH1106_WIDTH);
    }
}

static void oled_flush(void)
{
    if (!s_framebuf) return;
    for (int page = 0; page < SH1106_PAGES; page++) {
        oled_set_pos(0, page);
        oled_data_buf(&s_framebuf[page * SH1106_WIDTH], SH1106_WIDTH);
    }
}

static void oled_render(const char *text, size_t len)
{
    if (!s_fb) return;

    // Render via the kernel font ABI. gfx_draw_text needs a NUL-terminated
    // string and clips to the framebuffer, so bound the copy to a screen-full.
    char buf[256];
    size_t n = (len < sizeof(buf) - 1) ? len : sizeof(buf) - 1;
    memcpy(buf, text, n);
    buf[n] = '\0';

    gfx_fill_rect(s_fb, 0, 0, SH1106_WIDTH, SH1106_HEIGHT, 0);
    gfx_draw_text(s_fb, 0, 0, buf, s_font, 1);
    oled_flush();
}

static void oled_clear(void)
{
    oled_clear_display();
}

static int oled_get_rows(void) { return OLED_ROWS; }
static int oled_get_cols(void) { return OLED_COLS; }
static uint32_t oled_get_caps(void) { return (1 << 1); }

// Present an arbitrary framebuffer (GUI path). Expects the SH1106-native
// MONO_VLSB format; copies the pixels into our flush buffer and pushes them.
static int oled_present(const gfx_fb_t *fb)
{
    if (!s_fb || !fb || !fb->pixels) return -1;
    if (fb->fmt != GFX_FMT_MONO_VLSB) return -1;

    size_t n = fb->bytes < s_fb->bytes ? fb->bytes : s_fb->bytes;
    memcpy(s_framebuf, fb->pixels, n);
    oled_flush();
    return 0;
}

static const display_driver_ops_t s_display_ops = {
    .init     = NULL,
    .shutdown = NULL,
    .render   = oled_render,
    .clear    = oled_clear,
    .get_rows = oled_get_rows,
    .get_cols = oled_get_cols,
    .get_caps = oled_get_caps,
    .present  = oled_present,
};

static int oled_init_driver(void)
{
    // SH1106 native layout is MONO_VLSB (page-based), which is exactly the
    // kernel gfx MONO_VLSB format — so s_fb->pixels is the SH1106 flush buffer.
    s_fb = gfx_fb_alloc(SH1106_WIDTH, SH1106_HEIGHT, GFX_FMT_MONO_VLSB, 0);
    if (!s_fb) return -1;
    s_framebuf = s_fb->pixels;
    s_font = gfx_font_get("5x7");

    // Claim the shared display SPI bus (SPI2 + DMA) and add our device.
    if (disp_spi_add(PIN_CLK, PIN_MOSI, OLED_SPI_HZ, &s_spi) != 0) {
        vconsole_printf("[oled] display SPI init failed\n");
        gfx_fb_free(s_fb);
        s_fb = NULL; s_framebuf = NULL;
        return -1;
    }

    oled_gpio_init();
    oled_hw_init();
    oled_clear_display();

    display_mux_register("oled", &s_display_ops);
    vconsole_printf("[oled] SH1106 128x64 ready (HW SPI @ %d MHz)\n",
                    OLED_SPI_HZ / 1000000);
    return 0;
}

static int oled_shutdown_driver(void)
{
    display_mux_unregister("oled");
    oled_cmd(0xAE);
    if (s_fb) {
        gfx_fb_free(s_fb);
        s_fb = NULL;
        s_framebuf = NULL;
    }
    vconsole_printf("[oled] Display driver stopped\n");
    return 0;
}

static module_info_t info = {
    .magic       = MODULE_MAGIC,
    .abi_version = MODULE_ABI_VERSION,
    .name        = "oled",
    .version     = "1.2.0",
    .requires    = "vconsole",
    .flags       = 0,
};

static int oled_start(void)
{
    return oled_init_driver();
}

static int oled_stop(void)
{
    return oled_shutdown_driver();
}

static module_exports_t exports = {
    .info  = &info,
    .start = oled_start,
    .stop  = oled_stop,
};

int main(int argc, char *argv[])
{
    module_register(&exports);
    return 0;
}
