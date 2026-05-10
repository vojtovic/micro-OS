#ifndef PKG_MANAGER_H
#define PKG_MANAGER_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#define PKG_MAGIC       0x4D504B47
#define PKG_VERSION     1
#define PKG_NAME_LEN    32
#define PKG_VER_LEN     16
#define PKG_DESC_LEN    64
#define PKG_HASH_LEN    32
#define PKG_MAX_ENTRIES 64

typedef struct {
    char name[PKG_NAME_LEN];
    char version[PKG_VER_LEN];
    uint32_t size;
    char hash[65];
    char filename[64];
    char description[PKG_DESC_LEN];
} pkg_entry_t;

typedef struct {
    pkg_entry_t entries[PKG_MAX_ENTRIES];
    int count;
    char repo_url[128];
} pkg_repo_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t version;
    uint32_t elf_size;
    uint32_t ini_size;
    uint8_t  elf_hash[PKG_HASH_LEN];
} pkg_header_t;

esp_err_t pkg_manager_init(void);
esp_err_t pkg_sync(void);
esp_err_t pkg_install(const char *name);
esp_err_t pkg_remove(const char *name);
esp_err_t pkg_list_available(void);
esp_err_t pkg_list_installed(void);
esp_err_t pkg_set_repo(const char *url);
const pkg_repo_t *pkg_get_repo(void);

esp_err_t cmd_pkg_register(void);

#endif
