// Thing core: pool, cell lists, dispatch, level spawning, position / angle helpers.
// Translated from the disassembly of carpet.exe (addresses in the comments).
#include "thing.h"
#include "mc_math.h"
#include "terrain.h"
#include "gen/dispatch_tables.h"
#include "gen/thing_tables.h"
#include "tmaps.h"
#include "settings.h"
#include "tick_profile.h"
#include <cstring>
#include <vector>

uint8_t g_dummy_player_block[0x801 - 0x44f];
Pos     g_pos_scratch;

void (*g_hook_creature_wake_tick)() = nullptr;
void (*g_hook_sound_request)(int, int, int) = nullptr;
void (*g_hook_sound_fade)(int, int, int) = nullptr;
void (*g_hook_ai_record_threat)() = nullptr;
void (*g_hook_mana_totals_update)(Thing *) = nullptr;
void (*g_hook_effect_wizard_init)(Thing *, int) = nullptr;
void (*g_hook_sprite_group_priorities_clear)() = nullptr;
void (*g_hook_sprite_mark_needed_for_model)(const ThingInit *) = nullptr;
void (*g_hook_sprite_groups_reload_by_priority)() = nullptr;

// ---- dispatch ----------------------------------------------------------------------------------

static ThingUpdateFn s_update_fn[MC_NUM_CLASSES][MC_MAX_TABLE_A];
static ThingCreateFn s_create_fn[MC_NUM_CLASSES][MC_MAX_TABLE_B];
static uint32_t      s_missing_a[MC_NUM_CLASSES][MC_MAX_TABLE_A];
static uint32_t      s_missing_b[MC_NUM_CLASSES][MC_MAX_TABLE_B];

static const DispatchRec *rec_a(int cls, int state) {
    if (cls < 0 || cls >= MC_NUM_CLASSES) return nullptr;
    const DispatchClass &c = g_dispatch_classes[cls];
    return (state >= 0 && state < c.a_count) ? &c.a[state] : nullptr;
}
static const DispatchRec *rec_b(int cls, int type) {
    if (cls < 0 || cls >= MC_NUM_CLASSES) return nullptr;
    const DispatchClass &c = g_dispatch_classes[cls];
    return (type >= 0 && type < c.b_count) ? &c.b[type] : nullptr;
}

int thing_register_update(uint32_t orig_addr, ThingUpdateFn fn) {
    int n = 0;
    for (int c = 0; c < MC_NUM_CLASSES; c++)
        for (int i = 0; i < g_dispatch_classes[c].a_count; i++)
            if (g_dispatch_classes[c].a[i].handler == orig_addr) { s_update_fn[c][i] = fn; n++; }
    if (!n) std::fprintf(stderr, "thing_register_update: no Table A record has handler 0x%05x\n", orig_addr);
    return n;
}

int thing_register_create(uint32_t orig_addr, ThingCreateFn fn) {
    int n = 0;
    for (int c = 0; c < MC_NUM_CLASSES; c++)
        for (int i = 0; i < g_dispatch_classes[c].b_count; i++)
            if (g_dispatch_classes[c].b[i].handler == orig_addr) { s_create_fn[c][i] = fn; n++; }
    if (!n) std::fprintf(stderr, "thing_register_create: no Table B record has handler 0x%05x\n", orig_addr);
    return n;
}

ThingUpdateFn thing_update_fn(int cls, int state) {
    const DispatchRec *r = rec_a(cls, state);
    if (!r || !r->handler) return nullptr;
    if (!s_update_fn[cls][state]) s_missing_a[cls][state]++;
    return s_update_fn[cls][state];
}
ThingCreateFn thing_create_fn(int cls, int type) {
    const DispatchRec *r = rec_b(cls, type);
    if (!r || !r->handler) return nullptr;
    if (!s_create_fn[cls][type]) s_missing_b[cls][type]++;
    return s_create_fn[cls][type];
}
bool thing_table_a_enabled(int cls, int state) { const DispatchRec *r = rec_a(cls, state); return r && r->enabled; }
bool thing_table_b_enabled(int cls, int type)  { const DispatchRec *r = rec_b(cls, type);  return r && r->enabled; }

int thing_dispatch_report(FILE *out) {
    int n = 0;
    for (int c = 0; c < MC_NUM_CLASSES; c++) {
        for (int i = 0; i < g_dispatch_classes[c].a_count; i++)
            if (s_missing_a[c][i]) {
                const DispatchRec &r = g_dispatch_classes[c].a[i];
                if (out) std::fprintf(out, "  unported update  class %2d state %3d  0x%05x %-44s x%u\n", c, i, r.handler, r.name, s_missing_a[c][i]);
                n++;
            }
        for (int i = 0; i < g_dispatch_classes[c].b_count; i++)
            if (s_missing_b[c][i]) {
                const DispatchRec &r = g_dispatch_classes[c].b[i];
                if (out) std::fprintf(out, "  unported create  class %2d type  %3d  0x%05x %-44s x%u\n", c, i, r.handler, r.name, s_missing_b[c][i]);
                n++;
            }
    }
    return n;
}
void thing_dispatch_reset_stats() {
    std::memset(s_missing_a, 0, sizeof s_missing_a);
    std::memset(s_missing_b, 0, sizeof s_missing_b);
}

