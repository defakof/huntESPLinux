#pragma once

#include <cstdint>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>
#include <algorithm>
#include <unordered_map>

#include "mem_reader.h"
#include "../include/game_offsets.h"

#define BONE_DEBUG 0
#if BONE_DEBUG
#define BONE_LOG(...) printf(__VA_ARGS__)
#else
#define BONE_LOG(...) ((void)0)
#endif

static constexpr uint64_t PTR_MIN = 0x10000;
static constexpr uint64_t PTR_MAX = 0x800000000000ULL;

static inline bool is_valid_ptr(uint64_t ptr) {
    return ptr >= PTR_MIN && ptr <= PTR_MAX;
}

struct Vec3 {
    float x, y, z;

    float distance(const Vec3 &o) const {
        float dx = x - o.x, dy = y - o.y, dz = z - o.z;
        return sqrtf(dx * dx + dy * dy + dz * dz);
    }
};

struct Matrix34 {
    float m[3][4];

    Vec3 get_translation() const {
        return {m[0][3], m[1][3], m[2][3]};
    }

    Vec3 transform_point(const Vec3 &p) const {
        return {
            m[0][0] * p.x + m[0][1] * p.y + m[0][2] * p.z + m[0][3],
            m[1][0] * p.x + m[1][1] * p.y + m[1][2] * p.z + m[1][3],
            m[2][0] * p.x + m[2][1] * p.y + m[2][2] * p.z + m[2][3],
        };
    }
};

struct Matrix44 {
    float m[4][4];
};

enum EntityType {
    ENT_UNKNOWN = 0,
    ENT_HUNTER,
    ENT_BOSS,
    ENT_GRUNT,
};

struct EntityData {
    Vec3 position;
    Vec3 head_pos;
    bool has_head;
    EntityType type;
    char name[64];
    char raw_name[64];
};

struct GameState {
    Vec3 local_pos;
    Matrix44 render_mat;
    Matrix44 proj_mat;
    Matrix44 view_proj;
    std::vector<EntityData> entities;
    bool valid;
    int screen_w, screen_h;
    bool show_all;
};

class GameData {
public:
    GameData() = default;

    bool init(int32_t pid) {
        pid_ = pid;
        if (!reader_.open()) {
            fprintf(stderr, "Failed to open %s\n", HUNT_DEVICE_PATH);
            return false;
        }
        if (!reader_.set_pid(pid)) {
            fprintf(stderr, "Failed to set PID %d\n", pid);
            return false;
        }
        if (!reader_.get_module_base(MOD_GAMEHUNT, gamehunt_base_, gamehunt_size_)) {
            fprintf(stderr, "Failed to find %s\n", MOD_GAMEHUNT);
            return false;
        }
        printf("Found %s at 0x%lx\n", MOD_GAMEHUNT, gamehunt_base_);

        /*
         * Resolve gEnv from GameHunt.dll IAT.
         * On Wine/Proton the .rdata reference at OFF_GENV is not relocated,
         * so we scan the IAT for a pointer whose entity system yields valid
         * entity positions. See OFFSETS_AND_METHODS.md for details.
         */
        if (resolve_genv_from_iat())
            return true;

        /* Fallback: try HuntGame.exe globals directly */
        uint64_t he_sz = 0;
        reader_.get_module_base(MOD_HUNTGAME, huntgame_base_, he_sz);
        printf("HuntGame.exe at 0x%lx\n", huntgame_base_);

        if (huntgame_base_)
            probe_huntgame_globals();

        if (!entsys_data_addr_)
            find_entity_system();
        return true;
    }

