// Round 11 session 3 (task F): the 256-cell world through the round 11 code equals the pre-round-11 code, plus
// small wide-world probes the session 1 tests (world_{terrain,things,creatures,persist,render}_test, all at
// 1024) do not make. ${MC_SIM_ALL}, no renderer. argv[1] = game dir.
//
//  1. golden: for every level of levels.dat (0..69) and, when the Hidden Worlds data is installed (MC_HIDDEN_DIR,
//     <game>/hidden or <repo>/extracted/gog_cd/CARPET), every DDLEVELS entry (100..169): FNV-1a 32 of the five maps
//     (via g_world at 256) and of the GameState image (state_to_image, mc_image.h) right after sim_load_level and
//     after 1, 100 and 500 idle ticks must equal src/tests/data/world_levels_256.txt, written by the code of
//     commit bf77e98 (the last commit before round 11; docs/analysis/port_world_test.md says how). Every level
//     starts from the same state: GameState / Config as after sim_init, the dummy player block zeroed and the
//     four gameplay globals the original never resets (AI RNG ...) at their initial values - with that reset the
//     old code gives the same hashes in one process, in reverse order and in a fresh process per level;
//  2. 512 and 2048 tiled worlds (level 38): an archer walked east and south across the old 255 -> 256 boundary
//     and across the new seam N*256 -> 0 follows the 256 track mod 65536, moves on continuously and stays in its
//     cell list; collision / mana / area-damage / cell-kill searches (the spiral cell walks of spatial.cpp)
//     across both seams hit the Thing next to the searcher and not its 256-world alias;
//  3. a 4096 world: world_dist_sq / world_isqrt / pos_dist_sq_xyz / pos_dist_xyz / math_atan2_w / pos_angle_to
//     at 500, 1000 and 2000 cells (and 2100 / 3000: the torus' short way) - exact int64 squares, floor roots,
//     the true angle within 2 units (not the 256-world alias);
//  4. `rts --size 1024 --bots 3` (seed 1, a generated world: mode_start_run + sim_load_level(MODE_LEVEL_INDEX)),
//     2000 ticks twice in one process: identical per-tick net_state_checksum sequences.
// MC_WORLD_TEST_WRITE=<file> writes the golden lines of the current code (to compare with the old code's).
#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "sim.h"
#include "thing.h"
#include "player.h"
#include "spatial.h"
#include "creatures.h"
#include "ai_wizard.h"
#include "projectiles.h"
#include "terrain_paint.h"
#include "world_set.h"
#include "mode.h"
#include "mode_level.h"
#include "net.h"
#include "world.h"
#include "mc_image.h"
#include "mc_globals.h"
#include "crash_handler.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)
#define CHECK_EQ(a, b) do { long long va_ = (long long)(a), vb_ = (long long)(b); if (va_ != vb_) { \
    std::printf("FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, va_, vb_); g_fail++; } } while (0)

static std::string g_game;

static double ms_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

// ---- 1. golden ------------------------------------------------------------------------------------------

static uint32_t fnv(const void *p, size_t n, uint32_t h = 2166136261u) {
    const uint8_t *b = static_cast<const uint8_t *>(p);
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; }
    return h;
}

static GameStateImage g_image;
static uint32_t state_hash() {
    state_to_image(*g_state, &g_image);
    return fnv(&g_image, MC_STATE_IMAGE_SIZE);
}
static uint32_t ext_hash() {
    uint32_t h = 2166136261u;
    for (int i = MC_THING_SLOTS; i < thing_pool_slots(); i++) {
        ThingImage im;
        thing_to_image(*thing_at((unsigned)i), &im);
        h = fnv(&im, sizeof im, h);
    }
    return h;
}

static std::vector<uint8_t> g_state0, g_cfg0;

