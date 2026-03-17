# Hunt ESP Linux

An ESP overlay for **Hunt: Showdown 1896** running on Linux via Wine/Proton. It reads game memory through a custom kernel module and renders real-time entity positions (hunters, bosses, grunts) as a transparent overlay using ImGui + OpenGL.

Built for **Linux** (tested on Bazzite/Fedora), **Proton 9+**, **X11/Wayland (KWin)**.
Targets Hunt: Showdown patch **2.6.0**.

---

## Features

- **Entity ESP** — Draws bounding boxes, distance labels, and snap lines for hunters, bosses, and grunts
- **World-to-screen projection** — Accurate 3D-to-2D transformation using the game's view and projection matrices
- **Kernel-level memory reading** — Custom Linux kernel module (`hunt_reader.ko`) for safe, read-only cross-process memory access via `access_process_vm()`
- **Wine/Proton compatibility** — Handles PE header differences, non-contiguous VMA mappings, IAT scanning, and anonymous memory regions specific to Wine
- **Interactive ImGui menu** — Toggle ESP categories, customize colors, view a live entity table with copy-to-clipboard
- **Click-through overlay** — Full-screen transparent window with XFixes input passthrough; press **Insert** to toggle menu interaction
- **Auto PID detection** — Automatically finds the game process via `pgrep`
- **Entity caching** — Type and head-bone index caching with periodic invalidation for performance
- **Multiple fallback paths** — Three independent methods to locate the entity system in memory

---

## Project Structure

```
huntESPLinux-master/
├── driver/
│   ├── hunt_reader.c          # Linux kernel module (ioctl-based memory reader)
│   └── Makefile
├── include/
│   ├── game_offsets.h         # Game memory offsets and module names
│   └── hunt_shared.h          # Shared kernel/userspace ioctl protocol
├── overlay/
│   ├── main.cpp               # Overlay rendering, ImGui menu, ESP drawing
│   ├── game_data.h            # Game state manager, entity parsing, camera scanning
│   ├── mem_reader.h           # Userspace ioctl wrapper class
│   ├── CMakeLists.txt
│   └── imgui/                 # Embedded ImGui with OpenGL3/GLFW backend
├── run_esp.sh                 # Launch script (loads module + starts overlay)
└── OFFSETS_AND_METHODS.md     # Technical reference for offsets and RE methods
```

---

## Architecture

### Kernel Module (`driver/hunt_reader.c`)

A `misc` device driver at `/dev/hunt_read` exposing three ioctl commands:

| ioctl | Purpose |
|-------|---------|
| `HUNT_IOC_SET_PID` | Target a game process by PID |
| `HUNT_IOC_READ_MEM` | Read up to 4 MB from the target process address space |
| `HUNT_IOC_GET_MODULE` | Find a DLL/EXE base address by walking process VMAs |

### Game Data Layer (`overlay/game_data.h`)

Handles all game memory traversal:

| Function | Purpose |
|----------|---------|
| `resolve_genv_from_iat()` | Scans GameHunt.dll IAT for valid gEnv pointers (Wine-compatible) |
| `probe_huntgame_globals()` | Reads entity system directly from HuntGame.exe global offset |
| `find_entity_system()` | Fallback: pattern-scans HuntGame.exe `.data` for entity system signature |
| `read_entities()` | Iterates entity array, classifies entities by name patterns |
| `scan_camera()` | FOV-based pattern scan in CryRenderD3D12.dll `.data` for camera matrices |
| `find_head_bone_index()` | Traverses skeleton/bone hierarchy for head position |

### Overlay (`overlay/main.cpp`)

| Function | Purpose |
|----------|---------|
| `draw_esp()` | Renders boxes, distance labels, and snap lines for each entity |
| `draw_menu()` | ImGui settings panel and entity list table |
| `world_to_screen()` | View matrix * projection matrix -> NDC -> screen coordinates |
| `find_hunt_pid()` | Auto-detects game PID via `pgrep -f HuntGame.exe` |
| `set_clickthrough()` | Toggles XFixes input passthrough on the overlay window |

### Memory Reader (`overlay/mem_reader.h`)

