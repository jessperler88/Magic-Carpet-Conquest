# Port round 12 (task D): the Conquest flow headless - main menu urn -> setup screen -> Start -> a 1024 match with 3
# bots to its end by the match rules (1-minute timer) -> result screen -> main menu; the pause menu's forfeit; the
# match-rule options of `rts` / the scenario header (docs/analysis/port_r12_flow.md). mcport's flow: mode_flow.cpp +
# rts_run.cpp's rts_begin.
mc_unit_test(mode_flow_test tests/mode_flow_test.cpp ${MC_SIM_ALL} mcengine/frontend.cpp mcengine/fe_mode.cpp
    mcengine/savegame.cpp mcengine/ui_draw.cpp mcengine/fli.cpp mcengine/game.cpp mcengine/mode_flow.cpp
    mcport/rts_run.cpp mcport/mclog.cpp)
target_include_directories(mode_flow_test PRIVATE mcport)
