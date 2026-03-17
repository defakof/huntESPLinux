#ifndef GAME_OFFSETS_H
#define GAME_OFFSETS_H

#include <stdint.h>

/*
 * Hunt: Showdown 1896 - CryEngine offsets
 * Patch 2.6.0 (Feb 24 2026 build)
 *
 * Chain: GameHunt.dll+OFF_GENV -> gEnv -> IEntitySystem -> entities
 * Camera: gEnv+0x90 -> CCore+0x280 -> CCamera (Matrix34 + FOV)
 */

/* Module names (Wine/Proton memory maps) */
#define MOD_HUNTGAME    "HuntGame.exe"
#define MOD_GAMEHUNT    "GameHunt.dll"
#define MOD_CRYRENDER   "CryRenderD3D12.dll"

/* gEnv pointer (GameHunt.dll) */
#define OFF_GENV                0x2294320

/* SSystemGlobalEnvironment offsets (gEnv+) */
#define OFF_ENV_PCONSOLE        0x28    /* IConsole*       */
#define OFF_ENV_PRENDERER       0x68    /* IRenderer*      */
#define OFF_ENV_PSYSTEM         0x90    /* ISystem/CCore*  */
#define OFF_ENV_PENTITYSYSTEM   0xC0    /* IEntitySystem*  */

/* IEntitySystem (CEntitySystem) offsets */
#define OFF_ENTITY_COUNT        0x40092 /* uint16_t entity count  */
#define OFF_ENTITY_LIST         0x400A0 /* uint64_t entity ptr[]  */

/* IEntity (CEntity) offsets */
#define OFF_ENTITY_NAME         0x10    /* char* entity name      */
#define OFF_ENTITY_SLOTS        0xA8    /* CEntitySlot* array     */
#define OFF_ENTITY_WORLDTM      0x160   /* Matrix34 world transform (pos at +0x0C/+0x1C/+0x2C) */

/* Render node offsets (CEntitySlot) */
#define OFF_SLOT_RENDERNODE     0xA0    /* IRenderNode*   */
#define OFF_RNODE_FLAGS         0x10    /* render flags   */
#define OFF_RNODE_VIEWDIST      0x2C    /* float          */
#define OFF_RNODE_COLOR         0x130   /* uint32 RGBA    */

/* CCamera offsets (within CCore) */
#define OFF_SYSTEM_CAMERA       0x280   /* CCamera offset in CCore         */
#define OFF_CAMERA_MATRIX       0x00    /* Matrix34 (3x4, 48 bytes)        */
#define OFF_CAMERA_FOV          0x30    /* float, radians                  */
#define CAMERA_STRUCT_SIZE      784     /* total CCamera struct size        */

/* Entity class name filters */
#define ENTITY_CLASS_HUNTER     "Hunter"
#define ENTITY_CLASS_HUNTERBASIC "HunterBasic"
#define ENTITY_CLASS_BOSS       "Target"
#define ENTITY_CLASS_GRUNT      "Grunts.specials"

#define MAX_TRACKED_ENTITIES    128

#endif /* GAME_OFFSETS_H */
