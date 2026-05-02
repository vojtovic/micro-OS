#include "cmd_files.h"
#include "shell/shell.h"
#include "services/vconsole.h"
#include "hal/internal_fs.h"
#include "esp_console.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "esp_littlefs.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/unistd.h>
#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <stdbool.h>
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "services/http_client.h"
#include "services/wifi.h"

static const char *TAG = "cmd_files";

static int cmd_write(int argc, char **argv)
{
    if (argc < 3) {
        vconsole_printf("Usage: write <file> <text...>\n");
        return 1;
    }
    char path[256];
    shell_resolve_path(argv[1], path, sizeof(path));
    FILE *f = fopen(path, "w");
    if (!f) {
        vconsole_printf("Cannot open: %s (%s)\n", path, strerror(errno));
        return 1;
    }
    for (int i = 2; i < argc; i++) {
        fputs(argv[i], f);
        if (i < argc - 1) fputc(' ', f);
    }
    fputc('\n', f);
    fclose(f);
    vconsole_printf("Written: %s\n", argv[1]);
    return 0;
}

static int cmd_append(int argc, char **argv)
{
    if (argc < 3) {
        vconsole_printf("Usage: append <file> <text...>\n");
        return 1;
    }
    char path[256];
    shell_resolve_path(argv[1], path, sizeof(path));
    FILE *f = fopen(path, "a");
    if (!f) {
        vconsole_printf("Cannot open: %s (%s)\n", path, strerror(errno));
        return 1;
    }
    for (int i = 2; i < argc; i++) {
        fputs(argv[i], f);
        if (i < argc - 1) fputc(' ', f);
    }
    fputc('\n', f);
    fclose(f);
    return 0;
}

static int cmd_mkdir(int argc, char **argv)
{
    if (argc < 2) {
        vconsole_printf("Usage: mkdir <path>\n");
        return 1;
    }
    char path[256];
    shell_resolve_path(argv[1], path, sizeof(path));
    if (mkdir(path, 0755) != 0) {
        vconsole_printf("mkdir failed: %s (%s)\n", path, strerror(errno));
        return 1;
    }
    return 0;
}

static int cmd_rmdir(int argc, char **argv)
{
    if (argc < 2) {
        vconsole_printf("Usage: rmdir <path>\n");
        return 1;
    }
    char path[256];
    shell_resolve_path(argv[1], path, sizeof(path));
    if (rmdir(path) != 0) {
        vconsole_printf("rmdir failed: %s (%s)\n", path, strerror(errno));
        return 1;
    }
    return 0;
}

static int cmd_rm(int argc, char **argv)
{
    if (argc < 2) {
        vconsole_printf("Usage: rm <file>\n");
        return 1;
    }
    char path[256];
    shell_resolve_path(argv[1], path, sizeof(path));
    struct stat st;
    if (stat(path, &st) != 0) {
        vconsole_printf("Not found: %s\n", path);
        return 1;
    }
    if (S_ISDIR(st.st_mode)) {
        vconsole_printf("Is a directory (use rmdir): %s\n", path);
        return 1;
    }
    if (unlink(path) != 0) {
        vconsole_printf("rm failed: %s (%s)\n", path, strerror(errno));
        return 1;
    }
    return 0;
}

static int cmd_touch(int argc, char **argv)
{
    if (argc < 2) {
        vconsole_printf("Usage: touch <file>\n");
        return 1;
    }
    char path[256];
    shell_resolve_path(argv[1], path, sizeof(path));
    FILE *f = fopen(path, "a");
    if (!f) {
        vconsole_printf("Cannot create: %s (%s)\n", path, strerror(errno));
        return 1;
    }
    fclose(f);
    return 0;
}