// ---- pool --------------------------------------------------------------------------------------
//
// Phase 3 extension (docs/analysis/port_pool.md). Logical stack positions map to storage as follows
// (E = g_thing_slots - 1000, the number of extension slots):
//   free stack:        position p < E -> s_ext_free[p],  p >= E -> GameState.free_list[p - E];
//                      logical top = GameState.free_top + E.
//   recyclable stack:  position p < 1000 -> GameState.active_list[p], p >= 1000 -> s_ext_active[p - 1000];
//                      logical top = GameState.active_top (the game resets it to -1 directly).
// With E = 0 both are exactly the original's arrays and every function below is the translation.

// g_thing_slots / g_thing_ext are defined in mc_globals.cpp (renderer-only unit tests use thing_at()).
static std::vector<Thing>   s_ext_things;
static std::vector<int32_t> s_ext_free;
static std::vector<int32_t> s_ext_active;
static int s_forced_slots = 0;
uint32_t g_thing_alloc_failures = 0;   // port statistic: thing_alloc calls that returned null
static std::vector<uint32_t> s_slot_gen;   // thing_slot_generation (thing.h)
uint32_t thing_slot_generation(unsigned idx) { return idx < s_slot_gen.size() ? s_slot_gen[idx] : 0; }

static inline int ext_count() { return g_thing_slots - MC_THING_SLOTS; }
static inline int free_top_logical() { return g_state->free_top + ext_count(); }
static inline void free_top_set(int logical) { g_state->free_top = logical - ext_count(); }
static inline int32_t &free_entry(int p) {
    const int e = ext_count();
    return p < e ? s_ext_free[(size_t)p] : g_state->free_list[p - e];
}
static inline int32_t &active_entry(int p) {
    return p < MC_THING_SLOTS ? g_state->active_list[p] : s_ext_active[(size_t)(p - MC_THING_SLOTS)];
}

int thing_pool_wanted_slots() {
    int n = s_forced_slots ? s_forced_slots : g_settings.thing_slots;
    if (n < MC_THING_SLOTS) n = MC_THING_SLOTS;
    if (n > MC_THING_SLOTS_MAX) n = MC_THING_SLOTS_MAX;
    return n;
}
void thing_pool_force_slots(int slots) { s_forced_slots = slots < 0 ? 0 : slots; }
int      thing_pool_ext_count()        { return ext_count(); }
int32_t *thing_pool_ext_free_stack()   { return s_ext_free.empty() ? nullptr : s_ext_free.data(); }
int32_t *thing_pool_ext_active_stack() { return s_ext_active.empty() ? nullptr : s_ext_active.data(); }
int thing_pool_live_count() {
    int n = 0;
    for (int i = 1; i < g_thing_slots; i++) n += thing_at((unsigned)i)->cls != 0;
    return n;
}

// Resize the pool to `slots` with an empty extension; the GameState part is kept as it is.
static void pool_resize(int slots) {
    const int e = slots - MC_THING_SLOTS;
    // GameState.free_top of a 1000-slot image is the original's top (-1 .. 998); a value left from an
    // extended pool (< -1) means "only extension slots were free" and becomes -1 (GameState part empty).
    const int gs_top = g_state->free_top >= -1 ? g_state->free_top : -1;
    s_ext_things.assign((size_t)e, Thing{});
    s_ext_free.assign((size_t)e, 0);
    s_ext_active.assign((size_t)e, 0);
    g_thing_slots = slots;
    g_thing_ext = e ? s_ext_things.data() : nullptr;
    for (int i = 0; i < e; i++) s_ext_free[(size_t)i] = slots - 1 - i;      // highest slot at the bottom
    g_state->free_top = gs_top;
    if (g_state->active_top >= MC_THING_SLOTS) g_state->active_top = MC_THING_SLOTS - 1;
}

// Level generation (terrain features, the terrain-shaping effects) runs with the original's 1000 slots so
// that every level's map is the original's: level 39's terrain effects run out of slots 680 times while the
// map is made (port_pool.md), and more slots would carve a different landscape. thing_pool_reset leaves
// the pool at 1000 and the extension is switched on at the level start proper, switch_activate(0).
static bool s_extend_at_start = false;

void thing_pool_ext_reset() {
    s_extend_at_start = false;
    pool_resize(thing_pool_wanted_slots());
}

// thing_pool_reset_35460
void thing_pool_reset() {
    pool_resize(MC_THING_SLOTS);            // port: the original pool while the level is generated
    s_extend_at_start = true;
    g_state->things[0].player = 0;          // &DAT_000b6e90 (dummy player block)
    g_state->free_top = -1;
    g_state->active_top = -1;
    for (int i = MC_THING_SLOTS - 1; i >= 1; i--)
        g_state->free_list[++g_state->free_top] = i;
}

// models_initialise_354c0
void models_initialise() {
    free_top_set(-1);
    g_state->active_top = -1;
    for (int i = g_thing_slots - 1; i >= 1; i--) {
        const Thing &t = *thing_at((unsigned)i);
        if (t.cls == 0) {
            const int top = free_top_logical() + 1;
            free_entry(top) = i;
            free_top_set(top);
        } else if (t.flags & 0x20400) {
            active_entry(++g_state->active_top) = i;
        }
    }
}

