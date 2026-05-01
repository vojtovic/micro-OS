# Claude Code Configuration - RuFlo V3

## Behavioral Rules (Always Enforced)

- Do what has been asked; nothing more, nothing less
- NEVER create files unless they're absolutely necessary for achieving your goal
- ALWAYS prefer editing an existing file to creating a new one
- NEVER proactively create documentation files (*.md) or README files unless explicitly requested
- NEVER save working files, text/mds, or tests to the root folder
- Never continuously check status after spawning a swarm — wait for results
- ALWAYS read a file before editing it
- NEVER commit secrets, credentials, or .env files

## File Organization

- NEVER save to root folder — use the directories below
- Use `/src` for source code files
- Use `/tests` for test files
- Use `/docs` for documentation and markdown files
- Use `/config` for configuration files
- Use `/scripts` for utility scripts
- Use `/examples` for example code

## Project Architecture

- Follow Domain-Driven Design with bounded contexts
- Keep files under 500 lines
- Use typed interfaces for all public APIs
- Prefer TDD London School (mock-first) for new code
- Use event sourcing for state changes
- Ensure input validation at system boundaries

### Project Config

- **Topology**: hierarchical-mesh
- **Max Agents**: 15
- **Memory**: hybrid
- **HNSW**: Enabled
- **Neural**: Enabled

## Build & Test

```bash
# Build
npm run build

# Test
npm test

# Lint
npm run lint
```

- ALWAYS run tests after making code changes
- ALWAYS verify build succeeds before committing

## Security Rules

- NEVER hardcode API keys, secrets, or credentials in source files
- NEVER commit .env files or any file containing secrets
- Always validate user input at system boundaries
- Always sanitize file paths to prevent directory traversal
- Run `npx @claude-flow/cli@latest security scan` after security-related changes

## Concurrency: 1 MESSAGE = ALL RELATED OPERATIONS

- All operations MUST be concurrent/parallel in a single message
- Use Claude Code's Agent tool for spawning agents, not just MCP
- ALWAYS spawn ALL agents in ONE message with full instructions via Agent tool
- ALWAYS batch ALL file reads/writes/edits in ONE message
- ALWAYS batch ALL Bash commands in ONE message

## Swarm Orchestration

- MUST initialize the swarm using CLI tools when starting complex tasks
- MUST spawn concurrent agents using Claude Code's Agent tool
- Never use CLI tools alone for execution — Agent tool agents do the actual work
- MUST call CLI tools AND Agent tool in ONE message for complex work

### 3-Tier Model Routing (ADR-026)

| Tier | Handler | Latency | Cost | Use Cases |
|------|---------|---------|------|-----------|
| **1** | Agent Booster (WASM) | <1ms | $0 | Simple transforms (var→const, add types) — Skip LLM |
| **2** | Haiku | ~500ms | $0.0002 | Simple tasks, low complexity (<30%) |
| **3** | Sonnet/Opus | 2-5s | $0.003-0.015 | Complex reasoning, architecture, security (>30%) |

- For Tier 1 simple transforms, use Edit tool directly — no LLM agent needed

## Swarm Configuration & Anti-Drift

- ALWAYS use hierarchical topology for coding swarms
- Keep maxAgents at 6-8 for tight coordination
- Use specialized strategy for clear role boundaries
- Use `raft` consensus for hive-mind (leader maintains authoritative state)
- Run frequent checkpoints via `post-task` hooks
- Keep shared memory namespace for all agents

```bash
npx @claude-flow/cli@latest swarm init --topology hierarchical --max-agents 8 --strategy specialized
```

## Swarm Execution Rules

- ALWAYS use `run_in_background: true` for all Agent tool calls
- ALWAYS put ALL Agent calls in ONE message for parallel execution
- After spawning, STOP — do NOT add more tool calls or check status
- Never poll agent status repeatedly — trust agents to return
- When agent results arrive, review ALL results before proceeding

## V3 CLI Commands

### Core Commands

