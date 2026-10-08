// Test for constructors.cpp (the Table B create handlers). argv[1] = game dir.
//
// 1. Unit checks: every constructor bound by constructors_register_handlers is called once on an
//    empty pool (class / type / state sanity, cell link), and the segmented creatures (dragon, worm,
//    kraken) are checked link by link.
// 2. Level 38 is built the way engine_load_level does it (level file -> terrain_build ->
//    thing_pool_reset -> models_initialise -> switch_activate(0, true)); level features (agent A's
//    terrain_generate_features) are not linked here. The resulting pool is compared with the
//    engine's own snapshot movie/gam00000.dat (raw GameState 413 ticks into level 38), which is kept
//    in a separate buffer:
//    pass A ("plain")   - exactly the sequence above; static things are matched by class / type /
//                         cell to find the index mapping between the two pools;
//    pass B ("aligned") - the same, with the slots that feature generation owns in the original
//                         pre-occupied, GameState.rng set to the value the snapshot implies and the
//                         terrain taken from movie/map00000.dat, so that every thing lands in its
//                         original slot with its original seed and can be compared field by field,
//                         by index. Fields that a per-tick handler is known to change are either
//                         advanced on our side before the comparison (tick counter, tree z / flag,
//                         ...) or listed as explained; anything else fails the test.
// 3. Every level of levels.dat is loaded and spawned (crash + pool consistency test) and
//    thing_dispatch_report shows the constructors that level files use but nobody has bound.
#define _CRT_SECURE_NO_WARNINGS
#include "constructors.h"
#include "mc_math.h"
#include "terrain.h"
#include "gen/dispatch_tables.h"
#include "mcfile.h"
#include "rnc.h"
#include "crash_handler.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static const int kLevel = 38;

// ---- level loading (as engine_load_level) --------------------------------------------------------

static mc_blob s_tab, s_dat;

static bool levels_open(const char *game_dir) {
    char path[1024];
    mc_path_join(path, sizeof path, game_dir, "levels/levels.tab");
    if (!mc_read_file(path, &s_tab)) { std::fprintf(stderr, "cannot read %s\n", path); return false; }
    mc_path_join(path, sizeof path, game_dir, "levels/levels.dat");
    if (!mc_read_file(path, &s_dat)) { std::fprintf(stderr, "cannot read %s\n", path); return false; }
    return true;
}
// levels.tab has room for 1000 offsets; the entries after the last level repeat the end offset.
static int levels_count() {
    int n = (int)(s_tab.len / 4) - 1, used = 0;
    for (int i = 0; i < n; i++) {
        uint32_t a, b;
        std::memcpy(&a, s_tab.data + i * 4, 4);
        std::memcpy(&b, s_tab.data + (i + 1) * 4, 4);
        if (b > a && b <= s_dat.len) used = i + 1;
    }
    return used;
}

// The per-level reset of level_load_file_3d160 (also zeroes the per-type creature serials at +0xc).
static void level_reset() {
    std::memset(&g_state->padc, 0, 0x28 - 0xc);   // +0xc..+0x27: serials, volcano_thing, volcano_smoke
    g_state->free_top = 0;
    std::memset(g_state->things, 0, sizeof g_state->things);
    g_state->creature_count = 0;
    std::memset(g_state->spells_present, 0, sizeof g_state->spells_present);
    std::memset(g_state->start_pos, 0, sizeof g_state->start_pos);
    std::memset(g_cell_things, 0, g_world.cells * sizeof(uint16_t));
    std::memset(g_map_type, 0, g_world.cells);
    std::memset(g_map_height, 0, g_world.cells);
    std::memset(g_map_light, 0, g_world.cells);
    std::memset(g_map_flags, 0, g_world.cells);
}

static bool level_load(int index) {
    if (index < 0 || index >= levels_count()) return false;
    uint32_t a, b;
    std::memcpy(&a, s_tab.data + index * 4, 4);
    std::memcpy(&b, s_tab.data + (index + 1) * 4, 4);
    if (a >= s_dat.len || b > s_dat.len || b <= a) return false;
    level_reset();
    long got = rnc_unpack(s_dat.data + a, b - a, reinterpret_cast<uint8_t *>(&g_state->level), sizeof(LevelData));
    return got == (long)sizeof(LevelData);
}

// ---- the snapshot --------------------------------------------------------------------------------

static GameState *s_snap = nullptr;          // relinked copy of movie/gam00000.dat
static mc_blob    s_snap_maps;               // movie/map00000.dat (type, height, light, flags, cells, ...)

static bool snapshot_load(const char *game_dir) {
    char path[1024];
    mc_blob gam;
    mc_path_join(path, sizeof path, game_dir, "movie/gam00000.dat");
    if (!mc_read_file(path, &gam)) { std::printf("snapshot: %s missing\n", path); return false; }
    mc_path_join(path, sizeof path, game_dir, "movie/map00000.dat");
    if (!mc_read_file(path, &s_snap_maps)) { std::printf("snapshot: %s missing\n", path); mc_blob_free(&gam); return false; }
    if (gam.len != MC_STATE_IMAGE_SIZE || s_snap_maps.len < 0x60000) { mc_blob_free(&gam); return false; }
    s_snap = static_cast<GameState *>(std::calloc(1, sizeof(GameState)));
    const bool ok = thing_load_snapshot_image(gam.data, s_snap);
    mc_blob_free(&gam);
    return ok;
}

