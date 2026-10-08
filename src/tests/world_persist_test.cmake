# Round 11 task D: persistence / checksum / debug commands / the mode-run hook in a wide (tiled 1024) world
# (docs/analysis/port_world_persist.md).
mc_unit_test(world_persist_test tests/world_persist_test.cpp ${MC_SIM_ALL} mcengine/frontend.cpp mcengine/savegame.cpp
    mcengine/ui_draw.cpp mcengine/fli.cpp)