// thing_alloc_35560
Thing *thing_alloc() {
    Thing *t;
    int top = free_top_logical();
    if (top < 0) {
        if (g_state->active_top < 0) { g_thing_alloc_failures++; return nullptr; }
        // Recycle: the per-tick lists may reference the victim, so they are dropped.
        std::memset(g_cfg->creature_lists, 0, sizeof g_cfg->creature_lists);
        g_cfg->mana_ball_list = 0;
        g_cfg->wizard_list = 0;
        g_cfg->player_list = 0;
        g_cfg->projectile_list = 0;
        t = thing_at((unsigned)active_entry(g_state->active_top));
        thing_unlink_cell(t);
        t->cls = 0;
        g_state->active_top--;
    } else {
        t = thing_at((unsigned)free_entry(top));
        free_top_set(top - 1);
    }
    std::memset(t, 0, sizeof *t);
    int idx = thing_index(t);
    if (s_slot_gen.size() <= (size_t)idx) s_slot_gen.resize((size_t)MC_THING_SLOTS_MAX, 0);
    s_slot_gen[(size_t)idx]++;
    t->max_health = 300;
    t->flags = 8;
    t->speed_cur = 0x10;
    t->damage = 100;
    t->owner = (uint16_t)idx;
    t->filter_cls = 0xff;
    t->filter_type = 0xff;
    t->desc = 0;        // &g_move_desc[0] (0x96a10)
    t->player = 0;      // dummy player block (0xb6e90)
    t->impact_cls = 10;
    t->timer_a = 0xfa;
    t->rng = (uint32_t)idx + g_state->rng;
    t->tick = (uint8_t)idx;
    return t;
}

// thing_free_3e3f0
void thing_free(Thing *t) {
    thing_unlink_cell(t);
    t->cls = 0;
    const int top = free_top_logical() + 1;
    free_entry(top) = thing_index(t);
    free_top_set(top);
}

// thing_free_all_3e030
void thing_free_all() {
    for (int i = 1; i < g_thing_slots; i++)
        if (thing_at((unsigned)i)->cls != 0) thing_free(thing_at((unsigned)i));
}

// thing_free_count_359b0
int thing_free_count() { return free_top_logical() + 1; }

// thing_link_cell_3e250
void thing_link_cell(Thing *t, const Pos *pos) {
    if (t->flags & 4) return;
    uint16_t idx = thing_index(t);
    const uint32_t cell = world_cell_of(pos->x, pos->y);
    t->cell_prev = 0;
    uint16_t head = g_cell_things[cell];
    t->cell_next = head;
    if (head != 0) thing_at(head)->cell_prev = idx;
    g_cell_things[cell] = idx;
    Pos p = *pos;                 // pos may alias the thing's own position
    t->x = p.x; t->y = p.y; t->z = p.z;
    t->flags |= 4;
}

bool world_widen_tiled(int n) {
    if (g_world.size != 256 || n <= 256) return n == g_world.size;
    std::vector<uint8_t> maps(4 * (size_t)MC_MAP_CELLS);
    std::memcpy(maps.data(), g_map_type, MC_MAP_CELLS);
    std::memcpy(maps.data() + MC_MAP_CELLS, g_map_height, MC_MAP_CELLS);
    std::memcpy(maps.data() + 2 * MC_MAP_CELLS, g_map_light, MC_MAP_CELLS);
    std::memcpy(maps.data() + 3 * MC_MAP_CELLS, g_map_flags, MC_MAP_CELLS);
    if (!world_set_size(n)) return false;
    for (int cy = 0; cy < n; cy++)
        for (int cx = 0; cx < n; cx++) {
            const uint32_t c = world_cell(cx, cy), s = (uint32_t)(((cy & 0xff) << 8) | (cx & 0xff));
            g_map_type[c] = maps[s];
            g_map_height[c] = maps[MC_MAP_CELLS + s];
            g_map_light[c] = maps[2 * MC_MAP_CELLS + s];
            g_map_flags[c] = maps[3 * MC_MAP_CELLS + s];
        }
    // the cell lists from scratch, in slot order (each list's order is the reverse of the link order)
    for (int i = 1; i < thing_pool_slots(); i++) {
        Thing *t = thing_at((unsigned)i);
        if (!(t->flags & 4)) continue;
        t->flags &= ~4u;
        const Pos p = *thing_pos(t);
        thing_link_cell(t, &p);
    }
    return true;
}

// thing_unlink_cell_3e330
void thing_unlink_cell(Thing *t) {
    if (!(t->flags & 4)) return;
    if (t->cell_prev == 0)
        g_cell_things[world_cell_of(t->x, t->y)] = t->cell_next;
    else
        thing_at(t->cell_prev)->cell_next = t->cell_next;
    if (t->cell_next != 0)
        thing_at(t->cell_next)->cell_prev = t->cell_prev;
    t->flags &= ~4u;
}

// thing_move_to_3e1d0
int thing_move_to(Thing *t, const Pos *pos) {
    if ((t->x >> 8) == (pos->x >> 8) && (t->y >> 8) == (pos->y >> 8)) {
        Pos p = *pos;
        t->x = p.x; t->y = p.y; t->z = p.z;
        return 0;
    }
    Pos p = *pos;
    thing_unlink_cell(t);
    thing_link_cell(t, &p);
    return 1;
}

// thing_relink_cell_3e220
int thing_relink_cell(Thing *t, const Pos *pos) {
    Pos p = *pos;
    thing_unlink_cell(t);
    thing_link_cell(t, &p);
    return 1;
}

