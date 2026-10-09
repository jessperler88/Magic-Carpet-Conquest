// Bots v1 (mode_bots.h). Round 12 task C; report docs/analysis/port_r12_bots.md.
//
// The skill: the computer wizards' three AI knobs come from the level's player block (+4 aggression, +8 reaction,
// +0xc accuracy, int16 each; player_spawn copies them into PlayerBlock.ai_* when the wizard is created). The mode
// level's LevelData is a fresh copy of the template on every level start (mode_build_level through
// g_hook_level_source), so rewriting the copy in g_state->level here - after players_init_records, before the
// players' join command spawns them in the first tick - is idempotent across restarts and reaches the spawn.
// "normal" (MatchRules.bot_skill 0 or 2) leaves the template's values: exactly what a generated world played before
// round 12. The block is GameState: hashed, saved, recorded like the rest of the level.
#include "mode_bots.h"
#include "mode.h"
#include "mc_globals.h"
#include <cstring>

namespace {

int clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

// One knob (0..255, higher = stronger) at a skill: easy pulls the template's value toward 0 (aggression: halved,
// accuracy / reaction: -96), hard halves the distance to 255, brutal leaves a quarter of it.
int knob_at_skill(int v, int skill, bool aggression) {
    v = clamp255(v);
    switch (skill) {
    case 1:  return aggression ? v / 2 : clamp255(v - 96);
    case 3:  return v + (255 - v) / 2;
    case 4:  return 255 - (255 - v) / 4;
    default: return v;
    }
}

int16_t get16(const uint8_t *p) { int16_t v; std::memcpy(&v, p, 2); return v; }
void put16(uint8_t *p, int v) { const int16_t s = (int16_t)v; std::memcpy(p, &s, 2); }

} // namespace

int mode_bot_skill(uint8_t bot_skill) { return bot_skill == 0 ? 2 : bot_skill > 4 ? 4 : bot_skill; }

const char *mode_bot_skill_name(int skill) {
    switch (skill) {
    case 1: return "easy";
    case 2: return "normal";
    case 3: return "hard";
    case 4: return "brutal";
    default: return "?";
    }
}

void mode_bots_apply_skill(uint8_t *blk, int skill) {
    put16(blk + 4, knob_at_skill(get16(blk + 4), skill, true));         // aggression
    put16(blk + 8, knob_at_skill(get16(blk + 8), skill, false));        // reaction (and the original's respawn delay)
    put16(blk + 0xc, knob_at_skill(get16(blk + 0xc), skill, false));    // accuracy
}

void mode_bots_level_start() {
    if (!mode_active()) return;
    const int skill = mode_bot_skill(g_mode.params.match.bot_skill);
    if (skill == 2) return;                                             // normal: the template's knobs
    for (int p = 0; p < 8 && p < (int)g_state->player_count; p++) {
        if (g_state->players[p].is_computer != 1) continue;
        mode_bots_apply_skill(g_state->level.player_block[p], skill);
    }
}
