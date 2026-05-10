#ifndef SYMTAB_H
#define SYMTAB_H

#include "esp_err.h"
#include <stdbool.h>

esp_err_t symtab_init(void);
int       symtab_count(void);

// Phase 5.6: returns true if any exported kernel symbol's name starts with `prefix`.
// Used by manifest_check_compat to validate `[requires] kernel_deps` entries.
bool      symtab_has_prefix(const char *prefix);

#endif
