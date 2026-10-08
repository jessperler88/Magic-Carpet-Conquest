// Thing (entity) core of carpet.exe: pool, per-cell lists, class/state dispatch, level spawning and
// the small position / angle helpers every handler uses. Owner: thing.cpp.
//
// Dispatch: the class tables of the exe (Table A = update handlers by Thing.state, Table B =
// constructors by Thing.type) are extracted into gen/dispatch_tables.h. Translated handlers are bound
// to those records by their *original code address*:
//
//     static void effect_fire_update(Thing *t) { ... }            // effect_fire_update_23c20
//     void effects_register_handlers() { thing_register_update(0x23c20, effect_fire_update); }
//
// A record whose handler is not ported yet behaves like an empty handler (update) or a failed
// allocation (create) and is counted, see thing_dispatch_report().
#pragma once
#include <cstdint>
#include <cstdio>
#include "mc_types.h"
#include "mc_globals.h"
#include "mc_image.h"
#include "world.h"

// ---- handler binding ---------------------------------------------------------------------------
using ThingUpdateFn = void   (*)(Thing *t);          // Table A: stack arg = thing
using ThingCreateFn = Thing *(*)(const Pos *pos);    // Table B: stack arg = position, returns thing or null

// Bind a port function to every Table A / Table B record whose original handler address is `addr`.
// Returns the number of records bound (0 = no such handler in the tables: a typo).
int thing_register_update(uint32_t orig_addr, ThingUpdateFn fn);
int thing_register_create(uint32_t orig_addr, ThingCreateFn fn);
// Raw record access (level_spawn_effect_record_34e00 and level_run_terrain_effects_34fa0 bypass the
// index / enabled checks). Null when out of range or not ported.
ThingUpdateFn thing_update_fn(int cls, int state);
ThingCreateFn thing_create_fn(int cls, int type);
bool thing_table_a_enabled(int cls, int state);
bool thing_table_b_enabled(int cls, int type);   // g_class_table[cls].table_b[type].enabled != 0
// Print the handlers that were dispatched but are not ported (name, address, hit count).
// Returns the number of distinct missing handlers.
int  thing_dispatch_report(FILE *out);
void thing_dispatch_reset_stats();

