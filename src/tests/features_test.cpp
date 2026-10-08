// Test for level_features.cpp + terrain_paint.cpp: generates level 38 the way engine_load_level does
// (terrain_build, thing_pool_reset, terrain_generate_features) and compares the four terrain maps
// with the engine's own dump movie/map00000.dat and the wizard castle things with movie/gam00000.dat.
//
// The dump was taken 413 ticks into the game, so cells changed by play legitimately differ. The test
// therefore reports two results:
//   stage A  the level-start generation alone (the deliverable), with every remaining mismatching
//            cell attributed to the play-time event that reaches it;
//   stage B  the same after replaying the play-time events the snapshot documents and that only need
//            functions of this port (the two DisId-0 volcanoes of the level, wizard castles placed by
//            builder creatures, player castle footprints). Stage B is evidence for the handlers, it
//            is not something the engine does at level start.
// argv[1] = game dir, argv[2] (any) = verbose. Exit 0 = pass.
#define _CRT_SECURE_NO_WARNINGS
#include "level_features.h"
#include "terrain_paint.h"
#include "mc_math.h"
#include "terrain.h"
#include "mcfile.h"
#include "rnc.h"
#include "crash_handler.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static const int kLevel = 38;
static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static std::vector<uint8_t> s_level_packed;     // RNC-packed level record, kept to regenerate
static const char *s_game_dir = "";

static bool read_level(const char *game_dir, int index) {
    char path[1024];
    mc_blob tab, dat;
    mc_path_join(path, sizeof path, game_dir, "levels/levels.tab");
    if (!mc_read_file(path, &tab)) { std::fprintf(stderr, "cannot read %s\n", path); return false; }
    mc_path_join(path, sizeof path, game_dir, "levels/levels.dat");
    if (!mc_read_file(path, &dat)) { std::fprintf(stderr, "cannot read %s\n", path); mc_blob_free(&tab); return false; }
    bool ok = false;
    size_t n = tab.len / 4;
    if (index >= 0 && (size_t)index + 1 < n) {
        uint32_t a, b;
        std::memcpy(&a, tab.data + index * 4, 4);
        std::memcpy(&b, tab.data + (index + 1) * 4, 4);
        if (b <= a) b = (uint32_t)dat.len;
        if (a < dat.len && b <= dat.len) { s_level_packed.assign(dat.data + a, dat.data + b); ok = true; }
    }
    mc_blob_free(&tab);
    mc_blob_free(&dat);
    return ok;
}

// engine_load_level up to and including "Generate features".
static bool generate_level() {
    std::memset(&g_state->padc, 0, 0x28 - 0xc);   // +0xc..+0x27: serials, volcano_thing, volcano_smoke
    g_state->free_top = 0;
    std::memset(g_state->things, 0, sizeof g_state->things);
    g_state->creature_count = 0;
    std::memset(g_state->spells_present, 0, sizeof g_state->spells_present);
    std::memset(g_cell_things, 0, g_world.cells * sizeof(uint16_t));
    std::memset(g_map_type, 0, g_world.cells);
    std::memset(g_map_height, 0, g_world.cells);
    std::memset(g_map_light, 0, g_world.cells);
    std::memset(g_map_flags, 0, g_world.cells);
    std::memset(g_cfg->creature_lists, 0, sizeof g_cfg->creature_lists);
    g_cfg->player_list = g_cfg->mana_ball_list = g_cfg->wizard_list = g_cfg->projectile_list = 0;
    long got = rnc_unpack(s_level_packed.data(), s_level_packed.size(), reinterpret_cast<uint8_t *>(&g_state->level), sizeof(LevelData));
    if (got != (long)sizeof(LevelData)) { std::fprintf(stderr, "level unpack returned %ld\n", got); return false; }
    terrain_build(g_state->level.gen);
    thing_pool_reset();
    terrain_generate_features();
    return true;
}

enum { kHeight, kLight, kFlags, kType, kMaps };
static const char *kMapNames[kMaps] = { "height", "light", "flags", "type" };
struct Maps {
    std::vector<uint8_t> m[kMaps];
    void capture() {
        m[kHeight].assign(g_map_height, g_map_height + MC_MAP_CELLS);
        m[kLight].assign(g_map_light, g_map_light + MC_MAP_CELLS);
        m[kFlags].assign(g_map_flags, g_map_flags + MC_MAP_CELLS);
        m[kType].assign(g_map_type, g_map_type + MC_MAP_CELLS);
    }
};
static const uint8_t *s_ref[kMaps];

static int wrap_dist(int a, int b) { int d = (a - b) & 0xff; return d > 128 ? 256 - d : d; }

// A play-time event that explains a difference between a freshly generated level and the dump.
enum { kPlayerCastle, kBuilderCastle, kWizardDead, kPlayVolcano, kDent, kLiveEffect, kUnexplained, kKinds };
static const char *kKindNames[kKinds] = {
    "player castle built / enlarged during play",
    "wizard castle placed during play (builder)",
    "wizard castle destroyed during play",
    "volcano record spawned during play",
    "impact dent (effect type 0xa) during play",
    "other effect alive in the dump",
    "unexplained",
};

