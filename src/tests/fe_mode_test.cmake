# Round 12 (task A): the Conquest screens (fe_mode.h) driven through fe_frame - main-menu urn -> setup -> Start, the
# lobby's urn, the result screen, screenshots (PNG) - and worldgen_preview leaving the simulation untouched.
mc_unit_test(fe_mode_test tests/fe_mode_test.cpp ${MC_SIM_ALL} mcengine/frontend.cpp mcengine/fe_mode.cpp
    mcengine/savegame.cpp mcengine/ui_draw.cpp mcengine/fli.cpp mcport/screenshot.cpp)