// ---- spawning ------------------------------------------------------------------------------------

struct SpawnOptions {
    std::vector<int> taken_slots;        // slots occupied before the level records are spawned
    bool     set_rng = false;            // override GameState.rng before the spawn
    uint32_t rng = 0;
    bool     snapshot_terrain = false;   // replace the four terrain maps by the snapshot's
};

// level_load_and_init_3d3b0 from "Generate map" on, without the feature generator.
static void level_spawn(const SpawnOptions &o) {
    terrain_build(g_state->level.gen);
    if (o.snapshot_terrain && s_snap_maps.data) {
        std::memcpy(g_map_type, s_snap_maps.data, 0x10000);
        std::memcpy(g_map_height, s_snap_maps.data + 0x10000, 0x10000);
        std::memcpy(g_map_light, s_snap_maps.data + 0x20000, 0x10000);
        std::memcpy(g_map_flags, s_snap_maps.data + 0x30000, 0x10000);
    }
    thing_pool_reset();
    // Stand-ins for terrain_generate_features(): the things that survive feature generation in the
    // original (class 10 type 0x2d) keep their slots; models_initialise rebuilds the free stack
    // around them, exactly as it does in the original.
    for (int slot : o.taken_slots) {
        Thing &t = g_state->things[slot];
        t.cls = 10; t.type = 0x2d; t.state = 0x34; t.owner = (uint16_t)slot;
    }
    if (o.set_rng) g_state->rng = o.rng;
    std::memset(&g_pos_scratch, 0, sizeof g_pos_scratch);
    models_initialise();
    g_state->active_top = -1;
    switch_activate(0, true);
}

// Every cell list holds exactly the linked things of that cell, and every live thing is sane.
static void check_pool_consistency(const char *what) {
    int linked = 0, in_lists = 0, live = 0, bad = 0;
    for (int i = 1; i < MC_THING_SLOTS; i++) {
        const Thing &t = g_state->things[i];
        if (!t.cls) continue;
        live++;
        if (t.flags & 4) linked++;
        if (t.desc >= 30 || t.player != 0 || t.cls >= MC_NUM_CLASSES) bad++;
        else {
            const DispatchClass &c = g_dispatch_classes[t.cls];
            if (t.state >= c.a_count || c.a[t.state].index != t.state) bad++;
        }
    }
    std::vector<uint8_t> seen(MC_THING_SLOTS);
    for (int cell = 0; cell < MC_MAP_CELLS; cell++) {
        uint16_t prev = 0;
        for (uint16_t i = g_cell_things[cell]; i; i = g_state->things[i].cell_next) {
            const Thing &t = g_state->things[i];
            if (i >= MC_THING_SLOTS || seen[i]) { bad++; break; }
            seen[i] = 1; in_lists++;
            if (!t.cls || !(t.flags & 4) || mc_cell_of(t.x, t.y) != cell || t.cell_prev != prev) bad++;
            prev = i;
        }
    }
    int free_slots = 0;
    for (int i = 1; i < MC_THING_SLOTS; i++) if (!g_state->things[i].cls) free_slots++;
    // A slot can be class 0 and off the free stack (the allocate-and-drop constructors), never the
    // other way round.
    if (bad || in_lists != linked || thing_free_count() > free_slots) {
        std::printf("FAIL pool consistency (%s): %d bad, %d linked, %d in cell lists, %d live, %d free-stack / %d free slots\n",
                    what, bad, linked, in_lists, live, thing_free_count(), free_slots);
        g_fail++;
    }
}

// ---- 1. unit checks ------------------------------------------------------------------------------

static bool agent_a_effect(int type) {
    return type == 9 || type == 0xa || type == 0xb || (type >= 0x1b && type <= 0x20) || type == 0x2d || type == 0x32 || type == 0x33;
}

static void test_each_constructor() {
    level_reset();
    thing_pool_reset();
    thing_dispatch_reset_stats();
    Pos pos{0x4080, 0x5080, 0x120};
    int created = 0, null_ok = 0, records = 0;
    for (int cls = 1; cls < MC_NUM_CLASSES; cls++) {
        const DispatchClass &c = g_dispatch_classes[cls];
        for (int type = 0; type < c.b_count; type++) {
            if (!c.b[type].handler) continue;
            if (cls == 10 && agent_a_effect(type)) continue;
            records++;
            int free_before = thing_free_count();
            Thing *t = thing_create(&pos, cls, type);
            bool expect_null = cls == 1 || cls == 6 || cls == 8 || (cls == 7 && type < 4) || (cls == 3 && type >= 4) ||
                               (cls == 10 && (type == 0x14 || type == 0x15 || type == 0x16 || type == 0x18));
            if (expect_null) {
                if (t) { std::printf("FAIL class %d type %d: expected null\n", cls, type); g_fail++; }
                // allocate-and-drop takes a slot; the flyer markers and effect_create_shared_37ca0 do not
                bool takes_slot = cls == 1 || cls == 6 || cls == 8 || cls == 7;
                if (thing_free_count() != free_before - (takes_slot ? 1 : 0)) { std::printf("FAIL class %d type %d: slot use\n", cls, type); g_fail++; }
                null_ok++;
                continue;
            }
            if (!t) { std::printf("FAIL class %d type %d (%s): create failed\n", cls, type, c.b[type].name); g_fail++; continue; }
            created++;
            if (cls == 10 && type == 0x23) {        // effect_create_type35_39670: the raw allocation
                if (t->cls != 0) { std::printf("FAIL class 10 type 0x23: class %d\n", t->cls); g_fail++; }
                thing_free(t);
                continue;
            }
            bool ok = t->cls == cls && t->type == type && t->state < c.a_count && c.a[t->state].index == t->state &&
                      c.a[t->state].handler != 0 && t->desc < 30 && t->player == 0;
            if (t->flags & 4) ok = ok && mc_cell_of(t->x, t->y) == mc_cell_of(pos.x, pos.y) && g_cell_things[mc_cell_of(t->x, t->y)] == thing_index(t);
            if (!ok) {
                std::printf("FAIL class %d type %d (%s): class %d type %d state %d flags 0x%x\n", cls, type, c.b[type].name, t->cls, t->type, t->state, t->flags);
                g_fail++;
            }
        }
    }
    const PosImage pos_image{(uint16_t)(int32_t)pos.x, (uint16_t)(int32_t)pos.y, pos.z};
    for (int p = 0; p < 8; p++) CHECK(!std::memcmp(g_state->start_pos[p], &pos_image, 6));
    int missing = thing_dispatch_report(nullptr);
    CHECK(missing == 0);
    check_pool_consistency("each constructor");
    std::printf("== unit: %d Table B records in scope: %d create a thing, %d return null by design, %d unbound\n",
                records, created, null_ok, missing);
}