    bool update(GameState &state) {
        state.valid = false;
        state.entities.clear();
        diag_++;

        bool cam_ok = read_camera(state);
        if (!cam_ok && (diag_ == 1 || diag_ % 300 == 0))
            cam_ok = scan_camera(state);

        uint64_t entsys = 0;
        if (huntgame_base_)
            reader_.read_ptr(huntgame_base_ + 0x2A40E90, entsys);
        if (!is_valid_ptr(entsys) && entsys_data_addr_)
            reader_.read_ptr(entsys_data_addr_, entsys);
        if (!is_valid_ptr(entsys))
            goto done;

        read_entities(state, entsys);

    done:
        if (state.entities.empty() && diag_ % 600 == 0)
            find_entity_system();

        state.valid = cam_ok;
        return cam_ok;
    }

private:
    /* Skeleton/bone offsets (CEntity -> CEntitySlot -> ICharacterInstance -> bones) */
    static constexpr uint64_t OFF_SLOT_CHARINSTANCE = 0x88;
    static constexpr uint64_t OFF_CHAR_SKELPOSE     = 0xC80;
    static constexpr uint64_t OFF_SKELPOSE_BONES    = 0x38;
    static constexpr uint64_t OFF_CHAR_DEFSKEL      = 0x1C0;
    static constexpr uint64_t OFF_DEFSKEL_JOINTS    = 0x8;
    static constexpr uint64_t OFF_DEFSKEL_JOINTCNT  = 0xA0;
    static constexpr uint64_t BONE_STRIDE           = 0x20; /* QuatTS: Quat(16) + Vec3(12) + float(4) */
    static constexpr uint64_t BONE_POS_OFFSET       = 0x10;

    static constexpr int BONE_IDX_SEARCHING = -2;

    static constexpr uint32_t MAX_ENTS        = 50000;
    static constexpr uint32_t MAX_RENDER_ENTS = 500;
    static constexpr uint32_t ENTSYS_HDR_OFF  = 0x40090;
    static constexpr uint32_t ENTSYS_ARR_OFF  = 0x40098;

    struct CachedEnt {
        EntityType type;
        char name[64];
        char raw_name[64];
    };

    /* Entity name classification tables */
    static constexpr const char *SKIP_PATTERNS[] = {
        "BossArea", "Proximity_Warning", "_Shape", "_Register",
        "_Loot", "_DroppedItem", "particle", "GruntWeapon",
        "Water_Area", "Swamp_Water", "Dressing.", "_Ravens",
        "_Feathers_", "_Gramophone", "Menu", "Lobby", "Hitbox",
    };

    struct EntityClassRule {
        const char *pattern;
        EntityType type;
        const char *display;
    };

    static constexpr EntityClassRule ENTITY_RULES[] = {
        {"HunterBasic", ENT_HUNTER, "Hunter"},
        {"Hunter_",     ENT_HUNTER, "Hunter"},
        {"Butcher",     ENT_BOSS,   "Butcher"},
        {"Spider",      ENT_BOSS,   "Spider"},
        {"Assassin",    ENT_BOSS,   "Assassin"},
        {"Scrapbeak",   ENT_BOSS,   "Scrapbeak"},
        {"Rotjaw",      ENT_BOSS,   "Rotjaw"},
        {"Ursa",        ENT_BOSS,   "Ursa"},
        {"Immolator",   ENT_GRUNT,  "Immolator"},
        {"Hive",        ENT_GRUNT,  "Hive"},
        {"Meathead",    ENT_GRUNT,  "Meathead"},
        {"Hellhound",   ENT_GRUNT,  "Hellhound"},
        {"Armored",     ENT_GRUNT,  "Armored"},
        {"WaterDevil",  ENT_GRUNT,  "Waterdevil"},
        {"grunts/grunt/", ENT_GRUNT, "Grunt"},
        {"_Grunts",     ENT_GRUNT,  "Grunt"},
    };

    static constexpr const char *HEAD_BONE_NAMES[] = {
        "head", "Head", "Bip01 Head", "face_head", "def_head", "skull",
    };

    static constexpr const char *NECK_BONE_NAMES[] = {
        "neck", "Neck", "Bip01 Neck", "spine03", "Spine03",
    };

    /* Member variables */
    MemReader reader_;
    int32_t pid_ = 0;
    uint64_t gamehunt_base_ = 0;
    uint64_t gamehunt_size_ = 0;
    uint64_t genv_addr_ = 0;
    uint64_t direct_entsys_ = 0;
    uint64_t entsys_data_addr_ = 0;
    uint64_t huntgame_base_ = 0;
    uint64_t cam_base_ = 0;
    uint32_t cam_offset_ = 0;
    uint32_t entsys_off_ = 0;
    int diag_ = 0;

    std::unordered_map<uint64_t, CachedEnt> ent_type_cache_;
    std::unordered_map<uint64_t, int> head_bone_cache_;
    std::vector<uint64_t> ent_ptrs_buf_;

