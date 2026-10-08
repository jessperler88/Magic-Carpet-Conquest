// Round 11 task A: the terrain passes, painting and features in a wide (tiled) world. ${MC_SIM_ALL}, no
// renderer. argv[1] = game dir (SKIP without the game data).
//
//  1. Generation passes: level 38's generator is replayed step by step in the 256 world (and checked against
//     terrain_build); every local pass is then run on the 1024-cell tiling of its 256 input and compared with
//     the tiling of its 256 output. Order-independent passes must match exactly. Passes that rewrite the map
//     in place while sweeping it (the original's in-place filters) see already-updated neighbours across a
//     tile boundary that the 256 run saw not yet updated, so they may differ - only within a few cells of the
//     old tile boundaries (reported, bounded). Random passes (textures, flat-cell light) are checked for
//     validity (the rng sequence runs over a different cell order).
//  2. Features at the seams: a wizard castle (spawn record + build to completion), a player castle footprint
//     stamp, a crater-style spiral walk, a wall, a path, a canyon and a ridge, each built across the world
//     seam x = 1023 -> 0 (and y), across the old 255 -> 256 boundary and inside the tiling, in fresh copies of
//     the level; the terrain changes as offsets from the anchor cell must equal the 256 world's at the same
//     local position. castle_site_clear_at_pos around the stamped castle too.
//  3. Two runs of 2000 ticks of the tiled 1024 world give identical net_state_checksum sequences.
#define _CRT_SECURE_NO_WARNINGS
#include "sim.h"
#include "thing.h"
#include "player.h"
#include "net.h"
#include "terrain.h"
#include "terrain_paint.h"
#include "level_features.h"
#include "world.h"
#include "mc_globals.h"
#include "mc_math.h"
#include "gen/terrain_tables.h"
#include "crash_handler.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)
#define CHECK_EQ(a, b) do { long long _a = (long long)(a), _b = (long long)(b); if (_a != _b) { \
    std::printf("FAIL %s:%d: %s == %s (%lld != %lld)\n", __FILE__, __LINE__, #a, #b, _a, _b); g_fail++; } } while (0)

constexpr int kWide = 1024;
constexpr int kLevel = 38;

// ---- map snapshots ---------------------------------------------------------------------------------------
struct Maps {
    int n = 0;
    std::vector<uint8_t> type, height, light, flags;
};
static Maps grab() {
    Maps m;
    m.n = g_world.size;
    const size_t c = g_world.cells;
    m.type.assign(g_map_type, g_map_type + c);
    m.height.assign(g_map_height, g_map_height + c);
    m.light.assign(g_map_light, g_map_light + c);
    m.flags.assign(g_map_flags, g_map_flags + c);
    return m;
}
static void put(const Maps &m) {
    if (m.n != g_world.size) { std::printf("FAIL put: world %d, maps %d\n", g_world.size, m.n); g_fail++; return; }
    std::memcpy(g_map_type, m.type.data(), m.type.size());
    std::memcpy(g_map_height, m.height.data(), m.height.size());
    std::memcpy(g_map_light, m.light.data(), m.light.size());
    std::memcpy(g_map_flags, m.flags.data(), m.flags.size());
}
static Maps tile(const Maps &s, int n) {
    Maps m;
    m.n = n;
    const size_t c = (size_t)n * n;
    m.type.resize(c); m.height.resize(c); m.light.resize(c); m.flags.resize(c);
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) {
            const size_t d = (size_t)y * n + x, o = (size_t)(y & 0xff) * 256 + (x & 0xff);
            m.type[d] = s.type[o]; m.height[d] = s.height[o]; m.light[d] = s.light[o]; m.flags[d] = s.flags[o];
        }
    return m;
}
static bool cell_equal(const Maps &a, const Maps &b, size_t i) {
    return a.type[i] == b.type[i] && a.height[i] == b.height[i] && a.light[i] == b.light[i] && a.flags[i] == b.flags[i];
}
// Distance of a cell to the nearest old tile boundary line (vertical or horizontal).
static int seam_dist(int x, int y) {
    const int lx = x & 0xff, ly = y & 0xff;
    int dx = lx < 255 - lx ? lx : 255 - lx;
    int dy = ly < 255 - ly ? ly : 255 - ly;
    return dx < dy ? dx : dy;
}