// ---- pool --------------------------------------------------------------------------------------
// Phase 3 (round 7, task C, docs/analysis/port_pool.md): the pool can be larger than the original's 1000
// slots (PortSettings::thing_slots, at most MC_THING_SLOTS_MAX). Slots 0..999 stay in GameState.things
// (layout, snapshots and saves depend on it); slots 1000.. live in a separately allocated extension.
// Everything indexes the pool through thing_at() / thing_index() and bounds loops and index guards with
// thing_pool_slots(); nothing indexes g_state->things directly.
//
// The two stacks continue outside GameState as well (thing.cpp): the free stack's *bottom* lies in the
// extension (GameState.free_top = logical top - extension size, so it goes negative while extension slots
// are free), the recyclable stack's *top* (positions >= 1000). With every extension slot free the
// GameState part of both is exactly the original's: a 1000-slot image (snapshot, save) plus an empty
// extension is a valid pool, and the game runs byte for byte as with 1000 slots until those would be full.
extern int    g_thing_slots;    // slots in use incl. slot 0 (1000 = original)
extern Thing *g_thing_ext;      // slots MC_THING_SLOTS .. g_thing_slots-1 (null with 1000)
inline Thing *thing_at(unsigned idx) {
    return idx < (unsigned)MC_THING_SLOTS ? &g_state->things[idx] : &g_thing_ext[idx - MC_THING_SLOTS];
}
inline int thing_pool_slots()                     { return g_thing_slots; }
inline uint16_t thing_index(const Thing *t) {
    const uintptr_t p = reinterpret_cast<uintptr_t>(t), b = reinterpret_cast<uintptr_t>(g_state->things);
    if (p - b < sizeof(Thing) * MC_THING_SLOTS) return (uint16_t)((p - b) / sizeof(Thing));
    return (uint16_t)(MC_THING_SLOTS + (p - reinterpret_cast<uintptr_t>(g_thing_ext)) / sizeof(Thing));
}
// The port's guard for an index field the original dereferences unchecked (`things + idx * 0xa4`): a slot
// of the pool as it is, anything else folded into the original 1000 slots as the faithful port has always
// done (`idx % 1000`). A garbage value (a sign-extended index, g_projectile_null_hit_index) therefore reads
// the same slot whatever the pool size, and the faithful result is exactly the old `idx % MC_THING_SLOTS`.
inline unsigned thing_wrap(unsigned idx) {
    return idx < (unsigned)g_thing_slots ? idx : idx % (unsigned)MC_THING_SLOTS;
}
// True when t is one of the pool's slots 1.. (the original's "&things[0] < t < &things[1000]").
inline bool thing_in_pool(const Thing *t) {
    if (!t) return false;
    const uintptr_t p = reinterpret_cast<uintptr_t>(t), b = reinterpret_cast<uintptr_t>(g_state->things);
    if (p - b < sizeof(Thing) * MC_THING_SLOTS) return p != b;
    const uintptr_t e = reinterpret_cast<uintptr_t>(g_thing_ext);
    return g_thing_ext && p - e < sizeof(Thing) * (size_t)(g_thing_slots - MC_THING_SLOTS);
}
// Pool size wanted for the next level / snapshot load: thing_pool_force_slots() if set, else
// g_settings.thing_slots, clamped to MC_THING_SLOTS..MC_THING_SLOTS_MAX.
int  thing_pool_wanted_slots();
// Port-only override of the setting (0 = none): a network session uses the host's pool size, a movie the
// size it was recorded with (the original's movies: 1000). Takes effect at the next thing_pool_reset /
// thing_pool_ext_reset.
void thing_pool_force_slots(int slots);
// Size the pool to thing_pool_wanted_slots() and empty the extension: every slot >= 1000 class 0 and on
// the free stack below the GameState part, no recyclable entry above position 999. Call it after a
// 1000-slot GameState image was loaded into g_state (movie / quick-save snapshot, save game) - the image
// is then a valid pool of that size. thing_pool_reset() (level load) instead sets the original's 1000 slots
// for the level generation (terrain effects); switch_activate(0), the level start, then sizes the pool
// like this function - so every level's map stays the original's (level 39's generation fills 1000 slots).
void thing_pool_ext_reset();
// The extension's storage, for quick saves / net checksums of an extended pool (task E, net.cpp):
// things = slots 1000..g_thing_slots-1, stack entries = the free stack's positions 0..ext-1 and the
// recyclable stack's positions 1000..g_thing_slots-2. Sizes are 0 with the original pool.
int      thing_pool_ext_count();                 // g_thing_slots - 1000
int32_t *thing_pool_ext_free_stack();            // [thing_pool_ext_count()]
int32_t *thing_pool_ext_active_stack();          // [thing_pool_ext_count()]
// Things alive (class != 0) - a port-only statistic (pool_test, the debug overlay).
int  thing_pool_live_count();
// Port statistic: thing_alloc calls that found the pool full (nothing free, nothing recyclable) since start-up.
// Never read by the game; tests and the debug overlay may reset / print it.
extern uint32_t g_thing_alloc_failures;
// Port (round 8, render-time interpolation only): ++ every time thing_alloc hands out slot `idx`, so a
// slot freed and reused within one tick is told apart from the Thing it held. Not part of any state.
uint32_t thing_slot_generation(unsigned idx);
inline Pos *thing_pos(Thing *t)                 { return reinterpret_cast<Pos *>(&t->x); }
inline const Pos *thing_pos(const Thing *t)     { return reinterpret_cast<const Pos *>(&t->x); }
// Extents block {ext_z0, ext_x, ext_y, ext_h} at Thing+0x4e as math_bbox_overlap takes it.
inline const int16_t *thing_ext(const Thing *t) { return &t->ext_z0; }

// Owner player sub-block ("P", PlayerRec+0x44f). Thing.player holds its byte offset inside the
// GameState; 0 selects the dummy block (DAT_000b6e90, all zero in the exe's BSS).
extern uint8_t g_dummy_player_block[0x801 - 0x44f];
inline uint8_t *thing_player_block(const Thing *t) {
    return t->player ? reinterpret_cast<uint8_t *>(g_state) + t->player : g_dummy_player_block;
}
inline uint32_t player_block_offset(int player) {   // value stored in Thing.player for player p
    return (uint32_t)(offsetof(GameState, players) + player * sizeof(PlayerRec) + offsetof(PlayerRec, p));
}

// DAT_000adfc4: the shared position scratch many spawners fill before calling a constructor.
extern Pos g_pos_scratch;

void   thing_pool_reset();                         // thing_pool_reset_35460
void   models_initialise();                        // models_initialise_354c0: rebuild free / recyclable stacks
Thing *thing_alloc();                              // thing_alloc_35560 (null when the pool is exhausted)
void   thing_free(Thing *t);                       // thing_free_3e3f0
void   thing_free_all();                           // thing_free_all_3e030
int    thing_free_count();                         // thing_free_count_359b0
inline void thing_mark_delete(Thing *t) { t->flags |= 0x400; }   // thing_mark_delete_3e3e0