static int cmd_stat_cmd(int argc, char **argv)
{
    if (argc < 2) {
        vconsole_printf("Usage: stat <path>\n");
        return 1;
    }
    char path[256];
    shell_resolve_path(argv[1], path, sizeof(path));
    struct stat st;
    if (stat(path, &st) != 0) {
        vconsole_printf("Not found: %s\n", path);
        return 1;
    }
    const char *type = S_ISDIR(st.st_mode) ? "directory" : "file";
    vconsole_printf("  Path: %s\n", path);
    vconsole_printf("  Type: %s\n", type);
    vconsole_printf("  Size: %ld bytes\n", (long)st.st_size);
    return 0;
}

static int cmd_df(int argc, char **argv)
{
    vconsole_printf("Filesystem      Size      Used      Free   Use%%\n");

    uint64_t total = 0, free_bytes = 0;
    esp_err_t ret = esp_vfs_fat_info("/sdcard", &total, &free_bytes);
    if (ret == ESP_OK) {
        uint64_t used = total - free_bytes;
        vconsole_printf("/sdcard    %7lluMB %7lluMB %7lluMB   %llu%%\n",
               (unsigned long long)(total / (1024*1024)),
               (unsigned long long)(used / (1024*1024)),
               (unsigned long long)(free_bytes / (1024*1024)),
               (unsigned long long)(total > 0 ? used * 100 / total : 0));
    }

    if (internal_fs_is_mounted()) {
        size_t lfs_total = 0, lfs_used = 0;
        if (esp_littlefs_info("internal", &lfs_total, &lfs_used) == ESP_OK) {
            size_t lfs_free = lfs_total - lfs_used;
            vconsole_printf("/sys       %7zuKB %7zuKB %7zuKB   %zu%%\n",
                   lfs_total / 1024, lfs_used / 1024, lfs_free / 1024,
                   lfs_total > 0 ? lfs_used * 100 / lfs_total : 0);
        }
    }
    return 0;
}

static int cmd_cp(int argc, char **argv)
{
    if (argc < 3) {
        vconsole_printf("Usage: cp <src> <dst>\n");
        return 1;
    }
    char src_path[256], dst_path[256];
    shell_resolve_path(argv[1], src_path, sizeof(src_path));
    shell_resolve_path(argv[2], dst_path, sizeof(dst_path));
    FILE *src = fopen(src_path, "rb");
    if (!src) {
        vconsole_printf("Cannot open source: %s\n", src_path);
        return 1;
    }
    FILE *dst = fopen(dst_path, "wb");
    if (!dst) {
        vconsole_printf("Cannot open destination: %s\n", dst_path);
        fclose(src);
        return 1;
    }
    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), src)) > 0) {
        fwrite(buf, 1, n, dst);
    }
    fclose(src);
    fclose(dst);
    vconsole_printf("Copied: %s -> %s\n", src_path, dst_path);
    return 0;
}

static int cmd_mv(int argc, char **argv)
{
    if (argc < 3) {
        vconsole_printf("Usage: mv <src> <dst>\n");
        return 1;
    }
    char src_path[256], dst_path[256];
    shell_resolve_path(argv[1], src_path, sizeof(src_path));
    shell_resolve_path(argv[2], dst_path, sizeof(dst_path));
    if (rename(src_path, dst_path) != 0) {
        vconsole_printf("mv failed: %s (%s)\n", src_path, strerror(errno));
        return 1;
    }
    return 0;
}

static int cmd_head(int argc, char **argv)
{
    if (argc < 2) {
        vconsole_printf("Usage: head [-n N] <file>\n");
        return 1;
    }
    int lines = 10;
    const char *file = argv[1];
    if (argc >= 3 && strcmp(argv[1], "-n") == 0) {
        lines = atoi(argv[2]);
        file = (argc > 3) ? argv[3] : NULL;
    }
    if (!file) {
        vconsole_printf("Usage: head [-n N] <file>\n");
        return 1;
    }
    char path[256];
    shell_resolve_path(file, path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) {
        vconsole_printf("Cannot open: %s\n", path);
        return 1;
    }
    char buf[256];
    int count = 0;
    while (count < lines && fgets(buf, sizeof(buf), f)) {
        vconsole_write(buf, strlen(buf));
        if (strchr(buf, '\n')) count++;
    }
    fclose(f);
    return 0;
}