// The state every level starts from (the golden file's rule; see the header).
static void reset_globals() {
    std::memcpy(g_state, g_state0.data(), g_state0.size());
    std::memcpy(g_cfg, g_cfg0.data(), g_cfg0.size());
    std::memset(g_dummy_player_block, 0, sizeof g_dummy_player_block);
    g_ai_rand_seed = 1;
    g_ai_human_wizard = 0;
    g_projectile_null_hit_index = 0;
    g_terrain_nearly_flat = 0;
}

static std::string level_line(int L) {
    char buf[512];
    reset_globals();
    if (!sim_load_level(L)) { std::snprintf(buf, sizeof buf, "%d none", L); return buf; }
    CHECK_EQ(g_world.size, 256);
    const uint32_t ty = fnv(g_map_type, g_world.cells), he = fnv(g_map_height, g_world.cells),
                   li = fnv(g_map_light, g_world.cells), fl = fnv(g_map_flags, g_world.cells),
                   ct = fnv(g_cell_things, (size_t)g_world.cells * 2), s0 = state_hash();
    uint32_t t[3];
    int n = 0;
    const int at[3] = {1, 100, 500};
    for (int k = 0; k < 3; k++) {
        while (n < at[k]) { game_tick_sim(); n++; }
        t[k] = state_hash();
    }
    std::snprintf(buf, sizeof buf,
                  "%d type %08x height %08x light %08x flags %08x cells %08x state %08x t1 %08x t100 %08x t500 %08x pool %d ext %08x",
                  L, ty, he, li, fl, ct, s0, t[0], t[1], t[2], thing_pool_slots(), ext_hash());
    return buf;
}

static std::string hidden_dir() {
    if (const char *e = std::getenv("MC_HIDDEN_DIR")) return e;
    std::error_code ec;
    const fs::path a = fs::path(g_game) / "hidden";
    if (fs::is_directory(a, ec)) return a.string();
    const fs::path b = fs::path(MC_REPO_DIR) / "extracted" / "gog_cd" / "CARPET";
    return b.lexically_normal().string();
}

// "name value name value ..." after the level number -> the first differing name
static std::string first_diff(const std::string &a, const std::string &b) {
    std::istringstream sa(a), sb(b);
    std::string x, y, name;
    int k = 0;
    while (sa >> x && sb >> y) {
        if (x != y) return k == 0 ? "level" : name;
        if (k % 2 == 1) name = x;
        k++;
    }
    return "length";
}

static void golden() {
    const auto t0 = std::chrono::steady_clock::now();
    std::string path = MC_REPO_DIR "/src/tests/data/world_levels_256.txt";
    if (const char *e = std::getenv("MC_WORLD_GOLDEN")) path = e;
    std::ifstream in(path);
    CHECK(in.good());
    if (!in.good()) { std::printf("1. golden file %s missing\n", path.c_str()); return; }
    std::map<int, std::string> want;
    std::vector<int> order;
    for (std::string line; std::getline(in, line);) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        const int L = std::atoi(line.c_str());
        want[L] = line;
        order.push_back(L);
    }
    const std::string hd = hidden_dir();
    world_set_init(g_game.c_str(), hd.c_str());
    const bool hidden = world_set_hidden_available();
    g_state0.assign(reinterpret_cast<const uint8_t *>(g_state), reinterpret_cast<const uint8_t *>(g_state) + sizeof(GameState));
    g_cfg0.assign(reinterpret_cast<const uint8_t *>(g_cfg), reinterpret_cast<const uint8_t *>(g_cfg) + sizeof(Config));

    FILE *out = nullptr;
    if (const char *w = std::getenv("MC_WORLD_TEST_WRITE")) out = std::fopen(w, "wb");
    int checked = 0, same = 0, skipped = 0, base = 0;
    std::map<std::string, int> diff_by_part;
    for (int L : order) {
        if (world_is_hidden_level(L) && !hidden) { skipped++; continue; }
        const std::string got = level_line(L);
        if (out) std::fprintf(out, "%s\n", got.c_str());
        checked++;
        base += L < HW_LEVEL_BASE;
        if (got == want[L]) { same++; continue; }
        const std::string part = first_diff(want[L], got);
        diff_by_part[part]++;
        std::printf("FAIL level %d differs from bf77e98 (first: %s)\n  old %s\n  new %s\n", L, part.c_str(), want[L].c_str(),
                    got.c_str());
        g_fail++;
    }
    if (out) std::fclose(out);
    world_set_init(g_game.c_str(), hd.c_str());           // back to set 0
    std::printf("1. golden: %d levels (%d base, %d Hidden Worlds%s), %d identical to bf77e98, %d differ; %.0f ms\n", checked,
                base, checked - base, hidden ? "" : ", data not installed: skipped", same, checked - same, ms_since(t0));
    for (const auto &kv : diff_by_part) std::printf("   first difference in %s: %d levels\n", kv.first.c_str(), kv.second);
    CHECK(base == 70);
    CHECK(skipped == 0 || !hidden);
}

