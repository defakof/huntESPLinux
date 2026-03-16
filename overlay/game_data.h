#pragma once
#include <cstdint>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <unordered_map>
#include "mem_reader.h"
#include "../include/game_offsets.h"

/* Set to 1 to enable verbose bone/head debug logging */
#define BONE_DEBUG 0
#if BONE_DEBUG
#define BONE_LOG(...) printf(__VA_ARGS__)
#else
#define BONE_LOG(...) ((void)0)
#endif

struct Vec3 {
    float x, y, z;
    float distance(const Vec3 &o) const {
        float dx=x-o.x,dy=y-o.y,dz=z-o.z; return sqrtf(dx*dx+dy*dy+dz*dz);
    }
};
struct Matrix34 { float m[3][4]; Vec3 get_translation() const { return {m[0][3],m[1][3],m[2][3]}; } };
struct Matrix44 { float m[4][4]; };
enum EntityType { ENT_UNKNOWN=0, ENT_HUNTER, ENT_BOSS, ENT_GRUNT };
struct EntityData { Vec3 position; Vec3 head_pos; bool has_head; EntityType type; char name[64]; char raw_name[64]; };
struct GameState { Vec3 local_pos; Matrix44 render_mat; Matrix44 proj_mat; Matrix44 view_proj; std::vector<EntityData> entities; bool valid; int screen_w, screen_h; bool show_all; };

class GameData {
public:
    GameData() : pid_(0), gamehunt_base_(0), gamehunt_size_(0),
                 genv_addr_(0), direct_entsys_(0), entsys_data_addr_(0), huntgame_base_(0),
                 cam_base_(0), cam_offset_(0), diag_(0) {}

