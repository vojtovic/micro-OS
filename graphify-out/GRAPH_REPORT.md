# Graph Report - .  (2026-04-25)

## Corpus Check
- Corpus is ~2,070 words - fits in a single context window. You may not need a graph.

## Summary
- 50 nodes · 56 edges · 9 communities detected
- Extraction: 80% EXTRACTED · 20% INFERRED · 0% AMBIGUOUS · INFERRED: 11 edges (avg confidence: 0.82)
- Token cost: 0 input · 0 output

## Community Hubs (Navigation)
- [[_COMMUNITY_Foundation & System Services|Foundation & System Services]]
- [[_COMMUNITY_Module Runtime & Packaging|Module Runtime & Packaging]]
- [[_COMMUNITY_Hardware & Display Stack|Hardware & Display Stack]]
- [[_COMMUNITY_Constraints & Concurrency|Constraints & Concurrency]]
- [[_COMMUNITY_Build System (ESP-IDF)|Build System (ESP-IDF)]]
- [[_COMMUNITY_RuFlo Agent Framework|RuFlo Agent Framework]]
- [[_COMMUNITY_Application Entry Point|Application Entry Point]]
- [[_COMMUNITY_Event Sourcing|Event Sourcing]]
- [[_COMMUNITY_Repository Structure|Repository Structure]]

## God Nodes (most connected - your core abstractions)
1. `Target Hardware: ESP32-S3 N16R8` - 6 edges
2. `Phase 0 - Foundation` - 6 edges
3. `Non-Negotiable Constraints` - 5 edges
4. `PSRAM Memory Policy (MALLOC_CAP_SPIRAM)` - 5 edges
5. `Phase 1 - System Services` - 5 edges
6. `Phase 2 - Module Runtime (ELF)` - 5 edges
7. `Phase 3 - Display Stack` - 5 edges
8. `RuFlo V3 Framework` - 4 edges
9. `ESP-IDF CMake Toolchain Include` - 3 edges
10. `Micro-OS Vision: Arch-Linux-Inspired OS for ESP32-S3` - 3 edges

## Surprising Connections (you probably didn't know these)
- `Domain-Driven Design Architecture` --semantically_similar_to--> `HAL-Style APIs (Display, Input, Storage, Net, Timing)`  [INFERRED] [semantically similar]
  CLAUDE.md → roadmap.md
- `micro_arch CMake Project Root` --references--> `Source CMakeLists App Sources Registration`  [INFERRED]
  CMakeLists.txt → src/CMakeLists.txt
- `IDF Component Registration` --references--> `ESP-IDF CMake Toolchain Include`  [INFERRED]
  src/CMakeLists.txt → CMakeLists.txt
- `ESP-IDF CMake Toolchain Include` --implements--> `ESP-IDF v5.x Framework Constraint`  [INFERRED]
  CMakeLists.txt → roadmap.md
- `CLAUDE.md Project Configuration` --conceptually_related_to--> `Micro-OS Vision: Arch-Linux-Inspired OS for ESP32-S3`  [INFERRED]
  CLAUDE.md → roadmap.md

## Hyperedges (group relationships)
- **Phased Development Roadmap (Phase 0 through 4)** — roadmap_phase0_foundation, roadmap_phase1_system_services, roadmap_phase2_elf_runtime, roadmap_phase3_display_stack, roadmap_phase4_networking_packages [EXTRACTED 1.00]
- **SPI Bus Shared Peripherals (SD, OLED, E-ink)** — roadmap_spi_sharing, roadmap_microsd_storage, roadmap_spi_oled, roadmap_waveshare_eink, roadmap_bus_manager [EXTRACTED 0.90]
- **Dynamic Module Loading System (ELF, Symbols, Lifecycle, Contract)** — roadmap_elf_loader, roadmap_kernel_symbol_table, roadmap_module_lifecycle_api, roadmap_module_contract, roadmap_pacman_cli [INFERRED 0.85]

## Communities

### Community 0 - "Foundation & System Services"
Cohesion: 0.2
Nodes (11): Domain-Driven Design Architecture, Bus Manager (I2C/SPI Ownership), esp_console Basic Shell, HAL-Style APIs (Display, Input, Storage, Net, Timing), Phase 0 - Foundation, Phase 1 - System Services, PSRAM Memory Policy (MALLOC_CAP_SPIRAM), Rationale: Respect Phase Order, No Skipping Foundation (+3 more)

