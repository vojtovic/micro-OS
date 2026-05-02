#include "pkg_manager.h"
#include "wifi.h"
#include "http_client.h"
#include "config.h"
#include "vconsole.h"
#include "loader/module_mgr.h"
#include "esp_console.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "mbedtls/sha256.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>

static const char *TAG = "pkg_mgr";

static pkg_repo_t s_repo;

#define PACMAN_CONF     "/sys/etc/pacman.conf"
#define REPO_CACHE      "/sys/cache/repo.idx"
#define PKG_INSTALL_DIR "/sdcard/bin"
#define PKG_TMP_DIR     "/sdcard/tmp"

static void ensure_dirs(void)
{
    mkdir(PKG_INSTALL_DIR, 0755);
    mkdir(PKG_TMP_DIR, 0755);
}

static esp_err_t parse_repo_index(const char *data, size_t len)
{
    s_repo.count = 0;
    const char *p = data;
    const char *end = data + len;

    while (p < end && s_repo.count < PKG_MAX_ENTRIES) {
        const char *line_end = memchr(p, '\n', end - p);
        if (!line_end) line_end = end;

        size_t line_len = line_end - p;
        if (line_len == 0 || p[0] == '#') {
            p = line_end + 1;
            continue;
        }

        char line[256];
        size_t copy = (line_len < sizeof(line) - 1) ? line_len : sizeof(line) - 1;
        memcpy(line, p, copy);
        line[copy] = '\0';

        pkg_entry_t *e = &s_repo.entries[s_repo.count];
        memset(e, 0, sizeof(*e));

        char *tok = strtok(line, "|");
        if (tok) strncpy(e->name, tok, PKG_NAME_LEN - 1);
        tok = strtok(NULL, "|");
        if (tok) strncpy(e->version, tok, PKG_VER_LEN - 1);
        tok = strtok(NULL, "|");
        if (tok) e->size = (uint32_t)atoi(tok);
        tok = strtok(NULL, "|");
        if (tok) strncpy(e->hash, tok, sizeof(e->hash) - 1);
        tok = strtok(NULL, "|");
        if (tok) strncpy(e->filename, tok, sizeof(e->filename) - 1);
        tok = strtok(NULL, "|");
        if (tok) strncpy(e->description, tok, PKG_DESC_LEN - 1);

        if (e->name[0]) s_repo.count++;
        p = line_end + 1;
    }
    return ESP_OK;
}

static esp_err_t load_cached_index(void)
{
    FILE *f = fopen(REPO_CACHE, "r");
    if (!f) return ESP_ERR_NOT_FOUND;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (sz <= 0 || sz > 64 * 1024) {
        fclose(f);
        return ESP_FAIL;
    }

    char *buf = heap_caps_malloc(sz + 1, MALLOC_CAP_SPIRAM);
    if (!buf) { fclose(f); return ESP_ERR_NO_MEM; }

    fread(buf, 1, sz, f);
    fclose(f);
    buf[sz] = '\0';

    esp_err_t err = parse_repo_index(buf, sz);
    heap_caps_free(buf);
    return err;
}

static esp_err_t save_cached_index(const char *data, size_t len)
{
    mkdir("/sys/cache", 0755);
    FILE *f = fopen(REPO_CACHE, "w");
    if (!f) return ESP_FAIL;
    fwrite(data, 1, len, f);
    fclose(f);
    return ESP_OK;
}

esp_err_t pkg_manager_init(void)
{
    memset(&s_repo, 0, sizeof(s_repo));

    config_t *cfg = heap_caps_malloc(sizeof(config_t), MALLOC_CAP_SPIRAM);
    if (cfg) {
        if (config_load(cfg, PACMAN_CONF) == ESP_OK) {
            const char *url = config_get(cfg, "options", "repo_url");
            if (url)
                strncpy(s_repo.repo_url, url, sizeof(s_repo.repo_url) - 1);
        }
        heap_caps_free(cfg);
    }

    load_cached_index();
    ensure_dirs();

    ESP_LOGI(TAG, "Package manager ready (%d cached, repo: %s)",
             s_repo.count, s_repo.repo_url[0] ? s_repo.repo_url : "(none)");
    return ESP_OK;
}

