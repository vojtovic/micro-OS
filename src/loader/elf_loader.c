#include "elf_loader.h"
#include "symtab.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>

static const char *TAG = "elf_loader";

esp_err_t elf_loader_load(const char *path, elf_load_result_t *result)
{
    memset(result, 0, sizeof(*result));

    struct stat st;
    if (stat(path, &st) != 0) {
        ESP_LOGE(TAG, "File not found: %s", path);
        return ESP_ERR_NOT_FOUND;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open: %s", path);
        return ESP_FAIL;
    }

    result->file_size = st.st_size;
    result->file_buf = heap_caps_malloc(st.st_size, MALLOC_CAP_SPIRAM);
    if (!result->file_buf) {
        fclose(f);
        ESP_LOGE(TAG, "Cannot allocate %ld bytes for ELF", (long)st.st_size);
        return ESP_ERR_NO_MEM;
    }

    size_t nread = fread(result->file_buf, 1, st.st_size, f);
    fclose(f);
    if (nread != (size_t)st.st_size) {
        heap_caps_free(result->file_buf);
        result->file_buf = NULL;
        ESP_LOGE(TAG, "Short read: %zu/%ld", nread, (long)st.st_size);
        return ESP_FAIL;
    }

    if (result->file_buf[0] != 0x7F || result->file_buf[1] != 'E' ||
        result->file_buf[2] != 'L'  || result->file_buf[3] != 'F') {
        heap_caps_free(result->file_buf);
        result->file_buf = NULL;
        ESP_LOGE(TAG, "Not an ELF file: %s", path);
        return ESP_ERR_INVALID_ARG;
    }

    int ret = esp_elf_init(&result->elf);
    if (ret != 0) {
        heap_caps_free(result->file_buf);
        result->file_buf = NULL;
        ESP_LOGE(TAG, "esp_elf_init failed: %d", ret);
        return ESP_FAIL;
    }

    ret = esp_elf_relocate(&result->elf, result->file_buf);
    if (ret != 0) {
        esp_elf_deinit(&result->elf);
        heap_caps_free(result->file_buf);
        result->file_buf = NULL;
        ESP_LOGE(TAG, "esp_elf_relocate failed: %d", ret);
        return ESP_FAIL;
    }

    result->text_size = result->elf.sec[ELF_SEC_TEXT].size;
    result->data_size = result->elf.sec[ELF_SEC_DATA].size +
                        result->elf.sec[ELF_SEC_BSS].size +
                        result->elf.sec[ELF_SEC_RODATA].size;

    ESP_LOGI(TAG, "Loaded %s: text=%zuB, data=%zuB",
             path, result->text_size, result->data_size);
    return ESP_OK;
}

void elf_loader_unload(elf_load_result_t *result)
{
    if (!result) return;
    esp_elf_deinit(&result->elf);
    if (result->file_buf) {
        heap_caps_free(result->file_buf);
        result->file_buf = NULL;
    }
    ESP_LOGI(TAG, "ELF unloaded");
}

int elf_loader_call_entry(elf_load_result_t *result, int argc, char *argv[])
{
    if (!result) return -1;
    return esp_elf_request(&result->elf, 0, argc, argv);
}