// thing_set_sprite_35240. thing_set_sprite_small_352d0 recomputes the same three extents from the
// same table fields after calling 35240, so it is this function.
void thing_set_sprite(Thing *t, int sprite) {
    const SpriteDesc *d = mc_sprite_desc((unsigned)(int16_t)sprite);
    t->frame = 0;
    t->sprite = (uint16_t)sprite;
    t->draw_type = d->draw_type < sizeof g_draw_type_frames ? g_draw_type_frames[d->draw_type] : 0;
    uint16_t half_z = (uint16_t)d->half_z, half_xy = (uint16_t)d->half_xy;
    t->ext_z0 = (int16_t)(half_z >> 1);
    t->ext_x  = (int16_t)(half_xy >> 1);
    t->ext_y  = (int16_t)(half_xy >> 1);
    t->ext_h  = (int16_t)(half_z >> 1);
}

// thing_set_sprite_double_35340
void thing_set_sprite_double(Thing *t, int sprite) {
    thing_set_sprite(t, sprite);
    t->ext_x = (int16_t)(t->ext_x * 2);
    t->ext_y = (int16_t)(t->ext_y * 2);
    t->ext_h = (int16_t)(t->ext_h * 2);
}

// thing_set_sprite_halved_35380 (cwd / sub / sar = signed divide by two toward zero)
void thing_set_sprite_halved(Thing *t, int sprite) {
    thing_set_sprite(t, sprite);
    t->ext_x = (int16_t)(t->ext_x / 2);
    t->ext_y = (int16_t)(t->ext_y / 2);
    t->ext_h = (int16_t)(t->ext_h / 2);
}

// sprite_table_init_sizes_4bd10
bool sprite_table_init_sizes(const char *game_dir) {
    mc_tmap_set set;
    if (!mc_tmap_set_load(game_dir, &set)) return false;
    for (int i = 0; i < MC_SPRITE_DESC_COUNT; i++) {
        SpriteDesc &d = g_sprite_desc[i];
        if (d.half_xy == 0 && d.half_z == 0) break;        // end of table
        mc_tmap tm;
        uint32_t w, h;
        uint8_t type;
        if (mc_tmap_get(&set, d.base_sprite, &tm)) {
            w = tm.width; h = tm.height; type = tm.unk;
            mc_tmap_free(&tm);
        } else {
            w = h = 0xff; type = 1;
        }
        if (d.half_xy == 0) {
            if (h) d.half_xy = (int16_t)((int32_t)(w * (uint16_t)d.half_z) / (int32_t)h);
        } else if (d.half_z == 0) {
            if (w) d.half_z = (int16_t)((int32_t)(h * (uint16_t)d.half_xy) / (int32_t)w);
        }
        d.draw_type = type;
    }
    mc_tmap_set_free(&set);
    return true;
}

// thing_create_35690
Thing *thing_create(const Pos *pos, int cls, int type) {
    const DispatchRec *r = rec_b(cls, type);
    if (!r || r->enabled == 0 || r->index != type) return nullptr;
    ThingCreateFn fn = thing_create_fn(cls, type);
    return fn ? fn(pos) : nullptr;
}

// thing_update_all_3dce0
void thing_update_all() {
    g_state->rng = mc_lcg(g_state->rng);
    if (g_cfg->paused & 1) return;
    // port (round 10): tick profiler markers (tick_profile.h) - clock reads only, off when g_tick_profile is null
    TickProfile *const tp = g_tick_profile;
    const int64_t tp_t0 = tp ? tick_profile_now_ns() : 0;
    if (tp) tp->substeps++;

    for (int i = 1; i < g_thing_slots; i++) {
        Thing *t = thing_at((unsigned)i);
        if (t->cls != 0 && (t->flags & 0x400)) thing_free(t);
    }

    Thing *creature_tail[20] = {};
    Thing *player_tail = nullptr, *projectile_tail = nullptr, *mana_tail = nullptr, *wizard_tail = nullptr;
    std::memset(g_cfg->creature_lists, 0, sizeof g_cfg->creature_lists);
    g_cfg->mana_ball_list = 0;
    g_cfg->wizard_list = 0;
    g_cfg->player_list = 0;
    g_cfg->projectile_list = 0;
    for (int i = 1; i < g_thing_slots; i++) {
        Thing *t = thing_at((unsigned)i);
        switch (t->cls) {
        case 3:
            if (t->health >= 0 && !(t->flags & 0x10)) {
                if (!player_tail) g_cfg->player_list = (uint32_t)i; else player_tail->next = (uint32_t)i;
                player_tail = t;
                t->next = 0;
            }
            break;
        case 5:
            if (t->health >= 0 && t->state != 0x78) {
                unsigned ty = t->type;
                if (ty < 20) {       // the original indexes its 20-entry stack array unchecked
                    if (!creature_tail[ty]) g_cfg->creature_lists[ty] = (uint32_t)i; else creature_tail[ty]->next = (uint32_t)i;
                    t->next = 0;
                    creature_tail[ty] = t;
                }
            }
            break;
        case 9:
            if (!projectile_tail) g_cfg->projectile_list = (uint32_t)i; else projectile_tail->next = (uint32_t)i;
            t->next = 0;
            projectile_tail = t;
            break;
        case 10:
            if (t->type > 0x26) {
                if (t->type < 0x29) {
                    if (!mana_tail) g_cfg->mana_ball_list = (uint32_t)i; else mana_tail->next = (uint32_t)i;
                    mana_tail = t;
                } else {
                    if (t->type != 0x2d) break;
                    if (!wizard_tail) g_cfg->wizard_list = (uint32_t)i; else wizard_tail->next = (uint32_t)i;
                    wizard_tail = t;
                }
                t->next = 0;
            }
            break;
        default:
            break;
        }
    }

    if (!(g_cfg->flags & 0x10) && g_hook_creature_wake_tick) g_hook_creature_wake_tick();
    if (g_hook_ai_record_threat) g_hook_ai_record_threat();
    if (g_hook_mana_totals_update) {
        int lp = g_state->local_player;
        g_hook_mana_totals_update(thing_at(g_state->players[lp & 7].thing));
    }
    if (tp) tp->lists_ns += tick_profile_now_ns() - tp_t0;

    for (int i = 1; i < g_thing_slots; i++) {
        Thing *t = thing_at((unsigned)i);
        if (t->cls == 0) continue;
        int cls = (int8_t)t->cls, state = (int8_t)t->state;
        const DispatchRec *r = rec_a(cls, state);
        if (!r || r->index != state) {
            // "thing class %d type %d state %d" message into a stack buffer, then delete.
            thing_mark_delete(t);
            continue;
        }
        if (r->enabled != 0) {
            ThingUpdateFn fn = thing_update_fn(cls, state);
            if (fn) {
                if (tp) {
                    const int64_t a = tick_profile_now_ns();
                    fn(t);
                    tick_profile_add_class(tp, cls, tick_profile_now_ns() - a);
                } else {
                    fn(t);
                }
            }
            t->tick++;
        }
    }
}