// ---- 2. 512 / 2048 tiled worlds -----------------------------------------------------------------------------

static bool in_own_cell_list(const Thing *t) {
    if (!(t->flags & 4)) return false;
    int guard = 0;
    for (unsigned i = g_cell_things[world_cell_of(t->x, t->y)]; i != 0; i = thing_at(i)->cell_next) {
        if (thing_at(i) == t) return true;
        if (++guard > thing_pool_slots()) return false;
    }
    return false;
}

static Pos ground_pos(int32_t x, int32_t y) {
    Pos p{};
    p.x = x;
    p.y = y;
    p.z = 0;
    p.z = (int16_t)terrain_height_at(&p);
    return p;
}

struct Step { int32_t x, y; int z, yaw, health; };

// Restores the creature from `init` (everything behind the cell links) at (x0, y0) and steps it `steps` times
// along yaw with creature_move_step (terrain only).
static std::vector<Step> walk(Thing *c, const Thing &init, int32_t x0, int32_t y0, int yaw, int steps, bool cells) {
    thing_unlink_cell(c);
    std::memcpy(reinterpret_cast<uint8_t *>(c) + 0x18, reinterpret_cast<const uint8_t *>(&init) + 0x18, sizeof(Thing) - 0x18);
    c->flags &= ~4u;
    c->yaw = (uint16_t)yaw;
    c->target_yaw = (uint16_t)yaw;
    c->speed_cur = 0x40;
    c->health = 100;
    Pos p = ground_pos(x0, y0);
    thing_link_cell(c, &p);
    std::vector<Step> track;
    for (int s = 0; s < steps; s++) {
        c->target_yaw = (uint16_t)yaw;
        creature_move_step(c);
        track.push_back({c->x, c->y, c->z, c->yaw, c->health});
        if (cells && !in_own_cell_list(c)) { std::printf("FAIL: step %d: not in its cell list\n", s); g_fail++; cells = false; }
    }
    return track;
}

struct WalkRef { int32_t x0, y0; int yaw; std::vector<Step> track; };

// 256 references: the first row (east) / column (south) where the archer survives and crosses x / y = 65535 -> 0
// by more than 8 cells.
static std::vector<WalkRef> find_walks(Thing *c, const Thing &init) {
    std::vector<WalkRef> out;
    for (int dir = 0; dir < 2; dir++) {
        for (int k = 3; k < 256; k += 7) {
            const int yaw = dir == 0 ? 0x200 : 0x400;
            const int32_t x0 = dir == 0 ? 0xf800 : (k << 8) + 0x80;
            const int32_t y0 = dir == 0 ? (k << 8) + 0x80 : 0xf800;
            std::vector<Step> tr = walk(c, init, x0, y0, yaw, 220, false);
            int64_t moved = 0;
            int32_t px = x0, py = y0;
            bool alive = true;
            for (const Step &s : tr) {
                moved += dir == 0 ? world_signed(s.x - px) : world_signed(s.y - py);
                px = s.x; py = s.y;
                if (s.health < 0) alive = false;
            }
            if (!alive || moved < 0x1000) continue;
            out.push_back({x0, y0, yaw, tr});
            break;
        }
    }
    return out;
}