    /* --- Initialization helpers --- */

    bool resolve_genv_from_iat() {
        uint32_t pe_off = 0;
        reader_.read(gamehunt_base_ + 0x3C, pe_off);
        uint64_t pe_imagebase = 0;
        reader_.read(gamehunt_base_ + pe_off + 24 + 24, pe_imagebase);
        printf("PE ImageBase: 0x%lx, load base: 0x%lx\n", pe_imagebase, gamehunt_base_);

        uint64_t raw_val = 0;
        reader_.read_ptr(gamehunt_base_ + OFF_GENV, raw_val);
        printf("Raw at +0x%x: 0x%lx\n", OFF_GENV, raw_val);

        uint64_t opt_start = gamehunt_base_ + pe_off + 24;
        uint32_t import_rva = 0, import_size = 0;
        reader_.read(opt_start + 120, import_rva);
        reader_.read(opt_start + 124, import_size);
        printf("Import directory: RVA=0x%x size=0x%x\n", import_rva, import_size);

        uint32_t iat_rva = 0, iat_size = 0;
        reader_.read(opt_start + 192, iat_rva);
        reader_.read(opt_start + 196, iat_size);
        printf("IAT: RVA=0x%x size=0x%x\n", iat_rva, iat_size);

        if (OFF_GENV >= iat_rva && OFF_GENV < iat_rva + iat_size)
            printf("OFF_GENV is inside IAT\n");
        else
            printf("OFF_GENV is NOT in IAT (range: 0x%x-0x%x)\n", iat_rva, iat_rva + iat_size);

        /* Scan IAT entries for a valid gEnv pointer */
        printf("Scanning IAT for gEnv...\n");
        for (uint32_t ioff = 0; ioff < iat_size; ioff += 8) {
            uint64_t iat_val = 0;
            reader_.read_ptr(gamehunt_base_ + iat_rva + ioff, iat_val);
            if (!is_valid_ptr(iat_val)) continue;

            /* Probe known entity system offsets from gEnv candidate */
            for (uint32_t esoff : {0xC0U, 0xD8U, 0xE0U, 0x90U}) {
                uint64_t es = 0;
                reader_.read_ptr(iat_val + esoff, es);
                if (!is_valid_ptr(es)) continue;

                uint32_t cnt = 0;
                reader_.read(es + OFF_ENTITY_COUNT, cnt);
                if (cnt < 10 || cnt > 50000) continue;

                /* Validate by checking entity positions */
                if (validate_entity_system(es, cnt)) {
                    genv_addr_ = gamehunt_base_ + iat_rva + ioff;
                    entsys_off_ = esoff;
                    printf("Found gEnv at IAT+0x%x (offset +0x%x)\n", ioff, iat_rva + ioff);
                    return true;
                }
            }
        }
        return false;
    }

    bool validate_entity_system(uint64_t es, uint32_t cnt) {
        for (uint32_t j = 0; j < std::min(cnt, 100u); j++) {
            uint64_t ep = 0;
            reader_.read_ptr(es + OFF_ENTITY_LIST + j * 8, ep);
            if (!is_valid_ptr(ep)) continue;

            float px = 0, py = 0, pz = 0;
            reader_.read(ep + OFF_ENTITY_WORLDTM + 0x0C, px);
            reader_.read(ep + OFF_ENTITY_WORLDTM + 0x1C, py);
            reader_.read(ep + OFF_ENTITY_WORLDTM + 0x2C, pz);

            if (!std::isnan(px) && fabsf(px) > 10 && fabsf(px) < 2000 &&
                !std::isnan(py) && fabsf(py) > 10 && fabsf(py) < 2000) {
                uint64_t np = 0;
                reader_.read_ptr(ep + OFF_ENTITY_NAME, np);
                char nm[32] = {};
                if (is_valid_ptr(np))
                    reader_.read_mem(np, nm, sizeof(nm) - 1);
                printf("  gEnv candidate: es=0x%lx cnt=%u ent[%u] pos=(%.0f,%.0f,%.0f) name='%s'\n",
                       es, cnt, j, px, py, pz, nm);
                return true;
            }
        }
        return false;
    }