static int cmd_tail(int argc, char **argv)
{
    if (argc < 2) {
        vconsole_printf("Usage: tail [-n N] <file>\n");
        return 1;
    }
    int lines = 10;
    const char *file = argv[1];
    if (argc >= 3 && strcmp(argv[1], "-n") == 0) {
        lines = atoi(argv[2]);
        file = (argc > 3) ? argv[3] : NULL;
    }
    if (!file) {
        vconsole_printf("Usage: tail [-n N] <file>\n");
        return 1;
    }
    char path[256];
    shell_resolve_path(file, path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) {
        vconsole_printf("Cannot open: %s\n", path);
        return 1;
    }
    if (lines > 32) lines = 32;

    char (*ringbuf)[256] = heap_caps_malloc(lines * 256, MALLOC_CAP_SPIRAM);
    if (!ringbuf) {
        vconsole_printf("Out of memory\n");
        fclose(f);
        return 1;
    }

    int idx = 0, total = 0;
    while (fgets(ringbuf[idx % lines], 256, f)) {
        idx++;
        total++;
    }
    fclose(f);

    int show = (total < lines) ? total : lines;
    int start = (idx - show) % lines;
    if (start < 0) start += lines;
    for (int i = 0; i < show; i++) {
        vconsole_write(ringbuf[(start + i) % lines],
                       strlen(ringbuf[(start + i) % lines]));
    }
    heap_caps_free(ringbuf);
    return 0;
}

static int cmd_grep(int argc, char **argv)
{
    if (argc < 3) {
        vconsole_printf("Usage: grep <pattern> <file>\n");
        return 1;
    }
    char path[256];
    shell_resolve_path(argv[2], path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) {
        vconsole_printf("Cannot open: %s\n", path);
        return 1;
    }
    char line[256];
    int lineno = 0, matches = 0;
    while (fgets(line, sizeof(line), f)) {
        lineno++;
        if (strstr(line, argv[1])) {
            vconsole_printf("%d:%s", lineno, line);
            if (!strchr(line, '\n')) vconsole_putchar('\n');
            matches++;
        }
    }
    fclose(f);
    return (matches > 0) ? 0 : 1;
}

static int cmd_wc(int argc, char **argv)
{
    if (argc < 2) {
        vconsole_printf("Usage: wc <file>\n");
        return 1;
    }
    char path[256];
    shell_resolve_path(argv[1], path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) {
        vconsole_printf("Cannot open: %s\n", path);
        return 1;
    }
    int lines = 0, words = 0, bytes = 0;
    int in_word = 0;
    int c;
    while ((c = fgetc(f)) != EOF) {
        bytes++;
        if (c == '\n') lines++;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            in_word = 0;
        } else if (!in_word) {
            in_word = 1;
            words++;
        }
    }
    fclose(f);
    vconsole_printf("  %d  %d  %d  %s\n", lines, words, bytes, path);
    return 0;
}