// Three "dents" visible in the dump: the L-shaped triple of ring 0 (centre, +x, +y; the fourth cell of
// the ring is the one every spiral walk drops) turned into class 1 and lowered by `depth`. That is
// what effect_type10_s10_update_23ec0 does (class 10 type 0xa, the impact effect of some weapon).
static const struct { uint8_t x, y; int depth; } kDents[] = { { 254, 241, 0 }, { 18, 138, 5 }, { 19, 137, 2 } };
struct Source {
    std::string name;
    int x = 0, y = 0, rx = 0, ry = 0;   // cell centre and reach in x / y
    int kind = kUnexplained;
    unsigned cells[kMaps] = {};
};

struct Result {
    unsigned equal[kMaps] = {}, differ[kMaps] = {};
    unsigned by_kind[kKinds][kMaps] = {};
    std::vector<Source> src;            // with per-source counts; the last entry is "unexplained"
    std::vector<unsigned> unexplained_cells;
};

static Result compare(const Maps &ours, std::vector<Source> src) {
    Result r;
    Source rest; rest.name = "unexplained"; rest.kind = kUnexplained;
    src.push_back(rest);
    for (unsigned c = 0; c < MC_MAP_CELLS; c++) {
        bool any = false;
        for (int k = 0; k < kMaps; k++) {
            if (ours.m[k][c] == s_ref[k][c]) r.equal[k]++; else { r.differ[k]++; any = true; }
        }
        if (!any) continue;
        // the first source in list order (= kind priority) whose reach covers the cell; nearest within a kind
        int pick = (int)src.size() - 1, pick_d = 1 << 30, pick_kind = kKinds;
        for (size_t i = 0; i + 1 < src.size(); i++) {
            int dx = wrap_dist(c & 0xff, src[i].x), dy = wrap_dist(c >> 8, src[i].y);
            if (dx > src[i].rx || dy > src[i].ry) continue;
            int d = dx > dy ? dx : dy;
            if (src[i].kind < pick_kind || (src[i].kind == pick_kind && d < pick_d)) { pick = (int)i; pick_d = d; pick_kind = src[i].kind; }
        }
        if (src[pick].kind == kUnexplained) r.unexplained_cells.push_back(c);
        for (int k = 0; k < kMaps; k++)
            if (ours.m[k][c] != s_ref[k][c]) { src[pick].cells[k]++; r.by_kind[src[pick].kind][k]++; }
    }
    r.src = src;
    return r;
}

static void print_result(const char *title, const Result &r, const unsigned *baseline) {
    std::printf("== %s\n", title);
    std::printf("   %-46s %8s %8s %8s %8s\n", "", kMapNames[0], kMapNames[1], kMapNames[2], kMapNames[3]);
    std::printf("   %-46s %8u %8u %8u %8u\n", "cells equal (of 65536)", r.equal[0], r.equal[1], r.equal[2], r.equal[3]);
    if (baseline)
        std::printf("   %-46s %8u %8u %8u %8u\n", "terrain_build alone (no features)", baseline[0], baseline[1], baseline[2], baseline[3]);
    std::printf("   %-46s %8u %8u %8u %8u\n", "cells different", r.differ[0], r.differ[1], r.differ[2], r.differ[3]);
    for (int kd = 0; kd < kKinds; kd++)
        std::printf("     %-44s %8u %8u %8u %8u\n", kKindNames[kd], r.by_kind[kd][0], r.by_kind[kd][1], r.by_kind[kd][2], r.by_kind[kd][3]);
    std::printf("   per source (height / light / flags / type):\n");
    for (const Source &s : r.src) {
        if (!(s.cells[0] | s.cells[1] | s.cells[2] | s.cells[3])) continue;
        std::printf("     %-62s %5u %5u %5u %5u\n", s.name.c_str(), s.cells[0], s.cells[1], s.cells[2], s.cells[3]);
    }
    if (!r.unexplained_cells.empty()) {
        std::printf("   unexplained cells:");
        for (size_t i = 0; i < r.unexplained_cells.size() && i < 24; i++)
            std::printf(" (%u,%u)", r.unexplained_cells[i] & 0xff, r.unexplained_cells[i] >> 8);
        std::printf("%s\n", r.unexplained_cells.size() > 24 ? " ..." : "");
    }
}

