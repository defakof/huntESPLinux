# Hunt: Showdown 1896 — Linux/Proton ESP Technical Reference

**Binary version**: Feb 24 2026 build (patch 2.6.0)
**Platform**: Linux (Bazzite/Fedora), Wine/Proton, Wayland/KWin
**Last updated**: March 2026

---

## 1. MODULES

| Module | Purpose |
|--------|---------|
| `HuntGame.exe` | Main executable, contains global pointers, entity system |
| `GameHunt.dll` | Game logic (48MB), entity classes, Schematyc |
| `CryRenderD3D12.dll` | Renderer, contains camera/view matrix data |

---

## 2. ENTITY SYSTEM

### Global Entity System Pointer
- **Location**: `HuntGame.exe + 0x2A40E90` (RVA)
- **IDA symbol**: `qword_142A40E90` (IDA base `0x140000000`)
- **Type**: `CEntitySystem*` (direct pointer, NOT through gEnv)
- **Note**: HuntGame.exe loads at its preferred base `0x140000000` on Wine, so runtime address = IDA address

### GetEntity Function (IDA: `sub_1400CAB60`)
```c
// Decompiled from HuntGame.exe
__int64 __fastcall GetEntity(__int64 entitySystem, int entityId)
{
    // entityId & 0xFFFF = array index
    // HIWORD(entityId) = salt for validation
    uint16_t index = (uint16_t)entityId;
    uint16_t max_index = *(uint16_t*)(entitySystem + 262290);  // +0x40092

    // Bounds check with salt-style masking
    entity = *(uint64_t*)(entitySystem + 8 * index + 262296);  // +0x40098

    if (entity && (entity->GetId() >> 16) == HIWORD(entityId))
        return entity;
    return 0;
}
```

### Entity System Offsets
| Offset | Type | Description |
|--------|------|-------------|
| `+0x40092` | `uint16_t` | Max entity index (entity count) |
| `+0x40098` | `CEntity*[]` | Entity pointer array (8-byte stride, indexed by EntityId & 0xFFFF) |

### Entity Iteration
```c
uint64_t entitySys = Read<uint64_t>(huntgame_base + 0x2A40E90);
uint16_t maxIdx = Read<uint16_t>(entitySys + 0x40092);
for (uint16_t i = 0; i <= maxIdx; i++) {
    uint64_t entity = Read<uint64_t>(entitySys + 0x40098 + i * 8);
    if (entity == 0) continue;
    // ... use entity
}
```

### Entity (CEntity) Offsets
| Offset | Type | Description |
|--------|------|-------------|
| `+0x00` | `void*` | vtable pointer |
| `+0x10` | `char*` | Entity name pointer (e.g., "HunterBasic[]", "GameRules") |
| `+0x160` | `Matrix34` | World transform matrix (48 bytes) |
| `+0x160 + 0x0C` | `float` | Position X (Matrix34 row0 col3) |
| `+0x160 + 0x1C` | `float` | Position Y (Matrix34 row1 col3) |
| `+0x160 + 0x2C` | `float` | Position Z (Matrix34 row2 col3) |
| `+0xA8` | `void*` | Entity slots pointer (for render nodes/chams) |

### Entity Position Reading
```c
float posX = Read<float>(entity + 0x160 + 0x0C);  // Matrix34[0][3]
float posY = Read<float>(entity + 0x160 + 0x1C);  // Matrix34[1][3]
float posZ = Read<float>(entity + 0x160 + 0x2C);  // Matrix34[2][3]
```