// The world as a 256 level: world_set_size(256) clears the maps when the size changes.
static void to_256(const Maps &m) {
    world_set_size(256);
    put(m);
}
static void to_wide(const Maps &m256) {
    to_256(m256);
    CHECK(world_widen_tiled(kWide));
    CHECK_EQ(g_world.size, kWide);
}

// ---- 1. the generation passes ----------------------------------------------------------------------------
// The texture buckets exactly as terrain_assign_textures_30eb0 builds them (first 12 of each key).
struct Bucket { int n = 0; uint8_t tex[12], rot[12]; };
static std::vector<Bucket> texture_buckets() {
    std::vector<Bucket> b(2401);
    auto add = [&](int key, uint8_t tex, uint8_t rot) {
        Bucket &k = b[(size_t)key];
        if (k.n < 12) { k.tex[k.n] = tex; k.rot[k.n] = rot; k.n++; }
    };
    auto key = [](int c0, int c1, int c2, int c3) { return c0 * 0x157 + c1 * 0x31 + c2 * 7 + c3; };
    for (int t = 0; t < 0x94; t++) {
        const int8_t *e = g_corner_class_tuples + t * 4;
        int a = e[0], bb = e[1], c = e[2], d = e[3];
        if (a < 0 || bb < 0 || c < 0 || d < 0) continue;
        const uint8_t tex = (uint8_t)t;
        add(key(a, bb, c, d), tex, 0x00); add(key(bb, a, d, c), tex, 0x10);
        add(key(c, d, a, bb), tex, 0x30); add(key(d, c, bb, a), tex, 0x20);
        add(key(bb, c, d, a), tex, 0x60); add(key(c, bb, a, d), tex, 0x70);
        add(key(d, a, bb, c), tex, 0x50); add(key(a, d, c, bb), tex, 0x40);
    }
    return b;
}

enum PassKind { kExact, kInPlace, kTextures, kLight };
struct Pass {
    const char *name;
    PassKind kind;
    std::function<void()> run;
};