    void probe_huntgame_globals() {
        /* Try direct entity system global (HuntGame.exe+0x2A40E90) */
        uint64_t entsys = 0;
        reader_.read_ptr(huntgame_base_ + 0x2A40E90, entsys);
        printf("Global entitySys (qword_142A40E90): 0x%lx\n", entsys);
        if (is_valid_ptr(entsys)) {
            uint16_t max_idx = 0;
            reader_.read(entsys + OFF_ENTITY_COUNT, max_idx);
            printf("  max_idx = %u\n", max_idx);
            if (max_idx > 0) {
                uint64_t ep = 0;
                reader_.read_ptr(entsys + ENTSYS_ARR_OFF + 8, ep);
                printf("  entity[1] = 0x%lx\n", ep);
                if (is_valid_ptr(ep)) {
                    uint64_t np = 0;
                    reader_.read_ptr(ep + OFF_ENTITY_NAME, np);
                    char nm[32] = {};
                    if (is_valid_ptr(np))
                        reader_.read_mem(np, nm, sizeof(nm) - 1);
                    printf("  entity[1] name: '%s'\n", nm);
                }
            }
        }

        /* Scan nearby globals for CCore/pSystem (contains CCamera at +0x280) */
        printf("Searching HuntGame.exe globals for CCore/pSystem with camera...\n");
        for (uint32_t goff = 0x2A40E00; goff < 0x2A41000; goff += 8) {
            uint64_t ptr = 0;
            reader_.read_ptr(huntgame_base_ + goff, ptr);
            if (!is_valid_ptr(ptr)) continue;

            Matrix34 test_cam = {};
            reader_.read_mem(ptr + OFF_SYSTEM_CAMERA, &test_cam, sizeof(Matrix34));
            Vec3 test_pos = test_cam.get_translation();
            float test_fov = 0;
            reader_.read(ptr + OFF_SYSTEM_CAMERA + OFF_CAMERA_FOV, test_fov);

            if (fabsf(test_pos.x) > 10 && fabsf(test_pos.x) < 2000 &&
                fabsf(test_pos.y) > 10 && fabsf(test_pos.y) < 2000 &&
                test_fov > 0.3f && test_fov < 2.5f) {
                printf("  Global at +0x%x -> 0x%lx: cam pos=(%.0f,%.0f,%.0f) fov=%.2f\n",
                       goff, ptr, test_pos.x, test_pos.y, test_pos.z, test_fov);
                float rowlen = sqrtf(test_cam.m[0][0] * test_cam.m[0][0] +
                                     test_cam.m[0][1] * test_cam.m[0][1] +
                                     test_cam.m[0][2] * test_cam.m[0][2]);
                printf("    row0=(%.3f,%.3f,%.3f) len=%.3f\n",
                       test_cam.m[0][0], test_cam.m[0][1], test_cam.m[0][2], rowlen);
            }
        }
    }

    /* --- Entity system --- */

    void find_entity_system() {
        uint64_t he_base = 0, he_size = 0;
        reader_.get_module_base(MOD_HUNTGAME, he_base, he_size);
        if (!he_base) return;

        uint64_t data_start = he_base + 0x2A3C000;
        uint64_t data_size = 0xB6F000;
        printf("Re-scanning for entity system...\n");

        for (uint64_t doff = 0; doff < data_size; doff += 8) {
            uint64_t val = 0;
            reader_.read_ptr(data_start + doff, val);
            if (!is_valid_ptr(val)) continue;

            uint32_t cnt = 0;
            reader_.read(val + OFF_ENTITY_COUNT, cnt);
            if (cnt < 50 || cnt > 20000) continue;

            int with_pos = 0;
            for (uint32_t j = 0; j < std::min(cnt, 300u) && with_pos < 3; j++) {
                uint64_t ep = 0;
                reader_.read_ptr(val + ENTSYS_ARR_OFF + j * 8, ep);
                if (!is_valid_ptr(ep)) continue;

                float px = 0, py = 0, pz = 0;
                reader_.read(ep + OFF_ENTITY_WORLDTM + 0x0C, px);
                reader_.read(ep + OFF_ENTITY_WORLDTM + 0x1C, py);
                reader_.read(ep + OFF_ENTITY_WORLDTM + 0x2C, pz);

                if (!std::isnan(px) && fabsf(px) > 50 && fabsf(px) < 2000 &&
                    !std::isnan(py) && fabsf(py) > 50 && fabsf(py) < 2000 &&
                    !std::isnan(pz) && fabsf(pz) > 0.1f && fabsf(pz) < 200)
                    with_pos++;
            }
            if (with_pos >= 3) {
                entsys_data_addr_ = data_start + doff;
                direct_entsys_ = val;
                printf("  Found entity system at .data+0x%lx -> 0x%lx cnt=%u\n",
                       doff, val, cnt);
                return;
            }
        }
    }