// Head + n segments: parent / child chain, copies of the head, state 0x78.
static void check_chain(const char *name, int type, int segments, int head_sprite, const int *seg_sprites, int head_mana,
                        int first_seg_mana, int seg_mana) {
    level_reset();
    thing_pool_reset();
    g_state->rng = 0x1234;
    Pos pos{0x2080, 0x3080, 0x40};
    Thing *h = thing_create(&pos, 5, type);
    CHECK(h != nullptr);
    if (!h) return;
    int hi = thing_index(h);
    bool ok = hi == 1 && thing_free_count() == 999 - 1 - segments && h->sprite == head_sprite && h->parent == 0 &&
              h->child == 2 && h->mana == head_mana && h->health == h->max_health && (h->flags & 4) && h->tick == 0 &&
              g_state->padc[type] == 1;
    uint16_t prev = (uint16_t)hi;
    for (int i = 0; i < segments; i++) {
        const Thing &s = g_state->things[2 + i];
        ok = ok && s.cls == 5 && s.type == type && s.state == 0x78 && s.parent == prev && s.owner == hi &&
             s.child == (i + 1 < segments ? 3 + i : 0) && s.tick == i && s.sprite == seg_sprites[i] &&
             s.mana == (i == 0 ? first_seg_mana : seg_mana) &&
             s.max_health == h->max_health && s.health == s.max_health && s.desc == h->desc && (s.flags & 4) &&
             s.x == pos.x && s.y == pos.y && s.timer_a == h->timer_a && s.yaw == h->yaw && s.filter_cls == 3;
        prev = (uint16_t)(2 + i);
    }
    // the cell list holds the head first (linked last), then the segments newest first
    ok = ok && g_cell_things[mc_cell_of(pos.x, pos.y)] == hi;
    if (!ok) { std::printf("FAIL %s chain\n", name); g_fail++; }
    check_pool_consistency(name);
    std::printf("== unit: %s = head #%d (sprite 0x%x, state %d, hp %d, mana %d / total %d, desc %u, timer_a %d) + %d segments (speed 0x%x, mana %d)\n",
                name, hi, h->sprite, h->state, h->max_health, h->mana, h->mana_total, h->desc, h->timer_a, segments,
                g_state->things[2].speed, g_state->things[2].mana);
}

static void test_segmented() {
    int dragon[16], worm[16], kraken[2] = {0x32, 0xc1};
    for (int i = 0; i < 16; i++) { dragon[i] = 0x13 + i; worm[i] = 0x59 + i; }
    // dragon: every iteration writes mana_total / 32 into the *head*, so the first segment (copied
    // before that write) keeps 2250 and the later copies carry 140; worm: head 2250, every segment
    // 4500 / 32; kraken: head and both segments 4500 / 3.
    check_chain("dragon", 0, 16, 0x28, dragon, 4500 / 32, 2250, 4500 / 32);
    check_chain("worm", 3, 16, 0x58, worm, 2250, 4500 / 32, 4500 / 32);
    check_chain("kraken", 6, 2, 0x31, kraken, 1500, 1500, 1500);
    // the dragon needs 16 free slots, the worm does not
    level_reset();
    thing_pool_reset();
    Pos pos{0x2080, 0x3080, 0x40};
    for (int i = 0; i < 999 - 15; i++) { Thing *t = thing_alloc(); if (t) t->cls = 2; }
    CHECK(thing_free_count() == 15);
    CHECK(thing_create(&pos, 5, 0) == nullptr && thing_free_count() == 15);
    CHECK(thing_create(&pos, 5, 6) == nullptr && thing_free_count() == 15);
    Thing *w = thing_create(&pos, 5, 3);        // head + 14 segments, the last two allocations fail
    CHECK(w != nullptr && thing_free_count() == 0);
    check_pool_consistency("worm on a nearly full pool");
}

// ---- 2. comparison with the snapshot -------------------------------------------------------------

