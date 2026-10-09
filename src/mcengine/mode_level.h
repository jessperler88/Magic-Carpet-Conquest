// The level of a mode run (Phase 4, round 10 task A; docs/analysis/port_mode.md). Nothing here exists in
// carpet.exe.
//
// Until round 11's world module and placer, a mode level is one of the 20 multiplayer maps of levels.dat
// (indices 50..69, the lobby's levels) used as a template, 256 x 256:
//   - template: ModeParams.map when it is 50..69, else chosen from the seed among the maps with enough
//     player slots (footer player_count >= humans + bots; every map but 51 has 8) - mode_template_level();
//   - terrain: the template's own generator settings (castle sites, villages and creatures were placed for
//     that terrain); with MODE_PARAM_RESEED the generator seed is replaced by the match seed;
//   - player_count = humans + bots (1..8). Player records 0..humans-1 are the humans (offline: record 0 =
//     the local player), the rest are computer wizards (players_init_records: every non-local record outside
//     a network game);
//   - the mode checksum (mode_checksum) also covers g_ai_rand_seed, g_ai_human_wizard, g_terrain_nearly_flat;
//   - at the level start (g_hook_mode_level_start): the AI's CRT seed g_ai_rand_seed and its cached human
//     wizard g_ai_human_wizard are reset from the match seed, every record's campaign spell_found is cleared,
//     and the human players get their level start spells as in a network game (the campaign gives a human
//     only the spells found earlier in the campaign - none in a mode run).
// Round 11: ModeParams.world_size N > 256 (a power of two up to MC_WORLD_MAX) widens the level at the level start
// with world_widen_tiled(N) (world.h): the template repeated (N/256)^2 times, every Thing in the first copy -
// the session-1 test bed until the generator on N (session 2) replaces it.
// The level is loaded under MODE_LEVEL_INDEX (mode.h) through g_hook_level_source (sim.h), so
// game_level_begin, restarts (level_finish) and savestate loads find it like a levels.dat entry.
#pragma once
#include "mode.h"
#include "mc_types.h"
#include <string>

struct NetGameRules;    // net.h
// The template level a run with `params` plays (50..69), -1 when levels.dat is missing.
int mode_template_level(const char *game_dir, const ModeParams &params);

// Fills `out` with the level a mode run with `params` plays. False when the game data is missing.
bool mode_build_level(const char *game_dir, const ModeParams &params, LevelData *out);

// Installs the mode's hooks into the gameplay subsystems: g_hook_mode_level_start, and the movie hooks
// (demo.h g_hook_demo_mode_start / g_hook_demo_mode_stop / g_hook_demo_sim_globals_*) that let a movie v3
// start the mode run and carry the AI globals. Idempotent. sim_register_gameplay() should call it (requested
// in the report); mode_start_run calls it too.
void mode_level_register();

// Starts a mode run: mode_begin(mode, params) and g_hook_level_source (sim.h) answering MODE_LEVEL_INDEX
// with mode_build_level. Then load the level with game_level_begin(MODE_LEVEL_INDEX) (mcport) or
// sim_load_level(MODE_LEVEL_INDEX) (tests). mode_stop_run undoes both.
bool mode_start_run(const char *game_dir, uint32_t mode, const ModeParams &params);
void mode_stop_run();
bool mode_run_active();                 // between mode_start_run and mode_stop_run

// A mode run in a joined network session (round 11 session 3; round 12's lobby replaces the call site): every peer
// calls it with its own params (humans := the session's player count, bots trimmed to 8 records), the peers agree
// on the host's NetGameRules incl. its ModeParams (net.h), which are applied, Config.flags gets 0x10 and the run is
// started with the host's params. Records 0..humans-1 are the session's players, the rest computer wizards
// (players_init_records). Then load MODE_LEVEL_INDEX as usual. *differs: some peer proposed other rules / params.
bool mode_start_net_run(const char *game_dir, uint32_t mode, const ModeParams &params, bool *differs);

// ---- round 12 (task D): the lobby path, the menu flow, the match-rule options ------------------------------------
// mode_start_net_run in two halves, so the lobby (frontend.cpp mp_join_done, which already exchanged NetGameRules with
// the urn's proposal, mode_set_proposal) does not exchange twice:
//   mode_net_agree: this peer's run started with `params` (its pool proposed), net_agree_rules, the run stopped again;
//                   *agreed = the host's rules. False without a session or when the run cannot start.
//   mode_start_agreed_net_run: the run of the session from the agreed rules - humans := the session's player count,
//                   bots trimmed to 8 records (mode_net_session_params), net_apply_rules, Config.flags |= 0x10,
//                   mode_start_run. False when the rules carry no mode / no session.
bool mode_net_agree(const char *game_dir, uint32_t mode, const ModeParams &params, NetGameRules *agreed, bool *differs);
bool mode_start_agreed_net_run(const char *game_dir, const NetGameRules &agreed);
ModeParams mode_net_session_params(const ModeParams &params, int session_players);

// The campaign's progress while a mode run from the menu / the lobby borrows the game state (Config.level, the
// .gam's spells GameState+0x3bd6 and options +0x2195 - a mode level clears spell_found and its level end moves
// Config.level): saved before the run, put back when it ended. save is a no-op while a save is held (a mode state
// loaded during a mode run); restore releases it.
void mode_campaign_save();
void mode_campaign_restore();
bool mode_campaign_saved();

// What the result screen shows (fe_mode_set_result): the final mode block. `forfeit`: the local record left the match
// (the pause menu's "Forfeit" offline) - it is marked eliminated and a running match is shown as over without a
// winner (match_state MATCH_OVER, end_reason MATCH_END_NONE). g_mode is not changed.
ModeState mode_result_snapshot(int local_record, bool forfeit);

// Match rules from the command line (`rts --teams 2v2 ...`, rts_run.h) and the scenario header (`rts ... teams 2v2`).
// `key` without the leading dashes: teams, win, mana-pct, hold, time, respawn, respawn-level, desecrate, bot-skill
// ('_' = '-'). mode_match_option_known: whether `key` is one of them (all take a value).
//   teams   1,1,2,2 (team id 0..8 per record, 0 = own side) | 2v2 / 1v3 / 2v2v2v2 (team sizes in record order) | ffa
//   win     elim,mana,timer (any of them) | none          mana-pct 1..100     hold 1..255 (seconds)
//   time    1..255 (minutes)    respawn 1..255 (s)    respawn-level 1..255 (s)    desecrate 1..254 (s) | off
//   bot-skill 1..4 (easy .. brutal)
// mode_match_rules_finish after the last option: a time limit adds MATCH_WIN_TIMER to the win bits (the default
// bits written out first) unless the win is `none`; `--win timer` without a time limit is an error.
bool mode_match_option_known(const std::string &key);
bool mode_match_option(const std::string &key, const std::string &value, MatchRules *m, std::string *err);
bool mode_match_rules_finish(MatchRules *m, std::string *err);
// "win elim+mana, mana 75% for 30 s, respawn 10+5/level s, desecrate 25 s, teams 1,1,2,2" (log lines)
std::string mode_match_rules_describe(const MatchRules &m);

// A savestate written during a mode run (its MODE chunk) starts that run again so the state can be loaded:
// mcport's load_state_slot calls this before game_level_begin(header.level) when header.level is
// MODE_LEVEL_INDEX. Reads `path` (savegame.h layout: 96-byte header, then tag / length chunks). Returns false
// (nothing changed) when the file has no valid MODE chunk.
bool mode_start_run_for_state(const char *game_dir, const char *state_path);