// The walk started at (x0 + ox, y0 + oy) in the wide world: the 256 track mod 65536, no step longer than a cell,
// ends one tile further (unwrapped), in its cell list every step.
static void check_walk(Thing *c, const Thing &init, const WalkRef &w, int32_t ox, int32_t oy, const char *what) {
    const int32_t x0 = world_wrap(w.x0 + ox), y0 = world_wrap(w.y0 + oy);
    std::vector<Step> tr = walk(c, init, x0, y0, w.yaw, (int)w.track.size(), true);
    int bad = 0, jumps = 0;
    int32_t px = x0, py = y0;
    int64_t ux = x0, uy = y0;
    for (size_t i = 0; i < tr.size(); i++) {
        const Step &a = tr[i], &b = w.track[i];
        if (((a.x - ox) & 0xffff) != b.x || ((a.y - oy) & 0xffff) != b.y || a.z != b.z || a.yaw != b.yaw || a.health != b.health) {
            if (bad++ == 0)
                std::printf("FAIL %s yaw %x step %zu: wide (%x,%x,%d,%x) vs 256 (%x,%x,%d,%x)\n", what, w.yaw, i, a.x, a.y, a.z,
                            a.yaw, b.x, b.y, b.z, b.yaw);
        }
        const int32_t dx = world_signed(a.x - px), dy = world_signed(a.y - py);
        if (dx > 0x100 || dx < -0x100 || dy > 0x100 || dy < -0x100) jumps++;
        ux += dx; uy += dy;
        px = a.x; py = a.y;
    }
    if (bad) g_fail++;
    CHECK_EQ(jumps, 0);
    const bool east = w.yaw == 0x200;
    const int64_t end = east ? ux : uy, start = east ? x0 : y0;
    CHECK_EQ(end >> 16, (start >> 16) + 1);
    CHECK_EQ(east ? tr.back().x : tr.back().y, world_wrap((int32_t)end));
    std::printf("   %-34s %s from (%6x,%6x) to (%6x,%6x), %zu steps%s\n", what, east ? "east " : "south", x0, y0, tr.back().x,
                tr.back().y, tr.size(), bad ? " MISMATCH" : "");
}

static Thing *make_thing(int32_t x, int32_t y, int cls, int type, int ext) {
    Thing *t = thing_alloc();
    if (!t) return nullptr;
    t->cls = (uint8_t)cls;
    t->type = (uint8_t)type;
    t->state = 0;
    t->prop_flags = 3;
    thing_set_extents(t, ext, 0x100);
    Pos p = ground_pos(x, y);
    thing_link_cell(t, &p);
    return t;
}

