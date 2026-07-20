# Arch-ESP32-S3 Roadmap: Micro-OS Project

## Vision
A minimalist, Arch-Linux-inspired OS for ESP32-S3 (N16R8).  
The system boots into a CLI and supports installing drivers/apps as modules from SD card or over Wi-Fi.

## Target Hardware
- **MCU:** ESP32-S3 (16 MB Flash, 8 MB Octal PSRAM)
- **Primary display:** Waveshare E-ink (logs/static output)
- **Secondary display:** SPI OLED (active CLI feedback)
- **Input:** CardKB (I2C) or matrix keyboard
- **Storage:** MicroSD (SPI or SDMMC)

## Non-Negotiable Constraints
1. **Framework/toolchain:** ESP-IDF v5.x (not Arduino).
2. **Memory:** large buffers and module payloads should prefer PSRAM (`MALLOC_CAP_SPIRAM`).
3. **SPI sharing:** SD and displays may share SPI; enforce chip-select discipline and bus locking.
4. **Concurrency:** FreeRTOS tasks with explicit core affinity only when justified by profiling.

## Phase 0 - Foundation (Must Be Stable First)
**Goal:** make core firmware predictable before dynamic loading.

- [x] Board bring-up with logging, panic output, and watchdog configuration.
- [x] PSRAM enabled and validated (`heap_caps` usage + memory stats command).
- [x] Storage baseline: SD card mount/unmount and internal filesystem.
- [x] Basic shell (`esp_console`) with deterministic command registration.
- [x] Bus manager for I2C/SPI ownership and pin-conflict prevention.

**Exit criteria**
- Boots reliably to shell.
- Can mount SD and run basic file commands.
- Memory/bus diagnostics are available from CLI.

## Phase 1 - System Services
**Goal:** define platform interfaces before modules.

- [x] HAL-style APIs for display, input, storage, net, and timing.
- [x] Service registry (discoverable APIs, versions, and capabilities).
- [x] Boot sequence support:
  - Parse `/etc/fstab` equivalent.
  - Execute `/etc/init.sh` equivalent command list.
- [x] Virtual console buffer in PSRAM as single source for display output.

**Exit criteria**
- All device access goes through stable service interfaces.
- Boot script can start built-in services reproducibly.

## Phase 2 - Module Runtime (ELF)
**Goal:** load code at runtime without reflashing.

- [x] ELF loader integration (load from SD to RAM/PSRAM).
- [x] Kernel symbol export table and resolver.
- [x] Standard module lifecycle API (`module_init`, `module_start`, `module_stop`).
- [x] ABI/version checks at load time.
- [x] Memory boundaries and relocation safety checks.

**Exit criteria**
- A sample module loads, runs, and unloads repeatedly without reboot.
- Incompatible ABI modules are rejected with clear error messages.

## Phase 3 - Display Stack
**Goal:** multi-screen output with predictable behavior.

- [x] OLED output driver/service for low-latency terminal feedback.
- [x] E-ink service with partial refresh and configurable ghosting policy.
- [x] Display multiplexer: mirror/route virtual console to both displays.

**Exit criteria**
- Shell output appears on both displays by policy.
- E-ink ghosting/full-refresh behavior is measurable and tunable.

## Phase 4 - Networking and Packages
**Goal:** Arch-like package workflow.

- [x] Wi-Fi networking service (kernel-compiled STA mode).
- [x] Package CLI:
  - `pacman -Sy`: sync repository index.
  - `pacman -S <pkg>`: download/install module or script.
- [x] Package metadata format (MPKG: magic/version/sizes/SHA-256 + repo.idx pipe-delimited index).
- [ ] Optional Lua runtime for scripting.

**Exit criteria**
- Device can fetch repository metadata and install at least one package end-to-end.

## Phase 5 - High-Performance Module Runtime
**Goal:** Make loaded ELF modules run at near-native speed for graphics, audio, and DMA-heavy workloads.
See [docs/2026-05-10-high-perf-modules-plan.md](docs/2026-05-10-high-perf-modules-plan.md) for the detailed design.