static void write_ppm(const std::string &path, const Maps &ours) {
    FILE *f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    std::fprintf(f, "P6\n256 256\n255\n");
    for (unsigned i = 0; i < MC_MAP_CELLS; i++) {
        uint8_t rh = s_ref[kHeight][i];
        uint8_t g = (uint8_t)(rh > 0x7f ? 0xff : rh * 2);
        uint8_t px[3] = { g, g, g };
        bool dh = ours.m[kHeight][i] != rh, dt = ours.m[kType][i] != s_ref[kType][i];
        bool dl = ours.m[kLight][i] != s_ref[kLight][i], df = ours.m[kFlags][i] != s_ref[kFlags][i];
        if (dh)            { px[0] = 255; px[1] = 0;   px[2] = 0; }      // red: height differs
        else if (dt || df) { px[0] = 255; px[1] = 255; px[2] = 0; }      // yellow: texture / flags differ
        else if (dl)       { px[0] = 0;   px[1] = 255; px[2] = 255; }    // cyan: only the light differs
        else if (s_ref[kFlags][i] & 0x80) px[2] = 255;                   // blue tint: matching built-on cell
        std::fwrite(px, 1, 3, f);
    }
    std::fclose(f);
}

static bool is_wizard(const Thing &t) { return t.cls == 10 && t.type == 0x2d; }
static const Thing *find_wizard_at(const Thing *things, uint16_t x, uint16_t y) {
    for (int j = 1; j < MC_THING_SLOTS; j++)
        if (is_wizard(things[j]) && things[j].x == x && things[j].y == y) return &things[j];
    return nullptr;
}
static int footprint_reach(unsigned size, bool height, int margin) {
    const CastleFootprint *fp = castle_footprint(size);
    return (height ? fp->h : fp->w) / 2 + margin;
}

// castle_stamp_footprint must end with the heights effect_castle_build_update converges to: build a
// wizard castle at a spot, then stamp the same footprint on a pristine copy of the level.
static void test_stamp_equals_build() {
    const uint16_t saved_mode = g_video_mode_flags;
    g_video_mode_flags = 8;                               // footprint w / h as stored (not doubled)
    struct Restore { uint16_t m; ~Restore() { g_video_mode_flags = m; } } restore{saved_mode};
    if (!generate_level()) { CHECK(!"generate"); return; }
    Maps before; before.capture();
    static Thing things_before[MC_THING_SLOTS];
    std::memcpy(things_before, g_state->things, sizeof things_before);
    // pick the first generated wizard castle and rebuild it from the pristine terrain both ways
    int idx = 0;
    for (int i = 1; i < MC_THING_SLOTS && !idx; i++) if (is_wizard(g_state->things[i])) idx = i;
    CHECK(idx != 0);
    if (!idx) return;
    const Thing built = g_state->things[idx];
    const CastleFootprint *fp = castle_footprint(built.castle_size);
    // pristine terrain = generation with no features: rebuild and stamp
    terrain_build(g_state->level.gen);
    Thing *scratch = thing_at(0);
    std::memset(scratch, 0, sizeof *scratch);
    scratch->x = built.x; scratch->y = built.y;
    uint8_t cx = (uint8_t)((built.x + 0x80) >> 8), cy = (uint8_t)((built.y + 0x80) >> 8);
    uint8_t x0 = (uint8_t)(cx - fp->w / 2), y0 = (uint8_t)(cy - fp->h / 2);
    scratch->z = (int16_t)(terrain_height_avg(x0, y0, fp->h, fp->w) << 5);   // what effect_wizard_init uses as base
    scratch->castle_size = built.castle_size;
    castle_stamp_footprint(scratch);
    // compare the footprint interior (the outer ring of a built castle is touched by the border smoothing)
    unsigned cells = 0, same_h = 0, same_t = 0;
    for (unsigned y = 1; y + 1 < fp->h; y++)
        for (unsigned x = 1; x + 1 < fp->w; x++) {
            unsigned c = mc_cell((uint8_t)(x0 + x), (uint8_t)(y0 + y));
            cells++;
            same_h += g_map_height[c] == before.m[kHeight][c];
            same_t += g_map_type[c] == before.m[kType][c];
        }
    std::printf("== castle_stamp_footprint vs effect_castle_build_update (wizard #%d, size %u, %ux%u): height %u/%u, texture %u/%u cells equal\n",
                idx, built.castle_size, fp->w, fp->h, same_h, cells, same_t, cells);
    CHECK(same_h == cells);
    std::memset(scratch, 0, sizeof *scratch);
}

