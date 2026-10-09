# Port round 10 (task A): a game-mode (rts) run headless - determinism within one process, the JSON dump, a
# savestate round trip in the mode, restarts (docs/analysis/port_mode.md).
mc_unit_test(rts_headless_test tests/rts_headless_test.cpp ${MC_SIM_ALL} mcengine/frontend.cpp mcengine/fe_mode.cpp mcengine/savegame.cpp
    mcengine/ui_draw.cpp mcengine/fli.cpp)