// ---- level spawning ----------------------------------------------------------------------------

// level_run_terrain_effects_34fa0
void level_run_terrain_effects() {
    g_state->rng = mc_lcg(g_state->rng);
    for (;;) {
        bool any = false;
        for (int i = 1; i < g_thing_slots; i++) {
            Thing *t = thing_at((unsigned)i);
            if (t->cls == 0) continue;
            bool run = false;
            if (t->cls == 10) {
                uint8_t ty = t->type;
                if (ty < 0x1b)       run = (ty >= 9 && ty <= 0xb);
                else if (ty <= 0x20) run = true;
                else if (ty < 0x2d)  run = false;
                else if (ty == 0x2d) {
                    if (t->state != 0x33) goto check_delete;     // waiting wizard: left alone
                    run = true;
                } else               run = (ty >= 0x32 && ty <= 0x33);
            }
            if (run) {
                int cls = (int8_t)t->cls, state = (int8_t)t->state;
                const DispatchRec *r = rec_a(cls, state);
                any = true;
                if (r && r->handler != 0 && r->enabled != 0) {
                    ThingUpdateFn fn = thing_update_fn(cls, state);
                    if (fn) fn(t);
                    else thing_mark_delete(t);      // unported handler: would loop forever otherwise
                }
            } else {
                thing_mark_delete(t);
            }
        check_delete:
            if (t->flags & 0x400) thing_free(t);
        }
        if (!any) return;
    }
}

// level_spawn_thing_record_35800
void level_spawn_thing_record(const ThingInit *rec) {
    if (!thing_table_b_enabled(rec->cls, rec->model)) return;
    Pos pos;
    pos.x = rec->x * 0x100 + 0x80;             // round 11: WCoord wraps (the original's u16 store at 256)
    pos.y = rec->y * 0x100 + 0x80;
    pos.z = (int16_t)terrain_height_at(&pos);
    Thing *t = thing_create(&pos, rec->cls, rec->model);
    if (!t) return;
    switch (t->cls) {
    case 0xb:
        t->owner = rec->swi_id;
        thing_set_extents(t, (int16_t)(rec->swi_sz << 8), 0x1000);
        thing_restore_health(t);
        t->flags |= 1;
        break;
    case 0xc:
        t->state = (uint8_t)(t->state + (uint8_t)rec->swi_id);
        if (rec->swi_id >= 3) {
            t->state = (uint8_t)(t->state - 3);
            t->sprite = 0x118;
            t->flags |= 0x40000;
        }
        break;
    case 0xa:
        if (t->type == 0x22) {
            t->home.x = rec->child * 0x100 + 0x80;
            t->home.y = rec->parent * 0x100 + 0x80;
        } else if (t->type == 0x2d) {
            if (g_hook_effect_wizard_init) g_hook_effect_wizard_init(t, (rec->parent + 0x10) & 0xffff);
        } else if (t->type == 4) {
            t->owner = rec->swi_id;
            int e = (int16_t)(rec->swi_sz << 8);
            thing_set_extents(t, e, e);
            thing_restore_health(t);
        }
        break;
    default:
        break;
    }
}

