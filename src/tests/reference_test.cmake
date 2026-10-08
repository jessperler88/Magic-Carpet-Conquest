# Per-tick comparison with the original (tools/reference): movie 0 (the ctest gate) and, given a
# run_level.py reference directory, a campaign level. Lists its sources explicitly (the simulation plus
# the HUD's per-tick game-state writes hud_tick_state - the original renders once per tick) instead of
# linking the whole mcengine library.
mc_unit_test(reference_test tests/reference_test.cpp ${MC_SIM_ALL} mcengine/hud.cpp mcengine/ui_draw.cpp
             mcengine/raster.cpp mcengine/tables.cpp mcengine/render_landscape.cpp mcengine/render_things.cpp
             mcengine/sprite_cache.cpp)
# Round 5: the campaign-level references of tools/reference/run_level.py (suite mode: every levelNN/ with
# an index.json under the directory; SKIP / pass when none exists) - long playbacks and the level
# generation of every level.
add_test(NAME reference_levels COMMAND reference_test "${MC_GAME_DIR_CMAKE}" "${CMAKE_SOURCE_DIR}/../extracted/reference")
add_test(NAME reference_gen COMMAND reference_test "${MC_GAME_DIR_CMAKE}" "${CMAKE_SOURCE_DIR}/../extracted/reference/gen")
# Round 11 session 3 (task G, docs/analysis/port_world_refwide.md): movie 0 in the tiled 1024-cell world
# (MC_REF_WORLD, world_widen_tiled after the snapshot load). Movie 0 has an AI castle at (0,0) and the local wizard
# at cell 2: the first interactions across the old 255 -> 256 boundary happen in its first tick (413), so the gate
# (MC_REF_WORLD_GATE=16) is: the RNG identical, every slot that diverges is a boundary case (it, its owner or its
# target within 16 cells of the old boundary, outside tile (0,0), or a pair whose shortest way crosses it), every
# differing map cell within 16 cells of it - up to tick 548, the last tick before the first divergence that is a
# consequence of earlier ones away from the boundary (549).
add_test(NAME reference_world COMMAND reference_test "${MC_GAME_DIR_CMAKE}"
         "${CMAKE_SOURCE_DIR}/../extracted/reference/movie0" 548)
set_tests_properties(reference_world PROPERTIES ENVIRONMENT "MC_REF_WORLD=1024;MC_REF_WORLD_GATE=16")