struct Field { const char *name; size_t off, size; };
#define F(n) { #n, offsetof(Thing, n), sizeof(static_cast<Thing *>(nullptr)->n) }
static const Field kFields[] = {
    F(next), F(rng), F(max_health), F(health), F(flags), F(cell_next), F(cell_prev), F(owner), F(aux),
    F(prop_flags), F(yaw), F(pitch), F(target_yaw), F(target_pitch), F(killer), F(last_attacker),
    F(caster), F(damage), F(z_vel), F(cast_ticks), F(duration), F(parent), F(child), F(speed),
    F(timer_a), F(timer_b), F(spell_flags), F(burst), F(unk3e), F(tick), F(cls), F(type),
    F(filter_cls), F(filter_type), F(impact_cls), F(impact_type), F(state), F(castle_size), F(x), F(y),
    F(z), F(ext_z0), F(ext_x), F(ext_y), F(ext_h), F(sprite), F(frame), F(draw_type), F(damage_slots),
    F(speed_cur), F(speed_base), F(turn_rate), F(mana_cost), F(mana_total), F(mana), F(mana_owner),
    F(target), F(unk94), F(home), F(desc), F(player),
};
#undef F
static const int kNumFields = (int)(sizeof kFields / sizeof kFields[0]);
static uint64_t field_value(const Thing &t, const Field &f) {
    uint64_t v = 0;
    std::memcpy(&v, reinterpret_cast<const uint8_t *>(&t) + f.off, f.size < 8 ? f.size : 8);
    return v;
}

struct DiffStat { int n = 0, idx = 0; uint64_t ours = 0, ref = 0; };
struct KindStat {
    int pairs = 0;
    std::map<int, DiffStat> diff;        // field index -> stats
};
using KindKey = std::pair<int, int>;     // class, type
static std::map<KindKey, KindStat> s_stats;

static void compare_pair(const Thing &ours, const Thing &ref, int idx) {
    KindStat &ks = s_stats[{ours.cls, ours.type}];
    ks.pairs++;
    for (int i = 0; i < kNumFields; i++) {
        const Field &f = kFields[i];
        if (!std::memcmp(reinterpret_cast<const uint8_t *>(&ours) + f.off,
                         reinterpret_cast<const uint8_t *>(&ref) + f.off, f.size)) continue;
        DiffStat &d = ks.diff[i];
        if (d.n++ == 0) { d.idx = idx; d.ours = field_value(ours, f); d.ref = field_value(ref, f); }
    }
}

// Fields that legitimately differ 413 ticks later and are not worth predicting.
struct Explained { int cls, type; const char *fields; const char *why; };   // -1 = any
static const Explained kExplained[] = {
    {-1, -1, "next",                "per-tick class list link, rebuilt by thing_update_all_3dce0"},
    {-1, -1, "cell_next cell_prev", "cell list neighbours: other things entered / left the cell"},
    {5,  -1, "x y z yaw target_yaw aux state timer_a speed_cur target cast_ticks filter_type "
             "sprite frame ext_z0 ext_x ext_y ext_h castle_size rng health damage_slots",
                                    "creature AI ran 412 times (moved, turned, animated, thought, fought)"},
    {10, 0x27, "x y z z_vel timer_a cast_ticks speed_cur mana mana_owner sprite ext_z0 ext_x ext_y ext_h home rng frame yaw damage_slots tick",
                                    "mana balls roll downhill, merge (mana / sprite / extents), are claimed, or the slot was reused by a new ball (effect_mana_ball_update_25980)"},
};
static const char *explained(int cls, int type, const char *field) {
    size_t n = std::strlen(field);
    for (const Explained &e : kExplained) {
        if ((e.cls >= 0 && e.cls != cls) || (e.type >= 0 && e.type != type)) continue;
        for (const char *p = e.fields; (p = std::strstr(p, field)) != nullptr; p += n)
            if ((p == e.fields || p[-1] == ' ') && (p[n] == 0 || p[n] == ' ')) return e.why;
    }
    return nullptr;
}

// What the per-tick code is known to have done to a thing between its creation and the snapshot;
// applied to our copy so that the comparison stays exact.
static int  s_updates = 0;               // thing_update_all calls so far, mod 256
static bool s_local_owns_spell[24];
static const char *const kAdvanceNotes[] = {
    "every class: tick += the number of thing_update_all_3dce0 calls (mod 256)",
    "trees: flags |= 0x20000 and z = terrain height at (x, y) - scenery_tree_update_43ba0 does both on every call",
    "switches: flags &= ~1 - player_commands_process_3a8b0 (0x3b5d5) clears bit 0 of every class-11 thing",
    "spell pickups: flags |= 1 when the local player already owns the spell (spell_dropped_update_46ae0)",
};
static Thing advance_to_snapshot(const Thing &t) {
    Thing e = t;
    e.tick = (uint8_t)(e.tick + s_updates);
    if (e.cls == 2 && e.type == 0) {
        e.flags |= 0x20000;
        e.z = (int16_t)terrain_height_at(thing_pos(&e));
    } else if (e.cls == 11) {
        e.flags &= ~1u;
    } else if (e.cls == 12 && e.type < 24 && s_local_owns_spell[e.type]) {
        e.flags |= 1;
    }
    return e;
}