| Command | Subcommands | Description |
|---------|-------------|-------------|
| `init` | 4 | Project initialization |
| `agent` | 8 | Agent lifecycle management |
| `swarm` | 6 | Multi-agent swarm coordination |
| `memory` | 11 | AgentDB memory with HNSW search |
| `task` | 6 | Task creation and lifecycle |
| `session` | 7 | Session state management |
| `hooks` | 17 | Self-learning hooks + 12 workers |
| `hive-mind` | 6 | Byzantine fault-tolerant consensus |

### Quick CLI Examples

```bash
npx @claude-flow/cli@latest init --wizard
npx @claude-flow/cli@latest agent spawn -t coder --name my-coder
npx @claude-flow/cli@latest swarm init --v3-mode
npx @claude-flow/cli@latest memory search --query "authentication patterns"
npx @claude-flow/cli@latest doctor --fix
```

## Available Agents (16 Roles + Custom)

### Core Development
`coder`, `reviewer`, `tester`, `planner`, `researcher`

### Specialized
`security-architect`, `security-auditor`, `memory-specialist`, `performance-engineer`

### Coordination
`hierarchical-coordinator`, `mesh-coordinator`, `adaptive-coordinator`

### GitHub & Repository
`pr-manager`, `code-review-swarm`, `issue-tracker`, `release-manager`

Any string can be used as a custom agent type — these are the typed roles with specialized behavior.

## Memory & Vector Search

### MCP Tools (use via ToolSearch to discover)

| Tool | Description |
|------|-------------|
| `memory_store` | Store value with ONNX 384-dim vector embedding |
| `memory_search` | Semantic vector search by query |
| `memory_retrieve` | Get entry by key |
| `memory_list` | List entries in namespace |
| `memory_delete` | Delete entry |
| `memory_import_claude` | Import Claude Code memories into AgentDB (allProjects=true for all) |
| `memory_search_unified` | Search across ALL namespaces (Claude + AgentDB + patterns) |
| `memory_bridge_status` | Show bridge health, vectors, SONA, intelligence |

### CLI Commands

```bash
# Store with vector embedding
npx @claude-flow/cli@latest memory store --key "pattern-auth" --value "JWT with refresh" --namespace patterns

# Semantic search
npx @claude-flow/cli@latest memory search --query "authentication patterns"

# Import all Claude Code memories into AgentDB
node .claude/helpers/auto-memory-hook.mjs import-all
```

### Claude Code ↔ AgentDB Bridge

Claude Code auto-memory files (`~/.claude/projects/*/memory/*.md`) are automatically imported into AgentDB with ONNX vector embeddings on session start. Use `memory_search_unified` to search across both stores.

## Key MCP Tools (314 available — use ToolSearch to discover)

### Most Used Tools

| Category | Tools | What They Do |
|----------|-------|-------------|
| **Memory** | `memory_store`, `memory_search`, `memory_search_unified` | Store/search with ONNX vector embeddings |
| **Claude Bridge** | `memory_import_claude`, `memory_bridge_status` | Import Claude memories into AgentDB |
| **Swarm** | `swarm_init`, `swarm_status`, `swarm_health` | Multi-agent coordination |
| **Agents** | `agent_spawn`, `agent_list`, `agent_status` | Agent lifecycle |
| **Hive-Mind** | `hive-mind_init`, `hive-mind_spawn`, `hive-mind_consensus` | Byzantine/Raft consensus |
| **Hooks** | `hooks_route`, `hooks_session-start`, `hooks_post-task` | Task routing + learning |
| **Workers** | `hooks_worker-list`, `hooks_worker-dispatch` | 12 background workers |
| **Security** | `aidefence_scan`, `aidefence_is_safe` | Prompt injection detection |
| **Intelligence** | `hooks_intelligence`, `neural_status` | Pattern learning + SONA |

### Swarm Capabilities

