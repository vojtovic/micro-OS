#ifndef INIT_H
#define INIT_H

#include "esp_err.h"
#include <stdbool.h>

#define FSTAB_MAX_ENTRIES 8
#define FSTAB_PATH        "/sys/etc/fstab"

typedef struct {
    char device[32];
    char mountpoint[32];
    char fstype[16];
    char options[32];
} fstab_entry_t;

typedef struct {
    fstab_entry_t entries[FSTAB_MAX_ENTRIES];
    int           count;
} fstab_t;

esp_err_t init_filesystem(void);
esp_err_t init_run_bootscript(void);
void      init_boot_beep(void);

esp_err_t init_parse_fstab(fstab_t *fstab);
esp_err_t init_create_default_fstab(void);
esp_err_t init_mount_fstab(void);

esp_err_t cmd_fstab_register(void);

#endif
