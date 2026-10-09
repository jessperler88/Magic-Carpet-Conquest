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
#include "mc_types.h"       // Thing

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
// Recomputes g_mode_teams from g_mode (true when two records of the match share a side) and installs / removes the
// side hook (mode.cpp, so a MODE savestate chunk loaded into a fresh process gets its sides back: mode_deserialise,
// mode_begin, mode_end and mode_rules_level_start call it).
void mode_rules_sync();

// ---- calls from the simulation into the rules (port-only, null = nothing; defined in mode.cpp because the
// simulation core calls them and every unit test links mode.cpp; mode_rules.cpp installs them). Every call site
// tests mode_active() first: outside a mode run nothing is called.
// player_dying_update: the wizard has fallen and lies dead (state 3) - deaths, the killer's kill, the respawn timer.
extern void (*g_hook_mode_wizard_died)(Thing *wizard);
// creature_die: a creature (Thing.killer = the killer's owner) died - the killer's creature kill.
extern void (*g_hook_mode_creature_died)(Thing *creature);
// castle_collapse_level: the castle's last level fell (deleted; Thing.killer = the destroyer's owner) - castles razed.
extern void (*g_hook_mode_castle_gone)(Thing *castle);

// ---- level start / tick (installed through mode_level.cpp's hooks; never called outside a mode run) -----------
void mode_rules_level_start();      // sides, MODE_PLAYER_IN_MATCH / _BOT, g_mode_teams
void mode_rules_tick();             // respawn, elimination, desecration, win conditions, stats

// The match rules with every default filled in (MatchRules: 0 = default), in ticks.
struct MatchRulesResolved {
    uint8_t win;                    // MATCH_WIN_* bits (0 -> ELIMINATION | MANA, + TIMER when time_limit_min != 0)
    int     mana_pct;               // 1..100
    int     hold_ticks;             // mana dominance must hold this long
    int     time_limit_ticks;       // 0 = no time limit
    int     respawn_ticks;          // respawn delay at the castle ...
    int     respawn_level_ticks;    // ... + this per castle level
    int     desecrate_ticks;        // 0 = desecration off (MatchRules.desecrate_s 0xff)
};
MatchRulesResolved mode_rules_resolve(const MatchRules &m);
// The pause between the end of the match and the level end (every record's status set on one tick).
constexpr int MODE_MATCH_END_DELAY_TICKS = 5 * MODE_TICKS_PER_SECOND;

// Ends the running match now (debug commands `win` / `lose`, tests): `winner_side` = the winning ModePlayer.side,
// -1 = nobody wins. The places are computed, the level ends MODE_MATCH_END_DELAY_TICKS later. False when no match
// runs (no mode, or the match is already over).
bool mode_rules_end_match(int winner_side, uint32_t reason);
// Debug `win` / `lose` for record `rec` in a mode run (debug_cmd.cpp): win = rec's side wins; lose = the best other
// side by the timer ranking wins (nobody when there is none). MATCH_END_DEBUG.
bool mode_rules_debug_end(int rec, bool win);

// ---- queries (HUD, result screen, bots, tests) -----------------------------------------------------------------
bool mode_match_over();
// Seconds (rounded up) until record `rec` respawns, 0 when alive or the respawn is free.
int  mode_respawn_seconds(int rec);
// Desecration progress of the castle of record `rec` (0..100, the highest channel on it), 0 = none.
int  mode_desecration_pct(int rec);
// Seconds left of the time limit, -1 without one.
int  mode_time_left_seconds();
// A side's castle + transit mana (the mana dominance / timer measure), of its records not eliminated.
int64_t mode_side_castle_mana(int side);
// The world's mana the dominance compares with: Config.total_mana (game_check_level_won's divisor) without the
// local player's P.mana term mana_totals_update starts it with - the same number on every peer.
int64_t mode_world_mana();
