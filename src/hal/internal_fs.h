#ifndef INTERNAL_FS_H
#define INTERNAL_FS_H

#include "esp_err.h"
#include "hal/hal.h"
#include <stdbool.h>

esp_err_t internal_fs_mount(void);
esp_err_t internal_fs_unmount(void);
bool      internal_fs_is_mounted(void);
void      internal_fs_print_info(void);

const hal_storage_ops_t *internal_fs_get_ops(void);

#endif