void thing_link_cell(Thing *t, const Pos *pos);    // thing_link_cell_3e250 (copies pos, sets flag 4)
void thing_unlink_cell(Thing *t);                  // thing_unlink_cell_3e330
int  thing_move_to(Thing *t, const Pos *pos);      // thing_move_to_3e1d0: 1 when the cell changed
int  thing_relink_cell(Thing *t, const Pos *pos);  // thing_relink_cell_3e220: always 1

void thing_set_sprite(Thing *t, int sprite);        // thing_set_sprite_35240 (also 352d0, which is identical)
void thing_set_sprite_double(Thing *t, int sprite); // thing_set_sprite_double_35340
void thing_set_sprite_halved(Thing *t, int sprite); // thing_set_sprite_halved_35380
inline void thing_set_extents(Thing *t, int xy, int z) {         // thing_set_extents_353d0
    t->ext_x = t->ext_y = (int16_t)xy; t->ext_h = (int16_t)z;
}
inline void thing_restore_health(Thing *t)        { t->health = t->max_health; }        // 35080
inline void thing_set_mana_from_health(Thing *t)  { t->mana = t->max_health >> 1; }     // 35230
inline int  thing_set_state(Thing *t, int state)  { t->state = (uint8_t)state; return state & 0xff; }  // 3ea50
inline int  thing_anim_advance(Thing *t) {                       // thing_anim_advance_3ea70: 1 = last frame reached
    if (t->frame < t->draw_type) { t->frame++; return 0; }
    return 1;
}

// sprite_table_init_sizes_4bd10: complete g_sprite_desc from data/tmaps.dat (the extent that is 0 in
// the exe is derived from the tmap's aspect ratio, draw_type = tmap header byte 1). Must run before
// any thing_set_sprite; engine_init does it. Returns false when tmaps.dat cannot be read.
bool sprite_table_init_sizes(const char *game_dir);

// thing_create_35690(pos, class, type): Table B dispatch with the enabled + index checks.
Thing *thing_create(const Pos *pos, int cls, int type);

// thing_update_all_3dce0: one simulation sub-step (frees flagged things, rebuilds the per-class
// lists in g_cfg, runs every Table A handler, ++Thing.tick).
void thing_update_all();
// Subsystem hooks called from thing_update_all between the list pass and the handler pass. Null
// until the owning subsystem is ported and installs them from its *_register_handlers().
extern void (*g_hook_creature_wake_tick)();                       // creature_wake_tick_468e0
extern void (*g_hook_ai_record_threat)();                         // ai_record_threat_from_projectiles_150f0
extern void (*g_hook_mana_totals_update)(Thing *local_player);    // mana_totals_update_427d0

// ---- sound requests ----------------------------------------------------------------------------
// sound_request_49720(thing index, player, sound id) / sound_fade_player_sound_49c40: game code only
// *requests* positional sounds; the sound system (not ported yet) installs the hooks. Translated code
// calls these wrappers at every original call site instead of leaving a TODO.
extern void (*g_hook_sound_request)(int thing, int player, int sound);
extern void (*g_hook_sound_fade)(int thing, int player, int sound);
inline void sound_request(int thing, int player, int sound) { if (g_hook_sound_request) g_hook_sound_request(thing, player, sound); }
inline void sound_fade(int thing, int player, int sound)    { if (g_hook_sound_fade) g_hook_sound_fade(thing, player, sound); }

// ---- level spawning ----------------------------------------------------------------------------
// level_run_terrain_effects_34fa0: runs the terrain-shaping class-10 handlers (types 9..0xb,
// 0x1b..0x20, 0x2d in state 0x33, 0x32, 0x33) until none is left; everything else is deleted.
void level_run_terrain_effects();
// level_spawn_thing_record_35800: THING_INIT -> Thing at the cell centre, plus the per-class fix-ups.
void level_spawn_thing_record(const ThingInit *rec);
// switch_activate_356e0(dis_id, clear): spawn every level record whose DisId matches; dis_id 0 is
// the level start (also counts creatures / spells and reloads the sprite groups).
void switch_activate(unsigned dis_id, bool clear_records);
extern void (*g_hook_effect_wizard_init)(Thing *t, int arg);      // effect_wizard_init_35090
extern void (*g_hook_sprite_group_priorities_clear)();            // sprite_group_priorities_clear_4bec0
extern void (*g_hook_sprite_mark_needed_for_model)(const ThingInit *rec);   // sprite_mark_needed_for_model_4bee0
extern void (*g_hook_sprite_groups_reload_by_priority)();         // sprite_groups_reload_by_priority_4bfb0

