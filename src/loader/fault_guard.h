#ifndef FAULT_GUARD_H
#define FAULT_GUARD_H

#include "esp_err.h"
#include <stdbool.h>

// Invoke a module callback under stack/heap guards. Set check_leak=true only
// for calls that are expected to be heap-neutral (e.g. stop/teardown), so a
// net PSRAM increase is flagged. Pass false for calls that legitimately
// allocate persistent state (e.g. start allocating framebuffers), where a
// heap increase is expected, not a leak.
esp_err_t fault_guard_call(int (*fn)(void), int *result, bool check_leak);
bool fault_guard_in_module(void);
bool fault_guard_last_faulted(void);

#endif