- **Topologies**: hierarchical (anti-drift), mesh, ring, star, adaptive
- **Consensus**: Raft (leader-based), Byzantine (PBFT), Gossip (eventual)
- **Hive-Mind**: Queen-led coordination with spawn, broadcast, consensus voting, shared memory
- **12 Background Workers**: audit, optimize, testgaps, map, deepdive, document, refactor, benchmark, ultralearn, consolidate, predict, preload

### Memory Capabilities

- **ONNX Embeddings**: all-MiniLM-L6-v2, 384 dimensions — real neural vectors
- **DiskANN**: SSD-friendly vector search (8,000x faster insert than HNSW, perfect recall at 1K)
- **sql.js**: Cross-platform SQLite (WASM, no native compilation)
- **Claude Code Bridge**: Auto-imports MEMORY.md files into AgentDB on session start
- **Unified Search**: `memory_search_unified` searches Claude memories + AgentDB + patterns
- **SONA Learning**: Trajectory recording → pattern extraction → file persistence

### How to Discover Tools

Use ToolSearch to find specific tools:
```
ToolSearch("memory search")     → memory_store, memory_search, memory_search_unified
ToolSearch("swarm")             → swarm_init, swarm_status, swarm_health, swarm_shutdown
ToolSearch("hive consensus")    → hive-mind_consensus, hive-mind_status
ToolSearch("+aidefence")        → aidefence_scan, aidefence_is_safe, aidefence_has_pii
```

## Quick Setup

```bash
claude mcp add claude-flow -- npx -y @claude-flow/cli@latest
npx @claude-flow/cli@latest daemon start
npx @claude-flow/cli@latest doctor --fix
```

## Claude Code vs MCP Tools

- **Claude Code Agent tool** handles execution: agents, file ops, code generation, git
- **MCP tools** (via ToolSearch) handle coordination: swarm, memory, hooks, routing, hive-mind
- **CLI commands** (via Bash) are the same tools with terminal output
- Use `ToolSearch("keyword")` to discover available MCP tools

## Support

- Documentation: https://github.com/ruvnet/ruflo
- Issues: https://github.com/ruvnet/ruflo/issues

---

# Micro-OS Project Configuration

## Project Overview

Arch-Linux-inspired micro-OS for ESP32-S3 (N16R8: 16 MB Flash, 8 MB PSRAM).
Boots to a CLI shell, supports installing drivers/apps as ELF modules from SD or Wi-Fi.
See `roadmap.md` for full scope, phase gates, and exit criteria.

## Hardware

| Peripheral | Interface | Notes |
|------------|-----------|-------|
| ESP32-S3 N16R8 | — | 16 MB Flash, 8 MB Octal PSRAM |
| Waveshare E-ink | SPI | Logs, static output, partial refresh |
| SPI OLED | SPI | Active CLI feedback, low-latency |
| CardKB | I2C (0x5F) | Or matrix keyboard |
| MicroSD | SPI or SDMMC | Primary storage for modules/packages |

## Architecture: Lean Kernel + Loadable Modules

The firmware is a **lean kernel**. Only boot-essential hardware is compiled in.
Everything else (display drivers, Wi-Fi, apps) loads as ELF modules from SD card.

**Kernel (compiled in):**
- SD card SPI bus (boot storage)
- I2C bus (CardKB input)
- PSRAM management
- Shell (esp_console)
- Bus manager — exports `bus_spi_init()` so modules can init their own SPI bus
- Future: ELF loader, kernel symbol table, service registry

**Loadable modules (.elf on SD, future):**
- E-ink driver — reads pin config from `/etc/hardware.conf`
- OLED driver — same
- Wi-Fi service
- Any app or driver

**Pin config:** only boot-essential pins live in `src/config/pin_config.h`.
Display/peripheral pins go in `/etc/hardware.conf` on SD, read by modules at load time.

## Build System

**Framework:** ESP-IDF v5.x via PlatformIO.

```bash
pio run                        # build
pio run -t upload              # flash
pio device monitor             # serial monitor
pio run -t upload -t monitor   # flash + monitor
pio run -t menuconfig          # sdkconfig editor
pio run -t fullclean           # clean build
```