    bool init(int32_t pid) {
        pid_ = pid;
        if (!reader_.open()) { fprintf(stderr,"Failed to open %s\n",HUNT_DEVICE_PATH); return false; }
        if (!reader_.set_pid(pid)) { fprintf(stderr,"Failed to set PID %d\n",pid); return false; }
        if (!reader_.get_module_base(MOD_GAMEHUNT, gamehunt_base_, gamehunt_size_)) {
            fprintf(stderr,"Failed to find %s\n",MOD_GAMEHUNT); return false;
        }
        printf("Found %s at 0x%lx\n", MOD_GAMEHUNT, gamehunt_base_);

        /*
         * The community code reads gEnv from game_hunt_dll + 0x2294320.
         * On Windows, the PE loader applies relocations so the value there
         * is the runtime pointer to gEnv. On Wine, the value is NOT relocated
         * (it's the original PE VA 0x142a40dd0).
         *
         * Solution: Read the raw PE file value, compute the PE's original
         * ImageBase from the optional header, then apply relocation:
         *   relocated_value = raw_value - pe_original_imagebase + actual_loadbase
         *
         * The PE file's original ImageBase (before Wine changed it) can be
         * found from the raw value itself: gEnv at 0x142a40dd0 means the
         * original base was 0x140000000 (standard x64 PE base), because
         * gEnv is in HuntGame.exe which loads at 0x140000000.
         *
         * BUT WAIT: The value at GameHunt.dll+0x2294320 references a variable
         * in HuntGame.exe, not in GameHunt.dll itself. This is a cross-module
         * import that Windows resolves via the IAT (Import Address Table).
         *
         * On Wine, the IAT IS resolved correctly because Wine's loader handles
         * imports. The issue is that 0x2294320 is NOT in the IAT — it's in
         * the .rdata section as a direct reference.
         *
         * NEW APPROACH: Read the gEnv pointer from GameHunt.dll's IAT.
         * GameHunt.dll imports gEnv from HuntGame.exe. The IAT is in the
         * .idata section. Or: just apply the relocation ourselves.
         */

        /* Read PE original ImageBase from the optional header */
        uint32_t pe_off = 0;
        reader_.read(gamehunt_base_ + 0x3C, pe_off);
        uint64_t pe_imagebase = 0;
        reader_.read(gamehunt_base_ + pe_off + 24 + 24, pe_imagebase);
        printf("PE ImageBase in header: 0x%lx\n", pe_imagebase);
        printf("Actual load base: 0x%lx\n", gamehunt_base_);

        /* Read the raw value at the community offset */
        uint64_t raw_val = 0;
        reader_.read_ptr(gamehunt_base_ + 0x2294320, raw_val);
        printf("Raw at +0x2294320: 0x%lx\n", raw_val);

        /* Apply relocation: raw_val was relative to PE's original ImageBase.
         * The PE header says ImageBase = gamehunt_base_ (Wine sets it to load addr).
         * But the VALUE wasn't updated. The original ImageBase before Wine patched
         * the header was probably 0x180000000 (GameHunt.dll) or we can derive it.
         *
         * Actually: The value 0x142a40dd0 is in HuntGame.exe address space.
         * HuntGame.exe loads at 0x140000000. So this IS the correct runtime
         * address within HuntGame.exe.
         *
         * The REAL question is: what does the value AT 0x142a40dd0 point to?
         * We read 0x227040. But that's in a very low address range.
         *
         * On Wine WoW64, the game runs in a 32-bit-compatible mode where
         * Windows heap is mapped in the low 4GB. 0x227040 IS a valid address!
         * But when we read gEnv(0x227040)+0xC0, we got garbage.
         *
         * HOWEVER: the community code works on Windows. The difference is
         * that their process is native 64-bit Windows with proper addresses.
         * On Wine, the process has a DIFFERENT memory layout.
         *
         * FINAL APPROACH: The gEnv at 0x142a40dd0 contains 0x227040 which
         * may be valid but its internal offsets differ from native Windows.
         * Let's just read it and probe for the entity system at ALL offsets.
         */

        /*
         * The value at GameHunt.dll + 0x2294320 is 0x142a40dd0 (unrelocated).
         * On Windows, the PE loader would have added the relocation delta.
         *
         * GameHunt.dll's original ImageBase (from the PE on disk) is 0x180000000.
         * The value 0x142a40dd0 references HuntGame.exe space (base 0x140000000).
         * On Windows, GameHunt.dll is loaded at some base X, and the relocation
         * delta = X - 0x180000000 is added to ALL relocated addresses.
         *
         * But 0x142a40dd0 is a pointer to HuntGame.exe, not to GameHunt.dll.
         * The relocation adds the GameHunt.dll delta, which would be WRONG
         * for a cross-module reference. This means the community offset
         * 0x2294320 must be part of the IAT (Import Address Table), where
         * the loader writes the actual import address regardless of relocation.
         *
         * Let me check if this address is in the IAT by reading the PE
         * import directory.
         */
        printf("Checking PE import tables...\n");

        /* Read import directory RVA from optional header data directories */
        /* Data directory index 1 = Import Table */
        uint64_t opt_start = gamehunt_base_ + pe_off + 24;
        uint32_t import_rva = 0, import_size = 0;
        reader_.read(opt_start + 120, import_rva);  /* DataDirectory[1].VirtualAddress */
        reader_.read(opt_start + 124, import_size);
        printf("  Import directory: RVA=0x%x size=0x%x\n", import_rva, import_size);

        /* Data directory index 12 = IAT */
        uint32_t iat_rva = 0, iat_size = 0;
        reader_.read(opt_start + 192, iat_rva);  /* DataDirectory[12].VirtualAddress */
        reader_.read(opt_start + 196, iat_size);
        printf("  IAT: RVA=0x%x size=0x%x\n", iat_rva, iat_size);

        /* Check if 0x2294320 is within the IAT */
        if (0x2294320 >= iat_rva && 0x2294320 < iat_rva + iat_size) {
            printf("  0x2294320 IS in IAT!\n");
        } else {
            printf("  0x2294320 is NOT in IAT (IAT range: 0x%x-0x%x)\n",
                   iat_rva, iat_rva + iat_size);
        }

        /* Scan the IAT for a pointer to HuntGame.exe that looks like gEnv */
        printf("Scanning IAT for gEnv...\n");
        for (uint32_t ioff = 0; ioff < iat_size; ioff += 8) {
            uint64_t iat_val = 0;
            reader_.read_ptr(gamehunt_base_ + iat_rva + ioff, iat_val);
            if (iat_val < 0x10000 || iat_val > 0x800000000000ULL) continue;

            /* Test as gEnv: check +0xC0 for entity system */
            for (uint32_t esoff : {0xC0U, 0xD8U, 0xE0U, 0x90U}) {
                uint64_t es = 0;
                reader_.read_ptr(iat_val + esoff, es);
                if (es < 0x10000 || es > 0x800000000000ULL) continue;
                uint32_t cnt = 0;
                reader_.read(es + 0x40092, cnt);
                if (cnt < 10 || cnt > 50000) continue;
                /* Try first few entities for valid position */
                for (uint32_t j = 0; j < std::min(cnt, 100u); j++) {
                    uint64_t ep = 0;
                    reader_.read_ptr(es + 0x400A0 + j * 8, ep);
                    if (ep < 0x10000 || ep > 0x800000000000ULL) continue;
                    float px=0,py=0,pz=0;
                    reader_.read(ep + 0x160 + 0x0C, px);
                    reader_.read(ep + 0x160 + 0x1C, py);
                    reader_.read(ep + 0x160 + 0x2C, pz);
                    if (!std::isnan(px)&&fabsf(px)>10&&fabsf(px)<2000&&
                        !std::isnan(py)&&fabsf(py)>10&&fabsf(py)<2000) {
                        uint64_t np=0;
                        reader_.read_ptr(ep+0x10,np);
                        char nm[32]={};
                        if(np>0x10000&&np<0x800000000000ULL)reader_.read_mem(np,nm,31);
                        printf("  IAT+0x%x: gEnv=0x%lx +0x%x->es=0x%lx cnt=%u ent[%u] pos=(%.0f,%.0f,%.0f) name='%s'\n",
                               ioff, iat_val, esoff, es, cnt, j, px, py, pz, nm);
                        genv_addr_ = gamehunt_base_ + iat_rva + ioff;
                        entsys_off_ = esoff;
                        printf("  -> FOUND! gEnv at IAT offset 0x%x\n", iat_rva + ioff);
                        return true;
                    }
                }
            }
        }

        uint64_t he_sz = 0;
        reader_.get_module_base(MOD_HUNTGAME, huntgame_base_, he_sz);
        printf("HuntGame.exe at 0x%lx\n", huntgame_base_);

        /* Try direct global pointer first */
        if (huntgame_base_) {
            uint64_t entsys = 0;
            reader_.read_ptr(huntgame_base_ + 0x2A40E90, entsys);
            printf("Global entitySys (qword_142A40E90): 0x%lx\n", entsys);
            if (entsys > 0x10000) {
                uint16_t max_idx = 0;
                reader_.read(entsys + 0x40092, max_idx);
                printf("  max_idx = %u\n", max_idx);
                if (max_idx > 0) {
                    uint64_t ep = 0;
                    reader_.read_ptr(entsys + 0x40098 + 8, ep);
                    printf("  entity[1] = 0x%lx\n", ep);
                    if (ep > 0x10000 && ep < 0x800000000000ULL) {
                        uint64_t np = 0;
                        reader_.read_ptr(ep + 0x10, np);
                        char nm[32] = {};
                        if (np > 0x10000 && np < 0x800000000000ULL)
                            reader_.read_mem(np, nm, 31);
                        printf("  entity[1] name: '%s'\n", nm);
                    }
                }
            }

            /* Search nearby globals for pSystem (CCore) which has CCamera at +0x280.
             * From IDA: qword_142A40E60 was used in Render() as qword_142A40E60.
             * That's at RVA 0x2A40E60. Also try nearby addresses. */
            printf("Searching HuntGame.exe globals for CCore/pSystem with camera...\n");
            for (uint32_t goff = 0x2A40E00; goff < 0x2A41000; goff += 8) {
                uint64_t ptr = 0;
                reader_.read_ptr(huntgame_base_ + goff, ptr);
                if (ptr < 0x10000 || ptr > 0x800000000000ULL) continue;

                /* Try reading CCamera at ptr + 0x280 */
                Matrix34 test_cam = {};
                reader_.read_mem(ptr + 0x280, &test_cam, sizeof(Matrix34));
                Vec3 test_pos = test_cam.get_translation();
                float test_fov = 0;
                reader_.read(ptr + 0x280 + 0x30, test_fov);

                /* Valid camera: position matches our known position, FOV reasonable */
                if (fabsf(test_pos.x) > 10 && fabsf(test_pos.x) < 2000 &&
                    fabsf(test_pos.y) > 10 && fabsf(test_pos.y) < 2000 &&
                    test_fov > 0.3f && test_fov < 2.5f) {
                    printf("  Global at +0x%x -> 0x%lx: cam280 pos=(%.0f,%.0f,%.0f) fov=%.2f\n",
                           goff, ptr, test_pos.x, test_pos.y, test_pos.z, test_fov);
                    /* Check if rotation differs from renderer camera */
                    float rowlen = sqrtf(test_cam.m[0][0]*test_cam.m[0][0]+
                                        test_cam.m[0][1]*test_cam.m[0][1]+
                                        test_cam.m[0][2]*test_cam.m[0][2]);
                    printf("    row0=(%.3f,%.3f,%.3f) len=%.3f\n",
                           test_cam.m[0][0], test_cam.m[0][1], test_cam.m[0][2], rowlen);
                }
            }
        }

        /* Fallback scan */
        if (!entsys_data_addr_)
            find_entity_system();
        return true;
    }