// switch_activate_356e0
void switch_activate(unsigned dis_id, bool clear_records) {
    LevelData &lv = g_state->level;
    if (dis_id == 0 && s_extend_at_start) {
        // port: the level is generated - from here on the pool has the wanted size (an empty extension
        // below the GameState part: nothing the original's pool holds changes)
        s_extend_at_start = false;
        pool_resize(thing_pool_wanted_slots());
    }
    if (dis_id == 0) {
        g_state->creature_count = 0;
        std::memset(g_state->spells_present, 0, sizeof g_state->spells_present);
        if (g_hook_sprite_group_priorities_clear) g_hook_sprite_group_priorities_clear();
        for (ThingInit &r : lv.things) {
            if (g_hook_sprite_mark_needed_for_model) g_hook_sprite_mark_needed_for_model(&r);
            if (r.cls == 5) {
                if (r.model != 0xc && r.model != 0xd && r.model != 0xe && r.model != 0xf && r.model != 9)
                    g_state->creature_count++;
            } else if (r.cls == 0xc) {
                if (r.model < 24) g_state->spells_present[r.model]++;   // unchecked in the original
            }
        }
        if (g_hook_sprite_groups_reload_by_priority) g_hook_sprite_groups_reload_by_priority();
    }
    models_initialise();
    for (ThingInit &r : lv.things) {
        if (r.cls != 0 && r.dis_id == dis_id) {
            level_spawn_thing_record(&r);
            if (clear_records) r.cls = 0;
        }
    }
    g_state->active_top = -1;
}

// ---- terrain probes ----------------------------------------------------------------------------

// terrain_height_at_10bc0
int terrain_height_at(const Pos *p) { return terrain_sample_height(p->x, p->y); }

// terrain_height_at_offset_10be0
int terrain_height_at_offset(const Pos *p, int yaw, int dist) {
    unsigned a = (unsigned)yaw & 0x7ff;
    int32_t dy = (int32_t)((uint32_t)dist * (uint32_t)mc_cos(a)) >> 16;
    int32_t dx = (int32_t)((uint32_t)dist * (uint32_t)mc_sin(a)) >> 16;
    // round 11: the original's 16-bit sums ((int16_t)p->y - dy, dx + (int16_t)p->x) wrapped onto the world
    return terrain_sample_height(world_wrap(p->x + dx), world_wrap(p->y - dy));
}

// terrain_cell_flag_bit_103d0
uint32_t terrain_cell_flag_bit(const Pos *p) {
    return 1u << (g_map_flags[world_cell_of(p->x, p->y)] & 0xf);
}

// terrain_type_mask_at_10480: texture id of the cell -> terrain bit (jump table at 0x103f4).
uint32_t terrain_type_mask_at(const Pos *p) {
    static const uint32_t mask[0x23] = {
        0x1, 0x2, 0x4, 0x8, 0x10, 0x20, 0x800000, 0x800000, 0x100, 0x200, 0x100000, 0x200000,
        0x400000, 0x0, 0x0, 0x400, 0x400, 0x400, 0x400, 0x400, 0x400, 0x20000, 0x20000, 0x40000,
        0x20000, 0x80000, 0x10000, 0x80000, 0x400, 0x400, 0x400, 0x400, 0x400, 0x400, 0x400,
    };
    uint8_t tex = g_map_type[world_cell_of(p->x, p->y)];
    return tex > 0x22 ? 0x800000u : mask[tex];
}

// terrain_slope_vector_3e4b0
void terrain_slope_vector(const Pos *p, int16_t out[2]) {
    const int cx = world_cx(p->x), cy = world_cx(p->y);
    int h00 = g_map_height[world_cell(cx, cy)];
    int h10 = g_map_height[world_cell(cx + 1, cy)];
    int h11 = g_map_height[world_cell(cx + 1, cy + 1)];
    int h01 = g_map_height[world_cell(cx, cy + 1)];
    out[0] = (int16_t)(h00 - h10 - h11 + h01);
    out[1] = (int16_t)(h00 + h10 - h11 - h01);
}

// ---- position / angle helpers ------------------------------------------------------------------

// math_atan2_4cc33(dx, dy): both arguments are sign-extended words. 0 = toward -y, 0x200 = +x,
// 0x400 = +y, 0x600 = -x; result 0..0x800.
int math_atan2(int dx_in, int dy_in) {
    int32_t dx = (int16_t)dx_in, dy = (int16_t)dy_in;
    if (dx == 0 && dy == 0) return 0;
    auto at = [](uint32_t num, uint32_t den) -> int { return mc_atan_raw((num << 8) / den); };
    if (dx >= 0) {
        if (dy >= 0) {
            if (dx >= dy) return (uint16_t)(at(dy, dx) + 0x200);
            return (uint16_t)(0x400 - at(dx, dy));
        }
        dy = -dy;
        if (dx >= dy) return (uint16_t)(0x200 - at(dy, dx));
        return (uint16_t)at(dx, dy);
    }
    dx = -dx;
    if (dy >= 0) {
        if (dx >= dy) return (uint16_t)(0x600 - at(dy, dx));
        return (uint16_t)(at(dx, dy) + 0x400);
    }
    dy = -dy;
    if (dx >= dy) return (uint16_t)(at(dy, dx) + 0x600);
    return (uint16_t)(0x800 - at(dx, dy));
}

// Round 11: math_atan2 of a world difference. In the 256-cell world exactly math_atan2 (the arguments are
// sign-extended words); in a wider one both components are halved together until they fit a word.
int math_atan2_w(int32_t dx, int32_t dy) {
    if (g_world.size != 256)
        while (dx >= 0x8000 || dx < -0x8000 || dy >= 0x8000 || dy < -0x8000) { dx /= 2; dy /= 2; }
    return math_atan2(dx, dy);
}

