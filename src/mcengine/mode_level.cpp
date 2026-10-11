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
#include <cstdlib>
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
    // ... and the terrain painter's "nearly flat" result of its last cell, which the mode checksum covers too
    // (sim_globals_get): it was whatever the previous match's last castle wall left (2026-10-11: two runs of one
    // scripted match in a process differed in their first checksum)
    g_terrain_nearly_flat = 0;
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

ModeParams mode_net_session_params(const ModeParams &params, int session_players) {
    ModeParams m = params;
    m.humans = (uint32_t)(session_players < 1 ? 1 : session_players > 8 ? 8 : session_players);
    if (m.humans + m.bots > 8) m.bots = 8 - m.humans;
    return m;
}

bool mode_net_agree(const char *game_dir, uint32_t mode, const ModeParams &params, NetGameRules *agreed, bool *differs) {
    if (differs) *differs = false;
    if (!net_joined() || !agreed) return false;
    // this peer's run first, so its rules (the pool of this world) are what it proposes
    if (!mode_start_run(game_dir, mode, mode_net_session_params(params, net_player_count()))) return false;
    *agreed = net_agree_rules(net_local_rules(), differs);
    mode_stop_run();                                 // releases the forced rules: the host's are applied next
    return true;
}

bool mode_start_agreed_net_run(const char *game_dir, const NetGameRules &agreed) {
    if (!net_joined()) return false;
    if (agreed.mode == GAME_MODE_ORIGINAL || agreed.mode >= GAME_MODE_COUNT) return false;
    // every peer derives the same session params from the host's (the lobby's proposal says humans = 1)
    NetGameRules r = agreed;
    r.mode_params = mode_net_session_params(agreed.mode_params, net_player_count());
    net_apply_rules(r);
    g_cfg->flags |= 0x10;
    if (!mode_start_run(game_dir, r.mode, r.mode_params)) { net_release_rules(); return false; }
    return true;
}

bool mode_start_net_run(const char *game_dir, uint32_t mode, const ModeParams &params, bool *differs) {
    NetGameRules agreed{};
    if (!mode_net_agree(game_dir, mode, params, &agreed, differs)) return false;
    return mode_start_agreed_net_run(game_dir, agreed);
}

// ---- the campaign's progress around a mode run (mode_level.h) ------------------------------------------------------
namespace {
constexpr size_t kCampaignSpells = 0x3bd6, kCampaignOptions = 0x2195;   // GameState offsets the .gam saves
struct CampaignCarry {
    bool     saved = false;
    uint16_t level = 0;
    uint8_t  spells[0x18] = {};
    uint8_t  options[0xc] = {};
};
CampaignCarry s_carry;
uint8_t *state_at(size_t off) { return reinterpret_cast<uint8_t *>(g_state) + off; }
}

void mode_campaign_save() {
    if (s_carry.saved) return;
    s_carry.saved = true;
    s_carry.level = g_cfg->level;
    std::memcpy(s_carry.spells, state_at(kCampaignSpells), sizeof s_carry.spells);
    std::memcpy(s_carry.options, state_at(kCampaignOptions), sizeof s_carry.options);
}

void mode_campaign_restore() {
    if (!s_carry.saved) return;
    g_cfg->level = s_carry.level;
    std::memcpy(state_at(kCampaignSpells), s_carry.spells, sizeof s_carry.spells);
    std::memcpy(state_at(kCampaignOptions), s_carry.options, sizeof s_carry.options);
    s_carry = CampaignCarry{};
}

bool mode_campaign_saved() { return s_carry.saved; }

