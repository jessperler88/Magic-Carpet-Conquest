// The Conquest match between the front end and the level (Phase 4 round 12, task D; docs/analysis/port_r12_flow.md).
// Port-only. The engine half of mcport's flow, so a headless test (mode_flow_test) runs the same sequence:
//
//   setup screen "Start" (FE_START_MODE)       lobby FE_START_LEVEL with an agreed mode (mode_flow_lobby_agreed)
//       params = mode_flow_menu_params(fe_mode_params(), seed)
//   level = mode_flow_enter()                   campaign progress aside, game_after_frontend(MODE_LEVEL_INDEX)
//   start the run + game_level_begin(MODE_LEVEL_INDEX)   (mcport rts_begin: mode_start_run / mode_start_agreed_net_run)
//       (failed: mode_flow_abort())
//   game_level_tick until it is not GAME_RUNNING, game_level_end()
//   mode_flow_finish(false)                     the result for the front end (fe_mode_set_result), the run stopped,
//                                               the campaign back, game_before_frontend, fe_enter(FE_STATE_MODE_RESULT)
//   pause menu "Forfeit the match" offline: mode_flow_forfeit() (the level left at once, then mode_flow_finish(true))
//
// Nothing here is simulated: the result screen reads a copy of g_mode (mode_result_snapshot, mode_level.h).
#pragma once
#include "mode.h"

// The params an offline match from the setup screen plays: seed 0 = `fresh_seed` (the platform's random), one human,
// at most 7 bots, debug commands on (as in the campaign), the match rules completed (mode_match_rules_finish; a timer
// win without a time limit is dropped).
ModeParams mode_flow_menu_params(const ModeParams &setup, uint32_t fresh_seed);
// The lobby left with FE_START_LEVEL in a network game whose agreed rules (net.h net_rules_applied) carry a mode.
bool mode_flow_lobby_agreed();
// Before the run starts: the campaign's progress kept aside (mode_campaign_save), game_after_frontend(MODE_LEVEL_INDEX)
// (fade, title, sound banks). Returns the level to play (MODE_LEVEL_INDEX).
int  mode_flow_enter();
// The run could not start: the campaign back.
void mode_flow_abort();
// The run's level ended (after game_level_end) or was forfeited: fe_mode_set_result(mode_result_snapshot(local,
// forfeit), local), mode_stop_run, mode_campaign_restore, game_before_frontend, fe_enter(FE_STATE_MODE_RESULT).
void mode_flow_finish(bool forfeit);
// Offline forfeit while the level runs: sound / music stopped, game_level_end (the level is left without its end
// sequence), mode_flow_finish(true).
void mode_flow_forfeit();
