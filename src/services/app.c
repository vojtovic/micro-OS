#include "app.h"
#include "input.h"
#include "loader/elf_loader.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <dirent.h>

static const char *TAG = "app";

#define APP_BIN_DIR  "/sdcard/bin"

int app_run(const char *name, int argc, char **argv)
{
    if (!name || !*name) return APP_NOT_FOUND;

    char path[160];
    if (strchr(name, '/'))
        snprintf(path, sizeof(path), "%s", name);            // explicit path
    else
        snprintf(path, sizeof(path), "%s/%s.elf", APP_BIN_DIR, name);

    struct stat st;
    if (stat(path, &st) != 0)
        return APP_NOT_FOUND;

    // Load → run app_main (blocks until the app exits) → unload. Apps are
    // transient foreground programs, so this is deliberately separate from
    // module_mgr (which manages persistent, registered drivers).
    elf_load_result_t elf;
    esp_err_t err = elf_loader_load(path, &elf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "load failed for %s: %s", path, esp_err_to_name(err));
        return -1;
    }

    // Drop the keystroke that launched us (e.g. the Enter after the app name)
    // so it does not immediately satisfy the app's first input_get_key().
    input_flush();

    int ret = elf_loader_call_entry(&elf, argc, argv);
    elf_loader_unload(&elf);

    ESP_LOGI(TAG, "app '%s' exited with %d", name, ret);
    return ret;
}

int app_list(char (*names)[APP_NAME_MAX], int max)
{
    if (!names || max <= 0) return 0;

    DIR *d = opendir(APP_BIN_DIR);
    if (!d) return 0;

    int n = 0;
    struct dirent *e;
    while (n < max && (e = readdir(d)) != NULL) {
        const char *dot = strrchr(e->d_name, '.');
        if (!dot || strcmp(dot, ".elf") != 0) continue;   // only *.elf

        size_t len = (size_t)(dot - e->d_name);
        if (len >= APP_NAME_MAX) len = APP_NAME_MAX - 1;
        memcpy(names[n], e->d_name, len);
        names[n][len] = '\0';
        n++;
    }
    closedir(d);
    return n;
}

int app_read_file(const char *path, char *buf, int max)
{
    if (!path || !buf || max <= 0) return -1;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t n = fread(buf, 1, (size_t)max, f);
    fclose(f);
    return (int)n;
}

int app_write_file(const char *path, const char *buf, int len)
{
    if (!path || !buf || len < 0) return -1;
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t n = len ? fwrite(buf, 1, (size_t)len, f) : 0;
    fclose(f);
    return (n == (size_t)len) ? 0 : -1;
}