// Searcher A at (ax, y), victim V 0x80 east of it (across the seam when ax is just west of one), decoy D at V's
// 256-world alias (V + 65536).
static void spatial_case(const char *what, int32_t ax, int32_t y) {
    const int32_t vx = world_wrap(ax + 0x80);
    Thing *A = make_thing(ax, y, 10, 0x30, 0x80);
    Thing *V = make_thing(vx, y, 5, 1, 0x80);
    Thing *D = make_thing(world_wrap(vx + 65536), y, 5, 1, 0x80);
    CHECK(A && V && D);
    if (!A || !V || !D) return;
    int bad0 = g_fail;
    CHECK(in_own_cell_list(V) && in_own_cell_list(D));
    CHECK(thing_find_collision(A) == V);
    CHECK(thing_find_collision_other_owner(A) == V);
    V->damage_slots[0] = DamageSlot{0, 0};
    D->damage_slots[0] = DamageSlot{0, 0};
    thing_area_damage(A, 0, 77);
    CHECK_EQ(V->damage_slots[0].amount, 77);
    CHECK_EQ(D->damage_slots[0].attacker, 0);
    V->damage_slots[1] = DamageSlot{0, 0};
    thing_area_damage(A, 1, 55);
    CHECK_EQ(V->damage_slots[1].amount, 55);
    CHECK_EQ(D->damage_slots[1].attacker, 0);
    Thing *M = make_thing(world_wrap(ax - 0x60), y, 10, 0x27, 0x40);
    CHECK(M != nullptr);
    if (M) {
        CHECK(thing_find_mana_ball_touching(A) == M);
        Pos mp = *thing_pos(M);
        CHECK_EQ(thing_exists_near_pos(&mp, 10, 0x27), 1);
        Pos alias = mp;
        alias.x = mp.x + 65536;
        CHECK_EQ(thing_exists_near_pos(&alias, 10, 0x27), 0);
        thing_free(M);
    }
    V->health = 100;
    D->health = 100;
    cell_kill_things(world_cell_of(V->x, V->y), A->owner);
    CHECK_EQ(V->health, -1);
    CHECK_EQ(D->health, 100);
    std::printf("   spatial %-28s A x=%06x V x=%06x: collision / area damage / mana / cell kill %s\n", what, ax, vx,
                g_fail == bad0 ? "ok" : "WRONG");
    thing_free(A);
    thing_free(V);
    thing_free(D);
}

static void tiled_world(int n) {
    const auto t0 = std::chrono::steady_clock::now();
    reset_globals();
    CHECK(sim_load_level(38));
    for (int k = 0; k < 4; k++) game_tick_sim();
    Pos p0 = ground_pos(0x8080, 0x8080);
    Thing *archer = thing_create(&p0, 5, 4);
    CHECK(archer != nullptr);
    if (!archer) return;
    const Thing init = *archer;
    const std::vector<WalkRef> refs = find_walks(archer, init);
    CHECK_EQ(refs.size(), 2u);
    CHECK(world_widen_tiled(n));
    CHECK_EQ(g_world.size, n);
    CHECK_EQ(g_world.units, n * 256);
    std::printf("2. level 38 tiled to %d cells (%d x %d tiles):\n", n, n / 256, n / 256);
    const int32_t last = g_world.units - 0x10000;          // the last tile
    for (const WalkRef &w : refs) {
        const bool east = w.yaw == 0x200;
        check_walk(archer, init, w, 0, 0, "across the old boundary 65535->65536");
        if (east) check_walk(archer, init, w, last, 0x10000, "across the seam N*256->0 (x)");
        else check_walk(archer, init, w, 0x10000, last, "across the seam N*256->0 (y)");
        if (n > 512) check_walk(archer, init, w, last / 2 & ~0xffff, last / 2 & ~0xffff, "between two middle tiles");
    }
    thing_unlink_cell(archer);
    thing_free(archer);
    const int32_t y = 0x10000 + 0x4080;                     // a tile row without the level's Things
    spatial_case("across x = N*256 -> 0", g_world.units - 0x40, y);
    spatial_case("across x = 0xffff -> 0x10000", 0x10000 - 0x40, y);
    if (n > 512) spatial_case("across x = 0x2ffff -> 0x30000", 0x30000 - 0x40, world_wrap(g_world.units - 0x8000));
    std::printf("   %.0f ms\n", ms_since(t0));
}

// ---- 3. a 4096 world: distances and angles ------------------------------------------------------------------

static int ref_angle(double dx, double dy) {                // 0 = -y, 0x200 = +x, 2048 per turn
    double a = std::atan2(dx, -dy) * 1024.0 / 3.14159265358979323846;
    if (a < 0) a += 2048.0;
    return (int)std::lround(a) & 0x7ff;
}
static int angle_err(int a, int b) {
    const int d = (a - b) & 0x7ff;
    return d > 0x400 ? 0x800 - d : d;
}

