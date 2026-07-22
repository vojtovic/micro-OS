#ifndef RTC_SERVICE_H
#define RTC_SERVICE_H

// Module-safe RTC service contract. Deliberately free of ESP-IDF headers so
// both the RTC driver module and consumer apps can include it. The driver
// registers an rtc_ops_t vtable in the kernel registry under RTC_SERVICE_NAME;
// consumers fetch it with registry_vtable(RTC_SERVICE_NAME) and call through it.

#include <stdint.h>

#define RTC_SERVICE_NAME  "rtc"

typedef struct {
    uint16_t year;                 // full year, e.g. 2026
    uint8_t  month, day;           // 1-12, 1-31
    uint8_t  hour, min, sec;       // 24-hour
    uint8_t  wday;                 // 0 = Sunday
} rtc_time_t;

typedef struct {
    int (*get)(rtc_time_t *out);        // 0 == ok
    int (*set)(const rtc_time_t *in);   // 0 == ok
    int (*temp_c100)(int *out_c100);    // temperature x100 (e.g. 2775 = 27.75C)
} rtc_ops_t;

#endif // RTC_SERVICE_H