// math_rotate_offset_3e420
void math_rotate_offset(Pos *p, int yaw, int pitch, int dist) {
    int16_t d = (int16_t)dist;
    if (d == 0) return;
    unsigned ya = (unsigned)yaw & 0x7ff, pa = (unsigned)pitch & 0x7ff;
    if (pa != 0) {
        p->z = (int16_t)(p->z - ((mc_sin(pa) * (int32_t)d) >> 16));
        d = (int16_t)((mc_cos(pa) * (int32_t)d) >> 16);
    }
    p->x = p->x + ((mc_sin(ya) * (int32_t)d) >> 16);         // the original's 16-bit sum; WCoord wraps
    p->y = p->y - ((mc_cos(ya) * (int32_t)d) >> 16);
}

// pos_follow_ground_3e560
int pos_follow_ground(Pos *p, int ground, int lo, int hi, int step) {
    int z = p->z, r = 0;
    if (z > hi + ground) {
        p->z = (int16_t)(p->z + step);
        r = 1;
    } else if (z > ground + lo) {
        p->z = (int16_t)(step * 25 / 100 + z);
        r = 1;
    }
    if (p->z < ground + lo) {
        p->z = (int16_t)(ground + lo);
        r = 1;
    }
    return r;
}

// pos_sink_or_follow_ground_3e5f0
int pos_sink_or_follow_ground(Pos *p, int ground, int lo, int hi, int step) {
    int r = 0;
    if ((terrain_cell_flag_bit(p) & 1) && ground == 0) lo = -0x300;
    int z = p->z;
    if (z > hi + ground) {
        p->z = (int16_t)(p->z + step);
        r = 1;
    } else if (z > lo + ground) {
        p->z = (int16_t)(step * 25 / 100 + z);
        r = 1;
    }
    if (p->z < lo + ground) {
        p->z = (int16_t)(lo + ground);
        r = 1;
    }
    if (p->z == -0x300) r = -1;
    return r;
}

// pos_angle_to_3e6b0
int pos_angle_to(const Pos *from, const Pos *to) {
    return math_atan2_w(world_signed(to->x - from->x), world_signed(to->y - from->y));
}

// pos_pitch_to_3e6e0
int pos_pitch_to(const Pos *a, const Pos *b) {
    int d = pos_dist_xy(a, b);
    return math_atan2_w((int16_t)(a->z - b->z), g_world.size == 256 ? (int16_t)(-d) : -d);
}

// pos_pitch_from_dz_3e710
int pos_pitch_from_dz(int z0, int z1, int dist) {
    return math_atan2((int16_t)(z1 - z0), (int16_t)dist);
}

static inline int iabs(int v) { return v < 0 ? -v : v; }

// pos_dist_manhattan_3e730 (operands sign-extended before the subtraction: no 16-bit wrap)
int pos_dist_manhattan(const Pos *a, const Pos *b) {
    return iabs(world_signed(a->x) - world_signed(b->x)) + iabs(world_signed(a->y) - world_signed(b->y)) + iabs(a->z - b->z);
}

// pos_dist_chebyshev_xy_3e860 (16-bit wrapped differences)
int pos_dist_chebyshev_xy(const Pos *a, const Pos *b) {
    int dx = iabs(world_signed(b->x - a->x)), dy = iabs(world_signed(b->y - a->y));
    return dx >= dy ? dx : dy;
}

// pos_dist_sq_xyz_3e8f0 (round 11: int64 beyond the 256-cell world, world.h world_dist_sq3)
int64_t pos_dist_sq_xyz(const Pos *a, const Pos *b) {
    return world_dist_sq3(world_signed(b->x - a->x), world_signed(b->y - a->y), (int16_t)(b->z - a->z));
}
// pos_dist_xyz_3e8a0
int pos_dist_xyz(const Pos *a, const Pos *b) { return (int)world_isqrt(pos_dist_sq_xyz(a, b)); }
// pos_dist_sq_xy_3e970
int64_t pos_dist_sq_xy(const Pos *a, const Pos *b) {
    return world_dist_sq(world_signed(b->x - a->x), world_signed(b->y - a->y));
}
// pos_dist_xy_3e930
int pos_dist_xy(const Pos *a, const Pos *b) { return (int)world_isqrt(pos_dist_sq_xy(a, b)); }

// angle_diff_3e770
int angle_diff(int a, int b) {
    int d = iabs((a & 0x7ff) - (b & 0x7ff));
    if (d > 0x400) d = 0x800 - d;
    return d;
}

// angle_turn_dir_3e7a0
int angle_turn_dir(int from, int to) {
    int d = (to & 0x7ff) - (from & 0x7ff);
    if (d == 0) return 0;
    if (iabs(d) > 0x400) d += d < 0 ? 0x800 : -0x800;
    if (d == 0) return 0;
    return d / iabs(d);
}

// angle_turn_step_3e800 (the third argument is pushed by every caller but never read)
int angle_turn_step(int cur, int target, int /*unused*/, int max_step) {
    if ((uint16_t)cur == (uint16_t)target) return 0;
    int diff = angle_diff(cur & 0xffff, target & 0xffff);
    int dir = angle_turn_dir(cur & 0xffff, target & 0xffff);
    int step = diff;
    if ((int16_t)diff > (int)(uint16_t)max_step) step = (uint16_t)max_step;
    return step * dir;
}