### Entity Name Examples
| Name Pattern | What it is |
|-------------|-----------|
| `HunterBasic[]` | Player hunter (your team and enemies) |
| `HunterBasic[]_PlayAreaConfinerProcess` | Player-related process entity (pos=0,0,0 — skip) |
| `TeamEntity_Hunters_TeamX` | Team entity |
| `Grunts.specials.Spider_target_*` | Boss-related spike entities |
| `Grunts.Scrapbeak.Target_*` | Scrapbeak boss entities |
| `EntityContainerObject_TargetSpawns_*` | Boss spawn location |
| `cocooned_hunter` | Dead/cocooned hunter |
| `ComK_Butcher_FireEffect-*` | Butcher fire effects |
| `ATS_COM_*_BeeHive_*` | Bee hive world objects |
| `GameRules` | Game rules entity (index 1) |
| `sliding_doors.*` | Door entities |
| `supply_point_*` | Supply points |
| `SpidersMenu*-*` | Lobby spider menu decorations (skip — not real bosses) |
| `LobbyHunter_BountyHunt_*` | Lobby hunter models (skip — not real hunters) |
| `LobbyHunter_Recruitment_*` | Lobby recruitment hunter models (skip) |
| `Hunter_*_Rotation_Mouse_Hitbox_RigidBodyEx` | Lobby mouse interaction hitboxes (skip) |

### Entity Name Skip Filters
Entities matching any of these patterns are skipped before classification:
| Pattern | Reason |
|---------|--------|
| `BossArea` | Boss area trigger zones |
| `Proximity_Warning` | Proximity trigger entities |
| `_Shape` | Collision shapes |
| `_Register` | Registration entities |
| `_Loot`, `_DroppedItem` | Loot/item pickups |
| `particle` | Particle effects |
| `GruntWeapon` | Grunt weapon entities (not the grunt itself) |
| `Water_Area`, `Swamp_Water` | Water volume triggers |
| `Dressing.` | Map dressing/decorations |
| `_Ravens`, `_Feathers_` | Bird ambient entities |
| `_Gramophone` | Gramophone world objects |
| `Menu` | Lobby menu decorations (e.g. `SpidersMenu*`) |
| `Lobby` | Lobby hunter models (e.g. `LobbyHunter_*`) |
| `Hitbox` | Mouse interaction hitboxes (e.g. `Hunter_*_Hitbox_RigidBodyEx`) |

### Entity Classification Rules
After skip filtering, entities are classified by name substring:
| Pattern | Type | Display Name |
|---------|------|-------------|
| `HunterBasic`, `Hunter_` | Hunter | "Hunter" |
| `Butcher` | Boss | "Butcher" |
| `Spider` | Boss | "Spider" |
| `Assassin` | Boss | "Assassin" |
| `Scrapbeak` | Boss | "Scrapbeak" |
| `Rotjaw` | Boss | "Rotjaw" |
| `Ursa` | Boss | "Ursa" |
| `Immolator` | Grunt | "Immolator" |
| `Hive` | Grunt | "Hive" |
| `Meathead` | Grunt | "Meathead" |
| `Hellhound` | Grunt | "Hellhound" |
| `Armored` | Grunt | "Armored" |
| `WaterDevil` | Grunt | "Waterdevil" |
| `grunts/grunt/`, `_Grunts` | Grunt | "Grunt" |

### IDA Verification: GetPos (Script Binding)
```
// sub_140641740 — Entity:GetPos script function
// Reads position from entity at offsets:
//   [rcx + 0x16C] = pos.x   (entity + 0x160 + 0x0C)
//   [rcx + 0x17C] = pos.y   (entity + 0x160 + 0x1C)
//   [rcx + 0x18C] = pos.z   (entity + 0x160 + 0x2C)
```

---

## 3. CAMERA

### Camera Location
- **Module**: `CryRenderD3D12.dll`
- **Section**: `.data`
- **Offset within .data**: `0x135FA0` (found by FOV scan, may shift between builds)

### Camera Finding Method (FOV Scan)
Scan CryRenderD3D12.dll `.data` section for:
1. A `float` in range `0.01 — 2.0` (FOV in radians, ~78° default = 1.36 rad; scoped zoom can go as low as ~0.06 rad)
2. 48 bytes before the FOV: a Matrix34 where:
   - Position (at +0x0C, +0x1C, +0x2C) has world coordinates (50-2000 range)
   - First rotation row has unit length (~1.0)

