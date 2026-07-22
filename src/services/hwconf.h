#ifndef HWCONF_H
#define HWCONF_H

#include "esp_err.h"

// hwconf — the system hardware config service.
//
// Parses /sdcard/etc/hardware.conf (INI-style: [section] headers + key=value
// lines, '#' comments) into an in-memory table. This is how loadable driver
// modules learn their pins, resolution and orientation at load time instead of
// hardcoding them — the Arch-style "say how the system behaves" config.
//
// The getters lazy-load on first use, so modules can query without caring about
// boot order. hwconf_load() forces a (re)parse, e.g. after editing the file.

#define HWCONF_PATH  "/sdcard/etc/hardware.conf"

// (Re)parse the config file into memory. Returns ESP_OK even when the file is
// absent — in that case every lookup returns its supplied default.
esp_err_t hwconf_load(void);

// Look up [section] key as an integer (strtol, base 0: decimal or 0x hex).
// Returns `def` if the section/key is missing or unparseable.
int hwconf_get_int(const char *section, const char *key, int def);

// Look up [section] key as a string, copied into buf (always NUL-terminated).
// Returns 1 if found, 0 if not — in which case buf holds `def` (or "" if def
// is NULL). Pass buf=NULL to just test for presence.
int hwconf_get_str(const char *section, const char *key,
                   char *buf, int len, const char *def);

// Registers the `hwconf` diagnostic shell command (dump / get / reload).
esp_err_t cmd_hwconf_register(void);

#endif // HWCONF_H
