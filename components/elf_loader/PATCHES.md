# micro_arch fork of espressif/elf_loader

Vendored from `espressif/elf_loader` **v1.3.1** (was `managed_components/
espressif__elf_loader`). The upstream dependency was removed from
`src/idf_component.yml`; this directory is now project-owned.

Every local change is tagged `micro_arch PATCH-NNN` in a code comment so
future upstream rebases can locate and re-apply them.

## PATCH-000 — decouple from `cmake_utilities`

**File:** `CMakeLists.txt`

Upstream used `include(package_manager)` + `cu_pkg_define_version` (from the
`espressif/cmake_utilities` managed component) to derive
`ELF_LOADER_VER_*` compile definitions from `idf_component.yml`. Both files
are gone in the vendored fork, so the version macros are defined explicitly
(1.3.1 — the upstream release this fork is based on). Bump manually on
rebase.

## PATCH-001 — phantom `.text` range poisons early `.rodata` pointers

**File:** `src/esp_elf.c` (`esp_elf_load_section`)
**Design doc:** `docs/2026-07-20-elf-loader-psram-rodata-fix.md`

Upstream stored `sec[ELF_SEC_TEXT].size = ELF_ALIGN(shdr.size, 4)`. The
1–3 padding bytes made the text range overlap the start of `.rodata`, so
`esp_elf_map_sym()` resolved pointers to the first `.rodata` object into
`ptext`, and (with `CONFIG_ELF_LOADER_LOAD_PSRAM=y`) `elf_remap_text()`
then offset them into the ibus-only `0x42xxxxxx` alias — `LoadStoreError`
on any kernel data read. With `LOAD_PSRAM=n` the same bug silently read
garbage instead of crashing.

Fix: store the true section size; apply `ELF_ALIGN` only to the `ptext`
allocation size.

## Planned future patches

- **PATCH-002 (Phase 5.2b, deferred):** honor `.iram*` sections by
  allocating from `MALLOC_CAP_INTERNAL | MALLOC_CAP_EXEC` — only needed
  once a module outgrows what PSRAM performance allows.
- Load-time warning when an allocated section other than the five
  recognized names (`.text`, `.data`, `.rodata`, `.data.rel.ro`, `.bss`)
  is dropped, so toolchain changes can't silently lose data.