// Smoke test over every level of levels.dat: the feature generation must terminate and leave nothing
// but finished wizard castles (state 0x34) in the pool. Also counts which feature types the levels
// use, to show what level 38 does not exercise.
static void test_all_levels() {
    const char *game_dir = s_game_dir;
    std::vector<uint8_t> keep = s_level_packed;
    unsigned records[0x40] = {}, levels_with[0x40] = {};
    int levels = 0, bad = 0, wizards = 0;
    for (int lv = 0; lv < 100; lv++) {
        if (!read_level(game_dir, lv)) break;
        if (rnc_unpack(s_level_packed.data(), s_level_packed.size(), reinterpret_cast<uint8_t *>(&g_state->level), sizeof(LevelData)) != (long)sizeof(LevelData)) break;
        bool seen[0x40] = {};
        for (const ThingInit &r : g_state->level.things)
            if (r.cls == 10 && r.dis_id == 0xffff && r.model < 0x40) { records[r.model]++; seen[r.model] = true; }
        for (int m = 0; m < 0x40; m++) levels_with[m] += seen[m];
        if (!generate_level()) { bad++; continue; }
        levels++;
        for (int i = 1; i < MC_THING_SLOTS; i++) {
            const Thing &t = g_state->things[i];
            if (!t.cls) continue;
            if (is_wizard(t) && t.state == 0x34) { wizards++; continue; }
            if (bad < 5) std::printf("   level %d: thing #%d class %u type 0x%02x state 0x%02x left after generation\n", lv, i, t.cls, t.type, t.state);
            bad++;
        }
    }
    std::printf("== all levels: %d generated, %d wizard castles built, %d problems; DisId-0xffff effect records by type (records / levels):", levels, wizards, bad);
    for (int m = 0; m < 0x40; m++) if (records[m]) std::printf(" 0x%02x %u/%u", m, records[m], levels_with[m]);
    std::printf("\n");
    CHECK(levels >= 70 && bad == 0);
    s_level_packed = keep;
}