    bool update(GameState &state) {
        state.valid = false;
        state.entities.clear();
        diag_++;

        /* === CAMERA: 1 read === */
        bool cam_ok = read_camera(state);
        if (!cam_ok && (diag_ == 1 || diag_ % 300 == 0))
            cam_ok = scan_camera(state);

        /* === ENTITIES === */
        /* Read entsys pointer — 1 read */
        uint64_t entsys = 0;
        if (huntgame_base_)
            reader_.read_ptr(huntgame_base_ + 0x2A40E90, entsys);
        if (entsys < 0x10000 && entsys_data_addr_)
            reader_.read_ptr(entsys_data_addr_, entsys);
        if (entsys < 0x10000) goto done;

        {
            /* Read count + entire pointer array in one bulk read.
             * Layout: count(uint16) at +0x40092, array(uint64[]) at +0x40098.
             * Read from +0x40090 to include count, then array. */
            static constexpr uint32_t MAX_ENTS = 50000;
            static constexpr uint32_t HDR_OFF = 0x40090;
            static constexpr uint32_t ARR_OFF = 0x40098;
            uint8_t hdr[8] = {};
            reader_.read_mem(entsys + HDR_OFF, hdr, 8);
            uint16_t max_idx = *(uint16_t *)(hdr + 2); /* +0x40092 = HDR_OFF + 2 */
            uint32_t count = std::min((uint32_t)max_idx, MAX_ENTS);

            if (count == 0) goto done;

            /* Bulk read pointer array — 1 read (reuse buffer) */
            ent_ptrs_buf_.resize(count + 1);
            reader_.read_mem(entsys + ARR_OFF, ent_ptrs_buf_.data(), (count + 1) * 8);

            /* Invalidate caches periodically (entities respawn, char instances get reused) */
            if (diag_ % 1800 == 0) {
                ent_type_cache_.clear();
                head_bone_cache_.clear();
            }

            /* Process entities. For each valid pointer:
             * - If cached as SKIP: skip entirely (0 reads)
             * - If cached with type: read only worldTM for position (1 read)
             * - If not cached: read name_ptr (1 read) + worldTM (1 read) + name string (1 read) */
            for (uint32_t i = 0; i <= count && state.entities.size() < 500; i++) {
                uint64_t ep = ent_ptrs_buf_[i];
                if (ep < 0x10000 || ep > 0x800000000000ULL) continue;

                auto cached = ent_type_cache_.find(ep);
                if (cached != ent_type_cache_.end() && cached->second.type == ENT_UNKNOWN && !state.show_all) continue;

                /* Read position — always needed, 1 read */
                Matrix34 wtm = {};
                if (!reader_.read_mem(ep + 0x160, &wtm, sizeof(Matrix34))) continue;
                Vec3 pos = wtm.get_translation();
                if (std::isnan(pos.x) || pos.x == 0) continue;
                if (fabsf(pos.x) < 10 || fabsf(pos.x) > 2500) continue;
                if (fabsf(pos.y) < 10 || fabsf(pos.y) > 2500) continue;
                float dist = state.local_pos.distance(pos);
                if (dist < 3 || dist > 400) continue;

                if (cached != ent_type_cache_.end()) {
                    /* Known entity type — read head bone if possible */
                    EntityData ent = {};
                    ent.position = pos;
                    ent.type = cached->second.type;
                    ent.has_head = read_head_pos(ep, wtm, ent.head_pos);
                    strncpy(ent.name, cached->second.name, 63);
                    strncpy(ent.raw_name, cached->second.raw_name, 63);
                    state.entities.push_back(ent);
                } else {
                    /* Unknown entity — read name_ptr (1 read) + name string (1 read) */
                    uint64_t np = 0;
                    if (!reader_.read_ptr(ep + 0x10, np)) continue;

                    char name[64] = {};
                    if (np > 0x10000 && np < 0x800000000000ULL)
                        reader_.read_mem(np, name, 63);

                    EntityType type = ENT_UNKNOWN;
                    const char *display = "";

                    /* Skip known false positives (map geometry, triggers, particles, loot, dressing) */
                    bool skip = strstr(name,"BossArea") || strstr(name,"Proximity_Warning") ||
                                strstr(name,"_Shape") || strstr(name,"_Register") ||
                                strstr(name,"_Loot") || strstr(name,"_DroppedItem") ||
                                strstr(name,"particle") || strstr(name,"GruntWeapon") ||
                                strstr(name,"Water_Area") || strstr(name,"Swamp_Water") ||
                                strstr(name,"Dressing.") || strstr(name,"_Ravens") ||
                                strstr(name,"_Feathers_") || strstr(name,"_Gramophone");

                    if (!skip) {
                        /* Hunters — only actual player entities */
                        if (strstr(name,"HunterBasic") || strstr(name,"Hunter_")) { type = ENT_HUNTER; display = "Hunter"; }
                        /* Bosses */
                        else if (strstr(name,"Butcher"))    { type = ENT_BOSS; display = "Butcher"; }
                        else if (strstr(name,"Spider"))     { type = ENT_BOSS; display = "Spider"; }
                        else if (strstr(name,"Assassin"))   { type = ENT_BOSS; display = "Assassin"; }
                        else if (strstr(name,"Scrapbeak"))  { type = ENT_BOSS; display = "Scrapbeak"; }
                        else if (strstr(name,"Rotjaw"))     { type = ENT_BOSS; display = "Rotjaw"; }
                        else if (strstr(name,"Ursa"))       { type = ENT_BOSS; display = "Ursa"; }
                        /* Special grunts */
                        else if (strstr(name,"Immolator"))  { type = ENT_GRUNT; display = "Immolator"; }
                        else if (strstr(name,"Hive"))       { type = ENT_GRUNT; display = "Hive"; }
                        else if (strstr(name,"Meathead"))   { type = ENT_GRUNT; display = "Meathead"; }
                        else if (strstr(name,"Hellhound"))  { type = ENT_GRUNT; display = "Hellhound"; }
                        else if (strstr(name,"Armored"))    { type = ENT_GRUNT; display = "Armored"; }
                        else if (strstr(name,"WaterDevil")) { type = ENT_GRUNT; display = "Waterdevil"; }
                        /* Generic grunts (from behavior paths like "grunts/grunt/wander") */
                        else if (strstr(name,"grunts/grunt/") || strstr(name,"_Grunts")) { type = ENT_GRUNT; display = "Grunt"; }
                    }

                    CachedEnt ce = {}; ce.type = type; strncpy(ce.name, display, 63); strncpy(ce.raw_name, name, 63);
                    ent_type_cache_[ep] = ce;
                    if (type == ENT_UNKNOWN && !state.show_all) continue;
                    if (type == ENT_UNKNOWN) { display = name; } /* show_all: use raw name */

                    EntityData ent = {};
                    ent.position = pos;
                    ent.type = type;
                    ent.has_head = read_head_pos(ep, wtm, ent.head_pos);
                    strncpy(ent.name, display, 63);
                    strncpy(ent.raw_name, name, 63);
                    state.entities.push_back(ent);
                }
            }
        }

    done:
        /* Re-scan if we lost entities */
        if (state.entities.empty() && diag_ % 600 == 0)
            find_entity_system();


        state.valid = cam_ok;
        return cam_ok;
    }

private:
    uint32_t entsys_off_ = 0;
    uint32_t entity_name_off_ = 0x10;

