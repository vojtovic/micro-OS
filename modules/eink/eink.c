// Waveshare 3.52inch e-Paper HAT (360x240, mono) loadable driver.
//
// Ported from the proven Waveshare 3.52" driver used in the mp3-pedia/os
// project (controller: UC8xxx-family, PSR command set). This is NOT an
// SSD1681 (the 1.54" 200x200 part); the command set is entirely different:
// panel setting 0x00, power 0x01, booster 0x06, resolution 0x61, LUT
// download 0x20-0x24, data 0x13, refresh 0x17/0xA5. The panel needs its
// waveform LUT downloaded before every refresh — without it the panel
// accepts commands but never physically updates.
//
// Panel native geometry: 240 wide x 360 tall (portrait), 1bpp, 0xFF = white.

#include "module_types.h"
#include "display_types.h"
#include "gfx_abi.h"          // kernel graphics ABI (for the GUI present path)
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
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
// Hardware display SPI (SPI2 + DMA) — replaces bit-banging.
extern int   disp_spi_add(int clk, int mosi, int clk_hz, void **out_handle);
extern int   disp_spi_write(void *handle, const uint8_t *data, size_t len);
// Hardware config service — pins/orientation from hardware.conf.
extern int   hwconf_get_int(const char *section, const char *key, int def);

#define EINK_SPI_HZ  (10 * 1000 * 1000)   // 10 MHz
extern void  vTaskDelay(uint32_t ticks);   // CONFIG_FREERTOS_HZ=1000 → ticks == ms

#define MALLOC_CAP_SPIRAM  (1 << 10)
#define delay_ms(ms)       vTaskDelay(ms)

// Native panel memory geometry (fixed by hardware): 240 wide x 360 tall,
// portrait. The framebuffer and all panel transmits use these — rotation only
// changes how logical draw coordinates map onto this native buffer.
#define EPD_WIDTH    240
#define EPD_HEIGHT   360
#define EPD_BUF_SIZE (EPD_WIDTH * EPD_HEIGHT / 8)   // 10800 bytes

// Default pins — overridden at load time by hardware.conf.
#define DEF_PIN_CLK  12
#define DEF_PIN_MOSI 11
#define DEF_PIN_CS   16
#define DEF_PIN_DC   15
#define DEF_PIN_RST  17
#define DEF_PIN_BUSY 18

// Runtime config, filled from hardware.conf in eink_start().
static int s_pin_clk, s_pin_mosi, s_pin_cs, s_pin_dc, s_pin_rst, s_pin_busy;
static int s_rotation;        // 0/90/180/270 — logical->native pixel transform
static int s_lw, s_lh;        // logical draw dimensions (swap w/h at 90/270)

static uint8_t *s_framebuf;   // current frame (0xFF = white), native layout
static uint8_t *s_oldbuf;     // last frame pushed to the panel (change detection)
static void    *s_spi;        // hardware SPI device handle

#define EPD_STRIDE  (EPD_WIDTH / 8)   // 30 bytes per row

// EINK_PARTIAL: experimental UC8253 partial-window refresh (0x90/0x91/0x92).
// Refreshes only the changed row band instead of the whole panel — faster and
// localizes ghosting. The official Waveshare 3.52" driver does NOT ship this,
// so the panel may not support it: if the display shows garbage, set to 0 to
// fall back to full-frame DU/GC updates.
#define EINK_PARTIAL     1

// Refresh cadence: use fast DU refreshes, force a full GC refresh every
// GHOSTING_LIMIT updates to clear accumulated ghosting. Waveshare's 3.52"
// guidance is a full refresh after at most ~5 partials; we use 3 to keep
// ghosting low (lower = cleaner but more frequent full-flash).
#define GHOSTING_LIMIT   3
static int s_partial_count;

// ---------------------------------------------------------------------------
// Waveform LUTs — "GC" (full-quality) refresh. Trailing entries are zero;
// C zero-fills the remainder of each fixed-size array, matching the source.
// ---------------------------------------------------------------------------
// "GC" — full/quality refresh (clears ghosting, ~2s, black/white flash).
static const uint8_t lut_R20_GC[56] = { 0x01,0x0f,0x0f,0x0f,0x01,0x01,0x01 };
static const uint8_t lut_R21_GC[42] = { 0x01,0x4f,0x8f,0x0f,0x01,0x01,0x01 };
static const uint8_t lut_R22_GC[56] = { 0x01,0x0f,0x8f,0x0f,0x01,0x01,0x01 };
static const uint8_t lut_R23_GC[42] = { 0x01,0x4f,0x8f,0x4f,0x01,0x01,0x01 };
static const uint8_t lut_R24_GC[42] = { 0x01,0x0f,0x8f,0x4f,0x01,0x01,0x01 };

