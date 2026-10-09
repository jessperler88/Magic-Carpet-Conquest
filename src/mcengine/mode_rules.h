// Match rules v1 of the Conquest mode (Phase 4 round 12, task B; docs/port/BRIEFING_round12.md). Port-only.
//
// Everything here runs only in a mode run (mode_active()) and only from the simulation (level start, mode tick,
// the death / respawn / castle paths that call in): with GAME_MODE_ORIGINAL nothing changes and every reference stays
// byte-identical. State lives in g_mode (ModeState.match_*, ModePlayer: side, respawn, desecration, stats).
//
// - sides / teams: MatchRules.team -> ModePlayer.side at the level start; same_side() replaces the original's
//   `owner == owner` tests where an ally must count as "own" (targeting, damage, mana claims, ...);
// - respawn at the castle after respawn_s + respawn_level_s * castle level (humans and bots alike);
// - elimination: dead without a castle (the original's "no castle = level lost"), or the castle desecrated;
// - desecration: an enemy wizard alive inside the courtyard for desecrate_s seconds razes the castle;
// - win: elimination / mana dominance / timer (MatchRules.win); then the match is over (ModeState.match_state) and,
//   after a short pause, the level ends for every peer on the same tick;
// - stats for the result screen (ModePlayer kills / deaths / castles_razed / mana_peak).
#pragma once
#include <cstdint>
#include "mode.h"

// ---- sides ----------------------------------------------------------------------------------------------------
// True when any MatchRules.team entry is set for the running mode level (cached at the level start; false outside a
// mode run). Without teams every side is a single wizard and same_side is the original's owner test.
extern bool g_mode_teams;
// The side of a Thing's owner (a wizard's Thing index -> its record's ModePlayer.side), -1 for no wizard (wild
// creatures, the world). Installed by mode_rules (null = no teams).
extern int (*g_hook_mode_side_of_owner)(int owner);
// The original compares owners (`a->owner == b->owner`, Thing indices); in a mode run with teams, allied wizards and
// everything they own count as the same side. Faithful path: exactly `a == b`.
inline bool same_side(int owner_a, int owner_b) {
    if (owner_a == owner_b) return true;
    if (!g_mode_teams || !g_hook_mode_side_of_owner) return false;
    const int sa = g_hook_mode_side_of_owner(owner_a);
    return sa >= 0 && sa == g_hook_mode_side_of_owner(owner_b);
}
// The same for two player records (0..7).
bool mode_same_side_records(int rec_a, int rec_b);

// ---- level start / tick (installed through mode_level.cpp's hooks; never called outside a mode run) -----------
void mode_rules_level_start();      // sides, MODE_PLAYER_IN_MATCH / _BOT, g_mode_teams
void mode_rules_tick();             // respawn, elimination, desecration, win conditions, stats

// ---- queries (HUD, result screen, bots, tests) -----------------------------------------------------------------
bool mode_match_over();
// Seconds (rounded up) until record `rec` respawns, 0 when alive or the respawn is free.
int  mode_respawn_seconds(int rec);
// Desecration progress of the castle of record `rec` (0..100, the highest channel on it), 0 = none.
int  mode_desecration_pct(int rec);
// Seconds left of the time limit, -1 without one.
int  mode_time_left_seconds();
