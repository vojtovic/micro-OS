#ifndef ELF_LOADER_H
#define ELF_LOADER_H

#include "esp_err.h"
#include "esp_elf.h"
#include <stddef.h>

typedef struct {
    esp_elf_t  elf;
    uint8_t   *file_buf;
    size_t     file_size;
    size_t     text_size;
    size_t     data_size;
} elf_load_result_t;

esp_err_t elf_loader_load(const char *path, elf_load_result_t *result);
void      elf_loader_unload(elf_load_result_t *result);
int       elf_loader_call_entry(elf_load_result_t *result, int argc, char *argv[]);

#endif