    void find_entity_system() {
        uint64_t he_base = 0, he_size = 0;
        reader_.get_module_base(MOD_HUNTGAME, he_base, he_size);
        if (!he_base) return;
        uint64_t he_data = he_base + 0x2A3C000;
        uint64_t he_data_size = 0xB6F000;
        printf("Re-scanning for entity system...\n");
        for (uint64_t doff = 0; doff < he_data_size; doff += 8) {
            uint64_t val = 0;
            reader_.read_ptr(he_data + doff, val);
            if (val < 0x10000 || val > 0x800000000000ULL) continue;
            uint32_t cnt = 0;
            reader_.read(val + 0x40092, cnt);
            if (cnt < 50 || cnt > 20000) continue;
            int with_pos = 0;
            for (uint32_t j = 0; j < std::min(cnt, 300u) && with_pos < 3; j++) {
                uint64_t ep = 0;
                reader_.read_ptr(val + 0x40098 + j * 8, ep);
                if (ep < 0x10000 || ep > 0x800000000000ULL) continue;
                float px=0,py=0,pz=0;
                reader_.read(ep + 0x160 + 0x0C, px);
                reader_.read(ep + 0x160 + 0x1C, py);
                reader_.read(ep + 0x160 + 0x2C, pz);
                if (!std::isnan(px)&&fabsf(px)>50&&fabsf(px)<2000&&
                    !std::isnan(py)&&fabsf(py)>50&&fabsf(py)<2000&&
                    !std::isnan(pz)&&fabsf(pz)>0.1f&&fabsf(pz)<200)
                    with_pos++;
            }
            if (with_pos >= 3) {
                entsys_data_addr_ = he_data + doff;
                direct_entsys_ = val;
                printf("  Found entity system at .data+0x%lx -> 0x%lx cnt=%u\n", doff, val, cnt);
                return;
            }
        }
    }