static void world_4096() {
    const auto t0 = std::chrono::steady_clock::now();
    reset_globals();
    CHECK(sim_load_level(38));
    CHECK(world_widen_tiled(4096));
    CHECK_EQ(g_world.size, 4096);
    CHECK_EQ(g_world.units, 0x100000);
    // terrain at a far tile equals the first tile
    int terr_bad = 0;
    for (int i = 0; i < 64; i++) {
        Pos a = ground_pos(i * 0x3f1 & 0xffff, i * 0x1d3 & 0xffff), b = ground_pos(a.x + 13 * 0x10000, a.y + 9 * 0x10000);
        terr_bad += a.z != b.z;
    }
    CHECK_EQ(terr_bad, 0);
    Pos a{};
    a.x = 0x180;
    a.y = 0x7f080;
    a.z = 0x400;
    int checked = 0, worst = 0, aliased = 0;
    struct Dir { int ux, uy; } dirs[] = {{1, 0}, {0, 1}, {-1, 0}, {0, -1}, {1, 1}, {3, -2}, {-5, 7}, {7, 3}};
    for (int cells : {500, 1000, 2000, 2047, 2100, 3000}) {
        // along x, the short way round: 2100 cells east is 1996 west
        const int32_t d = cells * 256;
        const int32_t sd = cells < 2048 ? d : d - 0x100000;
        Pos b = a;
        b.x = a.x + d;
        b.z = 0x100;
        CHECK_EQ(world_signed(b.x - a.x), sd);
        const int64_t sq = (int64_t)sd * sd + (int64_t)(0x100 - 0x400) * (0x100 - 0x400);
        CHECK_EQ(pos_dist_sq_xyz(&a, &b), sq);
        CHECK_EQ(pos_dist_xyz(&a, &b), (int64_t)std::floor(std::sqrt((double)sq)));
        CHECK_EQ(pos_dist_sq_xy(&a, &b), (int64_t)sd * sd);
        CHECK_EQ(pos_dist_xy(&a, &b), sd < 0 ? -sd : sd);
        CHECK_EQ(pos_angle_to(&a, &b), sd > 0 ? 0x200 : 0x600);
        for (const Dir &u : dirs) {
            const int64_t ex = (int64_t)u.ux * d, ey = (int64_t)u.uy * d;
            // only differences that fit the torus' half width (otherwise the short way is another vector)
            if (ex >= 0x80000 || ex < -0x80000 || ey >= 0x80000 || ey < -0x80000) continue;
            const int32_t dx = (int32_t)ex, dy = (int32_t)ey;
            const int64_t q = world_dist_sq(dx, dy);
            CHECK_EQ(q, ex * ex + ey * ey);
            const uint32_t r = world_isqrt(q);
            CHECK((uint64_t)r * r <= (uint64_t)q && ((uint64_t)r + 1) * ((uint64_t)r + 1) > (uint64_t)q);
            const int ang = math_atan2_w(dx, dy), want = ref_angle((double)dx, (double)dy);
            const int err = angle_err(ang, want);
            worst = err > worst ? err : worst;
            CHECK(err <= 2);
            Pos c = a;
            c.x = a.x + dx;
            c.y = a.y + dy;
            CHECK_EQ(pos_angle_to(&a, &c), ang & 0x7ff);
            CHECK_EQ(pos_dist_xy(&a, &c), (int64_t)r);
            // the 256 world's alias of the same difference ((int16_t) per axis) points elsewhere for most of them
            const int alias = math_atan2((int16_t)dx, (int16_t)dy);
            aliased += angle_err(alias, want) > 8;
            checked++;
        }
    }
    std::printf("3. 4096 world: distances / angles at 500 .. 3000 cells, %d vector probes (the 256 world's (int16_t) alias "
                "would be wrong for %d), worst angle error %d units; %.0f ms\n", checked, aliased, worst, ms_since(t0));
    CHECK(checked >= 20);
    CHECK(aliased >= checked / 2);
}