### Community 1 - "Module Runtime & Packaging"
Cohesion: 0.25
Nodes (9): ELF Loader (Load from SD to RAM/PSRAM), Kernel Symbol Export Table and Resolver, Optional Lua Runtime for Scripting, Module Contract for Driver/App Authors, Module Lifecycle API (init/start/stop), Pacman-Style Package CLI, Phase 2 - Module Runtime (ELF), Phase 4 - Networking and Packages (+1 more)

### Community 2 - "Hardware & Display Stack"
Cohesion: 0.29
Nodes (8): CardKB I2C Keyboard Input, Display Multiplexer (Mirror/Route to Both Displays), ESP32-S3 MCU (16MB Flash, 8MB PSRAM), MicroSD Storage (SPI/SDMMC), Phase 3 - Display Stack, SPI OLED Display, Target Hardware: ESP32-S3 N16R8, Waveshare E-ink Display

### Community 3 - "Constraints & Concurrency"
Cohesion: 0.29
Nodes (7): CLAUDE.md Project Configuration, Non-Negotiable Constraints, FreeRTOS Concurrency with Core Affinity, Micro-OS Vision: Arch-Linux-Inspired OS for ESP32-S3, Rationale: Core Affinity Only When Justified by Profiling, Rationale: SPI Chip-Select Discipline and Bus Locking, SPI Bus Sharing Discipline

### Community 4 - "Build System (ESP-IDF)"
Cohesion: 0.4
Nodes (6): ESP-IDF CMake Toolchain Include, micro_arch CMake Project Root, ESP-IDF v5.x Framework Constraint, Rationale: ESP-IDF v5.x Not Arduino, Source CMakeLists App Sources Registration, IDF Component Registration

### Community 5 - "RuFlo Agent Framework"
Cohesion: 0.4
Nodes (5): 3-Tier Model Routing (ADR-026), Hierarchical-Mesh Topology, Memory and Vector Search (ONNX/DiskANN), RuFlo V3 Framework, Swarm Orchestration Configuration

### Community 6 - "Application Entry Point"
Cohesion: 1.0
Nodes (0): 

### Community 7 - "Event Sourcing"
Cohesion: 1.0
Nodes (1): Event Sourcing for State Changes

### Community 8 - "Repository Structure"
Cohesion: 1.0
Nodes (1): Suggested Repository Layout (bin/drivers/etc/home/tmp)

## Knowledge Gaps
- **18 isolated node(s):** `CLAUDE.md Project Configuration`, `Domain-Driven Design Architecture`, `Event Sourcing for State Changes`, `3-Tier Model Routing (ADR-026)`, `Hierarchical-Mesh Topology` (+13 more)
  These have ≤1 connection - possible missing edges or undocumented components.
- **Thin community `Application Entry Point`** (2 nodes): `app_main()`, `main.c`
  Too small to be a meaningful cluster - may be noise or needs more connections extracted.
- **Thin community `Event Sourcing`** (1 nodes): `Event Sourcing for State Changes`
  Too small to be a meaningful cluster - may be noise or needs more connections extracted.
- **Thin community `Repository Structure`** (1 nodes): `Suggested Repository Layout (bin/drivers/etc/home/tmp)`
  Too small to be a meaningful cluster - may be noise or needs more connections extracted.

## Suggested Questions
_Questions this graph is uniquely positioned to answer:_

- **Why does `Non-Negotiable Constraints` connect `Constraints & Concurrency` to `Foundation & System Services`, `Build System (ESP-IDF)`?**
  _High betweenness centrality (0.286) - this node is a cross-community bridge._
- **Why does `PSRAM Memory Policy (MALLOC_CAP_SPIRAM)` connect `Foundation & System Services` to `Module Runtime & Packaging`, `Constraints & Concurrency`?**
  _High betweenness centrality (0.234) - this node is a cross-community bridge._
- **Why does `Phase 0 - Foundation` connect `Foundation & System Services` to `Hardware & Display Stack`?**
  _High betweenness centrality (0.157) - this node is a cross-community bridge._
- **What connects `CLAUDE.md Project Configuration`, `Domain-Driven Design Architecture`, `Event Sourcing for State Changes` to the rest of the system?**
  _18 weakly-connected nodes found - possible documentation gaps or missing edges._