    bool read_camera(GameState &state) {
        if (!cam_base_) return false;

        /* Single bulk read of entire CCamera struct (784 bytes).
         * Contains: Matrix34 at +0x00, FOV at +0x30, ViewMatrix at +0x230,
         * ProjectionMatrix at +0x270, Position at +0x2F0 */
        uint8_t cam_buf[CAMERA_STRUCT_SIZE] = {};
        if (!reader_.read_mem(cam_base_ + cam_offset_, cam_buf, CAMERA_STRUCT_SIZE))
            return false;

        float fov = *(float *)(cam_buf + 0x30);
        if (fov < 0.2f || fov > 3.0f) return false;

        /* Position from +0x2F0 */
        memcpy(&state.local_pos, cam_buf + 0x2F0, sizeof(Vec3));

        /* Fallback to Matrix34 translation if +0x2F0 is garbage */
        if (std::isnan(state.local_pos.x) || fabsf(state.local_pos.x) < 1.0f) {
            Matrix34 *cam34 = (Matrix34 *)cam_buf;
            state.local_pos = cam34->get_translation();
        }

        /* ViewMatrix and ProjectionMatrix */
        memcpy(&state.render_mat, cam_buf + 0x230, sizeof(Matrix44));
        memcpy(&state.proj_mat, cam_buf + 0x270, sizeof(Matrix44));


        return true;
    }