ModeState mode_result_snapshot(int local_record, bool forfeit) {
    ModeState s = g_mode;
    if (forfeit && local_record >= 0 && local_record < 8) {
        ModePlayer &p = s.players[local_record];
        if (!(p.flags & MODE_PLAYER_ELIMINATED)) { p.flags |= MODE_PLAYER_ELIMINATED; p.eliminated_tick = s.tick; }
        p.flags &= ~MODE_PLAYER_DESECRATING;
        if (s.match_state != MATCH_OVER) {
            s.match_state = MATCH_OVER;
            s.match_end_tick = s.tick;
            s.end_reason = MATCH_END_NONE;      // no winner: the match was left
            s.winner_side = 0;
        }
    }
    return s;
}

// ---- match-rule options (mode_level.h) -------------------------------------------------------------------------
namespace {
std::string norm_key(const std::string &k) {
    std::string s = k;
    while (!s.empty() && s[0] == '-') s.erase(0, 1);
    for (char &c : s) { if (c == '_') c = '-'; else if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a'); }
    return s;
}
bool to_uint(const std::string &v, unsigned long *out) {
    if (v.empty()) return false;
    char *end = nullptr;
    const unsigned long x = std::strtoul(v.c_str(), &end, 10);
    if (!end || *end || v[0] == '-') return false;
    *out = x;
    return true;
}
bool ranged(const std::string &key, const std::string &v, unsigned long lo, unsigned long hi, uint8_t *dst, std::string *err) {
    unsigned long x = 0;
    if (!to_uint(v, &x) || x < lo || x > hi) {
        if (err) *err = "--" + key + " takes " + std::to_string(lo) + ".." + std::to_string(hi) + " (got " + v + ")";
        return false;
    }
    *dst = (uint8_t)x;
    return true;
}
bool parse_teams(const std::string &v0, uint8_t team[8], std::string *err) {
    std::string v = v0;
    for (char &c : v) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    uint8_t t[8] = {};
    auto bad = [&](const char *why) { if (err) *err = std::string("--teams ") + v0 + ": " + why +
        " (1,1,2,2 per record, 2v2 / 1v3 / 2v2v2v2 team sizes, or ffa)"; return false; };
    if (v == "ffa" || v == "none") {
        // every record its own side
    } else if (v.find('v') != std::string::npos) {
        int rec = 0, id = 0;
        size_t p = 0;
        while (p <= v.size()) {
            const size_t q = v.find('v', p);
            const std::string part = v.substr(p, q == std::string::npos ? std::string::npos : q - p);
            unsigned long n = 0;
            if (!to_uint(part, &n) || n < 1 || n > 8) return bad("a team size is 1..8");
            id++;
            for (unsigned long k = 0; k < n; k++) {
                if (rec >= 8) return bad("more than 8 records");
                t[rec++] = (uint8_t)id;
            }
            if (q == std::string::npos) break;
            p = q + 1;
        }
        if (id < 2) return bad("at least two teams");
    } else {
        int rec = 0;
        size_t p = 0;
        while (p <= v.size()) {
            const size_t q = v.find(',', p);
            const std::string part = v.substr(p, q == std::string::npos ? std::string::npos : q - p);
            unsigned long n = 0;
            if (!to_uint(part, &n) || n > 8) return bad("a team id is 0..8");
            if (rec >= 8) return bad("more than 8 records");
            t[rec++] = (uint8_t)n;
            if (q == std::string::npos) break;
            p = q + 1;
        }
    }
    std::memcpy(team, t, 8);
    return true;
}
} // namespace

bool mode_match_option_known(const std::string &key) {
    const std::string k = norm_key(key);
    return k == "teams" || k == "win" || k == "mana-pct" || k == "hold" || k == "time" || k == "respawn" ||
           k == "respawn-level" || k == "desecrate" || k == "bot-skill";
}

bool mode_match_option(const std::string &key, const std::string &value, MatchRules *m, std::string *err) {
    const std::string k = norm_key(key);
    if (k == "teams") return parse_teams(value, m->team, err);
    if (k == "win") {
        std::string v = value;
        for (char &c : v) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (v == "none") { m->win = MATCH_WIN_NONE; return true; }
        uint8_t bits = 0;
        size_t p = 0;
        while (p <= v.size()) {
            const size_t q = v.find_first_of(",+", p);
            const std::string w = v.substr(p, q == std::string::npos ? std::string::npos : q - p);
            if (w == "elim" || w == "elimination") bits |= MATCH_WIN_ELIMINATION;
            else if (w == "mana") bits |= MATCH_WIN_MANA;
            else if (w == "timer" || w == "time") bits |= MATCH_WIN_TIMER;
            else { if (err) *err = "--win takes elim,mana,timer (any of them) or none (got " + value + ")"; return false; }
            if (q == std::string::npos) break;
            p = q + 1;
        }
        m->win = bits;
        return true;
    }
    if (k == "mana-pct") return ranged(k, value, 1, 100, &m->mana_pct, err);
    if (k == "hold") return ranged(k, value, 1, 255, &m->mana_hold_s, err);
    if (k == "time") return ranged(k, value, 1, 255, &m->time_limit_min, err);
    if (k == "respawn") return ranged(k, value, 1, 255, &m->respawn_s, err);
    if (k == "respawn-level") return ranged(k, value, 1, 255, &m->respawn_level_s, err);
    if (k == "desecrate") {
        if (value == "off" || value == "OFF" || value == "none") { m->desecrate_s = 0xff; return true; }
        return ranged(k, value, 1, 254, &m->desecrate_s, err);
    }
    if (k == "bot-skill") return ranged(k, value, 1, 4, &m->bot_skill, err);
    if (err) *err = "unknown match option --" + k;
    return false;
}

bool mode_match_rules_finish(MatchRules *m, std::string *err) {
    if (m->win & MATCH_WIN_NONE) return true;
    if (m->time_limit_min > 0) {
        if (m->win == 0) m->win = MATCH_WIN_ELIMINATION | MATCH_WIN_MANA;
        m->win |= MATCH_WIN_TIMER;
    } else if (m->win & MATCH_WIN_TIMER) {
        if (err) *err = "--win timer needs a time limit (--time M)";
        return false;
    }
    return true;
}

std::string mode_match_rules_describe(const MatchRules &m) {
    char buf[256];
    std::string s = "win ";
    if (m.win & MATCH_WIN_NONE) s += "none";
    else {
        const uint8_t w = m.win ? m.win : (uint8_t)(MATCH_WIN_ELIMINATION | MATCH_WIN_MANA);
        std::string b;
        if (w & MATCH_WIN_ELIMINATION) b += "elim";
        if (w & MATCH_WIN_MANA) b += b.empty() ? "mana" : "+mana";
        if (w & MATCH_WIN_TIMER) b += b.empty() ? "timer" : "+timer";
        s += b.empty() ? "?" : b;
    }
    std::snprintf(buf, sizeof buf, ", mana %d%% for %d s, ", m.mana_pct ? m.mana_pct : 75, m.mana_hold_s ? m.mana_hold_s : 30);
    s += buf;
    s += m.time_limit_min ? "time " + std::to_string(m.time_limit_min) + " min" : std::string("no time limit");
    std::snprintf(buf, sizeof buf, ", respawn %d+%d/level s, desecrate ", m.respawn_s ? m.respawn_s : 10,
                  m.respawn_level_s ? m.respawn_level_s : 5);
    s += buf;
    if (m.desecrate_s == 0xff) s += "off";
    else s += std::to_string(m.desecrate_s ? m.desecrate_s : 25) + " s";
    s += ", bot skill " + std::to_string(m.bot_skill ? m.bot_skill : 0) + (m.bot_skill ? "" : " (template)");
    bool teams = false;
    for (int i = 0; i < 8; i++) teams |= m.team[i] != 0;
    if (teams) {
        s += ", teams ";
        for (int i = 0; i < 8; i++) s += (i ? "," : "") + std::to_string(m.team[i]);
    } else s += ", free for all";
    return s;
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
