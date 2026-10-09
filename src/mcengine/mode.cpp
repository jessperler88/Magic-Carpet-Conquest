// Port-only game modes: the mode state block (mode.h). Round 10 (task A).
#include "mode.h"
#include "mode_rules.h"
#include "settings.h"
#include "player.h"
#include <cstring>

ModeState g_mode;
// mode_rules.h: the sides of a mode run (here because the simulation core calls same_side; mode_rules.cpp sets them)
bool g_mode_teams = false;
int (*g_hook_mode_side_of_owner)(int owner) = nullptr;

bool mode_same_side_records(int rec_a, int rec_b) {
    if (rec_a == rec_b) return true;
    if (!g_mode_teams || rec_a < 0 || rec_a > 7 || rec_b < 0 || rec_b > 7) return false;
    return g_mode.players[rec_a].side == g_mode.players[rec_b].side;
}
// mode_rules.h: the death / kill / castle calls of the simulation into the match rules (installed by mode_rules.cpp)
void (*g_hook_mode_wizard_died)(Thing *) = nullptr;
void (*g_hook_mode_creature_died)(Thing *) = nullptr;
void (*g_hook_mode_castle_gone)(Thing *) = nullptr;

// g_hook_mode_side_of_owner (round 12, task B): everything a wizard owns has the wizard's Thing index as `owner`
// (thing_create starts every Thing with its own index; spells, projectiles, effects, the castle, balloons and
// guards copy the wizard's), so the owner is a player record's wizard exactly when it is the record's
// PlayerRec.thing. Integers and index order only.
static int side_of_owner(int owner) {
    if (owner <= 0 || !g_state) return -1;
    for (int p = 0; p < 8; p++) {
        const ModePlayer &m = g_mode.players[p];
        if (!(m.flags & MODE_PLAYER_IN_MATCH)) continue;
        if ((int)g_state->players[p].thing == owner) return m.side;
    }
    return -1;
}

void mode_rules_sync() {
    bool teams = false;
    if (mode_active()) {
        for (int a = 0; a < 8 && !teams; a++) {
            if (!(g_mode.players[a].flags & MODE_PLAYER_IN_MATCH)) continue;
            for (int b = a + 1; b < 8; b++)
                if ((g_mode.players[b].flags & MODE_PLAYER_IN_MATCH) && g_mode.players[b].side == g_mode.players[a].side) {
                    teams = true;
                    break;
                }
        }
    }
    g_mode_teams = teams;
    g_hook_mode_side_of_owner = teams ? side_of_owner : nullptr;
}
void (*g_hook_mode_tick)() = nullptr;        // player.h (defined here: every unit test links mode.cpp)
void (*g_hook_mode_level_start)() = nullptr;
uint32_t (*g_hook_mode_checksum_globals)(uint32_t h) = nullptr;

namespace {
uint32_t fnv(uint32_t h, const void *p, size_t n) {
    const uint8_t *b = static_cast<const uint8_t *>(p);
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; }
    return h;
}
}

const char *mode_name(uint32_t mode) {
    switch (mode) {
    case GAME_MODE_ORIGINAL: return "original";
    case GAME_MODE_CONQUEST: return "conquest";
    default: return "?";
    }
}

void mode_begin(uint32_t mode, const ModeParams &params) {
    g_mode = ModeState{};
    if (mode == GAME_MODE_ORIGINAL || mode >= GAME_MODE_COUNT) { mode_end(); return; }
    g_mode.version = MODE_STATE_VERSION;
    g_mode.mode = mode;
    g_mode.params = params;
    GameplayRules r = gameplay_rules();
    r.mode = (int)mode;
    gameplay_force_rules(&r);
    g_hook_mode_tick = mode_tick;
    mode_rules_sync();
}

void mode_end() {
    const bool was = mode_active();
    g_mode = ModeState{};
    mode_rules_sync();
    if (g_hook_mode_tick == mode_tick) g_hook_mode_tick = nullptr;
    if (was) gameplay_force_rules(nullptr);
}

