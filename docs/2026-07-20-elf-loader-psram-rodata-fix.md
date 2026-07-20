# ELF Loader PSRAM `.rodata` Pointer Fix

**Date:** 2026-07-20
**Status:** Approved (root-cause fix chosen over SRAM-loading workaround)
**Component:** `espressif/elf_loader` v1.3.1 → vendored fork in `components/elf_loader`

## Problem

With `CONFIG_ELF_LOADER_LOAD_PSRAM=y`, dereferencing certain module `.rodata`
string pointers from kernel code crashes with `LoadStoreError` at
`0x42xxxxxx` (instruction-bus-only address range). Observed with the display
name string passed by `oled.elf` to `display_mux_register()` and the service
name passed to `registry_add()`.

The interim workaround (`CONFIG_ELF_LOADER_LOAD_PSRAM=n` + manual byte-copies
in `display_mux.c`/`registry.c`) stopped the crash but:

1. Limits all modules to ~150 KB of internal SRAM — the scarcest memory.
2. Contradicts the architecture rule "module payloads go to PSRAM".
3. **Does not actually fix the bug** — see Root Cause: in SRAM mode the
   poisoned pointer still resolves into the wrong memory block; it reads
   copied-by-accident + out-of-bounds bytes instead of crashing.

## Root cause (verified on `oled.elf`)

Section layout produced by the module link (`-shared`, default linker script):

| Section   | VMA     | Size    | True end |
|-----------|---------|---------|----------|
| `.text`   | `0x3F4` | `0x46F` | `0x863`  |
| `.rodata` | `0x863` | `0x228` | `0xA8B`  |

`esp_elf_load_section()` stores the `.text` size **4-byte aligned**:

```c
elf->sec[ELF_SEC_TEXT].size = ELF_ALIGN(shdr[i].size, 4);   // 0x46F → 0x470
```

This creates a phantom text range `[0x3F4, 0x864)` that overlaps the first
1–3 bytes of `.rodata`. The `"oled"` string literal sits at exactly `0x863`.

During relocation of the `R_XTENSA_RELATIVE` literal (word `0x863` at
`.text+0x42C`):

1. `esp_elf_map_sym(elf, 0x863)` scans sections in index order;
   `ELF_SEC_TEXT` (index 0) matches first via the phantom range →
   the pointer resolves to `ptext + 0x46F` (**wrong block** — the text copy,
   not the rodata copy in `pdata`).
2. `elf_remap_text()` sees the result inside the relocated text range and
   adds `OFFSET_TEXT_VALUE` (= `SOC_IROM_LOW − SOC_DROM_LOW` = `+0x0600_0000`)
   → final pointer `0x42xxxxxx`, readable only by the instruction bus.
3. Kernel data-bus read (`printf %s`, `strcmp`, …) → `LoadStoreError`.

Only objects whose start address falls inside `[text_true_end,
text_aligned_end)` are poisoned — which is why the failures looked random
(one string crashed, its neighbours worked).

In SRAM mode (`LOAD_PSRAM=n`, no `CACHE_OFFSET`) step 2 is skipped, so the
pointer stays at `ptext + 0x46F`: a readable address containing `'o'` (the
over-aligned `memcpy` copies `0x470` bytes, dragging in the first `.rodata`
byte) followed by out-of-bounds heap bytes. Silent data corruption instead
of a crash.

## Fix

Store the **true** `.text` size for address mapping; align only the
allocation. In `components/elf_loader/src/esp_elf.c`:

```c
/* was: elf->sec[ELF_SEC_TEXT].size = ELF_ALIGN(shdr[i].size, 4); */
elf->sec[ELF_SEC_TEXT].size = shdr[i].size;

/* was: esp_elf_malloc(elf->sec[ELF_SEC_TEXT].size, true) */
elf->ptext = esp_elf_malloc(ELF_ALIGN(elf->sec[ELF_SEC_TEXT].size, 4), true);
```

With the true range `[0x3F4, 0x863)`, address `0x863` now falls through to
`ELF_SEC_RODATA` and maps into `pdata` — a data-bus PSRAM address readable
by both kernel and module. `elf_remap_text()` no longer touches it.

### Why vendor the component

- The IDF component manager hash-checks `managed_components/` and rejects
  in-place edits.
- Phase 5.2b already planned a loader fork for IRAM placement; this fork is
  the natural home for that later work.
- Fork = full copy of `espressif__elf_loader` v1.3.1 into
  `components/elf_loader` + `PATCHES.md` documenting each divergence, and
  the `espressif/elf_loader` dependency removed from `src/idf_component.yml`.

## Also in scope

- Re-enable `CONFIG_ELF_LOADER_LOAD_PSRAM=y` (auto-selects `CACHE_OFFSET`
  on S3) in `sdkconfig.defaults` + board sdkconfig.
- Remove the now-obsolete kernel-side byte-copy workarounds in
  `display_mux.c` and `registry.c` (plain `strncpy` is safe once pointers
  are data-bus addresses). Their old comments blamed ROM `strncpy` access
  width — the actual cause was the poisoned pointer, so the comments were
  misleading and must not survive.

## Out of scope (recorded, not fixed)

- `esp_elf_load_section()` only recognizes the five exact section names
  (`.text`, `.data`, `.rodata`, `.data.rel.ro`, `.bss`); `.got.loc`/`.got`
  are silently dropped. Harmless today (no relocations target them), but a
  future module toolchain change could break this silently. Candidate for a
  load-time warning in the fork.
- Module vtables (`display_driver_ops_t`) passed to the kernel live in
  module memory; safe under both modes, but any future loader change must
  keep them data-bus addressable.

## Test plan (on hardware — build alone cannot validate this)

1. `pio run -t upload -t monitor`
2. `modload oled` → expect clean `Registered display 'oled' (…)` log — the
   exact line that used to crash.
3. `display list` → name must read `oled`, not garbage (catches the silent
   SRAM-mode corruption).
4. `modload hello && modunload hello` ×10, `mem` before/after → no leak.
5. `lsmod -v` → module memory now reported in PSRAM again.
6. `modload eink`, `modload badabi` (ABI negative test still rejects).

## Rollback

Single revert commit; the SRAM workaround config
(`CONFIG_ELF_LOADER_LOAD_PSRAM=n`) remains a one-line fallback.