// "DU" — fast/partial refresh (~0.3s, no full flash, accumulates ghosting).
static const uint8_t lut_R20_DU[56] = { 0x01,0x0f,0x01,0x00,0x00,0x01,0x01 };
static const uint8_t lut_R21_DU[42] = { 0x01,0x0f,0x01,0x00,0x00,0x01,0x01 };
static const uint8_t lut_R22_DU[56] = { 0x01,0x8f,0x01,0x00,0x00,0x01,0x01 };
static const uint8_t lut_R23_DU[42] = { 0x01,0x4f,0x01,0x00,0x00,0x01,0x01 };
static const uint8_t lut_R24_DU[42] = { 0x01,0x0f,0x01,0x00,0x00,0x01,0x01 };

// ---------------------------------------------------------------------------
// Low-level SPI (hardware SPI2 + DMA) and control lines
// ---------------------------------------------------------------------------
// Build a 64-bit GPIO mask WITHOUT a runtime 64-bit shift: `1ULL << pin` with a
// runtime `pin` compiles to libgcc's __ashldi3, which is not in the kernel
// symbol table. Splitting into 32-bit shifts keeps it native.
static uint64_t pin_bit(int pin)
{
    return (pin < 32) ? (uint64_t)(1u << pin)
                      : ((uint64_t)(1u << (pin - 32)) << 32);
}

static void epd_gpio_init(void)
{
    // Only CS/DC/RST are GPIO outputs; CLK/MOSI are driven by the hardware SPI
    // peripheral (claimed by disp_spi_add). BUSY is an input.
    uint64_t out_mask = pin_bit(s_pin_cs) | pin_bit(s_pin_dc) | pin_bit(s_pin_rst);
    uint32_t out_cfg[8];
    memset(out_cfg, 0, sizeof(out_cfg));
    out_cfg[0] = out_mask & 0xFFFFFFFF;    // pin_bit_mask low 32
    out_cfg[1] = out_mask >> 32;           // pin_bit_mask high 32
    out_cfg[2] = 2;                        // mode = GPIO_MODE_OUTPUT
    gpio_config(out_cfg);

    uint64_t in_mask = pin_bit(s_pin_busy);
    uint32_t in_cfg[8];
    memset(in_cfg, 0, sizeof(in_cfg));
    in_cfg[0] = in_mask & 0xFFFFFFFF;
    in_cfg[1] = in_mask >> 32;
    in_cfg[2] = 1;                         // mode = GPIO_MODE_INPUT
    gpio_config(in_cfg);

    gpio_set_level(s_pin_cs, 1);
    gpio_set_level(s_pin_dc, 0);
    gpio_set_level(s_pin_rst, 1);
}

static void epd_cmd(uint8_t cmd)
{
    gpio_set_level(s_pin_dc, 0);
    gpio_set_level(s_pin_cs, 0);
    disp_spi_write(s_spi, &cmd, 1);
    gpio_set_level(s_pin_cs, 1);
}

// Send a data buffer (DC high) in one hardware-SPI (DMA) transfer.
static void epd_data_buf(const uint8_t *data, size_t len)
{
    gpio_set_level(s_pin_dc, 1);
    gpio_set_level(s_pin_cs, 0);
    disp_spi_write(s_spi, data, len);
    gpio_set_level(s_pin_cs, 1);
}

static void epd_data(uint8_t data)
{
    epd_data_buf(&data, 1);
}

// BUSY is HIGH while the panel is busy (matches the proven mono driver).
static void epd_wait_busy(void)
{
    int timeout_ms = 8000;
    while (gpio_get_level(s_pin_busy) == 1) {
        delay_ms(10);
        timeout_ms -= 10;
        if (timeout_ms <= 0) {
            vconsole_printf("[eink] BUSY timeout\n");
            break;
        }
    }
    delay_ms(200);
}

