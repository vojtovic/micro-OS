#ifndef FAULT_GUARD_H
#define FAULT_GUARD_H

#include "esp_err.h"
#include <stdbool.h>

esp_err_t fault_guard_call(int (*fn)(void), int *result);
bool fault_guard_in_module(void);
bool fault_guard_last_faulted(void);

#endif