void mode_level_start() {
    g_mode.tick = 0;
    g_mode.rng = g_mode.params.seed ^ 0x9e3779b9u;
    for (ModePlayer &p : g_mode.players) p = ModePlayer{};
    if (g_hook_mode_level_start) g_hook_mode_level_start();
}

void (*g_hook_mode_match_tick)() = nullptr;

void mode_tick() {
    if (!mode_active()) return;
    g_mode.tick++;
    if (g_hook_mode_match_tick) g_hook_mode_match_tick();
}

static uint32_t s_proposal_mode = GAME_MODE_ORIGINAL;
static ModeParams s_proposal_params;

void mode_set_proposal(uint32_t mode, const ModeParams &params) {
    s_proposal_mode = mode < GAME_MODE_COUNT ? mode : GAME_MODE_ORIGINAL;
    s_proposal_params = params;
}

uint32_t mode_proposal(ModeParams *params) {
    if (params) *params = s_proposal_mode != GAME_MODE_ORIGINAL ? s_proposal_params : ModeParams{};
    return s_proposal_mode;
}

uint32_t mode_rng_next() {
    g_mode.rng = g_mode.rng * 1664525u + 1013904223u;
    return g_mode.rng;
}

uint32_t mode_checksum(uint32_t h) {
    h = fnv(h, &g_mode, sizeof g_mode);
    return g_hook_mode_checksum_globals ? g_hook_mode_checksum_globals(h) : h;
}

size_t mode_serialised_size() { return 4 + sizeof(ModeState); }

void mode_serialise(uint8_t *out) {
    const uint32_t v = MODE_STATE_VERSION;
    std::memcpy(out, &v, 4);
    std::memcpy(out + 4, &g_mode, sizeof g_mode);
}

bool mode_params_read(const uint8_t *in, size_t n, ModeParams *out) {
    if (!in || !out || (n != MODE_PARAMS_V1_SIZE && n != sizeof(ModeParams))) return false;
    *out = ModeParams{};
    std::memcpy(out, in, n);                    // v1: the first 32 bytes, MatchRules stay the defaults
    return true;
}

// Version 1 (rounds 10-11): the same block with a 32-byte ModeParams.
static constexpr size_t MODE_STATE_V1_SIZE = sizeof(ModeState) - (sizeof(ModeParams) - MODE_PARAMS_V1_SIZE);

bool mode_serialised_size_ok(size_t n) { return n == mode_serialised_size() || n == 4 + MODE_STATE_V1_SIZE; }

bool mode_deserialise(const uint8_t *in, size_t n) {
    if (!in || n < 4) return false;
    uint32_t v;
    std::memcpy(&v, in, 4);
    ModeState m;
    if (v == MODE_STATE_VERSION && n == mode_serialised_size()) {
        std::memcpy(&m, in + 4, sizeof m);
    } else if (v == 1 && n == 4 + MODE_STATE_V1_SIZE) {
        constexpr size_t head = offsetof(ModeState, params);
        constexpr size_t tail = sizeof(ModeState) - offsetof(ModeState, tick);
        const uint8_t *p = in + 4;
        std::memcpy(&m, p, head);
        mode_params_read(p + head, MODE_PARAMS_V1_SIZE, &m.params);
        std::memcpy(reinterpret_cast<uint8_t *>(&m) + offsetof(ModeState, tick), p + head + MODE_PARAMS_V1_SIZE, tail);
        m.version = MODE_STATE_VERSION;
    } else {
        return false;
    }
    if (m.mode >= GAME_MODE_COUNT) return false;
    g_mode = m;
    g_hook_mode_tick = mode_active() ? mode_tick : nullptr;
    mode_rules_sync();                               // the sides of the loaded match (g_mode_teams is not in the chunk)
    return true;
}