static void epd_reset(void)
{
    gpio_set_level(s_pin_rst, 1);
    delay_ms(200);
    gpio_set_level(s_pin_rst, 0);
    delay_ms(2);
    gpio_set_level(s_pin_rst, 1);
    delay_ms(200);
}

// ---------------------------------------------------------------------------
// Controller init + LUT + refresh (ported from mp3-pedia/os EPD_3in52)
// ---------------------------------------------------------------------------
static void epd_init(void)
{
    epd_reset();

    epd_cmd(0x00);              // panel setting (PSR)
    epd_data(0xFF);
    epd_data(0x01);

    epd_cmd(0x01);              // power setting
    epd_data(0x03);
    epd_data(0x10);
    epd_data(0x3F);
    epd_data(0x3F);
    epd_data(0x03);

    epd_cmd(0x06);              // booster soft start
    epd_data(0x37);
    epd_data(0x3D);
    epd_data(0x3D);

    epd_cmd(0x60);              // TCON
    epd_data(0x22);

    epd_cmd(0x82);             // VCOM_DC
    epd_data(0x07);

    epd_cmd(0x30);
    epd_data(0x09);

    epd_cmd(0xE3);             // power saving
    epd_data(0x88);

    epd_cmd(0x61);             // resolution: 240 x 360
    epd_data(0xF0);
    epd_data(0x01);
    epd_data(0x68);

    epd_cmd(0x50);
    epd_data(0xB7);
}

static void epd_lut_gc(void)
{
    epd_cmd(0x20);  epd_data_buf(lut_R20_GC, 56);
    epd_cmd(0x21);  epd_data_buf(lut_R21_GC, 42);
    epd_cmd(0x24);  epd_data_buf(lut_R24_GC, 42);
    epd_cmd(0x22);  epd_data_buf(lut_R22_GC, 56);
    epd_cmd(0x23);  epd_data_buf(lut_R23_GC, 42);
}

static void epd_lut_du(void)
{
    epd_cmd(0x20);  epd_data_buf(lut_R20_DU, 56);
    epd_cmd(0x21);  epd_data_buf(lut_R21_DU, 42);
    epd_cmd(0x24);  epd_data_buf(lut_R24_DU, 42);
    epd_cmd(0x22);  epd_data_buf(lut_R22_DU, 56);
    epd_cmd(0x23);  epd_data_buf(lut_R23_DU, 42);
}

static void epd_refresh(void)
{
    epd_cmd(0x17);
    epd_data(0xA5);
    epd_wait_busy();
    delay_ms(200);
}

// Push the framebuffer and refresh. full=true → GC (quality, clears ghosting);
// full=false → DU (fast/partial, ~0.3s, accumulates ghosting).
static void epd_update(const uint8_t *buf, bool full)
{
    epd_cmd(0x13);                          // transfer new data (one DMA burst)
    epd_data_buf(buf, EPD_BUF_SIZE);
    if (full) epd_lut_gc();
    else      epd_lut_du();
    epd_refresh();
}

#if EINK_PARTIAL
// Experimental: refresh only rows [y0,y1] (full width) via the UC8253
// partial-window commands + a fast DU refresh. See EINK_PARTIAL above.
static void epd_update_partial(const uint8_t *buf, int y0, int y1)
{
    if (y0 < 0) y0 = 0;
    if (y1 > EPD_HEIGHT - 1) y1 = EPD_HEIGHT - 1;
    if (y1 < y0) return;

    epd_cmd(0x91);                          // PTIN — partial in
    epd_cmd(0x90);                          // PTL — partial window
    epd_data(0x00);                         // HRST: x_start = 0
    epd_data((EPD_WIDTH - 1) | 0x07);       // HRED: x_end = 239 (byte-aligned)
    epd_data((y0 >> 8) & 0x01);             // VRST[8]
    epd_data(y0 & 0xFF);                    // VRST[7:0]
    epd_data((y1 >> 8) & 0x01);             // VRED[8]
    epd_data(y1 & 0xFF);                    // VRED[7:0]

    // new data — window rows only (contiguous, one DMA burst)
    epd_cmd(0x13);
    epd_data_buf(&buf[y0 * EPD_STRIDE], (size_t)(y1 - y0 + 1) * EPD_STRIDE);

    epd_lut_du();
    epd_refresh();
    epd_cmd(0x92);                          // PTOUT — partial out
}
#endif

