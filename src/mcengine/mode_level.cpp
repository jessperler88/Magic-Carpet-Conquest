// The level of a mode run (mode_level.h). Round 10 task A.
#define _CRT_SECURE_NO_WARNINGS
#include "mode_level.h"
#include "sim.h"
#include "demo.h"
#include "level.h"
#include "mc_globals.h"
#include "world.h"
#include "ai_wizard.h"
#include "terrain_paint.h"
#include "worldgen.h"
#include "mode_rules.h"
#include "mode_bots.h"
#include "net.h"
#include "gen/player_tables.h"
#include "mcfile.h"
#include <cstdio>
#include <cstring>
#include <string>

namespace {
std::string s_game_dir;
bool        s_running = false;

constexpr int kFirstMap = 50, kMaps = 20;       // the multiplayer maps (lobby levels 50..69)

uint32_t players_wanted(const ModeParams &p) {
    uint32_t n = p.humans + p.bots;
    if (n < 1) n = 1;
    if (n > 8) n = 8;
    return n;
}

// A small integer hash of the seed (splitmix-style finaliser): the template choice and the AI seed.
uint32_t mix(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

bool level_source(int index, LevelData *out) {
    if (index != MODE_LEVEL_INDEX || !mode_active()) return false;
    return mode_build_level(s_game_dir.c_str(), g_mode.params, out);
}

// g_hook_mode_level_start: the level-start work of a mode level that needs the gameplay subsystems.
void level_start() {
    // Round 11: MODE_PARAM_TILED with world_size > 256 plays the template level repeated as an N-cell world
    // (world.h world_widen_tiled: the maps tiled, the Things stay in the first copy; the session 1 test bed).
    // Runs at the end of level_start_from_state (mode_level_start), after switch_activate and players_init_records,
    // before the players spawn (their join command in the first tick): the level reset set the world back to 256
    // first, so a restart widens again. A generated world (the default for > 256) was built by level_world below.
    const uint32_t ws = g_mode.params.world_size;
    if (!mode_params_generated(g_mode.params) && ws > 256 && ws <= (uint32_t)MC_WORLD_MAX && !(ws & (ws - 1)))
        world_widen_tiled((int)ws);
    if (mode_params_generated(g_mode.params)) worldgen_apply_woods();   // the oak woods' trees (worldgen.h)
    g_mode.template_level = (uint32_t)mode_template_level(s_game_dir.c_str(), g_mode.params);
    // the AI's crt_rand seed (outside GameState) from the match seed: a mode run never depends on what ran
    // before it in the process (the campaign leaves it wherever its last level left it)
    g_mode.ai_seed = mix(g_mode.params.seed ^ 0x5eedA1u);
    g_ai_rand_seed = g_mode.ai_seed;
    g_ai_human_wizard = 0;
    // The recyclable stack's entries above its top are leftovers of the previous level (the level reset does
    // not clear them; nothing reads them, but net_state_checksum hashes the whole stack): cleared, so a mode
    // run's checksums do not depend on what the process played before.
    for (int i = g_state->active_top + 1; i < 1000; i++) if (i >= 0) g_state->active_list[i] = 0;
    for (int p = 0; p < 8; p++) {
        PlayerRec *rec = &g_state->players[p];
        PlayerBlock *P = &rec->blk;
        std::memset(P->spell_found, 0, sizeof P->spell_found);     // no campaign progress in a mode run
        if (rec->is_computer == 1) continue;
        // a human: the level's start spells (player block +0x10) that are allowed (+0x74), as
        // players_init_records gives them in a network game
        const uint8_t *blk = g_state->level.player_block[p];
        for (int i = 0; i < 24; i++) P->spell_slot[i] = -1;
        P->slot_left = 0xff;
        P->slot_right = 0xff;
        int slot = 0;
        for (int i = 0; i < 24; i++) {
            const int id = g_spell_slot_order[i];
            P->hotkey_slot[i] = 0xff;
            if (blk[0x74 + id] == 1 && blk[0x10 + id] != 0) {
                P->spell_slot[slot] = id;
                if (P->slot_left == 0xff)       P->slot_left = (int16_t)slot;
                else if (P->slot_right == 0xff) P->slot_right = (int16_t)slot;
                P->hotkey_slot[slot] = (uint8_t)slot;
                slot++;
            }
        }
    }
    // round 12: the match rules (sides, seats) and the bots' skill (mode_rules.h, mode_bots.h)
    mode_rules_level_start();
    mode_bots_level_start();
}

// g_hook_level_world (worldgen.h): the level start of a mode level with a generated world builds it instead of
// terrain_build (sim.cpp level_start_from_state, after the level reset and with the template's LevelData loaded).
bool level_world() {
    if (!mode_active() || g_cfg->level != MODE_LEVEL_INDEX || !mode_params_generated(g_mode.params)) return false;
    return worldgen_build(g_mode.params, &g_state->level);
}

// demo.h hooks: a movie v3 starts / stops the mode run and carries the AI globals.
bool demo_mode_start(const char *game_dir, uint32_t mode, const ModeParams &params) {
    return mode_start_run(game_dir, mode, params);
}
void demo_mode_stop() { mode_stop_run(); }
void sim_globals_get(uint32_t v[DEMO_SIM_GLOBALS]) {
    std::memset(v, 0, sizeof(uint32_t) * DEMO_SIM_GLOBALS);
    v[0] = (uint32_t)g_terrain_nearly_flat;
    v[1] = g_ai_rand_seed;
    v[2] = g_ai_human_wizard;
}
void sim_globals_set(const uint32_t v[DEMO_SIM_GLOBALS]) {
    g_terrain_nearly_flat = (int32_t)v[0];
    g_ai_rand_seed = v[1];
    g_ai_human_wizard = (uint16_t)v[2];
}

// g_hook_mode_checksum_globals: FNV-1a over the globals a v3 snapshot carries (sim_globals_get).
uint32_t checksum_globals(uint32_t h) {
    uint32_t v[DEMO_SIM_GLOBALS];
    sim_globals_get(v);
    const uint8_t *b = reinterpret_cast<const uint8_t *>(v);
    for (size_t i = 0; i < sizeof v; i++) { h ^= b[i]; h *= 16777619u; }
    return h;
}

uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
} // namespace

int mode_template_level(const char *game_dir, const ModeParams &params) {
    if (params.map >= (uint32_t)kFirstMap && params.map < (uint32_t)(kFirstMap + kMaps)) return (int)params.map;
    // The candidates in index order, then one picked by the seed. Cached: the restart of a level asks again.
    static std::string s_dir;
    static int s_slots[kMaps] = {};
    if (s_dir != game_dir) {
        for (int i = 0; i < kMaps; i++) {
            mc_level lv;
            s_slots[i] = mc_level_load(game_dir, kFirstMap + i, &lv) ? lv.footer[1] : -1;
        }
        s_dir = game_dir;
    }
    int cand[kMaps], n = 0;
    const int want = (int)players_wanted(params);
    for (int i = 0; i < kMaps; i++) if (s_slots[i] >= want) cand[n++] = kFirstMap + i;
    if (n == 0) for (int i = 0; i < kMaps; i++) if (s_slots[i] > 0) cand[n++] = kFirstMap + i;
    if (n == 0) return -1;
    return cand[mix(params.seed) % (uint32_t)n];
}

bool mode_build_level(const char *game_dir, const ModeParams &params, LevelData *out) {
    const int tmpl = mode_template_level(game_dir, params);
    if (tmpl < 0) return false;
    static_assert(MC_LEVEL_SIZE == sizeof(LevelData), "mc_level image = LevelData");
    mc_level lv;
    if (!mc_level_load(game_dir, tmpl, &lv)) return false;
    mc_level_write(&lv, reinterpret_cast<uint8_t *>(out));
    if (params.flags & MODE_PARAM_RESEED) out->gen.seed = (int32_t)(params.seed & 0xffff);
    out->player_count = (uint16_t)players_wanted(params);
    return true;
}

void mode_level_register() {
    g_hook_mode_level_start = level_start;
    g_hook_level_world = level_world;
    g_hook_demo_mode_start = demo_mode_start;
    g_hook_demo_mode_stop = demo_mode_stop;
    g_hook_demo_sim_globals_get = sim_globals_get;
    g_hook_demo_sim_globals_set = sim_globals_set;
    g_hook_mode_checksum_globals = checksum_globals;
    g_hook_mode_match_tick = mode_rules_tick;           // round 12 (mode_rules.h)
}

bool mode_start_run(const char *game_dir, uint32_t mode, const ModeParams &params) {
    mode_level_register();
    s_game_dir = game_dir;
    mode_begin(mode, params);
    if (!mode_active()) return false;
    g_hook_level_source = level_source;
    s_running = true;
    return true;
}

void mode_stop_run() {
    if (g_hook_level_source == level_source) g_hook_level_source = nullptr;
    s_running = false;
    mode_end();
}

bool mode_run_active() { return s_running && mode_active(); }

bool mode_start_net_run(const char *game_dir, uint32_t mode, const ModeParams &params, bool *differs) {
    if (differs) *differs = false;
    if (!net_joined()) return false;
    ModeParams mine = params;
    mine.humans = (uint32_t)net_player_count();
    if (mine.humans + mine.bots > 8) mine.bots = mine.humans < 8 ? 8 - mine.humans : 0;
    // this peer's run first, so its rules (the pool of this world) are what it proposes
    if (!mode_start_run(game_dir, mode, mine)) return false;
    const NetGameRules agreed = net_agree_rules(net_local_rules(), differs);
    mode_stop_run();                                 // releases the forced rules: the host's are applied next
    if (agreed.mode == GAME_MODE_ORIGINAL || agreed.mode >= GAME_MODE_COUNT) return false;
    net_apply_rules(agreed);
    g_cfg->flags |= 0x10;
    if (!mode_start_run(game_dir, agreed.mode, agreed.mode_params)) { net_release_rules(); return false; }
    return true;
}

bool mode_start_run_for_state(const char *game_dir, const char *state_path) {
    mc_blob f;
    if (!mc_read_file(state_path, &f)) return false;
    bool ok = false;
    constexpr size_t kHeader = 96;                   // SAVESTATE_HEADER_SIZE (savegame.h)
    if (f.len >= kHeader && std::memcmp(f.data, "MCPSTATE", 8) == 0) {
        size_t off = get32(f.data + 12);             // header_size
        while (off + 8 <= f.len) {
            const uint8_t *c = f.data + off;
            const size_t len = get32(c + 4);
            if (off + 8 + len > f.len) break;
            if (!std::memcmp(c, "MODE", 4)) {
                // mode_deserialise reads both versions; g_mode is replaced by the state load right after anyway
                const ModeState keep = g_mode;
                if (mode_deserialise(c + 8, len)) {
                    const ModeState m = g_mode;
                    g_mode = keep;
                    if (m.mode != GAME_MODE_ORIGINAL && m.mode < GAME_MODE_COUNT)
                        ok = mode_start_run(game_dir, m.mode, m.params);
                }
                break;
            }
            off += 8 + len;
        }
    }
    mc_blob_free(&f);
    return ok;
}