static void count_kinds(const Thing *things, std::map<KindKey, int> &out) {
    for (int i = 1; i < MC_THING_SLOTS; i++) if (things[i].cls) out[{things[i].cls, things[i].type}]++;
}

static void print_counts(const char *title) {
    std::map<KindKey, int> ours, ref;
    count_kinds(g_state->things, ours);
    count_kinds(s_snap->things, ref);
    std::set<KindKey> keys;
    for (auto &k : ours) keys.insert(k.first);
    for (auto &k : ref) keys.insert(k.first);
    std::printf("%s\n   class type   ours  snapshot\n", title);
    int to = 0, tr = 0;
    for (const KindKey &k : keys) {
        int o = ours.count(k) ? ours[k] : 0, r = ref.count(k) ? ref[k] : 0;
        to += o; tr += r;
        std::printf("   %5d 0x%02x  %5d  %5d%s\n", k.first, k.second, o, r, o == r ? "" : "   <-- differs");
    }
    std::printf("   total       %5d  %5d\n", to, tr);
}

static bool is_static(const Thing &t) { return t.cls == 2 || t.cls == 11 || t.cls == 12; }
static uint8_t s_plain_start_pos[8][6];      // GameState.start_pos after pass A

// Pass A: match by class / type / cell, report the index mapping. Returns the first delta.
static int pass_plain() {
    level_load(kLevel);
    std::map<KindKey, int> recs;
    for (const ThingInit &r : g_state->level.things) if (r.cls && r.dis_id == 0) recs[{r.cls, r.model}]++;
    std::printf("== level %d: level-start records (DisId 0) per class / model:", kLevel);
    for (auto &r : recs) std::printf(" c%d m0x%02x x%d;", r.first.first, r.first.second, r.second);
    std::printf("\n");
    SpawnOptions o;
    level_spawn(o);
    std::memcpy(s_plain_start_pos, g_state->start_pos, sizeof s_plain_start_pos);
    check_pool_consistency("level 38 plain");
    print_counts("== pass A (plain spawn): live things per class / type");

    std::map<std::tuple<int, int, int>, std::vector<int>> by_cell;      // (class, type, cell) -> snapshot indices
    for (int i = 1; i < MC_THING_SLOTS; i++) {
        const Thing &r = s_snap->things[i];
        if (r.cls) by_cell[{r.cls, r.type, mc_cell_of(r.x, r.y)}].push_back(i);
    }
    std::map<int, int> delta_hist;
    std::map<int, std::pair<int, int>> matched_by_class;    // class -> (matched, total)
    int first_delta = 0, last_delta = 0, changes = 0;
    bool have = false;
    for (int i = 1; i < MC_THING_SLOTS; i++) {
        const Thing &t = g_state->things[i];
        if (!t.cls || !is_static(t)) continue;
        matched_by_class[t.cls].second++;
        auto it = by_cell.find({t.cls, t.type, mc_cell_of(t.x, t.y)});
        if (it == by_cell.end() || it->second.empty()) continue;
        int best = it->second.front();
        for (int c : it->second) if (have && c - i == last_delta) best = c;      // several trees in one cell
        int d = best - i;
        delta_hist[d]++;
        matched_by_class[t.cls].first++;
        if (!have) { first_delta = d; have = true; }
        else if (d != last_delta) {
            if (changes++ < 6) std::printf("   index mapping changes at our #%d (snapshot #%d): %+d -> %+d\n", i, best, last_delta, d);
        }
        last_delta = d;
    }
    std::printf("== pass A: static things (scenery, switches, spell pickups) found in the snapshot at the same class / type / cell:\n");
    for (auto &m : matched_by_class) std::printf("   class %2d: %d of %d\n", m.first, m.second.first, m.second.second);
    std::printf("   index mapping snapshot_index - our_index:");
    for (auto &d : delta_hist) std::printf(" %+d x%d", d.first, d.second);
    std::printf("\n");
    return first_delta;
}

static void print_kind_table(int cls_lo, int cls_hi, bool enforce) {
    for (auto &ks : s_stats) {
        int cls = ks.first.first, type = ks.first.second;
        if (cls < cls_lo || cls > cls_hi) continue;
        int unexplained = 0;
        for (auto &d : ks.second.diff) if (!explained(cls, type, kFields[d.first].name)) unexplained++;
        std::printf("   class %2d type 0x%02x: %3d pairs, %d of %d fields differ (%d unexplained)\n",
                    cls, type, ks.second.pairs, (int)ks.second.diff.size(), kNumFields, unexplained);
        for (auto &d : ks.second.diff) {
            const Field &f = kFields[d.first];
            const char *why = explained(cls, type, f.name);
            std::printf("      %-13s %3d / %3d   e.g. #%d ours 0x%llx ref 0x%llx   %s%s\n", f.name, d.second.n, ks.second.pairs,
                        d.second.idx, (unsigned long long)d.second.ours, (unsigned long long)d.second.ref,
                        why ? "explained: " : (enforce ? "UNEXPLAINED" : "(not enforced)"), why ? why : "");
            if (!why && enforce) g_fail++;
        }
        std::printf("      identical in all %d pairs:", ks.second.pairs);
        for (int i = 0; i < kNumFields; i++) if (!ks.second.diff.count(i)) std::printf(" %s", kFields[i].name);
        std::printf("\n");
    }
}