static int cmd_hexdump(int argc, char **argv)
{
    if (argc < 2) {
        vconsole_printf("Usage: hexdump <file> [offset] [length]\n");
        return 1;
    }
    char path[256];
    shell_resolve_path(argv[1], path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f) {
        vconsole_printf("Cannot open: %s\n", path);
        return 1;
    }

    long offset = 0;
    long length = 256;
    if (argc >= 3) offset = strtol(argv[2], NULL, 0);
    if (argc >= 4) length = strtol(argv[3], NULL, 0);
    if (length <= 0) length = 256;

    if (offset > 0) fseek(f, offset, SEEK_SET);

    unsigned char buf[16];
    long total = 0;
    while (total < length) {
        int to_read = 16;
        if (total + to_read > length) to_read = (int)(length - total);
        size_t n = fread(buf, 1, to_read, f);
        if (n == 0) break;

        vconsole_printf("%08lx  ", offset + total);
        for (int i = 0; i < 16; i++) {
            if (i < (int)n) vconsole_printf("%02x ", buf[i]);
            else vconsole_printf("   ");
            if (i == 7) vconsole_printf(" ");
        }
        vconsole_printf(" |");
        for (int i = 0; i < (int)n; i++) {
            vconsole_printf("%c", (buf[i] >= 0x20 && buf[i] <= 0x7e) ? buf[i] : '.');
        }
        vconsole_printf("|\n");
        total += n;
    }
    fclose(f);
    return 0;
}

static void tree_recurse(const char *base, int depth, int max_depth)
{
    if (depth >= max_depth) return;

    DIR *dir = opendir(base);
    if (!dir) return;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        for (int i = 0; i < depth; i++) vconsole_printf("  ");
        if (entry->d_type == DT_DIR) {
            vconsole_printf("  %s/\n", entry->d_name);
            char child[512];
            snprintf(child, sizeof(child), "%.255s/%.255s", base, entry->d_name);
            tree_recurse(child, depth + 1, max_depth);
        } else {
            vconsole_printf("  %s\n", entry->d_name);
        }
    }
    closedir(dir);
}

static int cmd_tree(int argc, char **argv)
{
    char path[256];
    if (argc > 1) {
        shell_resolve_path(argv[1], path, sizeof(path));
    } else {
        strncpy(path, shell_get_cwd(), sizeof(path));
        path[sizeof(path) - 1] = '\0';
    }

    vconsole_printf("%s\n", path);
    tree_recurse(path, 0, 8);
    return 0;
}

static int cmd_note(int argc, char **argv)
{
    const char *notes_path = "/sys/log/notes.log";

    if (argc < 2) {
        FILE *f = fopen(notes_path, "r");
        if (!f) {
            vconsole_printf("No notes yet.\n");
            return 0;
        }
        char line[256];
        while (fgets(line, sizeof(line), f)) vconsole_write(line, strlen(line));
        fclose(f);
        return 0;
    }

    int64_t us = esp_timer_get_time();
    int sec = (int)(us / 1000000);
    int hrs = (sec / 3600) % 24;
    int mins = (sec / 60) % 60;
    int secs = sec % 60;

    FILE *f = fopen(notes_path, "a");
    if (!f) {
        vconsole_printf("Cannot write to %s\n", notes_path);
        return 1;
    }
    fprintf(f, "[%02d:%02d:%02d] ", hrs, mins, secs);
    for (int i = 1; i < argc; i++) {
        fputs(argv[i], f);
        if (i < argc - 1) fputc(' ', f);
    }
    fputc('\n', f);
    fclose(f);
    vconsole_printf("Note saved.\n");
    return 0;
}

static int cmd_source(int argc, char **argv)
{
    if (argc < 2) {
        vconsole_printf("Usage: source <script>\n");
        return 1;
    }
    char path[256];
    shell_resolve_path(argv[1], path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) {
        vconsole_printf("Cannot open: %s\n", path);
        return 1;
    }
    char line[256];
    int lineno = 0;
    while (fgets(line, sizeof(line), f)) {
        lineno++;
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';
        if (len == 0 || line[0] == '#') continue;

        int ret;
        esp_err_t err = esp_console_run(line, &ret);
        if (err == ESP_ERR_NOT_FOUND) {
            vconsole_printf("%s:%d: unknown command: %s\n", argv[1], lineno, line);
        } else if (err != ESP_OK && err != ESP_ERR_INVALID_ARG) {
            vconsole_printf("%s:%d: error: %s\n", argv[1], lineno, esp_err_to_name(err));
        }
    }
    fclose(f);
    return 0;
}

