#ifndef ELF_LOADER_H
#define ELF_LOADER_H

#include "esp_err.h"
#include "esp_elf.h"
#include <stddef.h>
#include <stdbool.h>

typedef struct {
    esp_elf_t  elf;
    uint8_t   *file_buf;
    size_t     file_size;
    size_t     text_size;
    size_t     data_size;

    // Phase 5.2: IRAM/DRAM section accounting.
    // Modules use IRAM_ATTR / DRAM_ATTR to mark hot code/data; the linker
    // emits .iram*.text and .dram_data sections. We sum their sizes here so
    // diagnostics can show what *should* be in internal SRAM.
    //
    // iram_in_psram: true when the upstream esp_elf_loader has placed the
    // IRAM-marked code in PSRAM rather than internal SRAM (current behavior;
    // see Phase 5.2b for real placement). Used by the warning on load.
    size_t     iram_text_size;
    size_t     dram_data_size;
    bool       iram_in_psram;
} elf_load_result_t;

esp_err_t elf_loader_load(const char *path, elf_load_result_t *result);
void      elf_loader_unload(elf_load_result_t *result);
int       elf_loader_call_entry(elf_load_result_t *result, int argc, char *argv[]);

#endif
