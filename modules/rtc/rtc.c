// rtc — DS3231 real-time clock driver.
//
// A resident driver (not an app): on start it probes the DS3231 at I2C 0x68
// and registers an rtc_ops_t vtable in the kernel registry under "rtc". Other
// apps fetch it with registry_vtable("rtc") and read/set the time through it —
// e.g. the `date` CLI app, and later the desktop status bar.
//
//   modload /sdcard/drivers/rtc.elf
//   modstart rtc
//
// DS3231 sits on the same I2C bus the kernel brings up for the CardKB
// (SDA=8, SCL=9). Ported from mp3-pedia/os (RTClib RTC_DS3231, same wiring).

#include "module_types.h"
#include "rtc_service.h"
#include <stdint.h>
#include <stddef.h>

extern void module_register(module_exports_t *exports);
extern int  vconsole_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
extern int  bus_i2c_read(uint8_t addr, uint8_t *data, size_t len);   // 0 == ESP_OK
extern int  bus_i2c_write(uint8_t addr, const uint8_t *data, size_t len);
extern int  registry_provide(const char *name, uint16_t version, void *vtable, void *ctx);
extern int  registry_set_state(const char *name, int state);         // 0 = STOPPED

#define DS3231_ADDR  0x68

static int     bcd2dec(uint8_t b) { return (b >> 4) * 10 + (b & 0x0F); }
static uint8_t dec2bcd(int d)      { return (uint8_t)(((d / 10) << 4) | (d % 10)); }

static int reg_read(uint8_t reg, uint8_t *buf, size_t n)
{
    if (bus_i2c_write(DS3231_ADDR, &reg, 1) != 0) return -1;
    if (bus_i2c_read(DS3231_ADDR, buf, n) != 0)   return -1;
    return 0;
}

// Day of week (0=Sunday), Sakamoto — constant divisors, no libgcc __divsi3.
static int weekday(int y, int m, int d)
{
    static const int t[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (m < 3) y -= 1;
    return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

// ---- rtc_ops_t implementation -------------------------------------------
static int rtc_get(rtc_time_t *out)
{
    uint8_t t[7];
    if (!out || reg_read(0x00, t, 7) != 0) return -1;
    out->sec   = (uint8_t)bcd2dec(t[0] & 0x7F);
    out->min   = (uint8_t)bcd2dec(t[1] & 0x7F);
    out->hour  = (uint8_t)bcd2dec(t[2] & 0x3F);   // 24-hour
    out->wday  = (uint8_t)(t[3] ? bcd2dec(t[3]) - 1 : 0);
    out->day   = (uint8_t)bcd2dec(t[4] & 0x3F);
    out->month = (uint8_t)bcd2dec(t[5] & 0x1F);
    out->year  = (uint16_t)(2000 + bcd2dec(t[6]));
    return 0;
}

static int rtc_set(const rtc_time_t *in)
{
    if (!in) return -1;
    int dow = weekday(in->year, in->month, in->day);
    uint8_t buf[8] = {
        0x00,
        dec2bcd(in->sec), dec2bcd(in->min), dec2bcd(in->hour),  // 24-hour
        (uint8_t)(dow + 1),
        dec2bcd(in->day), dec2bcd(in->month), dec2bcd(in->year - 2000),
    };
    return (bus_i2c_write(DS3231_ADDR, buf, sizeof(buf)) == 0) ? 0 : -1;
}

static int rtc_temp_c100(int *out_c100)
{
    uint8_t tp[2];
    if (!out_c100 || reg_read(0x11, tp, 2) != 0) return -1;
    int whole = (int8_t)tp[0];
    int frac  = (tp[1] >> 6) * 25;                 // .00/.25/.50/.75
    *out_c100 = whole * 100 + (whole < 0 ? -frac : frac);
    return 0;
}

static const rtc_ops_t s_ops = {
    .get       = rtc_get,
    .set       = rtc_set,
    .temp_c100 = rtc_temp_c100,
};

// ---- driver lifecycle ----------------------------------------------------
static int rtc_start(void)
{
    rtc_time_t t;
    if (rtc_get(&t) != 0) {
        vconsole_printf("[rtc] DS3231 not detected on I2C 0x68\n");
        return -1;
    }
    registry_provide(RTC_SERVICE_NAME, 1, (void *)&s_ops, NULL);
    vconsole_printf("[rtc] DS3231 ready — %04d-%02d-%02d %02d:%02d:%02d (service '%s')\n",
                    t.year, t.month, t.day, t.hour, t.min, t.sec, RTC_SERVICE_NAME);
    return 0;
}

static int rtc_stop(void)
{
    registry_set_state(RTC_SERVICE_NAME, 0);       // SERVICE_STOPPED
    vconsole_printf("[rtc] service stopped\n");
    return 0;
}

static module_info_t info = {
    .magic       = MODULE_MAGIC,
    .abi_version = MODULE_ABI_VERSION,
    .name        = "rtc",
    .version     = "1.0.0",
    .requires    = "",
    .flags       = 0,
};

static module_exports_t exports = {
    .info  = &info,
    .start = rtc_start,
    .stop  = rtc_stop,
};

int main(int argc, char *argv[])
{
    module_register(&exports);
    return 0;
}
