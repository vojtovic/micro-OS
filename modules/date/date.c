// date — CLI front-end for the RTC service.
//
// Reads/sets the clock through the "rtc" service registered by the RTC driver,
// demonstrating one app using another module's service via the registry.
//
//   date                          print date/time (+ temperature)
//   date set YYYY MM DD HH MM SS   set the clock
//
// Requires the RTC driver to be running (modstart rtc); otherwise it reports
// that the service is unavailable.

#include "rtc_service.h"
#include <stdint.h>

extern int   vconsole_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
extern void *registry_vtable(const char *name);   // NULL if absent/stopped

static int parse_uint(const char *s, int *out)
{
    int v = 0, any = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; any = 1; }
    if (!any || *s) return -1;
    *out = v;
    return 0;
}

int main(int argc, char *argv[])
{
    const rtc_ops_t *rtc = (const rtc_ops_t *)registry_vtable(RTC_SERVICE_NAME);
    if (!rtc) {
        vconsole_printf("date: no RTC service — run 'modstart rtc' first\n");
        return 1;
    }

    if (argc >= 2 && argv[1][0] == 's') {          // "set"
        if (argc < 8) {
            vconsole_printf("Usage: date set YYYY MM DD HH MM SS\n");
            return 1;
        }
        int y, mo, d, h, mi, s;
        if (parse_uint(argv[2], &y)  || parse_uint(argv[3], &mo) ||
            parse_uint(argv[4], &d)  || parse_uint(argv[5], &h)  ||
            parse_uint(argv[6], &mi) || parse_uint(argv[7], &s)) {
            vconsole_printf("date: bad number\n");
            return 1;
        }
        if (y < 2000 || y > 2099 || mo < 1 || mo > 12 || d < 1 || d > 31 ||
            h > 23 || mi > 59 || s > 59) {
            vconsole_printf("date: value out of range\n");
            return 1;
        }
        rtc_time_t t = { (uint16_t)y, (uint8_t)mo, (uint8_t)d,
                         (uint8_t)h, (uint8_t)mi, (uint8_t)s, 0 };
        if (rtc->set(&t) != 0) {
            vconsole_printf("date: RTC write failed\n");
            return 1;
        }
        vconsole_printf("date: set to %04d-%02d-%02d %02d:%02d:%02d\n",
                        y, mo, d, h, mi, s);
        return 0;
    }

    rtc_time_t t;
    if (rtc->get(&t) != 0) {
        vconsole_printf("date: RTC read failed\n");
        return 1;
    }
    static const char *wd[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    vconsole_printf("%s %04d-%02d-%02d %02d:%02d:%02d",
                    wd[t.wday & 7], t.year, t.month, t.day, t.hour, t.min, t.sec);

    int c100;
    if (rtc->temp_c100 && rtc->temp_c100(&c100) == 0) {
        int w = c100 / 100, f = c100 % 100;
        if (f < 0) f = -f;
        vconsole_printf("   %d.%02d C", w, f);
    }
    vconsole_printf("\n");
    return 0;
}
