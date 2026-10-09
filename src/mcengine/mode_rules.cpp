// Match rules v1 (mode_rules.h). Round 12 task B. STUB from the integrator: task B replaces it.
// (g_mode_teams, g_hook_mode_side_of_owner and mode_same_side_records live in mode.cpp: every unit test links it, and
// same_side() is called from the simulation core, MC_SIM_CORE.)
#include "mode_rules.h"

void mode_rules_level_start() {}
void mode_rules_tick() {}

bool mode_match_over() { return g_mode.match_state == MATCH_OVER; }
int  mode_respawn_seconds(int) { return 0; }
int  mode_desecration_pct(int) { return 0; }
int  mode_time_left_seconds() { return -1; }