// ---- terrain probes ----------------------------------------------------------------------------
int      terrain_height_at(const Pos *p);                         // terrain_height_at_10bc0
int      terrain_height_at_offset(const Pos *p, int yaw, int dist);   // terrain_height_at_offset_10be0
uint32_t terrain_cell_flag_bit(const Pos *p);                     // terrain_cell_flag_bit_103d0: 1 << (flags & 0xf)
uint32_t terrain_type_mask_at(const Pos *p);                      // terrain_type_mask_at_10480 (MoveDesc.terrain_mask bits)
void     terrain_slope_vector(const Pos *p, int16_t out[2]);      // terrain_slope_vector_3e4b0

// ---- position / angle helpers (0x3e420..0x3e9a0, 0x4cc33) --------------------------------------
int  math_atan2(int dx, int dy);                                  // math_atan2_4cc33: 0 = -y, 0x200 = +x (args are 16-bit)
int  math_atan2_w(int32_t dx, int32_t dy);                        // round 11: of a world difference (world.h)
void math_rotate_offset(Pos *p, int yaw, int pitch, int dist);    // math_rotate_offset_3e420
int  pos_follow_ground(Pos *p, int ground, int lo, int hi, int step);          // pos_follow_ground_3e560
int  pos_sink_or_follow_ground(Pos *p, int ground, int lo, int hi, int step);  // pos_sink_or_follow_ground_3e5f0
int  pos_angle_to(const Pos *from, const Pos *to);                // pos_angle_to_3e6b0
int  pos_pitch_to(const Pos *a, const Pos *b);                    // pos_pitch_to_3e6e0
int  pos_pitch_from_dz(int z0, int z1, int dist);                 // pos_pitch_from_dz_3e710
int  pos_dist_manhattan(const Pos *a, const Pos *b);              // pos_dist_manhattan_3e730
int  pos_dist_chebyshev_xy(const Pos *a, const Pos *b);           // pos_dist_chebyshev_xy_3e860
int  pos_dist_xyz(const Pos *a, const Pos *b);                    // pos_dist_xyz_3e8a0
int64_t pos_dist_sq_xyz(const Pos *a, const Pos *b);              // pos_dist_sq_xyz_3e8f0 (int32 wrap at 256 cells)
int  pos_dist_xy(const Pos *a, const Pos *b);                     // pos_dist_xy_3e930
int64_t pos_dist_sq_xy(const Pos *a, const Pos *b);               // pos_dist_sq_xy_3e970 (int32 wrap at 256 cells)
int  angle_diff(int a, int b);                                    // angle_diff_3e770: 0..0x400
int  angle_turn_dir(int from, int to);                            // angle_turn_dir_3e7a0: -1, 0, +1
int  angle_turn_step(int cur, int target, int unused, int max_step);   // angle_turn_step_3e800
int  math_bbox_overlap(const Pos *pa, const int16_t *ext_a, const Pos *pb, const int16_t *ext_b);  // 10530
inline int thing_collide(const Thing *a, const Thing *b) {       // thing_collide_105c0
    return math_bbox_overlap(thing_pos(a), thing_ext(a), thing_pos(b), thing_ext(b));
}
Thing *thing_find_in_sight_of_class(Thing *t, int cls);           // thing_find_in_sight_of_class_3e9a0

// ---- snapshot support --------------------------------------------------------------------------
// Convert a raw GameState image saved by the original (movie/gam*.dat, save games) in place: every
// 32-bit pointer field becomes the port's index / offset form (see mc_types.h). `image` must be
// sizeof(GameState) bytes. Returns false when the block base cannot be derived from the free list.
bool thing_relink_snapshot(GameStateImage *image);
// Round 11: `bytes` (MC_STATE_IMAGE_SIZE, an original image) relinked and converted into `out` (mc_image.h);
// `keep_head_opts` keeps out's first dword and option bytes 0x2195..0x21b8 as demo_load_state_3c200 does.
bool thing_load_snapshot_image(const void *bytes, GameState *out, bool keep_head_opts = false);
// Address of things[0] in the run that wrote the last image thing_relink_snapshot converted (0 before
// the first). The original's (ptr - &things[0]) / 0xa4 on a null pointer depends on it
// (g_projectile_null_hit_index).
extern uint32_t g_snapshot_things_base;