static void test_passes() {
    std::printf("-- 1. generation passes, level %d, 256 vs %d tiled\n", kLevel, kWide);
    CHECK(sim_load_level(kLevel));
    const GenMap gen = g_state->level.gen;
    const Maps level_maps = grab();      // after features (castles etc.)
    (void)level_maps;

    // replay terrain_build in the 256 world, keeping the input of every pass
    world_set_size(256);
    g_rng16 = (uint16_t)gen.seed;
    g_state->rng = (uint32_t)gen.seed;
    std::memset(g_cell_things, 0, sizeof(uint16_t) * g_world.cells);
    terrain_fractal_fill(g_rng16, (uint16_t)gen.off, (uint16_t)gen.raise, (uint16_t)gen.gnarl);
    terrain_generate();
    std::memset(g_cell_things, 0, sizeof(uint16_t) * g_world.cells);
    terrain_carve_rivers(gen.river, (uint16_t)gen.sourc);

    const std::vector<Pass> passes = {
        {"flatten_water_quads", kInPlace, [] { terrain_flatten_water_quads(); }},
        {"classify_flat", kInPlace, [&] { terrain_classify_flat((uint16_t)gen.snflt); }},
        {"mark_lowland", kExact, [&] { terrain_mark_lowland((uint16_t)gen.bhlin, (uint16_t)gen.bhflt); }},
        {"insert_transitions", kInPlace, [] { terrain_insert_transitions(); }},
        {"mark_interior", kInPlace, [&] { terrain_mark_interior((uint16_t)gen.bhlin, (uint16_t)gen.bhflt); }},
        {"mark_steep", kInPlace, [&] { terrain_mark_steep((uint16_t)gen.rkste); }},
        {"flags_fill_holes", kInPlace, [] { terrain_flags_fill_holes(); }},
        {"clear_types", kExact, [] { std::memset(g_map_type, 0, g_world.cells); }},
        {"smooth_spikes", kInPlace, [] { terrain_smooth_spikes(); }},
        {"fix_shore_quads", kInPlace, [] { terrain_fix_shore_quads(); }},
        {"assign_textures", kTextures, [] { terrain_assign_textures(); }},
        {"mark_water_anim", kExact, [] { terrain_mark_water_anim(); }},
        {"build_lightmap", kLight, [] { terrain_build_lightmap(); }},
    };
    std::vector<Maps> in(passes.size()), out(passes.size());
    std::vector<uint16_t> rng_in(passes.size());
    for (size_t k = 0; k < passes.size(); k++) {
        in[k] = grab();
        rng_in[k] = g_rng16;
        passes[k].run();
        out[k] = grab();
    }
    // the replay is terrain_build
    {
        const Maps replay = out.back();
        terrain_build(gen);
        const Maps built = grab();
        size_t d = 0;
        for (size_t i = 0; i < g_world.cells; i++) d += !cell_equal(replay, built, i);
        CHECK_EQ(d, 0);
    }

    const std::vector<Bucket> buckets = texture_buckets();
    for (size_t k = 0; k < passes.size(); k++) {
        to_wide(in[k]);
        g_rng16 = rng_in[k];
        passes[k].run();
        const Maps got = grab();
        const Maps want = tile(out[k], kWide);
        const Maps inp = tile(in[k], kWide);
        size_t diff = 0, beyond = 0;
        int max_seam = -1;
        size_t rng_cells = 0, bad = 0;
        for (int y = 0; y < kWide; y++)
            for (int x = 0; x < kWide; x++) {
                const size_t i = (size_t)y * kWide + x;
                if (passes[k].kind == kTextures && inp.type[i] == 0) {
                    // a random texture of the cell's corner-class bucket (or 1 when the bucket is empty)
                    rng_cells++;
                    auto cls = [&](int cx, int cy) { return inp.flags[(size_t)(cy & (kWide - 1)) * kWide + (cx & (kWide - 1))] & 7; };
                    const int key = cls(x, y) * 0x157 + cls(x + 1, y) * 0x31 + cls(x + 1, y + 1) * 7 + cls(x, y + 1);
                    const Bucket &b = buckets[(size_t)key];
                    bool ok;
                    if (b.n == 0) ok = got.type[i] == 1 && got.flags[i] == inp.flags[i];
                    else {
                        ok = false;
                        for (int r = 0; r < b.n; r++)
                            if (got.type[i] == b.tex[r] && got.flags[i] == (uint8_t)((inp.flags[i] & 7) + b.rot[r])) ok = true;
                    }
                    ok = ok && got.height[i] == inp.height[i] && got.light[i] == inp.light[i];
                    bad += !ok;
                    continue;
                }
                if (passes[k].kind == kLight) {
                    const uint8_t a = inp.height[(size_t)((y + 1) & (kWide - 1)) * kWide + ((x + 1) & (kWide - 1))];
                    const uint8_t b = inp.height[(size_t)((y - 1) & (kWide - 1)) * kWide + ((x - 1) & (kWide - 1))];
                    if ((uint8_t)(0x20 - (uint8_t)(a - b)) == 0x20) {
                        rng_cells++;
                        const bool ok = got.light[i] >= 0x1c && got.light[i] <= 0x24 && got.type[i] == inp.type[i] &&
                                        got.height[i] == inp.height[i] && got.flags[i] == inp.flags[i];
                        bad += !ok;
                        continue;
                    }
                }
                if (!cell_equal(got, want, i)) {
                    diff++;
                    const int s = seam_dist(x, y);
                    if (s > max_seam) max_seam = s;
                    if (s > 8) beyond++;
                }
            }
        std::printf("   %-20s diff %6zu (max seam distance %d, %zu beyond 8)%s", passes[k].name, diff, max_seam, beyond,
                    passes[k].kind == kInPlace ? " in-place" : "");
        if (rng_cells) std::printf(", %zu random cells, %zu invalid", rng_cells, bad);
        std::printf("\n");
        if (passes[k].kind == kInPlace) CHECK_EQ(beyond, 0);
        else CHECK_EQ(diff, 0);
        CHECK_EQ(bad, 0);
    }

    // Hidden Worlds' snow pass on the fill_holes input (its second sweep rewrites in place)
    {
        const size_t k = 6;
        to_256(in[k]);
        terrain_mark_snow(0x50, 6);
        const Maps want = tile(grab(), kWide);
        to_wide(in[k]);
        terrain_mark_snow(0x50, 6);
        const Maps got = grab();
        size_t diff = 0, beyond = 0, changed = 0;
        const Maps inp = tile(in[k], kWide);
        for (int y = 0; y < kWide; y++)
            for (int x = 0; x < kWide; x++) {
                const size_t i = (size_t)y * kWide + x;
                changed += got.flags[i] != inp.flags[i];
                if (!cell_equal(got, want, i)) { diff++; beyond += seam_dist(x, y) > 8; }
            }
        std::printf("   %-20s diff %6zu (%zu beyond 8), %zu cells changed\n", "mark_snow", diff, beyond, changed);
        CHECK(changed > 0);
        CHECK_EQ(beyond, 0);
    }
}