    void read_entities(GameState &state, uint64_t entsys) {
        uint8_t hdr[8] = {};
        reader_.read_mem(entsys + ENTSYS_HDR_OFF, hdr, sizeof(hdr));
        uint16_t max_idx = *(uint16_t *)(hdr + 2);
        uint32_t count = std::min((uint32_t)max_idx, MAX_ENTS);
        if (count == 0) return;

        ent_ptrs_buf_.resize(count + 1);
        reader_.read_mem(entsys + ENTSYS_ARR_OFF, ent_ptrs_buf_.data(), (count + 1) * 8);

        /* Invalidate caches periodically (entity respawns, instance reuse) */
        if (diag_ % 1800 == 0) {
            ent_type_cache_.clear();
            head_bone_cache_.clear();
        }

        for (uint32_t i = 0; i <= count && state.entities.size() < MAX_RENDER_ENTS; i++) {
            uint64_t ep = ent_ptrs_buf_[i];
            if (!is_valid_ptr(ep)) continue;

            auto cached = ent_type_cache_.find(ep);
            if (cached != ent_type_cache_.end() &&
                cached->second.type == ENT_UNKNOWN && !state.show_all)
                continue;

            Matrix34 wtm = {};
            if (!reader_.read_mem(ep + OFF_ENTITY_WORLDTM, &wtm, sizeof(Matrix34)))
                continue;
            Vec3 pos = wtm.get_translation();
            if (std::isnan(pos.x) || pos.x == 0) continue;
            if (fabsf(pos.x) < 10 || fabsf(pos.x) > 2500) continue;
            if (fabsf(pos.y) < 10 || fabsf(pos.y) > 2500) continue;
            float dist = state.local_pos.distance(pos);
            if (dist < 3 || dist > 400) continue;

            if (cached != ent_type_cache_.end()) {
                EntityData ent = {};
                ent.position = pos;
                ent.type = cached->second.type;
                ent.has_head = read_head_pos(ep, wtm, ent.head_pos);
                strncpy(ent.name, cached->second.name, sizeof(ent.name) - 1);
                strncpy(ent.raw_name, cached->second.raw_name, sizeof(ent.raw_name) - 1);
                state.entities.push_back(ent);
            } else {
                process_new_entity(state, ep, wtm, pos);
            }
        }
    }

    void process_new_entity(GameState &state, uint64_t ep, const Matrix34 &wtm, const Vec3 &pos) {
        uint64_t np = 0;
        if (!reader_.read_ptr(ep + OFF_ENTITY_NAME, np)) return;

        char name[64] = {};
        if (is_valid_ptr(np))
            reader_.read_mem(np, name, sizeof(name) - 1);

        EntityType type = ENT_UNKNOWN;
        const char *display = "";

        if (!should_skip_entity(name))
            classify_entity(name, type, display);

        CachedEnt ce = {};
        ce.type = type;
        strncpy(ce.name, display, sizeof(ce.name) - 1);
        strncpy(ce.raw_name, name, sizeof(ce.raw_name) - 1);
        ent_type_cache_[ep] = ce;

        if (type == ENT_UNKNOWN && !state.show_all) return;
        if (type == ENT_UNKNOWN) display = name;

        EntityData ent = {};
        ent.position = pos;
        ent.type = type;
        ent.has_head = read_head_pos(ep, wtm, ent.head_pos);
        strncpy(ent.name, display, sizeof(ent.name) - 1);
        strncpy(ent.raw_name, name, sizeof(ent.raw_name) - 1);
        state.entities.push_back(ent);
    }

    static bool should_skip_entity(const char *name) {
        for (const char *pat : SKIP_PATTERNS) {
            if (strstr(name, pat)) return true;
        }
        return false;
    }

