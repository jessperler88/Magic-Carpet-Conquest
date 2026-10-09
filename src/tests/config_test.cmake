# Port round 7 (task E): mcport.ini / command line / environment, the controller mapping, save anywhere
# (savegame.h savestate_*), the full quick save, cell-list repair and extended-pool movies
# (docs/analysis/port_settings.md). The SDL half of gamepad.cpp is compiled out (no SDL runtime needed).
mc_unit_test(config_test tests/config_test.cpp ${MC_SIM_ALL} mcengine/frontend.cpp mcengine/fe_mode.cpp mcengine/savegame.cpp
    mcengine/ui_draw.cpp mcengine/fli.cpp mcport/config.cpp mcport/gamepad.cpp)
target_include_directories(config_test PRIVATE mcport
    $<TARGET_PROPERTY:$<IF:$<TARGET_EXISTS:SDL2::SDL2>,SDL2::SDL2,SDL2::SDL2-static>,INTERFACE_INCLUDE_DIRECTORIES>)
target_compile_definitions(config_test PRIVATE MC_GAMEPAD_NO_SDL)