    bool scan_camera(GameState &state) {
        uint64_t render_base = 0, render_size = 0;
        if (!reader_.get_module_base(MOD_CRYRENDER, render_base, render_size)) return false;
        uint16_t mz = 0; reader_.read(render_base, mz);
        if (mz != 0x5A4D) return false;
        uint32_t pe_off = 0; reader_.read(render_base + 0x3C, pe_off);
        uint16_t num_sects = 0, opt_sz = 0;
        reader_.read(render_base + pe_off + 6, num_sects);
        reader_.read(render_base + pe_off + 20, opt_sz);
        uint64_t sh = render_base + pe_off + 24 + opt_sz;
        for (int s = 0; s < num_sects; s++) {
            char sn[9]={}; reader_.read_mem(sh+s*40, sn, 8);
            uint32_t va=0,vs=0; reader_.read(sh+s*40+12,va); reader_.read(sh+s*40+8,vs);
            if (strcmp(sn,".data")!=0) continue;
            uint64_t sect=render_base+va;
            uint64_t len=std::min((uint64_t)vs,(uint64_t)0x200000);
            for (uint64_t off=0x30;off<len;off+=4) {
                float fov=0; reader_.read(sect+off,fov);
                if (fov<0.4f||fov>2.0f) continue;
                float px=0,py=0,pz=0;
                reader_.read(sect+off-0x24,px);reader_.read(sect+off-0x14,py);reader_.read(sect+off-0x04,pz);
                if(fabsf(px)<50||fabsf(py)<50||fabsf(px)>2000||fabsf(py)>2000) continue;
                float r0=0,r1=0,r2=0;
                reader_.read(sect+off-0x30,r0);reader_.read(sect+off-0x2C,r1);reader_.read(sect+off-0x28,r2);
                float l=sqrtf(r0*r0+r1*r1+r2*r2);
                if(l<0.9f||l>1.1f) continue;
                printf("Camera at CryRender .data+0x%lx pos=(%.0f,%.0f,%.0f) fov=%.2f\n",off-0x30,px,py,pz,fov);
                cam_base_=sect; cam_offset_=off-0x30;
                state.local_pos={px,py,pz};
                /* Read ViewMatrix and ProjectionMatrix from CCamera */
                reader_.read_mem(cam_base_ + cam_offset_ + 0x230, &state.render_mat, sizeof(Matrix44));
                reader_.read_mem(cam_base_ + cam_offset_ + 0x270, &state.proj_mat, sizeof(Matrix44));
                return true;
            }
        }
        return false;
    }

    /* Skeleton bone reading for head position */
    static constexpr uint64_t OFF_SLOT_CHARINSTANCE = 0x88;
    static constexpr uint64_t OFF_CHAR_SKELPOSE     = 0xC80;
    static constexpr uint64_t OFF_SKELPOSE_BONES    = 0x38;  /* aligned bone array (was 0x20 base, broken) */
    static constexpr uint64_t OFF_CHAR_DEFSKEL      = 0x1C0;
    static constexpr uint64_t OFF_DEFSKEL_JOINTS    = 0x8;
    static constexpr uint64_t OFF_DEFSKEL_JOINTCNT  = 0xA0;
    static constexpr uint64_t BONE_STRUCT_SIZE      = 0x20;  /* QuatTS: Quat(16) + Vec3(12) + float scale(4) */
    static constexpr uint64_t BONE_POS_OFFSET       = 0x10;

    /* Check if bone name is the actual head bone (not an IK target) */
    static bool is_real_head_bone(const char *name) {
        if (strcmp(name, "head") == 0 || strcmp(name, "Head") == 0) return true;
        if (strcmp(name, "Bip01 Head") == 0) return true;
        if (strcmp(name, "face_head") == 0) return true;
        if (strcmp(name, "def_head") == 0) return true;
        if (strcmp(name, "skull") == 0) return true;
        return false;
    }

    /* Fallback: contains "head" but not an IK target/hand bone */
    static bool is_fallback_head_bone(const char *name) {
        if (!strstr(name, "Head") && !strstr(name, "head")) return false;
        /* Reject IK targets, hand references */
        if (strstr(name, "target")) return false;
        if (strstr(name, "Target")) return false;
        if (strstr(name, "hand")) return false;
        if (strstr(name, "Hand")) return false;
        if (strstr(name, "RT_")) return false;
        if (strstr(name, "IK_")) return false;
        return true;
    }

