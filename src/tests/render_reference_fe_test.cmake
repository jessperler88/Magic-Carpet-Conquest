# Round 6 (task D): the front end against the original's frames (tools/reference/fb: patch_carpet.py --fe,
# run_reference.py --fe -> extracted/reference/fe; docs/analysis/port_render_reference2.md). SKIP without
# the dumps; every dump of the tour must be pixel-identical (MC_RFE_LENIENT=1 to only list differences).
# Round 12: fe_mode.cpp is linked (its screens register with the front end) - with mode_menu off nothing changes.
mc_unit_test(render_reference_fe_test tests/render_reference_fe_test.cpp ${MC_SIM_ALL} mcengine/frontend.cpp
    mcengine/fe_mode.cpp mcengine/savegame.cpp mcengine/ui_draw.cpp mcengine/fli.cpp)