// ---------------------------------------------------------------------------
// Framebuffer text rendering (5x7 font, black text on white)
// ---------------------------------------------------------------------------
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

// Set a pixel in LOGICAL coordinates (bounded by s_lw x s_lh). The rotation
// maps logical (x,y) onto the panel's native 240x360 memory, so all higher-
// level drawing (text) is orientation-agnostic.
static void epd_set_pixel(int x, int y, int color)
{
    if (x < 0 || x >= s_lw || y < 0 || y >= s_lh) return;

    int px, py;
    switch (s_rotation) {
        case 90:  px = EPD_WIDTH  - 1 - y; py = x;                 break;
        case 180: px = EPD_WIDTH  - 1 - x; py = EPD_HEIGHT - 1 - y; break;
        case 270: px = y;                  py = EPD_HEIGHT - 1 - x; break;
        default:  px = x;                  py = y;                 break;  // 0
    }
    if (px < 0 || px >= EPD_WIDTH || py < 0 || py >= EPD_HEIGHT) return;

    int byte_idx = (py * EPD_WIDTH + px) / 8;
    int bit_idx  = 7 - (px % 8);
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
            // font bit set -> black pixel (color 0); background stays white (1)
            epd_set_pixel(x0 + dx, y0 + dy, (bits >> dy) & 1 ? 0 : 1);
        }
    }
    for (int dy = 0; dy < 8; dy++) {
        epd_set_pixel(x0 + 5, y0 + dy, 1);   // inter-glyph column: white
    }
}

// ---------------------------------------------------------------------------
// display_mux driver ops
// ---------------------------------------------------------------------------
// Push s_framebuf to the panel, refreshing only the band of rows that changed
// since the last frame (skips the slow ~2 s full refresh when nothing changed).
static void eink_commit(void)
{
    // (Inline compare — memcmp is not in the kernel symbol table.)
    int y0 = EPD_HEIGHT, y1 = -1, diff = 0;
    for (int i = 0; i < EPD_BUF_SIZE; i++) {
        if (s_framebuf[i] != s_oldbuf[i]) {
            diff++;
            int row = i / EPD_STRIDE;
            if (row < y0) y0 = row;
            if (row > y1) y1 = row;
        }
    }
    if (y1 < 0) return;   // nothing changed

    // Force a full GC refresh (clean black/white flash, no ghosting) when either
    // the ghosting counter tripped OR a large fraction of the screen changed —
    // the latter catches whole-screen transitions (opening home, switching
    // pages) so they come up crisp, while small updates (a ticking clock) still
    // use the fast partial/DU path.
    bool full = (s_partial_count >= GHOSTING_LIMIT) ||
                (diff > EPD_BUF_SIZE * 2 / 5);
#if EINK_PARTIAL
    if (full) {
        epd_update(s_framebuf, true);
        s_partial_count = 0;
    } else {
        epd_update_partial(s_framebuf, y0, y1);
        s_partial_count++;
    }
#else
    epd_update(s_framebuf, full);
    s_partial_count = full ? 0 : (s_partial_count + 1);
#endif
    memcpy(s_oldbuf, s_framebuf, EPD_BUF_SIZE);
}

static void eink_render(const char *text, size_t len)
{
    if (!s_framebuf || !s_oldbuf) return;

    memset(s_framebuf, 0xFF, EPD_BUF_SIZE);

    int row = 0, col = 0;
    for (size_t i = 0; i < len && row < (s_lh / 8); i++) {
        if (text[i] == '\n') {
            row++;
            col = 0;
            continue;
        }
        if (col < (s_lw / 6)) {
            epd_draw_char(col, row, text[i]);
            col++;
        }
    }
    eink_commit();
}