    static void classify_entity(const char *name, EntityType &type, const char *&display) {
        for (const auto &rule : ENTITY_RULES) {
            if (strstr(name, rule.pattern)) {
                type = rule.type;
                display = rule.display;
                return;
            }
        }
    }

    /* --- Camera --- */

    bool read_camera(GameState &state) {
        if (!cam_base_) return false;

        uint8_t cam_buf[CAMERA_STRUCT_SIZE] = {};
        if (!reader_.read_mem(cam_base_ + cam_offset_, cam_buf, CAMERA_STRUCT_SIZE))
            return false;

        float fov = *(float *)(cam_buf + OFF_CAMERA_FOV);
        if (fov < 0.01f || fov > 3.0f) return false;

        /* Position from CCamera+0x2F0, fallback to Matrix34 translation */
        memcpy(&state.local_pos, cam_buf + 0x2F0, sizeof(Vec3));
        if (std::isnan(state.local_pos.x) || fabsf(state.local_pos.x) < 1.0f) {
            Matrix34 *cam34 = (Matrix34 *)cam_buf;
            state.local_pos = cam34->get_translation();
        }

        memcpy(&state.render_mat, cam_buf + 0x230, sizeof(Matrix44));
        memcpy(&state.proj_mat, cam_buf + 0x270, sizeof(Matrix44));
        return true;
    }

    /* Scan CryRenderD3D12.dll .data section for CCamera by matching FOV + rotation matrix */
    bool scan_camera(GameState &state) {
        uint64_t render_base = 0, render_size = 0;
        if (!reader_.get_module_base(MOD_CRYRENDER, render_base, render_size))
            return false;

        uint16_t mz = 0;
        reader_.read(render_base, mz);
        if (mz != 0x5A4D) return false;

        uint32_t pe_off = 0;
        reader_.read(render_base + 0x3C, pe_off);

        uint16_t num_sects = 0, opt_sz = 0;
        reader_.read(render_base + pe_off + 6, num_sects);
        reader_.read(render_base + pe_off + 20, opt_sz);

        uint64_t sect_hdr = render_base + pe_off + 24 + opt_sz;
        for (int s = 0; s < num_sects; s++) {
            char sname[9] = {};
            reader_.read_mem(sect_hdr + s * 40, sname, 8);

            uint32_t va = 0, vs = 0;
            reader_.read(sect_hdr + s * 40 + 12, va);
            reader_.read(sect_hdr + s * 40 + 8, vs);
            if (strcmp(sname, ".data") != 0) continue;

            uint64_t sect = render_base + va;
            uint64_t len = std::min((uint64_t)vs, (uint64_t)0x200000);

            for (uint64_t off = 0x30; off < len; off += 4) {
                float fov = 0;
                reader_.read(sect + off, fov);
                if (fov < 0.01f || fov > 2.0f) continue;

                float px = 0, py = 0, pz = 0;
                reader_.read(sect + off - 0x24, px);
                reader_.read(sect + off - 0x14, py);
                reader_.read(sect + off - 0x04, pz);
                if (fabsf(px) < 50 || fabsf(py) < 50 ||
                    fabsf(px) > 2000 || fabsf(py) > 2000)
                    continue;

                float r0 = 0, r1 = 0, r2 = 0;
                reader_.read(sect + off - 0x30, r0);
                reader_.read(sect + off - 0x2C, r1);
                reader_.read(sect + off - 0x28, r2);
                float row_len = sqrtf(r0 * r0 + r1 * r1 + r2 * r2);
                if (row_len < 0.9f || row_len > 1.1f) continue;

                printf("Camera at CryRender .data+0x%lx pos=(%.0f,%.0f,%.0f) fov=%.2f\n",
                       off - 0x30, px, py, pz, fov);
                cam_base_ = sect;
                cam_offset_ = off - 0x30;
                reader_.read_mem(cam_base_ + cam_offset_ + 0x230, &state.render_mat, sizeof(Matrix44));
                reader_.read_mem(cam_base_ + cam_offset_ + 0x270, &state.proj_mat, sizeof(Matrix44));
                return true;
            }
        }
        return false;
    }

    /* --- Bone/skeleton reading --- */