```c
// Scan .data for FOV pattern
for (offset = 0x30; offset < data_size; offset += 4) {
    float fov = Read<float>(data_start + offset);
    if (fov < 0.01 || fov > 2.0) continue;

    float px = Read<float>(data_start + offset - 0x24);  // pos.x
    float py = Read<float>(data_start + offset - 0x14);  // pos.y
    float pz = Read<float>(data_start + offset - 0x04);  // pos.z
    if (abs(px) < 50 || abs(py) < 50) continue;

    float r0 = Read<float>(data_start + offset - 0x30);  // rotation row0
    float r1 = Read<float>(data_start + offset - 0x2C);
    float r2 = Read<float>(data_start + offset - 0x28);
    float len = sqrt(r0*r0 + r1*r1 + r2*r2);
    if (len < 0.9 || len > 1.1) continue;

    // Found camera! Matrix34 starts at (offset - 0x30), FOV at offset
    camera_offset = offset - 0x30;
    break;
}
```

### Camera Structure (CCamera)
| Offset from camera start | Type | Description |
|--------------------------|------|-------------|
| `+0x00` | `Matrix34` (48 bytes) | Camera world transform |
| `+0x00` | `float[4]` | Row 0: right vector (r0,r1,r2) + pos.x |
| `+0x10` | `float[4]` | Row 1: forward vector (u0,u1,u2) + pos.y |
| `+0x20` | `float[4]` | Row 2: up vector (f0,f1,f2) + pos.z |
| `+0x30` | `float` | FOV in radians (valid range: 0.01–3.0; default ~1.36, scoped zoom ~0.06–0.15) |
| `+0x230` | `Matrix44` (64 bytes) | View matrix (RenderMatrix) — used for W2S step 1 |
| `+0x270` | `Matrix44` (64 bytes) | Projection matrix — used for W2S step 2 |
| `+0x2F0` | `Vec3` (12 bytes) | Camera position (alternative to Matrix34 translation) |
| Total struct | 784 bytes | Full CCamera size (from IDA SetCamera copy loop) |

### Camera Position
```c
float camX = Read<float>(camera_addr + 0x0C);  // Matrix34[0][3]
float camY = Read<float>(camera_addr + 0x1C);  // Matrix34[1][3]
float camZ = Read<float>(camera_addr + 0x2C);  // Matrix34[2][3]
float fov  = Read<float>(camera_addr + 0x30);  // radians
```

### Camera Limitations on Wine/Proton
- **Position**: Updates in real-time ✓
- **View/Projection matrices**: Read from CCamera at `+0x230` and `+0x270` — work correctly for W2S ✓
- **FOV during scope zoom**: Can drop to ~0.06 rad with high-magnification scopes — validity check must use threshold ≤0.01 ✓

### World-to-Screen
Both matrices are read from CCamera in a single bulk read (784 bytes from `cam_base + cam_offset`):
- **View matrix (RenderMatrix)**: `CCamera + 0x230` (Matrix44, 64 bytes)
- **Projection matrix**: `CCamera + 0x270` (Matrix44, 64 bytes)

```c
// Read matrices from CCamera struct
Matrix44 RM, PM;
ReadMem(camera_addr + 0x230, &RM, 64);  // View matrix
ReadMem(camera_addr + 0x270, &PM, 64);  // Projection matrix

Vector2 WorldToScreen(Vector3 pos) {
    // Step 1: Transform by view matrix
    Vector4 t = {
        pos.x * RM[0][0] + pos.y * RM[1][0] + pos.z * RM[2][0] + RM[3][0],
        pos.x * RM[0][1] + pos.y * RM[1][1] + pos.z * RM[2][1] + RM[3][1],
        pos.x * RM[0][2] + pos.y * RM[1][2] + pos.z * RM[2][2] + RM[3][2],
        pos.x * RM[0][3] + pos.y * RM[1][3] + pos.z * RM[2][3] + RM[3][3]
    };

    if (t.z >= 0.0f) return Zero;  // Behind camera

    // Step 2: Project
    Vector4 p = {
        t.x * PM[0][0] + t.y * PM[1][0] + t.z * PM[2][0] + t.w * PM[3][0],
        t.x * PM[0][1] + t.y * PM[1][1] + t.z * PM[2][1] + t.w * PM[3][1],
        t.x * PM[0][2] + t.y * PM[1][2] + t.z * PM[2][2] + t.w * PM[3][2],
        t.x * PM[0][3] + t.y * PM[1][3] + t.z * PM[2][3] + t.w * PM[3][3]
    };

    p.x /= p.w;
    p.y /= p.w;
    if (abs(p.x) > 1.5 || abs(p.y) > 1.5) return Zero;

    return { (1 + p.x) * width / 2, (1 - p.y) * height / 2 };
}
```

