#ifndef CMD_BENCH_H
#define CMD_BENCH_H

#include "esp_err.h"

// Registers the `bench` shell command (micro-benchmarks: cpu, mem, gfx).
esp_err_t cmd_bench_register(void);

#endif