Thin C++ wrapper around the kernel module:

| Method | Purpose |
|--------|---------|
| `open()` | Opens `/dev/hunt_read` device |
| `read_mem()` | Low-level ioctl read from target process |
| `get_module_base()` | Queries module base address and size |
| `read<T>()` | Templated typed read at an address |

---

## Entity Classification

Entities are classified by name pattern matching:

| Category | Patterns |
|----------|----------|
| **Hunters** | `HunterBasic`, `Hunter_` |
| **Bosses** | `Butcher`, `Spider`, `Assassin`, `Scrapbeak`, `Rotjaw`, `Ursa` |
| **Grunts** | `Immolator`, `Hive`, `Meathead`, `Hellhound`, `Armored`, `WaterDevil` |

40+ patterns (decorations, UI elements, effects) are filtered out to reduce noise.

---

## Key Memory Offsets

| Target | Offset | Description |
|--------|--------|-------------|
| `HuntGame.exe + 0x2A40E90` | Global | Entity system pointer |
| `GameHunt.dll + 0x2294320` | IAT | gEnv pointer (fallback) |
| `entity_system + 0x40098` | Array | Entity pointer array base |
| `entity + 0x10` | Field | Entity name string pointer |
| `entity + 0x160` | Field | World transform matrix (3x4) + position |
| `CCore + 0x280` | Block | Camera transform, FOV, view/projection matrices |

See [OFFSETS_AND_METHODS.md](OFFSETS_AND_METHODS.md) for full technical documentation.

---

## Dependencies

| Package | Purpose |
|---------|---------|
| `libglfw3-dev` | Window creation and input |
| `libx11-dev` | X11 display server |
| `libxfixes-dev` | Click-through regions |
| `libgl1-mesa-dev` | OpenGL rendering |
| `cmake` (3.16+) | Overlay build system |
| Linux kernel headers | Kernel module build |

---

## Building

### Kernel Module

```bash
cd driver/
make
```

Produces `hunt_reader.ko`.

### Overlay

```bash
cd overlay/
mkdir build && cd build
cmake ..
make
```

Produces the `hunt_esp` binary.

---

## Usage

### Quick Start

```bash
# Load the kernel module
sudo insmod driver/hunt_reader.ko

# Launch Hunt: Showdown in borderless windowed mode via Proton

# Run the overlay (auto-detects game PID)
./overlay/build/hunt_esp

# Or specify PID manually
./overlay/build/hunt_esp <pid>
```

Or use the launch script:

```bash
sudo ./run_esp.sh
```

### Controls

| Key | Action |
|-----|--------|
| **Insert** | Toggle menu (works globally, no focus needed) |
| **Ctrl+C** | Exit overlay |

### Requirements

- Game must be running in **borderless windowed** mode (not exclusive fullscreen)
- **Proton 9+** recommended
- Tested on **Bazzite / Fedora** with **KWin** (X11 and Wayland)

---

## Wine/Proton Compatibility Notes

This project addresses several challenges specific to running under Wine/Proton:

- **PE cross-module references** are not relocated by Wine — the code uses IAT scanning instead of direct `.rdata` pointer chasing
- **DLLs are mapped as multiple non-contiguous VMAs** — the kernel module walks the full VMA span to compute module size
- **Game sections are anonymous** (not file-backed) under Wine — PE header parsing with RVA offsets is used to locate `.data` sections
- **HuntGame.exe loads at its preferred base** (`0x140000000`) under Wine, matching IDA analysis

---

## Technical Reference

See [OFFSETS_AND_METHODS.md](OFFSETS_AND_METHODS.md) for in-depth documentation on:

- Memory traversal chains
- Entity system discovery methods
- Camera matrix extraction
- PE header parsing under Wine
- Offset derivation methodology

---

## Stats

| Metric | Value |
|--------|-------|
| Core source lines | ~2,270 |
| Languages | C (kernel), C++17 (userspace) |
| Target game | Hunt: Showdown 1896 (CryEngine) |
| Target patch | 2.6.0 |

---

## Disclaimer

This project is for **educational and research purposes only**. Use at your own risk. The authors are not responsible for any consequences resulting from the use of this software.