---

## 4. gEnv (SSystemGlobalEnvironment) — BROKEN ON WINE

### Community Offset (Works on Windows)
```c
// On Windows:
uint64_t gEnv = Read<uint64_t>(game_hunt_dll + 0x2294320);
uint64_t entitySys = Read<uint64_t>(gEnv + 0xC0);  // pEntitySystem
uint64_t pSystem = Read<uint64_t>(gEnv + 0x90);     // ISystem/CCore
```

### Why It Fails on Wine/Proton
- `GameHunt.dll + 0x2294320` is in `.rdata` section
- On Windows, the PE loader relocates this value to a runtime pointer
- On Wine/Proton, the value is **NOT relocated** — contains `0x142a40dd0` (IDA VA from HuntGame.exe)
- The `.reloc` section is not applied to cross-module references in `.rdata` on Wine
- This is a fundamental Wine/Proton PE loading difference

### gEnv Offsets (Verified from IDA, for reference)
| Offset | Type | Description | IDA Source |
|--------|------|-------------|------------|
| `+0x28` (40) | `IConsole*` | pConsole | GetIConsole decompiled |
| `+0x68` (104) | `IRenderer*` | pRenderer | RenderBegin decompiled |
| `+0x90` (144) | `ISystem*` (CCore) | pSystem | Render decompiled |
| `+0xC0` (192) | `IEntitySystem*` | pEntitySystem | Community code |

### CCore / ISystem Offsets
| Offset | Type | Description | IDA Source |
|--------|------|-------------|------------|
| `+0x280` (640) | `CCamera` | View camera (embedded, 784 bytes) | GetViewCamera decompiled |
| `+0x990` | `ISystemEventDispatcher*` | Event dispatcher | GetISystemEventDispatcher |
| `+0xB78` | `IPlatformOS*` | Platform OS | GetPlatformOS |
| `+0x5A0` (1440) | `bool` | Some state flag | RenderBegin/Render |

---

## 5. IDA KEY FUNCTIONS

| Address (IDA) | Name | Purpose |
|---------------|------|---------|
| `0x1400CAB60` | GetEntity | Entity lookup by EntityId — reveals entity array at +0x40098 |
| `0x14020BDE0` | CountEntities | Counts non-null entities in array |
| `0x14029D570` | CCore::GetViewCamera | Returns CCamera at this+640 (0x280) |
| `0x14029C660` | CCore::RenderBegin | Uses *(this[3]+104) = pRenderer |
| `0x14029B2D0` | CCore::Render | Uses *(this[3]+144) = pSystem, camera at this+640 |
| `0x140641740` | Entity:GetPos | Script binding, reads pos from entity+0x16C/17C/18C |
| `0x1419B8464` | Entity:SetPos | Script binding, writes Matrix34 via SetWorldTM |
| `0x1403061B0` | SetCamera | Copies 784 bytes = CCamera size, reads FOV at [cam+0x30] |
| `0x14035BE50` | EntitySystem internal | Manages entity salt/index free list at +0x9A (154) |
| `0x1403D4FD0` | DeleteEntity | Uses qword_142A40E60 for renderer notifications |