static void find_recurse(const char *base, const char *pattern, int depth)
{
    if (depth > 10) return;

    DIR *dir = opendir(base);
    if (!dir) return;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        char fullpath[512];
        snprintf(fullpath, sizeof(fullpath), "%.255s/%.255s", base, entry->d_name);

        if (strstr(entry->d_name, pattern)) {
            vconsole_printf("%s\n", fullpath);
        }

        if (entry->d_type == DT_DIR) {
            find_recurse(fullpath, pattern, depth + 1);
        }
    }
    closedir(dir);
}

static int cmd_find(int argc, char **argv)
{
    if (argc < 2) {
        vconsole_printf("Usage: find <pattern> [path]\n");
        return 1;
    }
    char path[256];
    if (argc >= 3) {
        shell_resolve_path(argv[2], path, sizeof(path));
    } else {
        strncpy(path, shell_get_cwd(), sizeof(path));
        path[sizeof(path) - 1] = '\0';
    }
    find_recurse(path, argv[1], 0);
    return 0;
}

static long du_recurse(const char *base, int depth, bool print)
{
    if (depth > 10) return 0;

    DIR *dir = opendir(base);
    if (!dir) return 0;

    long total = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        char fullpath[512];
        snprintf(fullpath, sizeof(fullpath), "%.255s/%.255s", base, entry->d_name);
        struct stat st;

        if (entry->d_type == DT_DIR) {
            long sub = du_recurse(fullpath, depth + 1, print);
            total += sub;
        } else if (stat(fullpath, &st) == 0) {
            total += (long)st.st_size;
        }
    }
    closedir(dir);

    if (print) {
        if (total > 1024 * 1024)
            vconsole_printf("%7ldK  %s\n", total / 1024, base);
        else
            vconsole_printf("%7ldB  %s\n", total, base);
    }
    return total;
}

static int cmd_du(int argc, char **argv)
{
    char path[256];
    if (argc > 1) {
        shell_resolve_path(argv[1], path, sizeof(path));
    } else {
        strncpy(path, shell_get_cwd(), sizeof(path));
        path[sizeof(path) - 1] = '\0';
    }
    du_recurse(path, 0, true);
    return 0;
}

static int cmd_basename(int argc, char **argv)
{
    if (argc < 2) {
        vconsole_printf("Usage: basename <path>\n");
        return 1;
    }
    const char *p = argv[1];
    const char *last = strrchr(p, '/');
    vconsole_printf("%s\n", last ? last + 1 : p);
    return 0;
}