// ---- 4. rts --size 1024 --bots 3, twice -------------------------------------------------------------------

static std::vector<uint32_t> rts_run(int ticks, uint32_t *maps) {
    std::vector<uint32_t> sums;
    ModeParams mp;
    mp.seed = 1;
    mp.world_size = 1024;
    mp.bots = 3;
    mp.humans = 1;
    mp.flags = MODE_PARAM_DEBUG;                            // as mcport rts
    g_cfg->flags = 0;
    g_cfg->paused = 0;
    CHECK(mode_start_run(g_game.c_str(), GAME_MODE_CONQUEST, mp));
    CHECK(sim_load_level(MODE_LEVEL_INDEX));
    CHECK_EQ(g_world.size, 1024);
    *maps = fnv(g_map_height, g_world.cells);
    *maps = fnv(g_map_type, g_world.cells, *maps);
    *maps = fnv(g_map_flags, g_world.cells, *maps);
    for (int i = 0; i < ticks; i++) {
        game_tick_sim();
        sums.push_back(net_state_checksum());
    }
    int live = thing_pool_live_count(), wizards = 0, beyond = 0;
    for (int p = 0; p < 8; p++) wizards += g_state->players[p].thing != 0;
    for (int k = 1; k < thing_pool_slots(); k++) {
        const Thing *t = thing_at((unsigned)k);
        beyond += t->cls != 0 && (t->x >= 0x10000 || t->y >= 0x10000);
    }
    std::printf("   run: %d ticks, %d Things live (%d outside the first 256 cells), %d player Things, last checksum %08x\n",
                ticks, live, beyond, wizards, sums.empty() ? 0u : sums.back());
    CHECK(wizards >= 4);
    CHECK(beyond > 0);
    mode_stop_run();
    return sums;
}

static void rts_twice() {
    const auto t0 = std::chrono::steady_clock::now();
    const int ticks = 2000;
    std::printf("4. rts --size 1024 --bots 3 (seed 1), %d ticks twice:\n", ticks);
    uint32_t m1 = 0, m2 = 0;
    const std::vector<uint32_t> a = rts_run(ticks, &m1);
    const std::vector<uint32_t> b = rts_run(ticks, &m2);
    CHECK_EQ(m1, m2);
    CHECK(a.size() == b.size());
    size_t first = a.size();
    for (size_t i = 0; i < a.size() && i < b.size(); i++) if (a[i] != b[i]) { first = i; break; }
    if (first != a.size()) { std::printf("FAIL: checksums differ from tick %zu\n", first + 1); g_fail++; }
    int changes = 0;
    for (size_t i = 1; i < a.size(); i++) changes += a[i] != a[i - 1];
    std::printf("   maps %08x, checksum sequences %s (%d of %zu ticks change the state); %.0f ms\n", m1,
                first == a.size() ? "identical" : "DIFFER", changes, a.size(), ms_since(t0));
    CHECK(changes > ticks / 2);
}

int main(int argc, char **argv) {
    mc_install_crash_handler();
    setvbuf(stdout, nullptr, _IONBF, 0);
    g_game = argc > 1 ? argv[1] : MC_DEFAULT_GAME_DIR;
    if (!sim_init(g_game.c_str())) { std::printf("SKIP: no game data in %s\n", g_game.c_str()); return 0; }
    sim_register_gameplay();
    g_hook_player_local_input = nullptr;
    const auto t0 = std::chrono::steady_clock::now();

    golden();                                       // before mode_level_register: the campaign path only
    tiled_world(512);
    tiled_world(2048);
    world_4096();
    mode_level_register();
    rts_twice();

    std::printf("world_test: %s (%d failure(s)), %.1f s\n", g_fail ? "FAIL" : "OK", g_fail, ms_since(t0) / 1000.0);
    return g_fail ? 1 : 0;
}