- [x] **5.1 Memory plumbing** - module IRAM pool (soft budget), cache config tuning, `dma_buf_alloc` helper, extended `mem` diagnostics.
- [~] **5.2 ELF loader IRAM placement** - PARTIAL (5.2a done): scan `.iram*`/`.dram_data` sections in loaded ELFs, account per-module, `lsmod -v` shows IRAM/DRAM/PSRAM bytes + IRAM_DEGRADED flag, warn on load that IRAM_ATTR code is in PSRAM. `espressif/elf_loader` is now vendored at `components/elf_loader` (fork of v1.3.1, see `components/elf_loader/PATCHES.md`) — PATCH-001 fixed an unrelated `.rodata` pointer bug (2026-07-20, see [docs/2026-07-20-elf-loader-psram-rodata-fix.md](docs/2026-07-20-elf-loader-psram-rodata-fix.md)) that had forced modules onto internal SRAM; PSRAM loading is restored. Real IRAM placement (5.2b, PATCH-002) still requires patching `esp_elf_load_section` + the Xtensa relocation walker for `.iram*` sections (deferred).
- [~] **5.3 Kernel ABI: graphics + DMA** - PARTIAL (5.3a done): 13 new exports (gfx_fb_alloc/free, gfx_set/get_pixel, gfx_fill/copy_rect, gfx_blit_masked, cache_flush/invalidate_range, dma_buf_alloc/sync/free). Deferred to 5.3b: gfx_draw_text + font system, display_blit_async (DMA SPI integration), generic dma_submit/_sync, port oled/eink to new ABI.
- [ ] **5.4 Kernel ABI: audio** - 8 new exports (`audio_*`); I2S DMA + ESP-DSP wrappers; sample tone-gen module.
- [ ] **5.5 Static-link escape hatch** - `MODULES_STATIC` build flag; constructor-section auto-registration; same source compiles loaded or static.
- [~] **5.6 Manifest extensions** - PARTIAL: `[memory] iram_pages/dram_pages`, `[requires] kernel_deps`, `[module] static_link` parsed in `manifest.c`; `kernel_deps` prefixes validated against symtab via new `symtab_has_prefix()`. Deferred: `pkg install` IRAM-warning that reads manifest before downloading.

**Exit criteria**
- 60+ fps on 128x64 OLED bouncing-ball example, CPU < 5%.
- 44.1 kHz stereo audio playback sustained 5+ minutes without underrun.
- `lsmod -v` reports per-module IRAM/DRAM/PSRAM bytes accurately.
- `pio run -e static-display` produces a single firmware where the OLED driver is built-in (zero load cost).

## Security and Reliability Requirements
- [x] Verify package/module integrity (hash; signature preferred).
- [x] Fail-safe module loading (no partial install left active after failure).
- [x] Watchdog-safe long operations (download, parse, load).
- [x] Crash policy: isolate module faults where possible, keep shell recoverable.

## Module Contract (for Driver/App Authors)
Every loadable module should define:
- Metadata: `name`, `version`, `api_version`, `author`, `description`
- Resource declaration: heap requirements, task count/stack sizes, peripheral needs
- Lifecycle entry points: init/start/stop
- Optional command registration hook for CLI integration

## Suggested Repository Layout
```text
/
├── bin/         # user applications (.elf, .lua)
├── drivers/     # hardware/system drivers (.elf)
├── etc/         # configs (init.conf, fstab, network.conf)
├── home/        # user data
└── tmp/         # temporary working data
```

## AI Agent Execution Notes (Claude/Copilot/others)
- Respect phase order; do not skip foundation work to implement ELF loading early.
- Prefer small, reviewable PRs per phase milestone.
- Keep public service APIs and module ABI versioned from the start.
- Any change touching memory, SPI, or tasking must include stress-test evidence.
- Treat this file as source of truth for scope and milestone gates.
