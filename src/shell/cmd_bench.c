// bench — micro-benchmarks for capacity planning (Phase 5.0).
//
// The headline measurement is `bench mem`: memcpy bandwidth from internal
// DRAM vs PSRAM. The PSRAM/internal ratio is the number that decides whether
// Phase 5.2b (forking the loader to place hot module code in internal SRAM)
// is worth the effort — if the gap on real workloads is small, the cache
// config already recovers it and 5.2b can be dropped. See
// docs/2026-05-10-high-perf-modules-plan.md (Post-implementation review).

#include "cmd_bench.h"
#include "services/vconsole.h"
#include "loader/kernel_abi.h"
#include "esp_console.h"
#include "esp_timer.h"
#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// bench mem — memcpy bandwidth per memory region
// ---------------------------------------------------------------------------
static void bench_mem_region(const char *label, uint32_t caps)
{
    const size_t BUF = 16 * 1024;
    const int    ITERS = 256;   // 4 MB moved per direction

    uint8_t *src = heap_caps_malloc(BUF, caps);
    uint8_t *dst = heap_caps_malloc(BUF, caps);
    if (!src || !dst) {
        vconsole_printf("  %-10s (unavailable — alloc failed)\n", label);
        if (src) heap_caps_free(src);
        if (dst) heap_caps_free(dst);
        return;
    }
    memset(src, 0xA5, BUF);
    memset(dst, 0x00, BUF);

    int64_t t0 = esp_timer_get_time();
    for (int i = 0; i < ITERS; i++) memcpy(dst, src, BUF);
    int64_t us = esp_timer_get_time() - t0;

    heap_caps_free(src);
    heap_caps_free(dst);

    if (us <= 0) us = 1;
    // MB/s = bytes / microseconds  (bytes/us == MB/s)
    double mbs = (double)((uint64_t)BUF * ITERS) / (double)us;
    vconsole_printf("  %-10s %8.1f MB/s   (%d KB x %d in %lld us)\n",
                    label, mbs, (int)(BUF / 1024), ITERS, (long long)us);
}

