# Phase 0 Commands to Implement (Linux-Style)

This file defines the next shell commands to add for **Phase 0 - Foundation**.  
Goal: improve board bring-up visibility, storage safety, and deterministic boot behavior before module runtime.

## 1) `i2cdetect`

**Purpose**  
Scan an I2C bus and show active device addresses (Linux-like bring-up command).

**Proposed syntax**
- `i2cdetect` (default bus)
- `i2cdetect -b <bus_id>`

**Expected behavior**
- Probes all valid 7-bit addresses on the selected bus.
- Prints a compact scan table (hex rows/cols) similar to Linux output.
- Marks found addresses and skips reserved ranges safely.
- Returns non-zero on bus error.

**Why this matters in Phase 0**
- Fast validation that CardKB and other I2C peripherals are physically reachable.
- Reduces guesswork when wiring/pin config is wrong.

---

## 2) `devls`

**Purpose**  
List detected/initialized platform devices in one place (`/dev`-style visibility for embedded target).

**Proposed syntax**
- `devls`
- `devls -v` (verbose: driver/state/capabilities)

**Expected behavior**
- Shows core devices and state: SD SPI bus, I2C bus, internal FS, SD mount state, optional peripherals.
- Includes status fields: `ready`, `mounted`, `error`, `not-present`.
- Verbose mode adds key metadata (bus id, pins if available, capability flags).

**Why this matters in Phase 0**
- Foundation exit criteria requires bus/storage diagnostics in CLI.
- Gives one command to quickly confirm system hardware/software state.

---

## 3) `watchdog`

**Purpose**  
Expose watchdog state and manual feed for diagnostics during long operations.

**Proposed syntax**
- `watchdog status`
- `watchdog feed`

**Expected behavior**
- `status` prints whether task/system WDT is enabled, timeout config, and monitored tasks (if available).
- `feed` triggers a safe manual feed path (for controlled diagnostics only).
- Clear errors when WDT is not configured.

**Why this matters in Phase 0**
- Bring-up often includes long blocking tests; watchdog visibility prevents silent resets.
- Supports roadmap requirement for watchdog-safe operations.

---

## 4) `loglevel`

**Purpose**  
Change runtime log verbosity without reflashing firmware.

**Proposed syntax**
- `loglevel` (show current)
- `loglevel <error|warn|info|debug|verbose>`
- `loglevel <tag> <error|warn|info|debug|verbose>`

**Expected behavior**
- Global mode changes default log level.
- Tag mode changes one module tag (for focused debugging).
- Invalid tag/level returns usage + non-zero status.

**Why this matters in Phase 0**
- Speeds up hardware and bus debugging while keeping normal boot output readable.
- Helps isolate noisy modules during stability work.

---

## 5) `mount -a`

**Purpose**  
Mount all configured filesystems deterministically from config (`/etc/fstab` equivalent in roadmap spirit).

**Proposed syntax**
- `mount -a`

**Expected behavior**
- Reads configured mount entries from `/sdcard/etc/fstab` (or agreed config path).
- Attempts mounts in order, prints per-entry result.
- Continues processing after individual failures and reports summary.
- Non-zero return if any required mount fails.

**Why this matters in Phase 0**
- Creates reproducible boot/storage behavior before higher-level services.
- Prepares clean path toward Phase 1 boot sequence requirements.

---

## 6) `sync`

**Purpose**  
Flush buffered filesystem writes to persistent storage.

**Proposed syntax**
- `sync`

**Expected behavior**
- Forces pending writes for SD/internal FS to flush.
- Prints success/failure per mounted filesystem when possible.
- Non-zero on flush errors.

**Why this matters in Phase 0**
- Improves SD reliability during power-loss or reboot testing.
- Important companion for new write-heavy file commands.

---

## Implementation notes (Phase 0 discipline)

- Keep commands deterministic, low-risk, and diagnostics-focused.
- All errors should be explicit (`ESP_LOGE` + CLI message), no silent failure paths.
- Prefer existing abstractions (`bus_manager`, `storage`, `internal_fs`) over direct low-level duplication.
- Preserve Linux-style UX where practical, but prioritize safety on embedded hardware.