// ---- 2. features at the seams ------------------------------------------------------------------------------
using Pattern = std::map<std::pair<int, int>, std::array<uint8_t, 4>>;

// The cells that changed, as torus offsets from the anchor cell.
static Pattern diff_pattern(const Maps &before, const Maps &after, int ax, int ay) {
    Pattern p;
    const int n = before.n, h = n / 2;
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) {
            const size_t i = (size_t)y * n + x;
            if (cell_equal(before, after, i)) continue;
            const int dx = ((x - ax + h) & (n - 1)) - h, dy = ((y - ay + h) & (n - 1)) - h;
            p[{dx, dy}] = {after.type[i], after.height[i], after.light[i], after.flags[i]};
        }
    return p;
}

static Thing scratch_thing(int cx, int cy, int frac) {
    Thing t;
    std::memset(&t, 0, sizeof t);
    t.x = cx * 256 + frac;
    t.y = cy * 256 + frac;
    return t;
}

// One feature case: `build(ax, ay)` in a fresh copy of the level (256 or tiled), the terrain change as a
// pattern around the anchor.
using Build = std::function<void(int ax, int ay)>;
struct CaseResult { Pattern p; std::vector<int> site; };
static CaseResult run_case(bool wide, int ax, int ay, const Build &build) {
    CHECK(sim_load_level(kLevel));
    if (wide) CHECK(world_widen_tiled(kWide));
    // flush: the level's own live effects (and every other Thing) go first, so the pattern is the feature's
    level_run_terrain_effects();
    g_rng16 = 0x1234;
    const Maps before = grab();
    build(ax, ay);
    const Maps after = grab();
    CaseResult r;
    r.p = diff_pattern(before, after, ax, ay);
    for (int dy = -12; dy <= 12; dy += 2)
        for (int dx = -12; dx <= 12; dx += 2) {
            Pos p;
            p.z = 0;
            p.x = (int32_t)(((uint32_t)(ax + dx) & g_world.cell_mask) << 8) + 0x80;
            p.y = (int32_t)(((uint32_t)(ay + dy) & g_world.cell_mask) << 8) + 0x80;
            r.site.push_back(castle_site_clear_at_pos(&p));
        }
    return r;
}

