// The Conquest mode's front-end screens (Phase 4 round 12, task A; docs/port/BRIEFING_round12.md). Port-only.
//
// - the red urn bottom right of the main menu (only with PortSettings.mode_menu) opens the setup screen;
// - the setup screen (front-end state FE_STATE_MODE_SETUP): seed, world size, bots and their skill, teams, generator
//   controls, win conditions, a minimap preview of the generated world; "Start" leaves the front end with
//   FE_START_MODE (frontend.h) and fe_mode_params() holds the choice;
// - the urn in the multiplayer lobby: the session plays Conquest with the host's setup (mode_set_proposal, mode.h,
//   before the lobby's rules exchange);
// - the result screen (FE_STATE_MODE_RESULT) after a mode run: winners, placings and the stats of ModePlayer.
// Nothing here touches the simulation: the choice reaches it as ModeParams through mcport (mode_start_run / the
// lobby's NetGameRules).
#pragma once
#include "mode.h"

constexpr int FE_STATE_MODE_SETUP = 11;     // front-end screen states (frontend.cpp dispatch; 3 stays unused)
constexpr int FE_STATE_MODE_RESULT = 12;

// The setup screen's current choice (defaults: a random seed, 1024 cells, 3 bots, every other field 0 = default).
// mcport starts the offline run with it after FE_START_MODE.
const ModeParams &fe_mode_params();
void fe_mode_set_params(const ModeParams &p);   // e.g. the last match's setup, or a test's

// After a mode run's level ended, before mode_stop_run clears g_mode: mcport hands the final block and the local
// record over, then fe_enter(FE_STATE_MODE_RESULT).
void fe_mode_set_result(const ModeState &final_state, int local_record);
const ModeState &fe_mode_result();
int fe_mode_result_local();
