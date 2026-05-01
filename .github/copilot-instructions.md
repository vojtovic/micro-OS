# Copilot Instructions for `micro_arch`

## Build, test, and lint commands

This project uses **PlatformIO + ESP-IDF** with environment `freenove_esp32_s3_wroom` (`platformio.ini`).

```bash
# Build firmware
pio run

# Clean build artifacts, then rebuild
pio run -t fullclean
pio run

# Run all tests (PlatformIO test runner)
pio test -e freenove_esp32_s3_wroom

# Run a single test by name
pio test -e freenove_esp32_s3_wroom -f <test_name>

# Flash and serial monitor (common dev loop)
pio run -t upload -t monitor
```

There is currently **no dedicated lint target** configured in this repository.

## High-level architecture

`src/main.c` is the boot orchestrator. Boot order is important:

1. Initialize kernel logging ring (`klog_init`) and virtual console ring in PSRAM (`vconsole_init`).
2. Mount internal LittleFS at `/sys` (`internal_fs_mount`) and create default `/sys/etc/fstab` if missing.
3. Initialize buses (`bus_init`): SD SPI (SPI3) + I2C.
4. Mount filesystems from fstab (`init_mount_fstab`) and prepare SD hierarchy (`init_filesystem`).
5. Initialize service registry (`registry_init`) and register HAL-style vtables (`bus`, `storage`, `internal_fs`, `vconsole`, `klog`).
6. Register shell commands (`shell_init`), optionally execute `/sdcard/etc/init.conf`, then enter REPL (`shell_start`).

Core runtime structure:

- **Shell layer**: `src/shell/*.c` registers Linux-style CLI commands via `esp_console`.
- **HAL/service layer**: `src/hal/*.c` + `src/services/registry.c` expose stable ops tables for built-ins and future modules.
- **Storage model**: `/sys` (internal LittleFS for system config) + `/sdcard` (user/module storage).
- **Logging/output split**:
  - `ESP_LOG*` output is captured by **klog** ring buffer (`dmesg` path).
  - User-facing command output should go through **vconsole** (display-facing ring + UART tee).
- **Roadmap discipline** (`roadmap.md`): keep kernel lean; display/network/app logic is intended as loadable modules in later phases.

## Key conventions in this codebase

- **Use `vconsole_printf`/`vconsole_write` for shell-visible output**, not plain `printf`, so output is available to display modules and `vconsole tail`.
- **Keep boot-essential pins only in `src/config/pin_config.h`** (SD SPI, I2C, buzzer). Display/peripheral pins belong in `/sdcard/etc/hardware.conf` and are module-configurable.
- **Resolve filesystem arguments through `shell_resolve_path`** so commands consistently honor CWD and normalized absolute paths.
- **Prefer service/HAL boundaries over direct cross-module calls**: add capabilities as ops tables registered in `registry`, not ad-hoc globals.
- **Treat `/sys/etc/fstab` and `/sdcard/etc/init.conf` as boot contracts**: filesystem mounts and startup behavior should remain deterministic and scriptable through these files.
- **Stay aligned with phase gates in `roadmap.md`**: avoid introducing later-phase module runtime/network features into early foundation/service work.