static void test_features() {
    std::printf("-- 2. features across the seams (256 local vs %d)\n", kWide);
    CHECK(sim_load_level(kLevel));
    int wizard_parent = 0;
    for (const ThingInit &r : g_state->level.things)
        if (r.dis_id == 0xffff && r.model == 0x2d) { wizard_parent = r.parent; break; }

    struct Feature { const char *name; Build build; };
    const std::vector<Feature> features = {
        {"wizard castle", [&](int ax, int ay) {
            ThingInit rec{};
            rec.cls = 10; rec.model = 0x2d; rec.x = (uint16_t)ax; rec.y = (uint16_t)ay; rec.dis_id = 0xffff;
            rec.parent = (uint16_t)wizard_parent;
            level_spawn_effect_record(&g_state->level, &rec);
            level_run_terrain_effects();
        }},
        {"castle stamp", [](int ax, int ay) {
            Thing t = scratch_thing(ax, ay, 0);
            t.z = (int16_t)(g_map_height[world_cell(ax, ay)] << 5);
            t.castle_size = 3;
            castle_stamp_footprint(&t);
        }},
        {"spiral walk", [](int ax, int ay) {
            Thing t = scratch_thing(ax, ay, 0x80);
            t.ext_x = 0x900;
            terrain_modify_cells_spiral(&t, 0, 9);
            CHECK(terrain_find_cell_spiral(&t, 0, 6, 5, 0) == 0);
        }},
        {"wall", [](int ax, int ay) {
            level_build_wall(ax, ay, world_wrap_cell(ax + 13), world_wrap_cell(ay + 6));
            level_build_wall(ax, ay, world_wrap_cell(ax + 2), world_wrap_cell(ay - 9));
            level_run_terrain_effects();
        }},
        {"path", [](int ax, int ay) {
            level_build_path(ax, ay, world_wrap_cell(ax + 11), world_wrap_cell(ay + 4));
            level_build_path(ax, ay, world_wrap_cell(ax - 3), world_wrap_cell(ay - 10));
            level_run_terrain_effects();
        }},
        {"canyon", [](int ax, int ay) {
            level_build_canyon(ax, ay, world_wrap_cell(ax + 12), world_wrap_cell(ay + 3));
            level_run_terrain_effects();
        }},
        {"ridge", [](int ax, int ay) {
            level_build_ridge(world_wrap_cell(ax - 6), ay, world_wrap_cell(ax + 10), world_wrap_cell(ay + 2));
            level_run_terrain_effects();
        }},
    };
    // anchors: local (lx, ly) in the 256 world; the same local cell in the tiled world across the world
    // seam, across an old tile boundary, and both axes at once
    struct Anchor { const char *name; int lx, ly; std::vector<std::pair<int, int>> wide; };
    const std::vector<Anchor> anchors = {
        {"x seam", 252, 120, {{252 + 768, 120 + 256}, {252, 120}, {252 + 256, 120 + 512}}},
        {"y seam", 60, 251, {{60 + 512, 251 + 768}, {60 + 256, 251}, {60, 251 + 256}}},
        {"corner", 253, 253, {{253 + 768, 253 + 768}, {253, 253}, {253 + 512, 253 + 256}}},
        {"inside", 130, 90, {{130 + 768, 90 + 256}, {130, 90}}},
    };
    // wide[0] crosses the world seam: must equal the 256 world (whose seam it is). The others cross an old
    // tile boundary: they must equal each other, and the 256 world too - except for the features built with
    // the original's *linear* cell cursors (a wall row's `c++`, terrain_smooth_cell's cell - 0x101 ...),
    // which step from (N-1, y) to (0, y + 1) at the world's x seam only; at an old boundary inside a wide
    // world they step to the true neighbour, so the 256 world's quirk cells differ there (counted).
    for (const Feature &f : features) {
        const bool linear = std::strcmp(f.name, "wizard castle") == 0 || std::strcmp(f.name, "wall") == 0;
        for (const Anchor &a : anchors) {
            const CaseResult ref = run_case(false, a.lx, a.ly, f.build);
            std::printf("   %-14s %-7s 256: %4zu cells", f.name, a.name, ref.p.size());
            CHECK(!ref.p.empty());
            CaseResult first_old;
            for (size_t k = 0; k < a.wide.size(); k++) {
                const auto &w = a.wide[k];
                const CaseResult got = run_case(true, w.first, w.second, f.build);
                const bool same = got.p == ref.p && got.site == ref.site;
                size_t differ = 0;
                for (const auto &e : got.p) { auto it = ref.p.find(e.first); if (it == ref.p.end() || it->second != e.second) differ++; }
                for (const auto &e : ref.p) if (!got.p.count(e.first)) differ++;
                std::printf("  (%d,%d): %s", w.first, w.second, same ? "same" : "differs");
                if (!same) std::printf(" [%zu cells, %zu differ%s]", got.p.size(), differ, got.site == ref.site ? "" : ", site test differs");
                if (k == 0 || !linear) {
                    CHECK(same);
                } else if (k == 1) {
                    first_old = got;
                    CHECK(differ < 32);                 // only the quirk cells along the boundary column
                } else {
                    CHECK(got.p == first_old.p && got.site == first_old.site);
                }
            }
            std::printf("\n");
        }
    }
}