## Code Conventions

- **Language:** C (C11), no C++ unless wrapping an ESP-IDF C++ API
- **Naming:** `snake_case` for functions/variables, `UPPER_SNAKE` for macros/constants
- **Prefixes:** module functions use their module name (e.g., `sd_mount()`, `display_init()`, `bus_lock_spi()`)
- **Headers:** one `.h` per `.c`, include guards (`#ifndef MODULE_NAME_H`)
- **Error handling:** check every `esp_err_t`, log with `ESP_LOGE`/`ESP_LOGW`/`ESP_LOGI`
- **Logging tag:** one `static const char *TAG = "module_name";` per file

## Memory Rules

- Large buffers (>4 KB) MUST use PSRAM: `heap_caps_malloc(size, MALLOC_CAP_SPIRAM)`
- Module payloads (ELF loading) go to PSRAM
- Always check allocation return values
- Use `heap_caps_get_free_size()` for diagnostics

## SPI Bus Layout

Two separate SPI buses (no sharing conflict):
- **SPI3 (HSPI):** SD card only (CLK=39, MOSI=38, MISO=40, CS=46)
- **SPI2:** Display bus — NOT initialized by kernel. Modules call `bus_spi_init()` to set it up.

Pin assignments from `mp3-pedia/os` project (verified working hardware):
- Display SPI: CLK=12, MOSI=11 (write-only, no MISO)
- OLED: CS=10, DC=21, RST=47
- E-ink: CS=16, DC=15, RST=17, BUSY=18
- These pins are documented here but NOT in kernel code — modules read them from SD config.

## FreeRTOS / Concurrency

- Default: no core affinity unless profiling proves a bottleneck
- Explicit core pinning requires a comment explaining why
- Use mutexes for shared resources (bus, display buffer)
- Long operations must feed or disable WDT

## Source Layout

```
src/
├── main.c              # app_main() — boot: PSRAM check → bus init → shell
├── config/pin_config.h # boot-essential pins ONLY (SD, I2C, buzzer)
├── bus/bus_manager.c/h  # SD SPI + I2C init, generic bus_spi_init() for modules
├── hal/storage.c/h      # SD card mount/unmount/info
├── shell/shell.c/h      # esp_console REPL (help, mem, mount, unmount, ls, cat, reboot)
├── services/            # (Phase 1) Service registry, boot sequence, virtual console
├── loader/              # (Phase 2) ELF loader, symbol table, module lifecycle
└── net/                 # (Phase 4) Wi-Fi, package download
```

## Phase 0 Progress

- [x] PSRAM enabled and validated (sdkconfig.defaults + mem command)
- [x] Storage baseline: SD card mount/unmount
- [x] Basic shell (esp_console) with command registration
- [x] Bus manager for SPI/I2C
- [ ] Board bring-up testing on real hardware (logging, panic, watchdog)
- [ ] File write command
- [ ] /etc/hardware.conf parser (for module pin config)

## Phase Discipline

Respect `roadmap.md` phase order. Current focus: **Phase 0 — Foundation**.
Do NOT implement features from later phases until current phase exit criteria are met.

## Reference: Prior Art

Pin assignments and working driver code exist in `~/Documents/PlatformIO/Projects/mp3-pedia/os/`:
- `src/HardwareManager.cpp` — I2C, SPI, CardKB init
- `src/StorageManager.cpp` — SD card on HSPI (pins 38-40, CS=46)
- `src/DisplayManager.cpp` — OLED SH1106 via U8G2
- `src/epdif.h` — E-ink pin definitions
This is Arduino/C++ code; micro_arch rewrites it as ESP-IDF C with loadable modules.

## Testing

- Any change touching memory, SPI, or tasking must include stress-test evidence
- Test on real hardware — ESP32-S3 PSRAM timing differs from emulation
- Log heap stats before/after operations to catch leaks
