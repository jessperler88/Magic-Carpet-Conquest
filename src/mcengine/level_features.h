// Level-start feature generation of carpet.exe (terrain_generate_features_34db0, the wall / path /
// canyon / ridge builders, the terrain-shaping class-10 effects and their constructors, wizard
// castles and castle footprints). Owner: level_features.cpp; the run-time terrain painting they
// use lives in terrain_paint.cpp.
#pragma once
#include "thing.h"

// Binds the terrain-effect constructors / update handlers to the class tables and installs
// g_hook_effect_wizard_init.
void level_features_register_handlers();

// Loads the data files this subsystem needs: data/building.tab + building.dat (the castle footprint
// maps the original reaches through the pointer at 0xadfb0) and data/search.dat (spiral walk order,
// see terrain_paint.h). When it was never called the first user loads from MC_DEFAULT_GAME_DIR.
bool level_features_load_data(const char *game_dir);

// terrain_generate_features_34db0: spawn every class-10 THING_INIT with DisId == 0xffff (clearing the
// record), then level_run_terrain_effects().
void terrain_generate_features();
// level_spawn_terrain_effects_34d60: the same pass without clearing the records, followed by
// level_flag_terrain_effects (used by level_finish_3d4e0).
void level_spawn_terrain_effects(LevelData *level);
// level_flag_terrain_effects_34f40: SwiId := 1 on every wall / path / canyon / ridge record.
void level_flag_terrain_effects(LevelData *level);
// level_spawn_effect_record_34e00 (level_spawn_effect_direct_34ed7 is its fall-through tail).
void level_spawn_effect_record(LevelData *level, ThingInit *rec);
// level_build_linked_feature_34c40: walk a Parent / Child chain of wall, path, canyon or ridge
// records from its head and run the builder on every segment.
void level_build_linked_feature(LevelData *level, ThingInit *rec);
// The four segment builders (cell coordinates of the two end points).
void level_build_wall(int x0, int y0, int x1, int y1);      // level_build_wall_342e0
void level_build_path(int x0, int y0, int x1, int y1);      // 0x34570
void level_build_canyon(int x0, int y0, int x1, int y1);    // 0x346b0
void level_build_ridge(int x0, int y0, int x1, int y1);     // 0x34760

// Helpers 0x34250..0x34bb0.
// Cell-coordinate arguments of the helpers and builders wrap on the world torus (world.h); cells are
// world_cell indices.
int  math_wrap_diff(int a, int b, int modulus);                               // math_wrap_diff_34250: b - a wrapped (16-bit; modulus = g_world.size for cells)
void terrain_set_pos_scratch(int x0, int y0, int x1, int y1);                 // terrain_set_pos_scratch_34280
int  terrain_rect_height_range(int x, int y, int dy, int dx);                 // terrain_rect_height_range_34820
void terrain_smooth_castle_border(int cx, int cy, int half_h, int half_w, int size);   // terrain_smooth_castle_border_348b0
void terrain_smooth_rect(int x, int y, int rows, int cols);                   // terrain_smooth_rect_34a00
void terrain_smooth_cell(unsigned cell);                                      // terrain_smooth_cell_34a40
int  terrain_height_avg(int x, int y, int dy, int dx);                        // terrain_height_avg_34b40
int  terrain_rect_min_height(int x, int y, int dy, int dx);                   // terrain_rect_min_height_34bb0

// Castle footprint maps (data/building.tab entry = castle size; span-encoded like the 2D sprites).
struct CastleFootprint {
    const uint8_t *map;     // row data: 0 = end of row, n < 0 = skip -n cells, n > 0 = n instruction bytes
    uint32_t       map_len; // bytes available from `map` to the end of building.dat
    uint8_t        w, h;    // cells; doubled in 320x200 (tab relocation), halved again by the castle code
};
const CastleFootprint *castle_footprint(unsigned size);

void effect_wizard_init(Thing *t, int size);        // effect_wizard_init_35090
void thing_set_castle_extents(Thing *t, int size);  // thing_set_castle_extents_353f0
void castle_stamp_footprint(Thing *t);              // castle_stamp_footprint_26320 (player_spawn_3f360 calls it on thing 0)

// Castle site tests (0x11820..0x11be0).
int  castle_near_thing(const Thing *t);              // castle_near_thing_11820 (dead code in retail)
void castle_crush_wizards(Thing *castle);            // castle_crush_wizards_118c0: kills wizard castles (effect 0x2d) under the grown box
int  castle_footprint_clear(Thing *castle);          // castle_footprint_clear_11980: 1 when the castle may grow to level aux + 1
int  castle_site_clear_at_pos(const Pos *pos);       // castle_site_clear_at_pos_11be0: 1 when a new castle may be founded at pos

// Inhabitants a wizard castle holds = footprint w * h >> 4 (effect_wizard_init_35090), with w / h as
// castle_footprint returns them: doubled in 320x200, so the capacity is 4x there. (The shipped movie
// and its snapshot were recorded in 320x200; round 3 mistook this for a build difference.)

// Hook into the player subsystem, called at the end of the wizard castle handler (state 0x34).
extern void (*g_hook_player_note_ridge_distance)(Thing *t);   // player_note_ridge_distance_3f2c0