// Test stand-in for effect_create_volcano_38b70 (agent A's; level 38 has two volcano records among
// its level-start records, and they are gone again before the snapshot): occupies a slot.
static Thing *volcano_stand_in(const Pos *pos) {
    Thing *t = thing_alloc();
    if (!t) return nullptr;
    t->cls = 10; t->type = 9; t->state = 9;
    t->x = pos->x; t->y = pos->y; t->z = pos->z;
    return t;
}

static int aligned_score() {
    int score = 0;
    for (int i = 1; i < MC_THING_SLOTS; i++) {
        const Thing &t = g_state->things[i], &r = s_snap->things[i];
        if (t.cls && is_static(t) && r.cls == t.cls && r.type == t.type && mc_cell_of(r.x, r.y) == mc_cell_of(t.x, t.y)) score++;
    }
    return score;
}

static void pass_runtime_kinds(const std::set<int> &paired_slots);

// Pass B: aligned spawn, field-by-field comparison by index.
static void pass_aligned(int first_delta) {
    level_load(kLevel);
    uint32_t seed = (uint32_t)g_state->level.gen.seed;
    // GameState.rng at spawn time, as the snapshot implies it: thing_alloc seeds Thing.rng with
    // index + GameState.rng and no switch handler steps it.
    std::map<uint32_t, int> rng_votes;
    for (int i = 1; i < MC_THING_SLOTS; i++) {
        const Thing &r = s_snap->things[i];
        if (r.cls == 11) rng_votes[r.rng - (uint32_t)i]++;
    }
    uint32_t rng = 0; int votes = 0, voters = 0;
    for (auto &v : rng_votes) { voters += v.second; if (v.second > votes) { votes = v.second; rng = v.first; } }
    int steps = -1;
    { uint32_t s = seed; for (int k = 0; k <= 64; k++) { if (s == rng) { steps = k; break; } s = mc_lcg(s); } }
    std::printf("== pass B (aligned spawn): GameState.rng at spawn = 0x%08x (implied by %d of %d switches; level seed 0x%08x",
                rng, votes, voters, seed);
    if (steps >= 0) std::printf(" stepped %d time%s)\n", steps, steps == 1 ? "" : "s"); else std::printf(", not within 64 LCG steps of it)\n");
    CHECK(votes == voters && steps == 1);       // level_run_terrain_effects_34fa0 steps it once

    // Slots owned by feature generation: things seeded with index + the unstepped level seed.
    std::vector<int> feature_slots;
    for (int i = 1; i < MC_THING_SLOTS; i++) {
        const Thing &r = s_snap->things[i];
        if (r.cls && r.rng == seed + (uint32_t)i) feature_slots.push_back(i);
    }
    std::printf("   snapshot things created before the level records (rng = level seed + index):");
    for (int s : feature_slots) std::printf(" #%d(c%d t0x%02x)", s, s_snap->things[s].cls, s_snap->things[s].type);
    std::printf("\n");

    thing_register_create(0x38b70, volcano_stand_in);
    // The low feature slots that were freed and reused during the 413 ticks cannot be seen in the
    // snapshot, so the number of leading slots is found by trying.
    int best_p = 0, best_score = -1;
    for (int p = first_delta; p >= 0 && p >= first_delta - 8; p--) {
        SpawnOptions o;
        for (int s = 1; s <= p; s++) o.taken_slots.push_back(s);
        for (int s : feature_slots) if (s > p) o.taken_slots.push_back(s);
        o.set_rng = true; o.rng = rng; o.snapshot_terrain = true;
        level_load(kLevel);
        level_spawn(o);
        int score = aligned_score();
        if (score > best_score) { best_score = score; best_p = p; }
    }
    SpawnOptions o;
    for (int s = 1; s <= best_p; s++) o.taken_slots.push_back(s);
    for (int s : feature_slots) if (s > best_p) o.taken_slots.push_back(s);
    o.set_rng = true; o.rng = rng; o.snapshot_terrain = true;
    level_load(kLevel);
    level_spawn(o);
    thing_register_create(0x38b70, nullptr);
    check_pool_consistency("level 38 aligned");
    std::printf("   slots taken before the spawn: 1..%d", best_p);
    for (int s : feature_slots) if (s > best_p) std::printf(", %d", s);
    std::printf("; %d static things then sit at their snapshot index in their snapshot cell\n", best_score);

    // thing_update_all calls between spawn and snapshot (mod 256), from the tick counters.
    std::map<int, int> tick_votes;
    for (int i = 1; i < MC_THING_SLOTS; i++) {
        const Thing &t = g_state->things[i], &r = s_snap->things[i];
        if (t.cls && r.cls == t.cls && r.type == t.type && (is_static(t) || t.cls == 5)) tick_votes[(uint8_t)(r.tick - t.tick)]++;
    }
    int tv = 0, tn = 0;
    for (auto &v : tick_votes) { tn += v.second; if (v.second > tv) { tv = v.second; s_updates = v.first; } }
    std::printf("   tick counters: snapshot - ours = %d (mod 256) for %d of %d static things and creatures; player 0's PlayerRec.tick = %d\n",
                s_updates, tv, tn, s_snap->players[0].tick);
    CHECK(tv == tn && s_updates == (int)((s_snap->players[0].tick - 1) & 0xff));

    int lp = s_snap->local_player & 7;
    const uint8_t *pblock = reinterpret_cast<const uint8_t *>(s_snap) + player_block_offset(lp);
    for (int s = 0; s < 24; s++) { int16_t v; std::memcpy(&v, pblock + 0x2a4 + s * 2, 2); s_local_owns_spell[s] = v != 0; }

    s_stats.clear();
    std::map<KindKey, int> ours_only, reused, ref_only, paired;
    std::set<int> paired_slots;
    for (int i = 1; i < MC_THING_SLOTS; i++) {
        const Thing &t = g_state->things[i], &r = s_snap->things[i];
        bool taken = false;
        for (int s : o.taken_slots) taken = taken || s == i;
        if (taken) continue;
        if (t.cls && r.cls == t.cls && r.type == t.type) { compare_pair(advance_to_snapshot(t), r, i); paired[{t.cls, t.type}]++; paired_slots.insert(i); }
        else if (t.cls && !r.cls) ours_only[{t.cls, t.type}]++;
        else if (t.cls) { reused[{t.cls, t.type}]++; ref_only[{r.cls, r.type}]++; }
        else if (r.cls) ref_only[{r.cls, r.type}]++;
    }
    std::printf("   paired by index (same class / type on both sides):");
    for (auto &k : paired) std::printf(" c%d t0x%02x x%d;", k.first.first, k.first.second, k.second);
    std::printf("\n   our things whose slot is free in the snapshot (gone within 413 ticks):");
    for (auto &k : ours_only) std::printf(" c%d t0x%02x x%d;", k.first.first, k.first.second, k.second);
    std::printf("\n   our things whose slot holds another kind in the snapshot (gone, slot reused):");
    for (auto &k : reused) std::printf(" c%d t0x%02x x%d;", k.first.first, k.first.second, k.second);
    std::printf("\n   snapshot things without a counterpart of ours (created during the 413 ticks, or by code that is not linked here):");
    for (auto &k : ref_only) std::printf(" c%d t0x%02x x%d;", k.first.first, k.first.second, k.second);
    std::printf("\n   before comparing, our things are advanced by what the per-tick code is known to do:\n");
    for (const char *n : kAdvanceNotes) std::printf("      - %s\n", n);

    std::printf("== pass B: static things, field by field (all %d fields = the whole 0xa4-byte Thing):\n", kNumFields);
    print_kind_table(2, 2, true);
    print_kind_table(11, 12, true);
    std::printf("== pass B: creatures:\n");
    print_kind_table(5, 5, true);
    std::printf("== pass B: other classes present on both sides:\n");
    print_kind_table(3, 4, false);
    print_kind_table(6, 10, false);

    // every static thing and every creature record of ours must have found its partner or be
    // accounted for as gone
    // The "Flyer" records (class 3 models 4..11) only store the players' start positions. z is the
    // terrain height when the record is spawned: the snapshot terrain has since been raised by the
    // castles built on those spots, the terrain_build output of pass A lacks the level features.
    std::printf("== player start positions (GameState+0x23d9, written by the class-3 type 4..11 constructors):\n");
    bool starts_xy = true;
    int starts_z = 0;
    for (int p = 0; p < 8; p++) {
        PosImage a, b, c;
        std::memcpy(&a, g_state->start_pos[p], 6); std::memcpy(&b, s_snap->start_pos[p], 6); std::memcpy(&c, s_plain_start_pos[p], 6);
        starts_xy = starts_xy && a.x == b.x && a.y == b.y && c.x == b.x && c.y == b.y;
        starts_z += c.z == b.z;
        if (b.x || b.y) std::printf("      player %d: snapshot (0x%04x, 0x%04x, %d)  ours (0x%04x, 0x%04x), z %d on the generated terrain, %d on the snapshot terrain\n",
                                    p, (unsigned)b.x, (unsigned)b.y, b.z, (unsigned)a.x, (unsigned)a.y, c.z, a.z);
    }
    std::printf("      x / y of all 8 %s; z on the generated terrain identical for %d of 8\n", starts_xy ? "identical" : "DIFFER", starts_z);
    CHECK(starts_xy && starts_z == 8);

    auto n_paired = [&](int c, int t) { return paired[KindKey(c, t)]; };
    auto n_gone = [&](int c, int t) { return ours_only[KindKey(c, t)] + reused[KindKey(c, t)]; };
    CHECK(n_paired(2, 0) + n_gone(2, 0) == 150);
    CHECK(n_paired(2, 0) >= 145);
    CHECK(n_paired(11, 0) == 3 && n_paired(11, 2) == 2 && n_paired(12, 0x12) == 1);
    CHECK(n_paired(5, 9) == 157);

    pass_runtime_kinds(paired_slots);
}