    int find_head_bone_index(uint64_t char_instance) {
        uint64_t def_skel = 0;
        if (!reader_.read_ptr(char_instance + OFF_CHAR_DEFSKEL, def_skel)) {
            BONE_LOG("BONE: FAIL read defskel ptr at char_inst+0x%lx\n", OFF_CHAR_DEFSKEL);
            /* Try alternate offsets for IDefaultSkeleton */
            for (uint64_t off : {0x1B8UL, 0x1C8UL, 0x1D0UL, 0x1B0UL, 0x200UL}) {
                reader_.read_ptr(char_instance + off, def_skel);
                if (def_skel > 0x10000 && def_skel < 0x800000000000ULL) {
                    uint32_t tc = 0; reader_.read(def_skel + OFF_DEFSKEL_JOINTCNT, tc);
                    if (tc > 5 && tc < 300) {
                        BONE_LOG("BONE: Found defskel at alt offset +0x%lx: 0x%lx cnt=%u\n", off, def_skel, tc);
                        break;
                    }
                }
                def_skel = 0;
            }
            if (!def_skel) return -1;
        }
        if (def_skel < 0x10000 || def_skel > 0x800000000000ULL) {
            BONE_LOG("BONE: defskel=0x%lx invalid\n", def_skel);
            return -1;
        }

        uint32_t joint_count = 0;
        reader_.read(def_skel + OFF_DEFSKEL_JOINTCNT, joint_count);
        if (joint_count == 0 || joint_count > 500) {
            BONE_LOG("BONE: joint_count=%u invalid (defskel=0x%lx)\n", joint_count, def_skel);
            /* Try alternate count offsets */
            for (uint64_t off : {0x98UL, 0xA8UL, 0xB0UL, 0x90UL}) {
                reader_.read(def_skel + off, joint_count);
                if (joint_count > 5 && joint_count < 300) {
                    BONE_LOG("BONE: Found joint count %u at defskel+0x%lx\n", joint_count, off);
                    break;
                }
                joint_count = 0;
            }
            if (joint_count == 0 || joint_count > 500) return -1;
        }

        uint64_t joints_ptr = 0;
        if (!reader_.read_ptr(def_skel + OFF_DEFSKEL_JOINTS, joints_ptr) ||
            joints_ptr < 0x10000 || joints_ptr > 0x800000000000ULL) {
            BONE_LOG("BONE: joints_ptr invalid at defskel+0x%lx\n", OFF_DEFSKEL_JOINTS);
            return -1;
        }

        BONE_LOG("BONE: Scanning %u joints at 0x%lx (defskel=0x%lx)\n", joint_count, joints_ptr, def_skel);

        for (uint32_t stride : {0x150U, 0x120U, 0x100U, 0xD8U, 0x98U}) {
            /* Validate stride: check that first few entries have valid name pointers */
            int valid_names = 0;
            for (uint32_t i = 0; i < std::min(joint_count, 5U); i++) {
                uint64_t np = 0;
                reader_.read_ptr(joints_ptr + i * stride, np);
                if (np > 0x10000 && np < 0x800000000000ULL) {
                    char test[8] = {};
                    reader_.read_mem(np, test, 7);
                    if (test[0] >= ' ' && test[0] <= 'z') valid_names++;
                }
            }
            if (valid_names < 2) continue; /* wrong stride */

            /* Two-pass: first exact match, then fallback */
            static int full_dump_count = 0;
            bool do_full_dump = (full_dump_count < 3); /* dump all bones for first 3 failing skeletons */
            int fallback = -1;
            int neck_idx = -1;
            for (uint32_t i = 0; i < joint_count; i++) {
                uint64_t name_ptr = 0;
                reader_.read_ptr(joints_ptr + i * stride, name_ptr);
                if (name_ptr < 0x10000 || name_ptr > 0x800000000000ULL) continue;
                char jname[48] = {};
                reader_.read_mem(name_ptr, jname, 47);

                /* Dump first 10 bone names always */
                if (i < 10)
                    BONE_LOG("  bone[%u] = '%s'\n", i, jname);

                if (is_real_head_bone(jname)) {
                    BONE_LOG("BONE: Found head bone '%s' at index %u (stride=0x%x)\n", jname, i, stride);
                    return (int)i;
                }
                if (fallback < 0 && is_fallback_head_bone(jname))
                    fallback = (int)i;
                /* Track neck/spine as last resort */
                if (neck_idx < 0 && (strcmp(jname, "neck") == 0 ||
                    strcmp(jname, "Neck") == 0 || strcmp(jname, "Bip01 Neck") == 0 ||
                    strcmp(jname, "spine03") == 0 || strcmp(jname, "Spine03") == 0))
                    neck_idx = (int)i;
            }
            if (fallback >= 0) {
                BONE_LOG("BONE: Using fallback head bone at index %d (stride=0x%x)\n", fallback, stride);
                return fallback;
            }
            /* Full dump for failing skeletons */
            if (do_full_dump) {
                full_dump_count++;
                BONE_LOG("BONE: FULL DUMP of %u joints (stride=0x%x):\n", joint_count, stride);
                for (uint32_t i = 10; i < joint_count; i++) {
                    uint64_t np2 = 0;
                    reader_.read_ptr(joints_ptr + i * stride, np2);
                    if (np2 < 0x10000 || np2 > 0x800000000000ULL) continue;
                    char jn2[48] = {};
                    reader_.read_mem(np2, jn2, 47);
                    if (jn2[0]) BONE_LOG("  bone[%u] = '%s'\n", i, jn2);
                }
            }
            if (neck_idx >= 0) {
                BONE_LOG("BONE: No head bone, using neck at index %d as fallback\n", neck_idx);
                return neck_idx; /* neck is close enough */
            }
            BONE_LOG("BONE: Valid stride 0x%x but no head bone found in %u joints\n", stride, joint_count);
            return -1;
        }

        return -1;
    }