    static bool is_real_head_bone(const char *name) {
        for (const char *bn : HEAD_BONE_NAMES) {
            if (strcmp(name, bn) == 0) return true;
        }
        return false;
    }

    static bool is_fallback_head_bone(const char *name) {
        if (!strstr(name, "Head") && !strstr(name, "head")) return false;
        if (strstr(name, "target") || strstr(name, "Target")) return false;
        if (strstr(name, "hand") || strstr(name, "Hand")) return false;
        if (strstr(name, "RT_") || strstr(name, "IK_")) return false;
        return true;
    }

    static bool is_neck_bone(const char *name) {
        for (const char *bn : NECK_BONE_NAMES) {
            if (strcmp(name, bn) == 0) return true;
        }
        return false;
    }

    int find_head_bone_index(uint64_t char_instance) {
        uint64_t def_skel = 0;
        if (!reader_.read_ptr(char_instance + OFF_CHAR_DEFSKEL, def_skel)) {
            BONE_LOG("BONE: FAIL read defskel at char_inst+0x%lx\n", OFF_CHAR_DEFSKEL);
            /* Try alternate offsets for IDefaultSkeleton */
            for (uint64_t off : {0x1B8UL, 0x1C8UL, 0x1D0UL, 0x1B0UL, 0x200UL}) {
                reader_.read_ptr(char_instance + off, def_skel);
                if (is_valid_ptr(def_skel)) {
                    uint32_t tc = 0;
                    reader_.read(def_skel + OFF_DEFSKEL_JOINTCNT, tc);
                    if (tc > 5 && tc < 300) {
                        BONE_LOG("BONE: Found defskel at alt +0x%lx: 0x%lx cnt=%u\n",
                                 off, def_skel, tc);
                        break;
                    }
                }
                def_skel = 0;
            }
            if (!def_skel) return -1;
        }
        if (!is_valid_ptr(def_skel)) {
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
            !is_valid_ptr(joints_ptr)) {
            BONE_LOG("BONE: joints_ptr invalid at defskel+0x%lx\n", OFF_DEFSKEL_JOINTS);
            return -1;
        }

        BONE_LOG("BONE: Scanning %u joints at 0x%lx (defskel=0x%lx)\n",
                 joint_count, joints_ptr, def_skel);

        for (uint32_t stride : {0x150U, 0x120U, 0x100U, 0xD8U, 0x98U}) {
            /* Validate stride by checking name pointers */
            int valid_names = 0;
            for (uint32_t i = 0; i < std::min(joint_count, 5U); i++) {
                uint64_t np = 0;
                reader_.read_ptr(joints_ptr + i * stride, np);
                if (is_valid_ptr(np)) {
                    char test[8] = {};
                    reader_.read_mem(np, test, sizeof(test) - 1);
                    if (test[0] >= ' ' && test[0] <= 'z') valid_names++;
                }
            }
            if (valid_names < 2) continue;

            static int full_dump_count = 0;
            bool do_full_dump = (full_dump_count < 3);
            int fallback = -1;
            int neck_idx = -1;

            for (uint32_t i = 0; i < joint_count; i++) {
                uint64_t name_ptr = 0;
                reader_.read_ptr(joints_ptr + i * stride, name_ptr);
                if (!is_valid_ptr(name_ptr)) continue;

                char jname[48] = {};
                reader_.read_mem(name_ptr, jname, sizeof(jname) - 1);

                if (i < 10)
                    BONE_LOG("  bone[%u] = '%s'\n", i, jname);

                if (is_real_head_bone(jname)) {
                    BONE_LOG("BONE: Found head '%s' at %u (stride=0x%x)\n", jname, i, stride);
                    return (int)i;
                }
                if (fallback < 0 && is_fallback_head_bone(jname))
                    fallback = (int)i;
                if (neck_idx < 0 && is_neck_bone(jname))
                    neck_idx = (int)i;
            }

            if (fallback >= 0) {
                BONE_LOG("BONE: Using fallback head at %d (stride=0x%x)\n", fallback, stride);
                return fallback;
            }
            if (do_full_dump) {
                full_dump_count++;
                BONE_LOG("BONE: FULL DUMP of %u joints (stride=0x%x):\n", joint_count, stride);
                for (uint32_t i = 10; i < joint_count; i++) {
                    uint64_t np = 0;
                    reader_.read_ptr(joints_ptr + i * stride, np);
                    if (!is_valid_ptr(np)) continue;
                    char jn[48] = {};
                    reader_.read_mem(np, jn, sizeof(jn) - 1);
                    if (jn[0]) BONE_LOG("  bone[%u] = '%s'\n", i, jn);
                }
            }
            if (neck_idx >= 0) {
                BONE_LOG("BONE: No head, using neck at %d\n", neck_idx);
                return neck_idx;
            }
            BONE_LOG("BONE: stride 0x%x valid but no head in %u joints\n", stride, joint_count);
            return -1;
        }

        return -1;
    }