// GUI path: blit a graphics framebuffer (MONO_HMSB, logical dimensions) onto
// the panel. gfx foreground pixels (1) become ink (black); the rotation and
// clipping are handled per-pixel by epd_set_pixel.
static int eink_present(const gfx_fb_t *fb)
{
    if (!s_framebuf || !s_oldbuf || !fb || !fb->pixels) return -1;
    if (fb->fmt != GFX_FMT_MONO_HMSB) return -1;

    memset(s_framebuf, 0xFF, EPD_BUF_SIZE);   // white paper

    for (int y = 0; y < fb->h; y++) {
        const uint8_t *prow = fb->pixels + (size_t)y * fb->stride;
        for (int x = 0; x < fb->w; x++) {
            int on = (prow[x >> 3] >> (7 - (x & 7))) & 1;
            epd_set_pixel(x, y, on ? 0 : 1);   // invert: gfx-1 -> black ink
        }
    }
    eink_commit();
    return 0;
}

static void eink_clear(void)
{
    if (!s_framebuf) return;
    memset(s_framebuf, 0xFF, EPD_BUF_SIZE);
    epd_update(s_framebuf, true);   // clear is always a full refresh
    s_partial_count = 0;
    if (s_oldbuf) memcpy(s_oldbuf, s_framebuf, EPD_BUF_SIZE);
}

static int eink_get_rows(void) { return (s_lh / 8); }
static int eink_get_cols(void) { return (s_lw / 6); }
static uint32_t eink_get_caps(void) { return DISPLAY_CAP_PARTIAL_REFRESH; }

static const display_driver_ops_t s_display_ops = {
    .init     = NULL,
    .shutdown = NULL,
    .render   = eink_render,
    .clear    = eink_clear,
    .get_rows = eink_get_rows,
    .get_cols = eink_get_cols,
    .get_caps = eink_get_caps,
    .present  = eink_present,
};

static int eink_start(void)
{
    // Read pins + orientation from hardware.conf (fall back to defaults).
    s_pin_clk  = hwconf_get_int("display_spi", "clk",  DEF_PIN_CLK);
    s_pin_mosi = hwconf_get_int("display_spi", "mosi", DEF_PIN_MOSI);
    s_pin_cs   = hwconf_get_int("eink", "cs",   DEF_PIN_CS);
    s_pin_dc   = hwconf_get_int("eink", "dc",   DEF_PIN_DC);
    s_pin_rst  = hwconf_get_int("eink", "rst",  DEF_PIN_RST);
    s_pin_busy = hwconf_get_int("eink", "busy", DEF_PIN_BUSY);
    s_rotation = hwconf_get_int("eink", "rotation", 0);
    if (s_rotation != 90 && s_rotation != 180 && s_rotation != 270)
        s_rotation = 0;
    // Logical draw dimensions: 90/270 present the panel in landscape.
    if (s_rotation == 90 || s_rotation == 270) {
        s_lw = EPD_HEIGHT;   // 360
        s_lh = EPD_WIDTH;    // 240
    } else {
        s_lw = EPD_WIDTH;    // 240
        s_lh = EPD_HEIGHT;   // 360
    }

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
    // Force the first render to differ from s_oldbuf so it always draws.
    memset(s_oldbuf, 0x00, EPD_BUF_SIZE);

    // Claim the shared display SPI bus (SPI2 + DMA) and add our device.
    if (disp_spi_add(s_pin_clk, s_pin_mosi, EINK_SPI_HZ, &s_spi) != 0) {
        vconsole_printf("[eink] display SPI init failed\n");
        heap_caps_free(s_framebuf); heap_caps_free(s_oldbuf);
        s_framebuf = NULL; s_oldbuf = NULL;
        return -1;
    }

    epd_gpio_init();
    epd_init();
    eink_clear();

    display_mux_register("eink", &s_display_ops);
    vconsole_printf("[eink] Waveshare 3.52\" e-paper ready — rot=%d, %dx%d px, %dx%d chars\n",
                    s_rotation, s_lw, s_lh, (s_lw / 6), (s_lh / 8));
    return 0;
}

static int eink_stop(void)
{
    display_mux_unregister("eink");

    epd_cmd(0x07);          // deep sleep
    epd_data(0xA5);

    if (s_framebuf) { heap_caps_free(s_framebuf); s_framebuf = NULL; }
    if (s_oldbuf)   { heap_caps_free(s_oldbuf);   s_oldbuf = NULL; }

    vconsole_printf("[eink] Display driver stopped\n");
    return 0;
}

static module_info_t info = {
    .magic       = MODULE_MAGIC,
    .abi_version = MODULE_ABI_VERSION,
    .name        = "eink",
    .version     = "2.5.0",
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