### IDA Signatures
```
gEnv access (GameHunt.dll):
  48 8B 05 ? ? ? ? 48 85 C0 0F 84 EF 0E
  48 8B 05 ? ? ? ? 48 8B 88 B0

gEnv mangled symbol:
  ?gEnv@@3USSystemGlobalEnvironment@@A  (at GameHunt.dll .rdata 0x2294320)

g_pIEntitySystem mangled symbol:
  ?g_pIEntitySystem@@3PEAVCEntitySystem@Entity@Cry@@EA  (at HuntGame.exe 0x142A336B3)
```

---

## 6. CHAMS (From Community Code — Windows)

```c
// For each entity with name containing "HunterBasic":
uintptr_t slotsPtr = Read<uintptr_t>(entity + 0xA8);
uintptr_t slotPtr = Read<uintptr_t>(slotsPtr);
uintptr_t renderNodePtr = Read<uint64_t>(slotPtr + 0xA0);

Write<uint32_t>(renderNodePtr + 0x10, 0x80018);     // render flags
Write<float>(renderNodePtr + 0x2C, 10000.f);        // view distance
Write<uint64_t>(renderNodePtr + 0x130, 0xFF000000);  // color (RGBA)
```

---

## 7. LINUX-SPECIFIC NOTES

### Kernel Module
- Uses `access_process_vm()` for cross-process memory reading
- Device at `/dev/hunt_read` with ioctls for SET_PID, READ_MEM, GET_MODULE
- Module base found by walking VMAs in `/proc/pid/maps`
- Wine maps DLLs with only PE header + .rsrc as file-backed VMAs; actual sections are anonymous

### Overlay
- Must use XWayland (X11 backend) for transparency on Wayland/KWin
- GLFW + OpenGL3 + ImGui for rendering
- `GLFW_TRANSPARENT_FRAMEBUFFER` + `GLFW_MOUSE_PASSTHROUGH` for click-through
- XFixes empty input region for full click passthrough
- Game must NOT be at native resolution borderless (KWin treats as exclusive fullscreen)
- Use windowed mode or non-native-resolution borderless

### PE Loading Differences (Wine vs Windows)
- HuntGame.exe loads at preferred base `0x140000000` ✓
- GameHunt.dll loads at `0x6ffff1760000` (varies) — PE header ImageBase is patched to match
- `.rdata` cross-module references are NOT relocated on Wine
- IAT imports ARE correctly resolved
- RIP-relative code addressing works correctly (use signature scanning for reliable offsets)

### Finding Modules on Wine
```c
// Wine maps PE DLLs as:
//   base+0x0000: r--p (PE header, 1 page)
//   base+0x1000: (sections mapped as anonymous, not file-backed)
//   ...
//   base+rsrc_rva: r--p (resource section, file-backed)
// Use PE header to find section RVAs, then base+RVA for section addresses
```

---

## 8. UPDATE CHECKLIST

When the game updates:

1. **Entity system global**: Check if `HuntGame.exe + 0x2A40E90` still points to entity system
   - Verify: `Read<uint16_t>(entitySys + 0x40092)` gives reasonable count (100-50000)
   - Verify: entities at `entitySys + 0x40098 + i*8` have name at `+0x10`

2. **Entity position**: Check if position is still at `entity + 0x160` (Matrix34)
   - Run IDA script on GetPos to verify offsets `+0x16C`, `+0x17C`, `+0x18C`

3. **Camera**: Re-scan CryRenderD3D12.dll .data for FOV pattern
   - FOV value ~1.36 radians (78° default)
   - Position should match player location

4. **gEnv offset** (if trying Windows approach): `GameHunt.dll + 0x2294320`
   - This may shift with updates — re-scan using IDA signature

5. **IDA signatures to re-find offsets**:
   - GetEntity: small function using `+262290` (0x40092) and `+262296` (0x40098)
   - GetViewCamera: tiny function returning `this + 640`
   - GetPos: function reading `[rcx+0x16C]`, `[rcx+0x17C]`, `[rcx+0x18C]`
