// Top-level engine API used by the port executable and the tests.
#pragma once
#include <cstdint>
#include "mc_types.h"
#include "render.h"

// Allocates the game state, loads palette / textures / tables / sky from <game_dir>. Returns false
// (after printing the reason) when a required file is missing.
bool engine_init(const char *game_dir);
void engine_shutdown();

// Loads campaign level `index` (0-based, levels/levels.dat + .tab) verbatim into g_state->level and
// runs the level start of level_load_and_init_3d3b0: terrain_build, feature generation, Thing
// spawning (switch_activate(0)), player records. Returns false on failure.
bool engine_load_level(int index);

// Loads a raw state snapshot written by the original's demo recorder, e.g.
// ("movie/gam00000.dat", "movie/map00000.dat"): GameState (pointers converted to the port's indices)
// plus the five maps. The level must not be regenerated afterwards.
bool engine_load_snapshot(const char *gam_rel, const char *map_rel);

// Camera placed above the level's first player start, looking along +y, for the free-fly viewer.
Camera engine_default_camera();

// The game directory given to engine_init.
const char *engine_game_dir();

// Port (Phase 4): the art of a generated Conquest world, render-only. On (the level start of a generated world,
// through worldgen.h g_hook_level_art), with the base data set active and Hidden Worlds installed: the texture
// tiles of every corner-class tuple containing class 6 come from Hidden Worlds' blk1 atlas (its snow; the world's
// classes are Hidden Worlds' there, terrain_build_passes(gen, true)), and Hidden Worlds' broadleaf tree (tmaps1-0
// chunk 140) becomes port sprite MC_SPRITE_OAK for SpriteDesc MC_SPRITE_DESC_OAK and its burnt one (chunk 299)
// MC_SPRITE_OAK_BURNT for MC_SPRITE_DESC_OAK_BURNT - all remapped to the base
// palette (nearest colour). A patched copy of the atlas is shown (the renderers rebuild their caches from the new
// pointer); off restores the data set's atlas, average colours and the oak's palm fallback. Nothing the
// simulation reads changes, so peers with and without Hidden Worlds stay in sync.
void world_art_apply(bool on);
bool world_art_active();

// One game tick: sprite animation, command packets (from the movie while one is playing, see
// demo_open in demo.h), simulation sub-steps, water animation phase. Returns false when a movie that
// was playing has ended.
bool engine_tick();