esp_err_t pkg_sync(void)
{
    if (!wifi_is_connected()) {
        ESP_LOGE(TAG, "Wi-Fi not connected");
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_repo.repo_url[0]) {
        ESP_LOGE(TAG, "No repository configured — use pacman --repo <url>");
        return ESP_ERR_INVALID_STATE;
    }

    char url[256];
    snprintf(url, sizeof(url), "%s/repo.idx", s_repo.repo_url);

    http_response_t resp;
    esp_err_t err = http_get(url, &resp);
    if (err != ESP_OK) return err;

    if (resp.status_code != 200) {
        ESP_LOGE(TAG, "Server returned %d", resp.status_code);
        http_response_free(&resp);
        return ESP_FAIL;
    }

    err = parse_repo_index(resp.body, resp.body_len);
    if (err == ESP_OK)
        save_cached_index(resp.body, resp.body_len);

    http_response_free(&resp);
    ESP_LOGI(TAG, "Synchronized %d packages", s_repo.count);
    return ESP_OK;
}

static const pkg_entry_t *find_package(const char *name)
{
    for (int i = 0; i < s_repo.count; i++) {
        if (strcmp(s_repo.entries[i].name, name) == 0)
            return &s_repo.entries[i];
    }
    return NULL;
}

esp_err_t pkg_install(const char *name)
{
    const pkg_entry_t *pkg = find_package(name);
    if (!pkg) {
        ESP_LOGE(TAG, "Package '%s' not found — run pacman -Sy first", name);
        return ESP_ERR_NOT_FOUND;
    }

    if (!wifi_is_connected()) {
        ESP_LOGE(TAG, "Wi-Fi not connected");
        return ESP_ERR_INVALID_STATE;
    }

    char url[256], tmp_path[128];
    snprintf(url, sizeof(url), "%s/%s", s_repo.repo_url, pkg->filename);
    snprintf(tmp_path, sizeof(tmp_path), "%s/%s", PKG_TMP_DIR, pkg->filename);

    esp_err_t err = http_download_file(url, tmp_path);
    if (err != ESP_OK) return err;

    FILE *f = fopen(tmp_path, "rb");
    if (!f) { remove(tmp_path); return ESP_FAIL; }

    pkg_header_t hdr;
    if (fread(&hdr, 1, sizeof(hdr), f) != sizeof(hdr)) {
        fclose(f); remove(tmp_path);
        ESP_LOGE(TAG, "Invalid package (truncated header)");
        return ESP_ERR_INVALID_SIZE;
    }

    if (hdr.magic != PKG_MAGIC || hdr.version != PKG_VERSION) {
        fclose(f); remove(tmp_path);
        ESP_LOGE(TAG, "Invalid package (bad magic/version)");
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t *elf_buf = heap_caps_malloc(hdr.elf_size, MALLOC_CAP_SPIRAM);
    if (!elf_buf) { fclose(f); remove(tmp_path); return ESP_ERR_NO_MEM; }

    if (fread(elf_buf, 1, hdr.elf_size, f) != hdr.elf_size) {
        heap_caps_free(elf_buf); fclose(f); remove(tmp_path);
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t computed_hash[32];
    mbedtls_sha256(elf_buf, hdr.elf_size, computed_hash, 0);

    if (memcmp(computed_hash, hdr.elf_hash, 32) != 0) {
        ESP_LOGE(TAG, "SHA-256 mismatch — package corrupt or tampered");
        heap_caps_free(elf_buf); fclose(f); remove(tmp_path);
        return ESP_ERR_INVALID_CRC;
    }

    char elf_path[128], ini_path[128];
    snprintf(elf_path, sizeof(elf_path), "%s/%s.elf", PKG_INSTALL_DIR, name);
    snprintf(ini_path, sizeof(ini_path), "%s/%s.ini", PKG_INSTALL_DIR, name);

    FILE *ef = fopen(elf_path, "wb");
    if (!ef) {
        heap_caps_free(elf_buf); fclose(f); remove(tmp_path);
        return ESP_FAIL;
    }
    fwrite(elf_buf, 1, hdr.elf_size, ef);
    fclose(ef);
    heap_caps_free(elf_buf);

    if (hdr.ini_size > 0) {
        uint8_t *ini_buf = heap_caps_malloc(hdr.ini_size, MALLOC_CAP_SPIRAM);
        if (ini_buf) {
            if (fread(ini_buf, 1, hdr.ini_size, f) == hdr.ini_size) {
                FILE *inf = fopen(ini_path, "wb");
                if (inf) {
                    fwrite(ini_buf, 1, hdr.ini_size, inf);
                    fclose(inf);
                }
            }
            heap_caps_free(ini_buf);
        }
    }

    fclose(f);
    remove(tmp_path);

    ESP_LOGI(TAG, "Installed %s v%s (%lu bytes)", name, pkg->version,
             (unsigned long)hdr.elf_size);
    return ESP_OK;
}

esp_err_t pkg_remove(const char *name)
{
    if (module_mgr_find(name)) {
        ESP_LOGE(TAG, "Module '%s' is loaded — modstop + modunload first", name);
        return ESP_ERR_INVALID_STATE;
    }

    char elf_path[128], ini_path[128];
    snprintf(elf_path, sizeof(elf_path), "%s/%s.elf", PKG_INSTALL_DIR, name);
    snprintf(ini_path, sizeof(ini_path), "%s/%s.ini", PKG_INSTALL_DIR, name);

    struct stat st;
    if (stat(elf_path, &st) != 0) {
        ESP_LOGE(TAG, "Package '%s' not installed", name);
        return ESP_ERR_NOT_FOUND;
    }

    remove(elf_path);
    remove(ini_path);
    ESP_LOGI(TAG, "Removed %s", name);
    return ESP_OK;
}

esp_err_t pkg_list_available(void)
{
    if (s_repo.count == 0) {
        vconsole_printf("No packages — run pacman -Sy to sync\n");
        return ESP_OK;
    }

    vconsole_printf("%-16s %-8s %6s  %s\n", "NAME", "VERSION", "SIZE", "DESCRIPTION");
    for (int i = 0; i < s_repo.count; i++) {
        const pkg_entry_t *e = &s_repo.entries[i];
        vconsole_printf("%-16s %-8s %6lu  %s\n", e->name, e->version,
                        (unsigned long)e->size, e->description);
    }
    vconsole_printf("(%d packages)\n", s_repo.count);
    return ESP_OK;
}

esp_err_t pkg_list_installed(void)
{
    DIR *dir = opendir(PKG_INSTALL_DIR);
    if (!dir) {
        vconsole_printf("No packages installed\n");
        return ESP_OK;
    }

    int count = 0;
    vconsole_printf("%-16s %-8s %s\n", "NAME", "VERSION", "PATH");

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        size_t namelen = strlen(ent->d_name);
        if (namelen < 5 || strcmp(ent->d_name + namelen - 4, ".elf") != 0)
            continue;

        char base[PKG_NAME_LEN];
        strncpy(base, ent->d_name, namelen - 4);
        base[namelen - 4] = '\0';

        char ini_path[128];
        snprintf(ini_path, sizeof(ini_path), "%s/%s.ini", PKG_INSTALL_DIR, base);

        char version[PKG_VER_LEN] = "?";
        config_t *cfg = heap_caps_malloc(sizeof(config_t), MALLOC_CAP_SPIRAM);
        if (cfg) {
            if (config_load(cfg, ini_path) == ESP_OK) {
                const char *v = config_get(cfg, "module", "version");
                if (v) strncpy(version, v, PKG_VER_LEN - 1);
            }
            heap_caps_free(cfg);
        }

        vconsole_printf("%-16s %-8s %s/%s\n", base, version,
                        PKG_INSTALL_DIR, ent->d_name);
        count++;
    }
    closedir(dir);

    if (count == 0)
        vconsole_printf("No packages installed\n");
    else
        vconsole_printf("(%d installed)\n", count);
    return ESP_OK;
}

esp_err_t pkg_set_repo(const char *url)
{
    if (!url) return ESP_ERR_INVALID_ARG;
    strncpy(s_repo.repo_url, url, sizeof(s_repo.repo_url) - 1);

    mkdir("/sys/etc", 0755);
    FILE *f = fopen(PACMAN_CONF, "w");
    if (f) {
        fprintf(f, "[options]\nrepo_url = %s\n", url);
        fclose(f);
    }

    ESP_LOGI(TAG, "Repository set to %s", url);
    return ESP_OK;
}

const pkg_repo_t *pkg_get_repo(void)
{
    return &s_repo;
}

static int cmd_pacman(int argc, char **argv)
{
    if (argc < 2) {
        vconsole_printf("Usage: pacman <-Sy|-S name|-R name|-Ss|-Q|--repo url>\n");
        return 1;
    }

    if (strcmp(argv[1], "-Sy") == 0) {
        vconsole_printf("Synchronizing package database...\n");
        esp_err_t err = pkg_sync();
        if (err == ESP_OK)
            vconsole_printf("Synchronized %d packages\n", s_repo.count);
        else
            vconsole_printf("Sync failed: %s\n", esp_err_to_name(err));
        return (err == ESP_OK) ? 0 : 1;
    }

    if (strcmp(argv[1], "-S") == 0) {
        if (argc < 3) {
            vconsole_printf("Usage: pacman -S <package>\n");
            return 1;
        }
        vconsole_printf("Installing %s...\n", argv[2]);
        esp_err_t err = pkg_install(argv[2]);
        if (err == ESP_OK)
            vconsole_printf("Done.\n");
        else
            vconsole_printf("Install failed: %s\n", esp_err_to_name(err));
        return (err == ESP_OK) ? 0 : 1;
    }

    if (strcmp(argv[1], "-R") == 0) {
        if (argc < 3) {
            vconsole_printf("Usage: pacman -R <package>\n");
            return 1;
        }
        esp_err_t err = pkg_remove(argv[2]);
        if (err == ESP_OK)
            vconsole_printf("Removed %s\n", argv[2]);
        else
            vconsole_printf("Remove failed: %s\n", esp_err_to_name(err));
        return (err == ESP_OK) ? 0 : 1;
    }

    if (strcmp(argv[1], "-Ss") == 0) {
        return (pkg_list_available() == ESP_OK) ? 0 : 1;
    }

    if (strcmp(argv[1], "-Q") == 0) {
        return (pkg_list_installed() == ESP_OK) ? 0 : 1;
    }

    if (strcmp(argv[1], "--repo") == 0) {
        if (argc < 3) {
            if (s_repo.repo_url[0])
                vconsole_printf("Repo: %s\n", s_repo.repo_url);
            else
                vconsole_printf("No repository configured\n");
            return 0;
        }
        esp_err_t err = pkg_set_repo(argv[2]);
        if (err == ESP_OK)
            vconsole_printf("Repository set to %s\n", argv[2]);
        return (err == ESP_OK) ? 0 : 1;
    }

    vconsole_printf("Unknown option: %s\n", argv[1]);
    return 1;
}

esp_err_t cmd_pacman_register(void)
{
    const esp_console_cmd_t cmd = {
        .command = "pacman",
        .help = "Package manager: -Sy sync, -S install, -R remove, -Ss list, -Q installed",
        .func = cmd_pacman,
    };
    return esp_console_cmd_register(&cmd);
}