// math_bbox_overlap_10530: ext = {z offset, half x, half y, half height}
int math_bbox_overlap(const Pos *pa, const int16_t *ea, const Pos *pb, const int16_t *eb) {
    if (iabs(world_signed(pb->x - pa->x)) >= ea[1] + eb[1]) return 0;
    if (iabs(world_signed(pb->y - pa->y)) >= ea[2] + eb[2]) return 0;
    if (iabs((pa->z + ea[0]) - (pb->z + eb[0])) >= ea[3] + eb[3]) return 0;
    return 1;
}

// thing_find_in_sight_of_class_3e9a0: the last living thing of class `cls` with another owner
// within the searcher's sight radius.
Thing *thing_find_in_sight_of_class(Thing *t, int cls) {
    Thing *found = nullptr;
    for (int i = 1; i < g_thing_slots; i++) {
        Thing *o = thing_at((unsigned)i);
        if ((int8_t)o->cls != (int)(uint8_t)cls) continue;
        if (o->flags & 0x20) continue;
        if (o->owner == t->owner) continue;
        if (o->health < 0) continue;
        int range = (int16_t)mc_move_desc(t->desc)->sight_radius;
        if ((uint32_t)pos_dist_xyz(thing_pos(o), thing_pos(t)) <= (uint32_t)range) found = o;
    }
    return found;
}

// ---- snapshot support --------------------------------------------------------------------------

// Raw 32-bit views: in a saved image the port's index fields still hold the original pointers.
uint32_t g_snapshot_things_base = 0;

bool thing_relink_snapshot(GameStateImage *s) {
    // Thing pointers are base + 0x7463 + idx*0xa4. The free stack holds exactly the slots whose
    // class is 0, so its largest pointer belongs to the highest free slot.
    int top = s->free_top;
    if (top < -1 || top >= MC_THING_SLOTS) return false;
    uint32_t things_base;
    if (top >= 0) {
        uint32_t max_ptr = 0;
        for (int i = 0; i <= top; i++) max_ptr = (uint32_t)s->free_list[i] > max_ptr ? (uint32_t)s->free_list[i] : max_ptr;
        int max_idx = 0;
        for (int i = MC_THING_SLOTS - 1; i >= 1; i--) if (s->things[i].cls == 0) { max_idx = i; break; }
        if (!max_idx) return false;
        things_base = max_ptr - (uint32_t)max_idx * sizeof(ThingImage);
    } else {
        // Full pool (free stack empty, e.g. level 49 from tick 2 on - round 5 reference): player 0's
        // Thing.player points at its P block, GameState + 0x340b + 0x44f.
        uint32_t pp = s->things[s->players[0].thing % MC_THING_SLOTS].player;
        if (!pp) return false;
        things_base = pp - (uint32_t)(offsetof(GameStateImage, players) + offsetof(PlayerRec, p)) +
                      (uint32_t)offsetof(GameStateImage, things);
    }
    g_snapshot_things_base = things_base;
    uint32_t state_base = things_base - (uint32_t)offsetof(GameStateImage, things);
    auto to_index = [&](uint32_t ptr) -> int32_t {
        if (ptr < things_base) return 0;
        uint32_t d = ptr - things_base;
        return (d % sizeof(ThingImage) == 0 && d / sizeof(ThingImage) < MC_THING_SLOTS) ? (int32_t)(d / sizeof(ThingImage)) : 0;
    };
    for (int i = 0; i <= top; i++) {
        int32_t idx = to_index((uint32_t)s->free_list[i]);
        if (idx <= 0) return false;
        s->free_list[i] = idx;
    }
    for (int i = 0; i <= s->active_top && i < MC_THING_SLOTS; i++)
        s->active_list[i] = to_index((uint32_t)s->active_list[i]);

    // Descriptor pointers: demo_relink_state_pointers_3dc10 rebases them so that player 0's thing
    // has g_move_desc[7] (0x96af0); do the same and convert to an index.
    uint32_t anchor = s->things[s->players[0].thing % MC_THING_SLOTS].desc;
    uint32_t players_lo = state_base + (uint32_t)offsetof(GameStateImage, players);
    uint32_t players_hi = state_base + (uint32_t)offsetof(GameStateImage, commands);
    for (int i = 0; i < MC_THING_SLOTS; i++) {
        ThingImage &t = s->things[i];
        if (i != 0 && t.cls == 0) { t.next = 0; t.desc = 0; t.player = 0; continue; }
        t.next = (uint32_t)to_index(t.next);
        uint32_t d = t.desc - anchor + 7u * sizeof(MoveDesc);
        t.desc = (d % sizeof(MoveDesc) == 0 && d / sizeof(MoveDesc) < 30) ? d / (uint32_t)sizeof(MoveDesc) : 0;
        t.player = (t.player >= players_lo && t.player < players_hi) ? t.player - state_base : 0;
    }
    return true;
}

bool thing_load_snapshot_image(const void *bytes, GameState *out, bool keep_head_opts) {
    static GameStateImage img;
    std::memcpy(&img, bytes, sizeof img);
    if (keep_head_opts) {
        uint8_t *d = reinterpret_cast<uint8_t *>(&img);
        const uint8_t *s = reinterpret_cast<const uint8_t *>(out);
        std::memcpy(d, s, 4);
        std::memcpy(d + 0x2195, s + 0x2195, 0x24);
    }
    const bool ok = thing_relink_snapshot(&img);
    state_from_image(img, out);
    return ok;
}