int main(int argc, char **argv) {
    mc_install_crash_handler();
    const char *game_dir = argc > 1 ? argv[1] : MC_DEFAULT_GAME_DIR;
    bool verbose = argc > 2;
    s_game_dir = game_dir;
    mc_globals_init();
    CHECK(sprite_table_init_sizes(game_dir));
    CHECK(level_features_load_data(game_dir));
    level_features_register_handlers();
    if (!read_level(game_dir, kLevel)) return 2;

    // ---- reference data ----
    char path[1024];
    mc_blob dump, gam;
    mc_path_join(path, sizeof path, game_dir, "movie/map00000.dat");
    if (!mc_read_file(path, &dump) || dump.len < 0x60000) { std::fprintf(stderr, "cannot read %s\n", path); return 2; }
    mc_path_join(path, sizeof path, game_dir, "movie/gam00000.dat");
    if (!mc_read_file(path, &gam) || gam.len != MC_STATE_IMAGE_SIZE) { std::fprintf(stderr, "cannot read %s\n", path); return 2; }
    s_ref[kType] = dump.data;
    s_ref[kHeight] = dump.data + 0x10000;
    s_ref[kLight] = dump.data + 0x20000;
    s_ref[kFlags] = dump.data + 0x30000;
    static GameState snap_state;
    GameState *snap = &snap_state;
    CHECK(thing_load_snapshot_image(gam.data, snap));

    // ---- stage A: generate with both video modes ----
    Maps maps[2];
    unsigned total[2] = {};
    static Thing things_gen[2][MC_THING_SLOTS];
    static const uint16_t kModes[2] = { 1, 8 };
    int unported = 0;
    for (int m = 0; m < 2; m++) {
        g_video_mode_flags = kModes[m];
        thing_dispatch_reset_stats();
        if (!generate_level()) return 2;
        maps[m].capture();
        std::memcpy(things_gen[m], g_state->things, sizeof things_gen[m]);
        unsigned eq[kMaps];
        for (int k = 0; k < kMaps; k++) {
            eq[k] = 0;
            for (unsigned c = 0; c < MC_MAP_CELLS; c++) eq[k] += maps[m].m[k][c] == s_ref[k][c];
            total[m] += eq[k];
        }
        int live = 0;
        for (int i = 1; i < MC_THING_SLOTS; i++) live += g_state->things[i].cls != 0;
        std::printf("g_video_mode_flags = %u: height %u, light %u, flags %u, type %u of 65536 cells equal; %d things left, g_rng16 0x%04x\n",
                    kModes[m], eq[kHeight], eq[kLight], eq[kFlags], eq[kType], live, g_rng16);
        if (m == 1) {
            std::printf("  handlers dispatched during generation that are not ported here:\n");
            unported = thing_dispatch_report(stdout);
            if (unported == 0) std::printf("    (none)\n");
        }
    }
    // In 320x200 the original doubles building.tab w / h at tab relocation and the castle code halves
    // them again (castle_footprint, level_features.h): both modes must give the same terrain. The
    // snapshot was recorded in 320x200 (wizard castle capacities, below).
    std::printf("=> both video modes give %s terrain (%u / %u matching map bytes)\n",
                total[0] == total[1] ? "the same" : "DIFFERENT", total[0], total[1]);
    CHECK(total[0] == total[1]);
    for (int k = 0; k < kMaps; k++) CHECK(maps[0].m[k] == maps[1].m[k]);
    const int best = 0;                                   // mode 1, the recording's
    const Maps &ours = maps[best];
    const Thing *gen = things_gen[best];
    g_video_mode_flags = kModes[best];

    // ---- wizard castles: generated things vs the snapshot ----
    int wiz_gen = 0, wiz_snap = 0, wiz_found = 0, wiz_exact = 0, wiz_same_index = 0, cap_retail = 0, cap_shift2 = 0;
    std::vector<int> wiz_dead;              // generated wizard castles that are gone in the snapshot
    std::vector<int> wiz_new;               // snapshot wizard castles that were not generated
    for (int i = 1; i < MC_THING_SLOTS; i++) {
        const Thing &s = snap->things[i];
        if (!is_wizard(s)) continue;
        wiz_snap++;
        if (!find_wizard_at(gen, s.x, s.y)) wiz_new.push_back(i);
    }
    std::printf("== wizard castles (class 10 type 0x2d), generated vs snapshot\n");
    for (int i = 1; i < MC_THING_SLOTS; i++) {
        const Thing &t = gen[i];
        if (!is_wizard(t)) continue;
        wiz_gen++;
        const Thing *s = find_wizard_at(snap->things, t.x, t.y);
        if (!s) {
            wiz_dead.push_back(i);
            std::printf("   #%3d (%3u,%3u) size %2u: not in the snapshot any more (destroyed during play)\n", i, t.x >> 8, t.y >> 8, t.castle_size);
            continue;
        }
        wiz_found++;
        bool same = s->x == t.x && s->y == t.y && s->z == t.z && s->castle_size == t.castle_size &&
                    s->ext_z0 == t.ext_z0 && s->ext_x == t.ext_x && s->ext_y == t.ext_y && s->ext_h == t.ext_h &&
                    s->home.x == t.home.x && s->home.y == t.home.y && s->home.z == t.home.z && s->state == t.state &&
                    s->max_health == t.max_health && s->health == t.health && s->damage == t.damage &&
                    s->prop_flags == t.prop_flags && s->flags == t.flags &&
                    s->sprite == t.sprite && s->owner == t.owner && s->rng == t.rng;
        wiz_exact += same;
        // capacity (+0x80): the exe computes w * h >> 4 (sar eax, 4 at 0x350d9); the run that
        // made the recording ran in 320x200, where castle_footprint doubles w and h (capacity 4x)
        const CastleFootprint *fp = castle_footprint(t.castle_size);
        cap_retail += s->speed_base == (int16_t)((fp->w * fp->h) >> 4);
        cap_shift2 += s->speed_base == (int16_t)((fp->w * fp->h) >> 2);
        CHECK(t.speed_base == (int16_t)((fp->w * fp->h) >> 4));
        wiz_same_index += (int)(s - snap->things) == i;
        if (!same || verbose)
            std::printf("   #%3d (%3u,%3u) z %5d size %2u ext %d %d %d %d home %u %u %d state 0x%02x hp %d/%d flags %x prop %x cap %d rng %08x\n"
                        "   snap #%3d      z %5d size %2u ext %d %d %d %d home %u %u %d state 0x%02x hp %d/%d flags %x prop %x cap %d rng %08x%s\n",
                        i, t.x >> 8, t.y >> 8, t.z, t.castle_size, t.ext_z0, t.ext_x, t.ext_y, t.ext_h, (unsigned)t.home.x, (unsigned)t.home.y, t.home.z, t.state,
                        t.health, t.max_health, t.flags, t.prop_flags, t.speed_base, t.rng,
                        (int)(s - snap->things), s->z, s->castle_size, s->ext_z0, s->ext_x, s->ext_y, s->ext_h, (unsigned)s->home.x, (unsigned)s->home.y, s->home.z, s->state,
                        s->health, s->max_health, s->flags, s->prop_flags, s->speed_base, s->rng, same ? "" : "  DIFFERS");
    }
    std::printf("   generated %d, in the snapshot %d; still there %d, of those: same thing index %d, all of position (x, y, z),\n"
                "   castle_size, extents, home, state, health, max_health, damage, flags, prop_flags, sprite, owner, rng equal %d\n"
                "   capacity (+0x80) in the snapshot: w*h>>4 of castle_footprint (doubled in 320x200) for %d, w*h>>2 for %d\n",
                wiz_gen, wiz_snap, wiz_found, wiz_same_index, wiz_exact, cap_retail, cap_shift2);
    for (int i : wiz_new) {
        const Thing &s = snap->things[i];
        std::printf("   snapshot #%3d (%3u,%3u) size %2u: no level record (placed by a builder creature %u updates before the dump)\n",
                    i, s.x >> 8, s.y >> 8, s.castle_size, (unsigned)(uint8_t)(s.tick - (uint8_t)i));
    }
    CHECK(wiz_gen > 0 && wiz_found > 0 && wiz_exact == wiz_found && wiz_same_index == wiz_found);

    // ---- the play-time events that can explain differences ----
    std::vector<Source> src;
    auto add_source = [&](int kind, const char *what, int x, int y, int rx, int ry) {
        char name[160];
        std::snprintf(name, sizeof name, "%s at (%d,%d)", what, x & 0xff, y & 0xff);
        Source s; s.name = name; s.x = x & 0xff; s.y = y & 0xff; s.rx = rx; s.ry = ry; s.kind = kind;
        src.push_back(s);
    };
    char what[128];
    for (int i = 1; i < MC_THING_SLOTS; i++) {          // player castles: footprints 0..aux, + the smoothed ring
        const Thing &o = snap->things[i];
        if (o.cls != 3 || o.type != 2) continue;
        std::snprintf(what, sizeof what, "player castle #%d (owner %u, level %d)", i, o.owner, o.aux + 1);
        unsigned top = (unsigned)(o.aux < 0 ? 0 : o.aux);
        add_source(kPlayerCastle, what, (o.home.x + 0x80) >> 8, (o.home.y + 0x80) >> 8, footprint_reach(top, false, 7), footprint_reach(top, true, 7));
    }
    for (int i : wiz_new) {                              // footprint + the 5-cell smoothed border + light
        const Thing &s = snap->things[i];
        std::snprintf(what, sizeof what, "builder-placed wizard castle #%d (size %u)", i, s.castle_size);
        add_source(kBuilderCastle, what, (s.x + 0x80) >> 8, (s.y + 0x80) >> 8, footprint_reach(s.castle_size, false, 7), footprint_reach(s.castle_size, true, 7));
    }
    for (int i : wiz_dead) {                             // footprint + retexture / light ring
        const Thing &t = gen[i];
        std::snprintf(what, sizeof what, "destroyed wizard castle #%d (size %u)", i, t.castle_size);
        add_source(kWizardDead, what, (t.x + 0x80) >> 8, (t.y + 0x80) >> 8, footprint_reach(t.castle_size, false, 3), footprint_reach(t.castle_size, true, 3));
    }
    for (const auto &d : kDents) add_source(kDent, "dent", d.x, d.y, 3, 3);
    std::vector<const ThingInit *> start_volcanoes;      // DisId 0: spawned by switch_activate(0), erupt in the first 18 ticks
    for (const ThingInit &r : g_state->level.things) {
        if (r.cls != 10 || r.model != 9 || r.dis_id == 0xffff) continue;
        std::snprintf(what, sizeof what, "volcano record (DisId %u)", r.dis_id);
        add_source(kPlayVolcano, what, r.x, r.y, 7, 7);
        if (r.dis_id == 0) start_volcanoes.push_back(&r);
    }
    for (int i = 1; i < MC_THING_SLOTS; i++) {
        const Thing &o = snap->things[i];
        if (o.cls != 10 || o.type == 0x2d || o.type == 0x27) continue;      // 0x27 = mana balls
        std::snprintf(what, sizeof what, "effect #%d type 0x%02x state 0x%02x", i, o.type, o.state);
        add_source(kLiveEffect, what, (o.x + 0x80) >> 8, (o.y + 0x80) >> 8, 5, 5);
    }

    Result ra = compare(ours, src);
    char title[160];
    std::snprintf(title, sizeof title, "stage A: level %d after terrain_generate_features, g_video_mode_flags = %u", kLevel, kModes[best]);
    // baseline: the generator without any feature (measured here; port_terrain.md quotes 56029 / 57884 / 59202 / 59802)
    unsigned baseline[kMaps] = {};
    {
        terrain_build(g_state->level.gen);
        Maps plain; plain.capture();
        for (int k = 0; k < kMaps; k++)
            for (unsigned c = 0; c < MC_MAP_CELLS; c++) baseline[k] += plain.m[k][c] == s_ref[k][c];
    }
    print_result(title, ra, baseline);
    for (int k = 0; k < kMaps; k++) CHECK(ra.equal[k] > baseline[k]);
    CHECK(ra.unexplained_cells.empty());

    std::string exe = argv[0];
    size_t slash = exe.find_last_of("/\\");
    std::string dir = slash == std::string::npos ? std::string(".") : exe.substr(0, slash + 1);
    write_ppm(dir + "features_diff.ppm", ours);
    {
        FILE *f = std::fopen((dir + "features_ours.bin").c_str(), "wb");
        if (f) {
            std::fwrite(ours.m[kType].data(), 1, MC_MAP_CELLS, f); std::fwrite(ours.m[kHeight].data(), 1, MC_MAP_CELLS, f);
            std::fwrite(ours.m[kLight].data(), 1, MC_MAP_CELLS, f); std::fwrite(ours.m[kFlags].data(), 1, MC_MAP_CELLS, f);
            std::fclose(f);
        }
    }

    // ---- stage B: replay of the play-time events this port can run ----
    if (!generate_level()) return 2;
    models_initialise();
    g_state->active_top = -1;
    std::printf("== stage B: replay of play-time events with the functions of this port\n");
    {
        // B1. The level's DisId-0 volcanoes. switch_activate(0) spawns them among the other level-start
        // things, so their thing index (-> Thing.rng) depends on constructors that are not linked into
        // this test. The snapshot still holds the effect one of them left behind (class 10 type 0x12,
        // owner = the volcano's index): pad the pool so that record gets that index.
        int anchor_rec = -1, anchor_idx = 0;
        for (size_t v = 0; v < start_volcanoes.size() && anchor_rec < 0; v++)
            for (int i = 1; i < MC_THING_SLOTS; i++) {
                const Thing &o = snap->things[i];
                if (o.cls == 10 && o.type == 0x12 && (o.x >> 8) == start_volcanoes[v]->x && (o.y >> 8) == start_volcanoes[v]->y) {
                    anchor_rec = (int)v; anchor_idx = o.owner; break;
                }
            }
        if (anchor_rec >= 0 && anchor_idx - anchor_rec > 0) {
            std::vector<Thing *> pad;
            int first = anchor_idx - anchor_rec;
            while (g_state->free_top >= 0 && g_state->free_list[g_state->free_top] < first) {
                Thing *p = thing_alloc();
                p->cls = 0xd;
                pad.push_back(p);
            }
            for (const ThingInit *r : start_volcanoes) level_spawn_thing_record(r);
            for (Thing *p : pad) thing_free(p);
            int spawned = 0;
            for (int i = 1; i < MC_THING_SLOTS; i++) spawned += g_state->things[i].cls == 10 && g_state->things[i].type == 9;
            for (int tick = 0; tick < 64; tick++) thing_update_all();
            std::printf("   B1: %d DisId-0 volcano(es) spawned at thing index %d.. (index taken from the type-0x12 effect #%d they left in the snapshot), 64 ticks run\n",
                        spawned, first, anchor_idx);
            for (int i : wiz_dead) {
                const Thing &t = g_state->things[i];
                std::printf("       wizard castle #%d (%u,%u) after the eruption: %s\n", i, gen[i].x >> 8, gen[i].y >> 8,
                            t.cls == 0 ? "destroyed (collapse handler ran)" : "still alive");
            }
        } else {
            std::printf("   B1: no DisId-0 volcano with a surviving type-0x12 effect, skipped\n");
        }
        // B2. Wizard castles placed by builder creatures (creature_genie_s72_place_castle_1d540 =
        // thing_create + effect_wizard_init + state 0x33), then 30 build ticks.
        for (int i : wiz_new) {
            const Thing &s = snap->things[i];
            Pos pos = { s.x, s.y, 0 };
            Thing *t = thing_create(&pos, 10, 0x2d);
            CHECK(t != nullptr);
            if (!t) continue;
            effect_wizard_init(t, s.castle_size);
            t->state = 0x33;
            for (int tick = 0; tick < 31; tick++) thing_update_all();
            std::printf("   B2: wizard castle (%u,%u) size %u built: z %d (snapshot %d), state 0x%02x (snapshot 0x%02x), extents %s\n",
                        s.x >> 8, s.y >> 8, s.castle_size, t->z, s.z, t->state, s.state,
                        (t->ext_x == s.ext_x && t->ext_y == s.ext_y && t->ext_h == s.ext_h && t->ext_z0 == s.ext_z0) ? "equal" : "DIFFER");
            CHECK(t->x == s.x && t->y == s.y && t->state == s.state && t->ext_x == s.ext_x && t->ext_y == s.ext_y);
        }
        // B3. Player castles: player_spawn_3f360 / the castle spell stamp the footprints 0..level-1 at
        // the castle's home position through the scratch thing 0.
        for (int i = 1; i < MC_THING_SLOTS; i++) {
            const Thing &o = snap->things[i];
            if (o.cls != 3 || o.type != 2) continue;
            Thing *scratch = thing_at(0);
            for (int size = 0; size <= o.aux; size++) {
                scratch->x = o.home.x; scratch->y = o.home.y; scratch->z = o.home.z;
                scratch->type = 0; scratch->aux = 0; scratch->owner = o.owner;
                scratch->castle_size = (uint8_t)size;
                castle_stamp_footprint(scratch);
            }
            std::printf("   B3: player castle #%d: footprints 0..%d stamped at home (%u,%u) base height %d\n",
                        i, o.aux, o.home.x >> 8, o.home.y >> 8, o.home.z >> 5);
        }
        // B4. The three dents: a class 10 type 0xa effect whose random depth (rng % 7) is forced.
        for (const auto &d : kDents) {
            Pos pos = { (uint16_t)(d.x << 8), (uint16_t)(d.y << 8), 0 };
            pos.z = (int16_t)terrain_height_at(&pos);
            Thing *t = thing_create(&pos, 10, 0xa);
            CHECK(t != nullptr);
            if (!t) continue;
            uint32_t seed = 0;
            while ((int)(mc_lcg(seed) % 7u) != d.depth) seed++;
            t->rng = seed;
            thing_update_all();
            std::printf("   B4: dent of depth %d at (%u,%u) through effect_type10_s10_update_23ec0\n", d.depth, d.x, d.y);
        }
        // B5. The two wizard castles destroyed during play: run the collapse handler (state 0x35). Its
        // random wall heights come from the castle's own rng, which is still the level-start value
        // unless villagers were spawned; the number of inhabitants (aux) at the time of death is not
        // known and decides how many rng steps the leaving villagers take, so every value is tried.
        ThingUpdateFn collapse = thing_update_fn(10, 0x35);
        CHECK(collapse != nullptr);
        for (int i : wiz_dead) {
            if (!collapse) break;
            Thing *t = thing_at(i);
            if (!is_wizard(*t)) continue;
            const CastleFootprint *fp = castle_footprint(t->castle_size);
            uint8_t cx = (uint8_t)((t->x + 0x80) >> 8), cy = (uint8_t)((t->y + 0x80) >> 8);
            int rx = fp->w / 2 + 1, ry = fp->h / 2 + 1;
            Maps keep; keep.capture();
            const Thing keep_thing = *t;
            const uint16_t keep_rng16 = g_rng16;
            int best_aux = -1, best_aux_last = -1; unsigned best_bad = ~0u, best_cells = 0;
            for (int aux = 0; aux <= 40; aux++) {
                *t = keep_thing;
                t->aux = (int16_t)aux;
                t->state = 0x35;
                collapse(t);
                unsigned bad = 0, cells = 0;
                for (int dy = -ry; dy <= ry; dy++)
                    for (int dx = -rx; dx <= rx; dx++) {
                        unsigned c = mc_cell((uint8_t)(cx + dx), (uint8_t)(cy + dy));
                        cells++;
                        bad += g_map_height[c] != s_ref[kHeight][c];
                    }
                if (bad < best_bad) { best_bad = bad; best_aux = aux; best_cells = cells; }
                if (bad == best_bad) best_aux_last = aux;
                std::memcpy(g_map_height, keep.m[kHeight].data(), MC_MAP_CELLS);
                std::memcpy(g_map_light, keep.m[kLight].data(), MC_MAP_CELLS);
                std::memcpy(g_map_flags, keep.m[kFlags].data(), MC_MAP_CELLS);
                std::memcpy(g_map_type, keep.m[kType].data(), MC_MAP_CELLS);
                g_rng16 = keep_rng16;
            }
            *t = keep_thing;
            t->aux = (int16_t)best_aux;
            t->state = 0x35;
            collapse(t);
            thing_update_all();          // frees the castle
            std::printf("   B5: wizard castle #%d (%u,%u) collapsed through effect_type51_s53_update_27930 with %d..%d inhabitants (best of 0..40):\n"
                        "       %u of %u heights in the footprint + 1 differ from the dump\n",
                        i, cx, cy, best_aux, best_aux_last, best_bad, best_cells);
            CHECK(best_bad == 0);
        }
    }
    Maps replayed; replayed.capture();
    Result rb = compare(replayed, src);
    print_result("stage B: after the replay (the kinds now count what the replay did not reproduce)", rb, nullptr);
    {
        // texture rotation of textures < 8 is drawn from g_rng16, which cannot be in sync hundreds of ticks into the game
        unsigned rot_only = 0, other = 0;
        for (unsigned c = 0; c < MC_MAP_CELLS; c++) {
            if (replayed.m[kFlags][c] == s_ref[kFlags][c]) continue;
            if ((replayed.m[kFlags][c] & 0x8f) == (s_ref[kFlags][c] & 0x8f) && replayed.m[kType][c] == s_ref[kType][c] && s_ref[kType][c] < 8) rot_only++;
            else other++;
        }
        std::printf("   of the %u flag mismatches %u are only the random rotation bits (0x70) of a texture < 8, %u are something else\n",
                    rot_only + other, rot_only, other);
    }
    write_ppm(dir + "features_replay_diff.ppm", replayed);
    std::printf("wrote features_diff.ppm, features_replay_diff.ppm and features_ours.bin (type, height, light, flags) to %s\n", dir.c_str());
    // What the replay must reproduce exactly (heights, light and textures; flags only where g_rng16 is
    // still in sync, i.e. for the volcanoes of the first ticks and the level-start player castle).
    for (int k = 0; k < kMaps; k++) {
        CHECK(rb.by_kind[kPlayVolcano][k] == 0);
        CHECK(rb.by_kind[kDent][k] == 0);
        CHECK(rb.by_kind[kUnexplained][k] == 0);
        if (k == kFlags) continue;
        CHECK(rb.by_kind[kBuilderCastle][k] == 0);
    }
    CHECK(rb.by_kind[kWizardDead][kHeight] == 0 && rb.by_kind[kWizardDead][kType] == 0);
    for (const Source &s : rb.src)
        if (s.name.find("player castle #485") != std::string::npos) CHECK((s.cells[0] | s.cells[1] | s.cells[2] | s.cells[3]) == 0);
    CHECK(rb.differ[kHeight] <= 27);

    test_stamp_equals_build();
    test_all_levels();

    mc_blob_free(&dump);
    mc_blob_free(&gam);
    (void)unported;
    if (g_fail) { std::printf("features_test: %d FAILED\n", g_fail); return 1; }
    std::printf("features_test: OK\n");
    return 0;
}