// Pass C: the snapshot things that were created at run time (players, their spells, projectiles,
// ...) went through the same constructors, so a fresh thing of the same class / type at the same
// position must agree in every field that the creating code and the per-tick code leave alone.
static void pass_runtime_kinds(const std::set<int> &paired_slots) {
    s_stats.clear();
    s_updates = 0;
    for (int i = 1; i < MC_THING_SLOTS; i++) {
        const Thing &r = s_snap->things[i];
        if (!r.cls || paired_slots.count(i)) continue;
        if (r.cls == 10 && agent_a_effect(r.type)) continue;
        thing_pool_reset();
        std::memset(g_cell_things, 0, g_world.cells * sizeof(uint16_t));
        std::memset(&g_state->padc, 0, 0x28 - 0xc);   // +0xc..+0x27: serials, volcano_thing, volcano_smoke
        Thing *t = thing_create(thing_pos(&r), r.cls, r.type);
        if (!t) { std::printf("FAIL pass C: class %d type 0x%02x not created\n", r.cls, r.type); g_fail++; continue; }
        compare_pair(*t, r, i);
    }
    // Fields that the code which created / drives these things is expected to have changed; every
    // other field must be what the constructor leaves.
    static const Explained kRuntime[] = {
        {-1, -1,   "next rng cell_next cell_prev owner tick", "per-thing identity (slot, seed, lists, update count)"},
        {3,  0,    "flags speed_cur mana_cost mana_total mana player", ""},
        {3,  1,    "yaw pitch target_yaw ext_x ext_y sprite speed_cur mana_cost mana_total mana target unk94 home player", ""},
        {3,  2,    "max_health health flags aux state ext_z0 ext_x ext_y ext_h sprite damage_slots mana_total mana mana_owner home", ""},
        {3,  3,    "yaw state sprite mana_owner target", ""},
        {5,  4,    "health aux yaw pitch target_yaw cast_ticks timer_a damage_slots", ""},
        {9,  0xd,  "health flags yaw pitch damage filter_cls filter_type sprite target", ""},
        {10, 0x12, "aux yaw", ""},
        {10, 0x27, "yaw z_vel timer_a ext_z0 ext_x ext_y ext_h sprite damage_slots speed_cur mana mana_owner home", ""},
        {12, -1,   "flags caster mana_cost", ""},
        {12, 0x10, "mana_total mana", ""},
    };
    std::printf("== pass C: fresh things vs the snapshot things created at run time (same class / type / position):\n");
    for (auto &ks : s_stats) {
        int cls = ks.first.first, type = ks.first.second, unexpected = 0;
        std::printf("   class %2d type 0x%02x: %2d pairs, %2d of %d fields identical; differ:", cls, type, ks.second.pairs,
                    kNumFields - (int)ks.second.diff.size(), kNumFields);
        for (auto &d : ks.second.diff) {
            const char *field = kFields[d.first].name;
            size_t n = std::strlen(field);
            bool allowed = false;
            for (const Explained &e : kRuntime) {
                if ((e.cls >= 0 && e.cls != cls) || (e.type >= 0 && e.type != type)) continue;
                for (const char *p = e.fields; (p = std::strstr(p, field)) != nullptr; p += n)
                    if ((p == e.fields || p[-1] == ' ') && (p[n] == 0 || p[n] == ' ')) allowed = true;
            }
            std::printf(" %s%s(%d)", allowed ? "" : "UNEXPECTED ", field, d.second.n);
            if (!allowed) unexpected++;
        }
        std::printf("\n");
        g_fail += unexpected;
    }
}