    /* Cache key for positional head bone: defskel -> bone index found by position */
    static constexpr int BONE_IDX_SEARCHING = -2; /* sentinel: positional search not done yet */

    bool read_head_pos(uint64_t ep, const Matrix34 &wtm, Vec3 &out) {
        uint64_t slots_ptr = 0;
        if (!reader_.read_ptr(ep + 0xA8, slots_ptr)) return false;
        if (slots_ptr < 0x10000 || slots_ptr > 0x800000000000ULL) return false;

        uint64_t slot0 = 0;
        if (!reader_.read_ptr(slots_ptr, slot0)) return false;
        if (slot0 < 0x10000 || slot0 > 0x800000000000ULL) return false;

        uint64_t char_inst = 0;
        if (!reader_.read_ptr(slot0 + OFF_SLOT_CHARINSTANCE, char_inst)) return false;
        if (char_inst < 0x10000 || char_inst > 0x800000000000ULL) return false;

        uint64_t bone_arr = 0;
        if (!reader_.read_ptr(char_inst + OFF_CHAR_SKELPOSE + OFF_SKELPOSE_BONES, bone_arr)) return false;
        if (bone_arr < 0x10000 || bone_arr > 0x800000000000ULL) return false;

        /* Find head bone index (cached per char_inst) */
        int head_idx = -1;
        auto it = head_bone_cache_.find(char_inst);
        if (it != head_bone_cache_.end()) {
            head_idx = it->second;
        } else {
            head_idx = find_head_bone_index(char_inst);
            if (head_idx < 0) {
                /* Positional fallback: highest Z bone in model space */
                uint64_t def_skel = 0;
                reader_.read_ptr(char_inst + OFF_CHAR_DEFSKEL, def_skel);
                uint32_t jcount = 0;
                if (def_skel > 0x10000) {
                    reader_.read(def_skel + OFF_DEFSKEL_JOINTCNT, jcount);
                    if (jcount > 500) jcount = 0;
                }
                if (jcount > 0) {
                    uint32_t arr_size = jcount * BONE_STRUCT_SIZE;
                    if (arr_size > 0x10000) arr_size = 0x10000;
                    std::vector<uint8_t> bone_data(arr_size);
                    if (reader_.read_mem(bone_arr, bone_data.data(), arr_size)) {
                        float best_z = -999.0f;
                        int best_idx = -1;
                        for (uint32_t i = 1; i < jcount; i++) {
                            uint32_t off = i * BONE_STRUCT_SIZE + BONE_POS_OFFSET;
                            if (off + 12 > arr_size) break;
                            Vec3 *bp = (Vec3 *)(bone_data.data() + off);
                            if (!std::isnan(bp->z) && bp->z > 1.4f && bp->z < 2.2f &&
                                fabsf(bp->x) < 0.3f && fabsf(bp->y) < 0.3f &&
                                bp->z > best_z) {
                                best_z = bp->z;
                                best_idx = (int)i;
                            }
                        }
                        if (best_idx >= 0) head_idx = best_idx;
                    }
                }
            }
            head_bone_cache_[char_inst] = head_idx;
        }
        if (head_idx < 0) return false;

        Vec3 model_pos = {};
        if (!reader_.read_mem(bone_arr + head_idx * BONE_STRUCT_SIZE + BONE_POS_OFFSET,
                              &model_pos, sizeof(Vec3))) return false;
        if (std::isnan(model_pos.x) || fabsf(model_pos.x) > 50) return false;

        out.x = wtm.m[0][0]*model_pos.x + wtm.m[0][1]*model_pos.y + wtm.m[0][2]*model_pos.z + wtm.m[0][3];
        out.y = wtm.m[1][0]*model_pos.x + wtm.m[1][1]*model_pos.y + wtm.m[1][2]*model_pos.z + wtm.m[1][3];
        out.z = wtm.m[2][0]*model_pos.x + wtm.m[2][1]*model_pos.y + wtm.m[2][2]*model_pos.z + wtm.m[2][3];
        return true;
    }

    MemReader reader_;
    int32_t pid_;
    uint64_t gamehunt_base_, gamehunt_size_, genv_addr_, direct_entsys_, entsys_data_addr_, huntgame_base_;
    uint64_t cam_base_;
    uint32_t cam_offset_;
    int diag_;
    struct CachedEnt { EntityType type; char name[64]; char raw_name[64]; };
    std::unordered_map<uint64_t, CachedEnt> ent_type_cache_;
    std::unordered_map<uint64_t, int> head_bone_cache_;
    std::vector<uint64_t> ent_ptrs_buf_; /* reusable buffer for entity pointer array */
};