static void bench_mem(void)
{
    vconsole_printf("bench mem — memcpy bandwidth\n");
    bench_mem_region("internal", MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    bench_mem_region("psram",    MALLOC_CAP_SPIRAM   | MALLOC_CAP_8BIT);
    vconsole_printf("  (internal/psram ratio drives the Phase 5.2b decision)\n");
}

// ---------------------------------------------------------------------------
// bench cpu — integer + float throughput
// ---------------------------------------------------------------------------
static void bench_cpu(void)
{
    const int N = 4 * 1000 * 1000;
    volatile uint32_t sink_u = 0;
    volatile float    sink_f = 0.0f;

    // Integer LCG + xorshift mix
    uint32_t acc = 0x12345678u;
    uint32_t c0 = esp_cpu_get_cycle_count();
    int64_t  t0 = esp_timer_get_time();
    for (int i = 0; i < N; i++) {
        acc = acc * 1664525u + 1013904223u;
        acc ^= acc >> 15;
    }
    int64_t  it_us = esp_timer_get_time() - t0;
    uint32_t it_cyc = esp_cpu_get_cycle_count() - c0;
    sink_u = acc;

    // Float multiply-accumulate
    float f = 1.0001f, g = 0.0f;
    c0 = esp_cpu_get_cycle_count();
    t0 = esp_timer_get_time();
    for (int i = 0; i < N; i++) {
        g = g * 0.999999f + f;
        f = f * 1.0000001f + 0.5f;
    }
    int64_t  fl_us = esp_timer_get_time() - t0;
    uint32_t fl_cyc = esp_cpu_get_cycle_count() - c0;
    sink_f = g;

    (void)sink_u; (void)sink_f;
    if (it_us <= 0) it_us = 1;
    if (fl_us <= 0) fl_us = 1;
    vconsole_printf("bench cpu — %d Mops each\n", N / 1000000);
    vconsole_printf("  integer   %6.1f Mops/s  (%.2f cyc/op)\n",
                    (double)N / (double)it_us, (double)it_cyc / (double)N);
    vconsole_printf("  float     %6.1f Mops/s  (%.2f cyc/op)\n",
                    (double)N / (double)fl_us, (double)fl_cyc / (double)N);
}

// ---------------------------------------------------------------------------
// bench gfx — kernel graphics primitive throughput (Phase 5.3a)
// ---------------------------------------------------------------------------
static void bench_gfx(void)
{
    const int W = 240, H = 320, ITERS = 200;

    gfx_fb_t *fb = gfx_fb_alloc(W, H, GFX_FMT_RGB565, 0);
    if (!fb) {
        vconsole_printf("bench gfx — framebuffer alloc failed (%dx%d)\n", W, H);
        return;
    }

    int64_t t0 = esp_timer_get_time();
    for (int i = 0; i < ITERS; i++)
        gfx_fill_rect(fb, 0, 0, W, H, (uint32_t)i);
    int64_t us = esp_timer_get_time() - t0;

    if (us <= 0) us = 1;
    double frames_s = (double)ITERS * 1e6 / (double)us;
    double mpix_s   = (double)((uint64_t)W * H * ITERS) / (double)us;
    vconsole_printf("bench gfx — gfx_fill_rect %dx%d RGB565\n", W, H);
    vconsole_printf("  %6.1f fills/s   %6.1f Mpixel/s   (%.1f us/fill)\n",
                    frames_s, mpix_s, (double)us / ITERS);

    // Text throughput (Phase 5.3b gfx_draw_text)
    const gfx_font_t *font = gfx_font_get("5x7");
    const char *line = "The quick brown fox 0123456789";  // 30 chars
    const int TITERS = 500;
    t0 = esp_timer_get_time();
    for (int i = 0; i < TITERS; i++)
        gfx_draw_text(fb, 0, (i % 40) * 8, line, font, 0xFFFF);
    int64_t tus = esp_timer_get_time() - t0;
    gfx_fb_free(fb);

    if (tus <= 0) tus = 1;
    int glyphs = 30 * TITERS;
    vconsole_printf("  gfx_draw_text: %6.1f Kglyph/s   (%.2f us/glyph)\n",
                    (double)glyphs / (double)tus * 1000.0, (double)tus / glyphs);
}

// gfxtest — render text through the gfx font ABI into a MONO framebuffer and
// dump it as ASCII. Validates gfx_fb_alloc + gfx_draw_text + gfx_get_pixel end
// to end in software, without needing a physical display.
static int cmd_gfxtest(int argc, char **argv)
{
    const char *text = (argc >= 2) ? argv[1] : "Hello 5x7!";
    int len = (int)strlen(text);
    if (len > 18) len = 18;            // keep the ASCII dump terminal-friendly
    if (len < 1) len = 1;

    const int w = len * 6, h = 8;
    gfx_fb_t *fb = gfx_fb_alloc((uint16_t)w, (uint16_t)h, GFX_FMT_MONO_VLSB, 0);
    if (!fb) {
        vconsole_printf("gfxtest: framebuffer alloc failed\n");
        return 0;
    }

    gfx_fill_rect(fb, 0, 0, w, h, 0);
    const gfx_font_t *f = gfx_font_get("5x7");

    char buf[19];
    memcpy(buf, text, len);
    buf[len] = '\0';
    gfx_draw_text(fb, 0, 0, buf, f, 1);

    vconsole_printf("gfxtest: \"%s\" (%dx%d)\n", buf, w, h);
    for (int y = 0; y < h; y++) {
        char line[128];
        int n = 0;
        for (int x = 0; x < w && n < 126; x++)
            line[n++] = gfx_get_pixel(fb, x, y) ? '#' : '.';
        line[n] = '\0';
        vconsole_printf("  %s\n", line);
    }
    gfx_fb_free(fb);
    return 0;
}

static int cmd_bench(int argc, char **argv)
{
    if (argc < 2) {
        vconsole_printf("Usage: bench <cpu|mem|gfx|all>\n");
        return 0;
    }

    if (strcmp(argv[1], "cpu") == 0)       bench_cpu();
    else if (strcmp(argv[1], "mem") == 0)  bench_mem();
    else if (strcmp(argv[1], "gfx") == 0)  bench_gfx();
    else if (strcmp(argv[1], "all") == 0) { bench_cpu(); bench_mem(); bench_gfx(); }
    else vconsole_printf("Unknown bench '%s' (cpu|mem|gfx|all)\n", argv[1]);

    return 0;
}

esp_err_t cmd_bench_register(void)
{
    const esp_console_cmd_t bench_cmd = {
        .command = "bench",
        .help = "Micro-benchmarks: bench <cpu|mem|gfx|all>",
        .func = cmd_bench,
    };
    esp_err_t ret = esp_console_cmd_register(&bench_cmd);
    if (ret != ESP_OK) return ret;

    const esp_console_cmd_t gfxtest_cmd = {
        .command = "gfxtest",
        .help = "Render text via gfx font ABI and dump as ASCII: gfxtest [text]",
        .func = cmd_gfxtest,
    };
    return esp_console_cmd_register(&gfxtest_cmd);
}