// ---- 3. all levels -------------------------------------------------------------------------------

static void all_levels() {
    int n = levels_count();
    std::printf("== spawning all %d levels (start records -> things)\n", n);
    thing_dispatch_reset_stats();
    int loaded = 0;
    for (int lv = 0; lv < n; lv++) {
        if (!level_load(lv)) { std::printf("   level %d: cannot unpack\n", lv); continue; }
        loaded++;
        int records = 0;
        for (const ThingInit &r : g_state->level.things) records += r.cls != 0 && r.dis_id == 0;
        SpawnOptions o;
        level_spawn(o);
        int live = 0, by_class[16] = {};
        for (int i = 1; i < MC_THING_SLOTS; i++) {
            const Thing &t = g_state->things[i];
            if (!t.cls) continue;
            live++;
            if (t.cls < 16) by_class[t.cls]++;
        }
        char name[32];
        std::snprintf(name, sizeof name, "level %d", lv);
        check_pool_consistency(name);
        int starts = 0;
        for (int p = 0; p < 8; p++) starts += g_state->start_pos[p][0] || g_state->start_pos[p][1] || g_state->start_pos[p][2] || g_state->start_pos[p][3];
        std::printf("   level %2d: %4d records -> %3d things (scenery %3d, creatures %3d, weather %d, effects %3d, switches %3d, spells %2d), %d start positions, %3d free\n",
                    lv, records, live, by_class[2], by_class[5], by_class[7], by_class[10], by_class[11], by_class[12], starts, thing_free_count());
    }
    CHECK(loaded == n && n == 70);
    std::printf("== constructors that the level-start records of the %d levels use and that are not bound here:\n", n);
    int missing = thing_dispatch_report(stdout);
    std::printf("   %d distinct unported create handlers (all class-10 terrain effects of level_features.cpp)\n", missing);
}

int main(int argc, char **argv) {
    mc_install_crash_handler();
    const char *game_dir = argc > 1 ? argv[1] : MC_DEFAULT_GAME_DIR;
    mc_globals_init();
    CHECK(sprite_table_init_sizes(game_dir));
    constructors_register_handlers();
    if (!levels_open(game_dir)) return 2;

    test_each_constructor();
    test_segmented();
    if (snapshot_load(game_dir)) {
        int delta = pass_plain();
        pass_aligned(delta);
    } else {
        std::printf("snapshot comparison skipped\n");
    }
    all_levels();

    if (g_fail) { std::printf("constructors_test: %d FAILED\n", g_fail); return 1; }
    std::printf("constructors_test: OK\n");
    return 0;
}
