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

// ==== additions by task A (round 12) ================================================================================
// Registers the screens with the front end (frontend.h fe_set_mode_screens). A static initialiser of fe_mode.cpp calls
// it already (any program that links fe_mode.cpp); mcport calls it once more at start-up. Idempotent.
void fe_mode_register();
// The screen after the result screen: 2 = the main menu, 4 = the multiplayer lobby. fe_mode_set_result picks 4 when
// Config.flags has the network bit (0x10) at that moment, else 2; this overrides it (call after fe_mode_set_result).
void fe_mode_result_return_to(int state);
// The setup screen's seed is always a concrete number: its "new seed" (a click on SEED, key R, the first visit when
// fe_mode_set_params was never called) rolls one at once, so fe_mode_params() is exactly what the preview showed.
// The minimap preview (tests): how many worlds were generated so far, the last 64 x 64 image (palette indices of the
// setup screen's palette) and its seat count. false = none yet.
int  fe_mode_preview_generations();
bool fe_mode_preview_image(uint8_t *out_64x64, int *seats);