// The canyon / ridge builders' heading and length between end points 400 cells apart (no torus alias).
static Thing *find_new(uint8_t type, int32_t x, int32_t y) {
    for (int i = 1; i < thing_pool_slots(); i++) {
        Thing *t = thing_at((unsigned)i);
        if (t->cls == 10 && t->type == type && !(t->flags & 0x400) && t->x == x && t->y == y) return t;
    }
    return nullptr;
}
static void test_long_builders() {
    std::printf("-- 2b. canyon / ridge segments 400 cells long in the %d world\n", kWide);
    CHECK(sim_load_level(kLevel));
    CHECK(world_widen_tiled(kWide));
    level_run_terrain_effects();
    struct Seg { int x0, y0, x1, y1; };
    // +x, -y (across the seam), and a diagonal; each against the same heading 10 cells long
    const Seg segs[] = { {100, 300, 500, 300}, {700, 100, 700, 724}, {900, 900, 1000 + 300 - 1024, 1000 + 300 - 1024} };
    const Seg shorts[] = { {100, 300, 110, 300}, {700, 100, 700, 90}, {900, 900, 910, 910} };
    const int cells[] = { 400, 400, 565 };
    for (int k = 0; k < 3; k++) {
        const Seg &s = segs[k], &q = shorts[k];
        level_build_canyon(s.x0, s.y0, s.x1, s.y1);
        Thing *c = find_new(0x20, s.x0 << 8, s.y0 << 8);
        level_build_ridge(s.x0, s.y0, s.x1, s.y1);
        Thing *r = find_new(0x33, s.x0 << 8, s.y0 << 8);
        CHECK(c && r);
        if (!c || !r) continue;
        const int cyaw = c->yaw, chealth = c->health, ryaw = r->yaw, rhealth = r->health;
        thing_mark_delete(c); thing_mark_delete(r);
        level_build_canyon(q.x0, q.y0, q.x1, q.y1);
        Thing *cq = find_new(0x20, q.x0 << 8, q.y0 << 8);
        CHECK(cq);
        const int qyaw = cq ? cq->yaw : -1;
        if (cq) thing_mark_delete(cq);
        std::printf("   (%d,%d)->(%d,%d): canyon yaw %#x (10-cell %#x) length %d cells, ridge yaw %#x steps %d\n",
                    s.x0, s.y0, s.x1, s.y1, cyaw, qyaw, chealth, ryaw, rhealth);
        CHECK_EQ(cyaw, qyaw);
        CHECK_EQ(ryaw, qyaw);
        CHECK(chealth >= cells[k] - 1 && chealth <= cells[k]);
        CHECK(rhealth >= cells[k] / 4 - 1 && rhealth <= cells[k] / 4);
    }
}

// ---- 3. determinism of the tiled world ---------------------------------------------------------------------
static std::vector<uint32_t> run_ticks(int ticks) {
    std::vector<uint32_t> sums;
    CHECK(sim_load_level(kLevel));
    CHECK(world_widen_tiled(kWide));
    for (int t = 0; t < ticks; t++) {
        game_tick_sim();
        sums.push_back(net_state_checksum());
    }
    return sums;
}
static void test_determinism() {
    constexpr int kTicks = 2000;
    std::printf("-- 3. %d ticks of the tiled %d world, twice\n", kTicks, kWide);
    const std::vector<uint32_t> a = run_ticks(kTicks);
    const std::vector<uint32_t> b = run_ticks(kTicks);
    size_t first = a.size();
    for (size_t i = 0; i < a.size() && i < b.size(); i++) if (a[i] != b[i]) { first = i; break; }
    std::printf("   checksums %08x .. %08x, first mismatch %s\n", a.front(), a.back(),
                first == a.size() ? "none" : std::to_string(first).c_str());
    CHECK(first == a.size());
}

int main(int argc, char **argv) {
    mc_install_crash_handler();
    setvbuf(stdout, nullptr, _IONBF, 0);
    const char *game = argc > 1 ? argv[1] : MC_DEFAULT_GAME_DIR;
    if (!sim_init(game)) { std::printf("SKIP: no game data in %s\n", game); return 0; }
    sim_register_gameplay();
    g_hook_player_local_input = nullptr;            // idle local player: the AI wizards play
    test_passes();
    test_features();
    test_long_builders();
    test_determinism();
    world_set_size(256);
    std::printf(g_fail ? "world_terrain_test: %d FAILED\n" : "world_terrain_test: OK\n", g_fail);
    return g_fail ? 1 : 0;
}
