#ifndef GAME_OFFSETS_H
#define GAME_OFFSETS_H

#include <stdint.h>

/*
 * Hunt: Showdown 1896 offsets - CryEngine based
 * Binary version: Feb 24 2026 build (patch 2.6.0)
 *
 * Memory layout (Wine/Proton):
 *   GameHunt.dll + OFF_GENV -> SSystemGlobalEnvironment*
 *   gEnv + OFF_ENV_PENTITYSYSTEM -> IEntitySystem (CEntitySystem)
 *   IEntitySystem + OFF_ENTITY_COUNT -> entity count
 *   IEntitySystem + OFF_ENTITY_LIST -> entity pointer array
 *
 * Signature for gEnv: 48 8B 05 ? ? ? ? 48 85 C0 0F 84 EF 0E
 * Alt signature:      48 8B 05 ? ? ? ? 48 8B 88 B0
 *
 * Camera chain:
 *   gEnv + 0x90 -> pSystem (CCore*)
 *   CCore + 0x280 -> CCamera (embedded, 784 bytes)
 *   CCamera + 0x00 -> Matrix34 m_Matrix (48 bytes, world transform)
 *   CCamera + 0x30 -> float m_fov (radians)
 *   Position within Matrix34: m[0][3]=+0x0C, m[1][3]=+0x1C, m[2][3]=+0x2C
 *
 * Verified from IDA decompilation of HuntGame.exe:
 *   GetViewCamera: return this + 640 (0x280)
 *   SetCamera: reads [camera+0x0C], [camera+0x1C], [camera+0x2C] as position
 *              reads [camera+0x30] as FOV
 *              copies 784 bytes total (camera struct size)
 *   RenderBegin: *(this[3] + 104) = pRenderer -> gEnv+0x68 confirmed
 *   Render: *(this[3] + 144) = pSystem -> gEnv+0x90 confirmed
 *   GetIConsole: *(this[3] + 40) -> gEnv+0x28 confirmed
 */

/* === Module names as seen in Wine process memory maps === */
#define MOD_HUNTGAME    "HuntGame.exe"
#define MOD_GAMEHUNT    "GameHunt.dll"
#define MOD_CRYRENDER   "CryRenderD3D12.dll"

/* === SSystemGlobalEnvironment (gEnv) === */
/* gEnv is a global pointer in GameHunt.dll */
#define OFF_GENV                0x2294320

/* SSystemGlobalEnvironment member offsets (verified from IDA) */
#define OFF_ENV_PCONSOLE        0x28   /* 40  - IConsole* (verified) */
#define OFF_ENV_PRENDERER       0x68   /* 104 - IRenderer* (verified) */
#define OFF_ENV_PSYSTEM         0x90   /* 144 - ISystem / CCore* (verified) */
#define OFF_ENV_PENTITYSYSTEM   0xC0   /* 192 - IEntitySystem* (community) */

/* === IEntitySystem (CEntitySystem) === */
#define OFF_ENTITY_COUNT        0x40092  /* uint32_t entity count (community) */
#define OFF_ENTITY_LIST         0x400A0  /* entity pointer array base (community) */

/* === IEntity (CEntity) === */
#define OFF_ENTITY_NAME         0x10   /* pointer to entity name string (community) */
#define OFF_ENTITY_SLOTS        0xA8   /* pointer to CEntitySlot array (community) */

/*
 * Entity position: Verified from IDA decompilation of GetPos (sub_140641740):
 *   vmovss xmm0, dword ptr [rcx+16Ch]  -> pos.x
 *   vmovss xmm1, dword ptr [rcx+17Ch]  -> pos.y
 *   vmovss xmm0, dword ptr [rcx+18Ch]  -> pos.z
 *
 * This means entity has a Matrix34 m_worldTM at offset 0x160:
 *   +0x160: Matrix34 start (48 bytes)
 *   +0x16C: m[0][3] = position.x  (0x160 + 0x0C)
 *   +0x17C: m[1][3] = position.y  (0x170 + 0x0C)
 *   +0x18C: m[2][3] = position.z  (0x180 + 0x0C)
 *
 * SetPos (sub_1419B8464) confirms: reads GetWorldTM (vtable+320),
 * modifies pos at +0x0C/+0x1C/+0x2C in Matrix34, calls SetWorldTM (vtable+288).
 */
#define OFF_ENTITY_WORLDTM      0x160  /* Matrix34 world transform (VERIFIED) */

/* === Render node offsets (verified from community chams code) === */
#define OFF_SLOT_RENDERNODE     0xA0   /* render node ptr within slot */
#define OFF_RNODE_FLAGS         0x10   /* render flags */
#define OFF_RNODE_VIEWDIST      0x2C   /* view distance (float) */
#define OFF_RNODE_COLOR         0x130  /* RGBA color override */

/* === CCamera (verified from IDA decompilation) === */
#define OFF_SYSTEM_CAMERA       0x280  /* CCamera within CCore (verified: GetViewCamera) */
#define OFF_CAMERA_MATRIX       0x00   /* Matrix34 (3x4 float, 48 bytes) (verified) */
#define OFF_CAMERA_FOV          0x30   /* float fov in radians (verified: SetCamera) */
#define CAMERA_STRUCT_SIZE      784    /* total CCamera size (verified: SetCamera copy loop) */

/* === Entity class names for filtering (from community code) === */
#define ENTITY_CLASS_HUNTER     "Hunter"
#define ENTITY_CLASS_HUNTERBASIC "HunterBasic"
#define ENTITY_CLASS_BOSS       "Target"
#define ENTITY_CLASS_GRUNT      "Grunts.specials"

/* === Max entities to track === */
#define MAX_TRACKED_ENTITIES    128

#endif /* GAME_OFFSETS_H */
