// The Conquest match between the front end and the level (mode_flow.h). Round 12 task D.
#include "mode_flow.h"
#include "mode_level.h"
#include "fe_mode.h"
#include "frontend.h"
#include "game.h"
#include "sound.h"
#include "net.h"
#include "mc_globals.h"
#include <string>

ModeParams mode_flow_menu_params(const ModeParams &setup, uint32_t fresh_seed) {
    ModeParams mp = setup;
    if (mp.seed == 0) mp.seed = fresh_seed ? fresh_seed : 1;
    mp.humans = 1;
    if (mp.bots > 7) mp.bots = 7;
    mp.flags |= MODE_PARAM_DEBUG;
    std::string err;
    if (!mode_match_rules_finish(&mp.match, &err)) mp.match.win &= (uint8_t)~MATCH_WIN_TIMER;
    return mp;
}

bool mode_flow_lobby_agreed() {
    NetGameRules r{};
    return (g_cfg->flags & 0x10) && net_rules_applied(&r) && r.mode != GAME_MODE_ORIGINAL && r.mode < GAME_MODE_COUNT;
}

int mode_flow_enter() {
    mode_campaign_save();
    return game_after_frontend(MODE_LEVEL_INDEX);
}

void mode_flow_abort() {
    mode_stop_run();
    mode_campaign_restore();
}

void mode_flow_finish(bool forfeit) {
    const int local = g_state->local_player & 7;
    fe_mode_set_result(mode_result_snapshot(local, forfeit), local);
    mode_stop_run();
    mode_campaign_restore();
    game_before_frontend();
    fe_enter(FE_STATE_MODE_RESULT);
}

void mode_flow_forfeit() {
    sound_stop_all();
    music_stop();
    game_level_end();           // the loop did not end: this only leaves the level
    mode_flow_finish(true);
}
