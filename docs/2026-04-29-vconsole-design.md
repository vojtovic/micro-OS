# Virtual Console — Full Integration Design

**Date:** 2026-04-29
**Phase:** 1, Step 4 (vconsole)
**Status:** Architecture approved, implementation pending

## Summary

The virtual console (vconsole) is a PSRAM ring buffer that captures all shell output, providing a single source of truth for display modules (Phase 3). Display tasks consume output via FreeRTOS event groups (zero-poll, instant wake). All 64 shell commands are migrated from `printf()` to `vconsole_printf()`.

## Design Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Capture method | Explicit migration (`vconsole_printf`) | Clean separation: shell output in vconsole, kernel logs in klog |
| Fallback capture | Optional `vconsole_attach_stdout()` | One-function escape hatch if a display module wants everything |
| Notification | FreeRTOS event group | Zero CPU when idle, instant wake, no polling overhead |
| Line lookup | Line index array | O(1) line retrieval for display rendering vs O(n) scan |
| Buffer location | PSRAM (32 KB) | No static RAM impact, plenty of scrollback |
| Thread safety | Mutex on all read/write ops | Display task reads while shell task writes |

## Architecture

```
Shell Commands (vconsole_printf)
  ls, cat, mem, pwd, cd, ... (64 commands)
              |
        vconsole_write()
              |
  +-----------+-----------+
  | PSRAM Ring Buffer 32KB |
  | + mutex               |
  | + line index           |
  +-----------+-----------+
              |
     +--------+--------+
     |        |        |
  UART out  Event   [attach_stdout]
  (serial)  Group   (optional hook)
              |
     +--------+--------+
     |                  |
  OLED task        E-ink task
  (Phase 3)        (Phase 3)
```

## API Surface

### vconsole.h (updated)

```c
// Init / teardown
esp_err_t vconsole_init(size_t buf_size);

// Output (tees to ring buffer + UART)
int vconsole_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int vconsole_putchar(char c);
int vconsole_write(const char *data, size_t len);

// Reading (for display modules)
size_t vconsole_read_tail(char *buf, size_t buf_size);
size_t vconsole_get_lines(int count, char *buf, size_t buf_size);

// Buffer management
size_t vconsole_used(void);
size_t vconsole_capacity(void);
void   vconsole_clear(void);

// Event-driven notification (Phase 3 display modules)
EventGroupHandle_t vconsole_get_event_group(void);
#define VCONSOLE_NEW_DATA_BIT  (1 << 0)
#define VCONSOLE_CLEARED_BIT   (1 << 1)

// Optional: capture all stdout (not called by default)
void vconsole_attach_stdout(void);
void vconsole_detach_stdout(void);

// HAL vtable + service registration
const hal_vconsole_ops_t *vconsole_get_ops(void);
esp_err_t cmd_vconsole_register(void);
```

### hal_vconsole_ops_t

```c
typedef struct hal_vconsole_ops {
    int    (*write)(const char *data, size_t len);
    size_t (*read_tail)(char *buf, size_t buf_size);
    size_t (*get_lines)(int count, char *buf, size_t buf_size);
    void   (*clear)(void);
    size_t (*used)(void);
} hal_vconsole_ops_t;
```

## Line Index Design

Instead of scanning the entire ring buffer for newlines on every `get_lines()` call:

- Maintain a circular array of `uint16_t` offsets pointing to line starts in the ring buffer
- Max 1024 lines tracked (2 KB overhead in PSRAM)
- On every `\n` written, record the next position as a new line start
- `get_lines(n)` reads directly from the last N entries — O(1)

## Event Group Usage

```c
// In vconsole_write():
xEventGroupSetBits(s_event_group, VCONSOLE_NEW_DATA_BIT);

// In a display task (Phase 3):
EventBits_t bits = xEventGroupWaitBits(
    vconsole_get_event_group(),
    VCONSOLE_NEW_DATA_BIT,
    pdTRUE,        // clear bit on return
    pdFALSE,       // any bit
    pdMS_TO_TICKS(1000)  // timeout for periodic refresh
);
if (bits & VCONSOLE_NEW_DATA_BIT) {
    // new content — re-render display
}
```

## stdout Hook (Optional)

`vconsole_attach_stdout()` replaces stdout with a `fopencookie()` FILE* that tees writes to both the ring buffer and the original UART. Not called during normal boot — available for display modules or debug scenarios that want to capture everything.

```c
void vconsole_attach_stdout(void) {
    s_orig_stdout = stdout;
    cookie_io_functions_t funcs = { .write = vconsole_cookie_write };
    stdout = fopencookie(NULL, "w", funcs);
    setvbuf(stdout, NULL, _IONBF, 0);
}

void vconsole_detach_stdout(void) {
    if (s_orig_stdout) {
        fclose(stdout);
        stdout = s_orig_stdout;
        s_orig_stdout = NULL;
    }
}
```

## Command Migration Plan

All commands across 4 files need `printf()` → `vconsole_printf()` migration:

| File | Commands | Count |
|------|----------|-------|
| `shell/shell.c` | reboot, mem, mount, unmount, ls, cat, cd, pwd, history + REPL prompt | 9 + prompt |
| `shell/cmd_files.c` | write, append, mkdir, rmdir, rm, touch, stat, df, cp, mv, head, tail, grep, wc, hexdump, tree, note, source, find, du, basename, dirname, test | 23 |
| `shell/cmd_system.c` | uname, uptime, date, ps, clear, echo, hostname, env, free, lscpu, shutdown, lsblk, which, lsmod, modinfo, dmesg, sleep, time, crc32 | 19 |
| `shell/cmd_diag.c` | i2cdetect, i2cget, i2cset, devls, watchdog, loglevel, gpio, sync, beep | 9 |
| `services/registry.c` | service list, service info | 2 |
| `services/config.c` | config | 1 |

**Total: ~64 commands**

Migration is mechanical: `printf(` → `vconsole_printf(`, `putchar(c)` → `vconsole_putchar(c)`. The `vconsole` command itself keeps raw `printf()` to avoid recursion in `vconsole tail`.

## Shell Prompt

The REPL prompt in `shell.c` (`shell_read_line`) also goes through vconsole so display modules show the full interactive session:

```c
// Before: printf("[%s] uarch> ", s_cwd);
// After:  vconsole_printf("[%s] uarch> ", s_cwd);
```

Character echo during input (`putchar(c)`, backspace handling) also migrated to `vconsole_putchar()`.

## What This Does NOT Cover

- Display rendering (Phase 3)
- Scroll position / viewport tracking (Phase 3, display-side concern)
- Input multiplexing (keyboard → shell is unchanged)
- Color/ANSI escape sequences (future enhancement)

## Build Impact

Expected: ~0% RAM increase (all buffers in PSRAM), ~1-2 KB flash for event group + line index code.

## Testing

- `vconsole status` — shows buffer usage, capacity, line count
- `vconsole clear` — clears buffer
- `vconsole tail [n]` — shows last N lines from buffer
- Manual verification: run commands, check `vconsole tail` matches serial output
