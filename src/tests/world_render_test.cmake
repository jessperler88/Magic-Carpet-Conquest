# Round 11 (task E): the renderers, radar / map screen, projection export and the follow camera in a tiled
# 1024-cell world (PPMs into argv[2] / MC_WRENDER_OUT, default the current directory).
mc_unit_test(world_render_test tests/world_render_test.cpp ${MC_SIM_ALL}
  mcengine/engine.cpp mcengine/tables.cpp mcengine/render_landscape.cpp mcengine/render_things.cpp
  mcengine/sprite_cache.cpp mcengine/raster.cpp mcengine/hud.cpp mcengine/ui_draw.cpp
  mcengine/render_ext.cpp mcengine/render_ext_raster.cpp mcengine/compose.cpp mcengine/debug_overlay.cpp)
