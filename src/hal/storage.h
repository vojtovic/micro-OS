#ifndef STORAGE_H
#define STORAGE_H

#include "esp_err.h"
#include "hal/hal.h"
#include <stdbool.h>

esp_err_t storage_sd_mount(void);
esp_err_t storage_sd_unmount(void);
bool      storage_sd_is_mounted(void);
void      storage_sd_print_info(void);

const hal_storage_ops_t *storage_sd_get_ops(void);

#endif
