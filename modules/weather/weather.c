// weather — live weather via the Open-Meteo API (HTTPS, no key).
//
// Proves the networking foundation: http_fetch (HTTPS + CA bundle) + the json
// service. Fetches the current temperature/condition/wind for a location and
// shows it on both displays. Needs Wi-Fi connected first: `wifi connect <ssid>
// <pass>` in the console. R = refresh, ESC = quit.
//
// Location is hard-coded (Prague) for now — a config-driven location is a natural
// follow-up.

#include "gfx_abi.h"
#include <stdint.h>
#include <stddef.h>

extern int   vconsole_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
extern int   input_get_key(uint32_t timeout_ms);
extern int   snprintf(char *str, size_t size, const char *fmt, ...);
extern char *http_fetch(const char *url, int *out_len);
extern void  http_free(char *body);
extern int   app_mkdir(const char *path);
extern int   app_write_file(const char *path, const char *buf, int len);

// Cache the last reading so the home widget can show it without doing its own
// (blocking) network fetch.  Format: "temp_tenths;code;wind_tenths;location"
#define CACHE_PATH "/sys/cache/weather.txt"
extern void *json_parse(const char *text);
extern void  json_free(void *root);
extern void *json_get(void *node, const char *key);
extern int   json_get_int(void *node, const char *key, int scale, int *out);

typedef struct duo duo_t;
extern duo_t *duo_open(void);
extern void   duo_close(duo_t *d);
extern void   duo_message(duo_t *d, const char *text);
extern void  *duo_eink_fb(duo_t *d);
extern int    duo_eink_w(duo_t *d);
extern int    duo_eink_h(duo_t *d);
extern void   duo_eink_commit(duo_t *d);

#define LOCATION "Prague, CZ"
#define URL "https://api.open-meteo.com/v1/forecast?latitude=50.08&longitude=14.44" \
            "&current=temperature_2m,weather_code,wind_speed_10m"

static duo_t            *d;
static gfx_fb_t         *efb;
static const gfx_font_t *font;
static int  ew, eh;

static const char *condition(int code)
{
    if (code == 0)  return "Clear sky";
    if (code <= 3)  return "Partly cloudy";
    if (code <= 48) return "Fog";
    if (code <= 57) return "Drizzle";
    if (code <= 67) return "Rain";
    if (code <= 77) return "Snow";
    if (code <= 82) return "Showers";
    if (code <= 86) return "Snow showers";
    if (code <= 99) return "Thunderstorm";
    return "Unknown";
}

// Format tenths as "12.3<suffix>" (integer-only — modules avoid double math,
// which would pull in libgcc soft-float helpers not in the symbol table).
static void fmt_tenths(char *out, int len, int t10, const char *suffix)
{
    int i = t10 / 10, fdig = t10 % 10; if (fdig < 0) fdig = -fdig;
    snprintf(out, len, "%d.%d%s", i, fdig, suffix ? suffix : "");
}

static void draw(int have, int temp10, int code, int wind10)
{
    if (!efb) return;
    gfx_fill_rect(efb, 0, 0, ew, eh, 0);
    gfx_draw_rect(efb, 0, 0, ew, eh, 1, 1);
    gfx_fill_rect(efb, 2, 2, ew - 4, 16, 1);
    gfx_draw_text_scaled(efb, 6, 3, "WEATHER", font, 0, 1);
    gfx_draw_text(efb, 6, 24, LOCATION, font, 1);

    // sun icon
    int sx = 34, sy = 66;
    gfx_draw_circle(efb, sx, sy, 12, 0, 1);
    for (int a = 0; a < 8; a++) {
        static const int dx[] = { 1, 1, 0, -1, -1, -1, 0, 1 };
        static const int dy[] = { 0, 1, 1, 1, 0, -1, -1, -1 };
        gfx_draw_line(efb, sx + dx[a] * 16, sy + dy[a] * 16,
                      sx + dx[a] * 22, sy + dy[a] * 22, 1);
    }

    char s[40];
    if (have) {
        fmt_tenths(s, sizeof(s), temp10, " C");
        int tw = 0; for (const char *p = s; *p; p++) tw += 6 * 4;
        gfx_draw_text_scaled(efb, ew - tw - 8, 44, s, font, 1, 4);
        gfx_draw_text(efb, 6, 96, condition(code), font, 1);
        fmt_tenths(s, sizeof(s), wind10, " km/h wind");
        gfx_draw_text(efb, 6, 110, s, font, 1);
    } else {
        gfx_draw_text_scaled(efb, 8, 60, "No data", font, 1, 2);
        gfx_draw_text(efb, 8, 96, "Wi-Fi connected?", font, 1);
        gfx_draw_text(efb, 8, 108, "wifi connect <ssid> <pass>", font, 1);
    }
    gfx_draw_text(efb, 6, eh - 11, "R refresh   ESC quit", font, 1);
    duo_eink_commit(d);

    if (have) { fmt_tenths(s, sizeof(s), temp10, " C"); duo_message(d, s); }
    else        duo_message(d, "no data");
}

static void fetch(void)
{
    duo_message(d, "Loading...");
    int len = 0;
    char *body = http_fetch(URL, &len);
    if (!body) { draw(0, 0, 0, 0); return; }

    void *root = json_parse(body);
    http_free(body);
    void *cur = root ? json_get(root, "current") : NULL;
    int temp10 = 0, code = 0, wind10 = 0;
    int ok = cur && json_get_int(cur, "temperature_2m", 10, &temp10);
    json_get_int(cur, "weather_code",   1,  &code);
    json_get_int(cur, "wind_speed_10m", 10, &wind10);
    json_free(root);

    if (ok) {                                          // cache for the home widget
        app_mkdir("/sys/cache");
        char c[64];
        int cn = snprintf(c, sizeof(c), "%d;%d;%d;%s\n", temp10, code, wind10, LOCATION);
        app_write_file(CACHE_PATH, c, cn);
    }
    draw(ok, temp10, code, wind10);
}

int main(int argc, char *argv[])
{
    d = duo_open();
    if (!d) { vconsole_printf("weather: display init failed\n"); return 1; }
    efb = (gfx_fb_t *)duo_eink_fb(d);
    ew = duo_eink_w(d); eh = duo_eink_h(d);
    font = gfx_font_get("5x7");

    vconsole_printf("weather: fetching %s ...\n", LOCATION);
    fetch();

    for (;;) {
        int k = input_get_key(1000000);
        if (k < 0) continue;
        if (k == 0x1B) break;
        if (k == 'r' || k == 'R') fetch();
    }

    duo_close(d);
    vconsole_printf("weather: bye\n");
    return 0;
}