static int cmd_dirname(int argc, char **argv)
{
    if (argc < 2) {
        vconsole_printf("Usage: dirname <path>\n");
        return 1;
    }
    char buf[256];
    strncpy(buf, argv[1], sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *last = strrchr(buf, '/');
    if (!last) {
        vconsole_printf(".\n");
    } else if (last == buf) {
        vconsole_printf("/\n");
    } else {
        *last = '\0';
        vconsole_printf("%s\n", buf);
    }
    return 0;
}

static int cmd_test(int argc, char **argv)
{
    if (argc < 3) {
        vconsole_printf("Usage: test <-f|-d|-e|-s> <path>\n");
        vconsole_printf("  -f  file exists     -d  directory exists\n");
        vconsole_printf("  -e  path exists     -s  file is non-empty\n");
        return 1;
    }

    char path[256];
    shell_resolve_path(argv[2], path, sizeof(path));
    struct stat st;
    int exists = (stat(path, &st) == 0);

    if (strcmp(argv[1], "-e") == 0) {
        return exists ? 0 : 1;
    } else if (strcmp(argv[1], "-f") == 0) {
        return (exists && S_ISREG(st.st_mode)) ? 0 : 1;
    } else if (strcmp(argv[1], "-d") == 0) {
        return (exists && S_ISDIR(st.st_mode)) ? 0 : 1;
    } else if (strcmp(argv[1], "-s") == 0) {
        return (exists && st.st_size > 0) ? 0 : 1;
    }

    vconsole_printf("Unknown flag: %s\n", argv[1]);
    return 1;
}

static int cmd_wget(int argc, char **argv)
{
    if (argc < 2) {
        vconsole_printf("Usage: wget <url> [dest_path]\n");
        return 1;
    }

    if (!wifi_is_connected()) {
        vconsole_printf("Wi-Fi not connected — use 'wifi connect' first\n");
        return 1;
    }

    char dest[256];
    if (argc >= 3) {
        shell_resolve_path(argv[2], dest, sizeof(dest));
    } else {
        const char *url = argv[1];
        const char *slash = strrchr(url, '/');
        const char *fname = (slash && slash[1]) ? slash + 1 : "download";
        snprintf(dest, sizeof(dest), "%s/%s", shell_get_cwd(), fname);
    }

    vconsole_printf("Downloading %s\n", argv[1]);
    vconsole_printf("  -> %s\n", dest);

    esp_err_t err = http_download_file(argv[1], dest);
    if (err != ESP_OK) {
        vconsole_printf("Download failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    struct stat st;
    if (stat(dest, &st) == 0) {
        vconsole_printf("Saved %ld bytes\n", (long)st.st_size);
    } else {
        vconsole_printf("Done\n");
    }
    return 0;
}

#define RECV_SOH   0x01
#define RECV_EOT   0x04
#define RECV_ACK   0x06
#define RECV_NAK   0x15
#define RECV_CAN   0x18
#define RECV_BLOCK 128

static int recv_getchar_timeout(int timeout_ms)
{
    int elapsed = 0;
    while (elapsed < timeout_ms) {
        int c = getchar();
        if (c != EOF) return c;
        vTaskDelay(pdMS_TO_TICKS(10));
        elapsed += 10;
    }
    return -1;
}

static int cmd_recv(int argc, char **argv)
{
    if (argc < 2) {
        vconsole_printf("Usage: recv <dest_path>\n");
        vconsole_printf("  Receives file via XMODEM over serial.\n");
        vconsole_printf("  Start XMODEM send from your terminal after running this.\n");
        return 1;
    }

    char dest[256];
    shell_resolve_path(argv[1], dest, sizeof(dest));

    FILE *f = fopen(dest, "wb");
    if (!f) {
        vconsole_printf("Cannot create: %s\n", dest);
        return 1;
    }

    vconsole_printf("Ready for XMODEM transfer to %s\n", dest);
    vconsole_printf("Start sending from your terminal now...\n");

    vTaskDelay(pdMS_TO_TICKS(500));

    int block_num = 1;
    long total = 0;
    int retries = 0;
    bool started = false;

    while (true) {
        if (!started) {
            putchar(RECV_NAK);
            fflush(stdout);
        }

        int ch = recv_getchar_timeout(10000);
        if (ch < 0) {
            retries++;
            if (retries > 10) {
                vconsole_printf("\nTimeout — transfer aborted\n");
                fclose(f);
                remove(dest);
                return 1;
            }
            if (!started) {
                putchar(RECV_NAK);
                fflush(stdout);
            }
            continue;
        }

        if (ch == RECV_EOT) {
            putchar(RECV_ACK);
            fflush(stdout);
            break;
        }

        if (ch == RECV_CAN) {
            vconsole_printf("\nSender cancelled\n");
            fclose(f);
            remove(dest);
            return 1;
        }

        if (ch != RECV_SOH) continue;

        started = true;
        retries = 0;

        int blk = recv_getchar_timeout(1000);
        int blk_inv = recv_getchar_timeout(1000);
        if (blk < 0 || blk_inv < 0) {
            putchar(RECV_NAK);
            fflush(stdout);
            continue;
        }

        if ((blk + blk_inv) != 0xFF) {
            for (int i = 0; i < RECV_BLOCK; i++) recv_getchar_timeout(100);
            recv_getchar_timeout(100);
            putchar(RECV_NAK);
            fflush(stdout);
            continue;
        }

        uint8_t data[RECV_BLOCK];
        uint8_t cksum = 0;
        bool short_read = false;
        for (int i = 0; i < RECV_BLOCK; i++) {
            int b = recv_getchar_timeout(1000);
            if (b < 0) { short_read = true; break; }
            data[i] = (uint8_t)b;
            cksum += data[i];
        }

        int recv_ck = recv_getchar_timeout(1000);
        if (short_read || recv_ck < 0 || (uint8_t)recv_ck != cksum) {
            putchar(RECV_NAK);
            fflush(stdout);
            continue;
        }

        if (blk == (block_num & 0xFF)) {
            fwrite(data, 1, RECV_BLOCK, f);
            total += RECV_BLOCK;
            block_num++;
        }

        putchar(RECV_ACK);
        fflush(stdout);
    }

    fclose(f);

    struct stat st;
    if (stat(dest, &st) == 0) {
        vconsole_printf("\nReceived %ld bytes -> %s\n", (long)st.st_size, dest);
    }
    return 0;
}

esp_err_t cmd_files_register(void)
{
    const esp_console_cmd_t cmds[] = {
        { .command = "write",  .help = "Write text to file: write <file> <text...>",  .func = cmd_write },
        { .command = "append", .help = "Append text to file: append <file> <text...>",.func = cmd_append },
        { .command = "mkdir",  .help = "Create directory",                             .func = cmd_mkdir },
        { .command = "rmdir",  .help = "Remove empty directory",                       .func = cmd_rmdir },
        { .command = "rm",     .help = "Remove a file",                                .func = cmd_rm },
        { .command = "touch",  .help = "Create empty file",                            .func = cmd_touch },
        { .command = "stat",   .help = "Show file info",                               .func = cmd_stat_cmd },
        { .command = "df",     .help = "Show filesystem usage",                        .func = cmd_df },
        { .command = "cp",     .help = "Copy file: cp <src> <dst>",                   .func = cmd_cp },
        { .command = "mv",     .help = "Move/rename: mv <src> <dst>",                 .func = cmd_mv },
        { .command = "head",   .help = "Show first N lines: head [-n N] <file>",      .func = cmd_head },
        { .command = "tail",   .help = "Show last N lines: tail [-n N] <file>",       .func = cmd_tail },
        { .command = "grep",   .help = "Search text: grep <pattern> <file>",          .func = cmd_grep },
        { .command = "wc",      .help = "Word/line/byte count",                           .func = cmd_wc },
        { .command = "hexdump", .help = "Hex viewer: hexdump <file> [offset] [len]",     .func = cmd_hexdump },
        { .command = "tree",    .help = "Recursive directory listing",                    .func = cmd_tree },
        { .command = "note",    .help = "Save/show notes: note [text...]",               .func = cmd_note },
        { .command = "source",   .help = "Run script: source <file>",                     .func = cmd_source },
        { .command = "find",     .help = "Find files: find <pattern> [path]",            .func = cmd_find },
        { .command = "du",       .help = "Disk usage: du [path]",                        .func = cmd_du },
        { .command = "basename", .help = "Strip directory from path",                    .func = cmd_basename },
        { .command = "dirname",  .help = "Strip filename from path",                     .func = cmd_dirname },
        { .command = "test",     .help = "Test file: test <-f|-d|-e|-s> <path>",         .func = cmd_test },
        { .command = "wget",     .help = "Download file: wget <url> [dest]",             .func = cmd_wget },
        { .command = "recv",     .help = "Receive file via XMODEM: recv <dest_path>",    .func = cmd_recv },
    };

    for (int i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        esp_err_t ret = esp_console_cmd_register(&cmds[i]);
        if (ret != ESP_OK) return ret;
    }
    return ESP_OK;
}
