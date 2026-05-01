# micro-OS (micro_arch)

Minimal Arch-Linux-inspired micro-OS for **ESP32-S3 (N16R8)** built with **PlatformIO + ESP-IDF**.

It boots into a CLI shell, mounts internal and SD filesystems, initializes core buses/services, and is structured for loadable modules (drivers/apps) in later phases.

## Current status

- Foundation and system-service phases are in place (boot flow, shell, storage, service registry, virtual console).
- Module runtime and package/network phases are planned in `roadmap.md`.

## Hardware target

- ESP32-S3 (16 MB Flash, 8 MB PSRAM)
- MicroSD storage
- CardKB (I2C) or compatible input
- OLED + E-ink support via module-oriented architecture

## Build and run

```bash
# Build firmware
pio run

# Clean and rebuild
pio run -t fullclean
pio run

# Flash and monitor
pio run -t upload -t monitor
```

## Tests

```bash
# Run all tests
pio test -e freenove_esp32_s3_wroom

# Run one test
pio test -e freenove_esp32_s3_wroom -f <test_name>
```

## Project layout

```text
src/
├── main.c               # boot orchestration
├── shell/               # CLI command layer
├── hal/                 # storage and hardware abstraction
├── bus/                 # SPI/I2C ownership and setup
├── services/            # registry, init flow, virtual console, config
└── loader/              # ELF/module runtime pieces
```

## Boot sequence (high level)

1. Initialize kernel log ring and virtual console.
2. Mount internal LittleFS at `/sys` and ensure default fstab.
3. Initialize buses (SD SPI + I2C).
4. Mount filesystems and prepare SD hierarchy.
5. Initialize service registry and register HAL/service ops.
6. Initialize shell, optionally run boot script, then enter REPL.

## Notes

- User-facing shell output goes through `vconsole_*` APIs.
- `/sys/etc/fstab` and `/sdcard/etc/init.conf` are boot contracts.
- Keep roadmap phase discipline (`roadmap.md`) when adding features.
