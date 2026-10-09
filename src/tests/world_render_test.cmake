# Round 11 (task E): the renderers, radar / map screen, projection export and the follow camera in a tiled
# 1024-cell world (PPMs into argv[2] / MC_WRENDER_OUT, default the current directory).
mc_unit_test(world_render_test tests/world_render_test.cpp ${MC_SIM_ALL}
  mcengine/engine.cpp mcengine/tables.cpp mcengine/render_landscape.cpp mcengine/render_things.cpp
  mcengine/sprite_cache.cpp mcengine/raster.cpp mcengine/hud.cpp mcengine/ui_draw.cpp
  mcengine/render_ext.cpp mcengine/render_ext_raster.cpp mcengine/compose.cpp mcengine/debug_overlay.cpp)
# Part 9 (the Hidden Worlds art of a generated world) needs the expansion's files: <game>/hidden, else the CD's CARPET
# folder extracted next to the sources (skipped when neither exists).
if(EXISTS "${CMAKE_SOURCE_DIR}/../extracted/gog_cd/CARPET/LEVELS" AND NOT EXISTS "${MC_GAME_DIR_CMAKE}/hidden")
  set_tests_properties(world_render_test PROPERTIES ENVIRONMENT "MC_HIDDEN_DIR=${CMAKE_SOURCE_DIR}/../extracted/gog_cd/CARPET")
endif()