    bool read_head_pos(uint64_t ep, const Matrix34 &wtm, Vec3 &out) {
        uint64_t slots_ptr = 0;
        if (!reader_.read_ptr(ep + OFF_ENTITY_SLOTS, slots_ptr) || !is_valid_ptr(slots_ptr))
            return false;

        uint64_t slot0 = 0;
        if (!reader_.read_ptr(slots_ptr, slot0) || !is_valid_ptr(slot0))
            return false;

        uint64_t char_inst = 0;
        if (!reader_.read_ptr(slot0 + OFF_SLOT_CHARINSTANCE, char_inst) || !is_valid_ptr(char_inst))
            return false;

        uint64_t bone_arr = 0;
        if (!reader_.read_ptr(char_inst + OFF_CHAR_SKELPOSE + OFF_SKELPOSE_BONES, bone_arr) ||
            !is_valid_ptr(bone_arr))
            return false;

        int head_idx = -1;
        auto it = head_bone_cache_.find(char_inst);
        if (it != head_bone_cache_.end()) {
            head_idx = it->second;
        } else {
            head_idx = find_head_bone_index(char_inst);
            if (head_idx < 0)
                head_idx = find_head_by_position(char_inst, bone_arr);
            head_bone_cache_[char_inst] = head_idx;
        }
        if (head_idx < 0) return false;

        Vec3 model_pos = {};
        if (!reader_.read_mem(bone_arr + head_idx * BONE_STRIDE + BONE_POS_OFFSET,
                              &model_pos, sizeof(Vec3)))
            return false;
        if (std::isnan(model_pos.x) || fabsf(model_pos.x) > 50)
            return false;

        out = wtm.transform_point(model_pos);
        return true;
    }

    /* Positional fallback: pick highest-Z bone near head height (1.4-2.2m) */
    int find_head_by_position(uint64_t char_inst, uint64_t bone_arr) {
        uint64_t def_skel = 0;
        reader_.read_ptr(char_inst + OFF_CHAR_DEFSKEL, def_skel);
        uint32_t jcount = 0;
        if (is_valid_ptr(def_skel)) {
            reader_.read(def_skel + OFF_DEFSKEL_JOINTCNT, jcount);
            if (jcount > 500) jcount = 0;
        }
        if (jcount == 0) return -1;

        uint32_t arr_size = jcount * BONE_STRIDE;
        if (arr_size > 0x10000) arr_size = 0x10000;

        std::vector<uint8_t> bone_data(arr_size);
        if (!reader_.read_mem(bone_arr, bone_data.data(), arr_size))
            return -1;

        float best_z = -999.0f;
        int best_idx = -1;
        for (uint32_t i = 1; i < jcount; i++) {
            uint32_t off = i * BONE_STRIDE + BONE_POS_OFFSET;
            if (off + 12 > arr_size) break;

            Vec3 *bp = (Vec3 *)(bone_data.data() + off);
            if (!std::isnan(bp->z) && bp->z > 1.4f && bp->z < 2.2f &&
                fabsf(bp->x) < 0.3f && fabsf(bp->y) < 0.3f &&
                bp->z > best_z) {
                best_z = bp->z;
                best_idx = (int)i;
            }
        }
        return best_idx;
    }
};

/* Static constexpr member definitions (required pre-C++17 for ODR-use) */
constexpr const char *GameData::SKIP_PATTERNS[];
constexpr GameData::EntityClassRule GameData::ENTITY_RULES[];
constexpr const char *GameData::HEAD_BONE_NAMES[];
constexpr const char *GameData::NECK_BONE_NAMES[];
