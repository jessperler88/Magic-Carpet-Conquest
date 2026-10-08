// Round 11 task C: creatures, wizard AI and projectiles in a world wider than 256 cells. ${MC_SIM_ALL}, no
// renderer. argv[1] = game dir. Level 38 is loaded and widened to 1024 cells with world_widen_tiled (the 256
// level repeated 4 x 4 times), which makes every terrain-only query at (x + k*65536, y + m*65536) an oracle:
//
//  1. terrain_slope_at (creature_common.cpp) on the tiled maps == the 256 world at (x mod 65536, y mod 65536);
//     terrain_rect_is_flat the same (INFO only: it calls level_features.cpp's terrain_rect_height_range);
//  2. walks: an archer (walker) and a bee (flyer) are stepped with creature_move_step (terrain-only) toward
//     the east (and the south) across x = 65535 in the 256 world; the same walks started in the tiled world
//     (a) across the old 256 boundary 65535 -> 65536, (b) across the new seam N*256 -> 0 and (c) inside
//     other tiles give the same track mod 65536, move on continuously (no step longer than a cell) into the
//     next tile, and the walker stays in the right cell list every step;
//  3. a creature in its attack state with its target 3 cells away on the other side of the N*256 seam keeps
//     the target and the attack state for 64 ticks and aims east (target_yaw 0x200);
//  4. AI wizard: a target 400 cells east (and 30 south) - ai_mode4_approach_target's target_yaw is the angle
//     of the true difference, not of its 256-world alias; player_find_nearest_by_type picks a castle 200
//     cells away over one 300 cells away (whose squared distance overflows 32 bits); the castle-site search
//     (ai_find_castle_site) in the 1024 world returns sites in all 4 x 4 quadrants of 256 cells (corners and
//     centres), in the 256 world the original's 64-cell quadrants;
//  5. determinism: level 38 tiled to 1024 with a third of the creatures moved into other tiles, 3000 ticks
//     of game_tick_sim twice: identical net_state_checksum sequences, cell lists consistent.
#include "sim.h"
#include "creatures.h"
#include "ai_wizard.h"
#include "player.h"
#include "net.h"
#include "mc_math.h"
#include "crash_handler.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <utility>
#include <vector>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)
#define CHECK_EQ(a, b) do { long long va_ = (long long)(a), vb_ = (long long)(b); if (va_ != vb_) { \
    std::printf("FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, va_, vb_); g_fail++; } } while (0)

static const int kN = 1024;

static bool in_own_cell_list(const Thing *t) {
    if (!(t->flags & 4)) return false;
    int guard = 0;
    for (unsigned i = g_cell_things[world_cell_of(t->x, t->y)]; i != 0; i = thing_at(i)->cell_next) {
        if (thing_at(i) == t) return true;
        if (++guard > thing_pool_slots()) return false;
    }
    return false;
}

static Pos make_pos(int32_t x, int32_t y) {
    Pos p{};
    p.x = x;
    p.y = y;
    p.z = 0;
    p.z = (int16_t)terrain_height_at(&p);
    return p;
}

// ---- 1. terrain probes -------------------------------------------------------------------------------------
struct Probe { int32_t x, y; int slope; int flat; };
static std::vector<Probe> probe_points() {
    std::vector<Probe> v;
    uint32_t r = 12345;
    for (int i = 0; i < 4000; i++) {
        r = r * 1103515245u + 12345u; int32_t x = (int32_t)(r >> 8) & 0xffff;
        r = r * 1103515245u + 12345u; int32_t y = (int32_t)(r >> 8) & 0xffff;
        if (i < 64) { x = 0xff00 | (i * 4); y = (i * 0x400) & 0xffff; }        // the last column: neighbours across the seam
        else if (i < 128) { y = 0xff00 | ((i - 64) * 4); }                    // the last row
        v.push_back({x, y, 0, 0});
    }
    return v;
}
static void probe_fill(std::vector<Probe> &v, int32_t ox, int32_t oy) {
    for (Probe &p : v) {
        Pos q{};
        q.x = p.x + ox;
        q.y = p.y + oy;
        p.slope = terrain_slope_at(&q);
        p.flat = terrain_rect_is_flat(&q, 5, 3, 0x10);
    }
}

// ---- 2. walks ----------------------------------------------------------------------------------------------
struct Step { int32_t x, y; int z, yaw, health; };
static Thing g_walker0, g_flyer0;   // the constructed creatures, restored before every walk

static std::vector<Step> walk(Thing *c, const Thing &init, int32_t x0, int32_t y0, int yaw, int steps, bool cells) {
    thing_unlink_cell(c);
    std::memcpy(reinterpret_cast<uint8_t *>(c) + 0x18, reinterpret_cast<const uint8_t *>(&init) + 0x18, sizeof(Thing) - 0x18);
    c->flags &= ~4u;
    c->yaw = (uint16_t)yaw;
    c->target_yaw = (uint16_t)yaw;
    c->speed_cur = 0x40;
    c->health = 100;
    Pos p = make_pos(x0, y0);
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

struct WalkCase { Thing *c; const Thing *init; const char *name; int32_t x0, y0; int yaw; std::vector<Step> ref; };

// The 256 reference: rows / columns where the creature survives and crosses the seam by more than 8 cells.
static std::vector<WalkCase> find_walks(Thing *c, const Thing &init, const char *name) {
    std::vector<WalkCase> out;
    for (int dir = 0; dir < 2; dir++) {
        int found = 0;
        for (int k = 0; k < 256 && found < 2; k += 5) {
            const int yaw = dir == 0 ? 0x200 : 0x400;                 // east (+x) / south (+y)
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
            out.push_back({c, &init, name, x0, y0, yaw, tr});
            found++;
        }
    }
    return out;
}

static void check_walk(const WalkCase &w, int32_t ox, int32_t oy, const char *what) {
    const int32_t x0 = world_wrap(w.x0 + ox), y0 = world_wrap(w.y0 + oy);
    std::vector<Step> tr = walk(w.c, *w.init, x0, y0, w.yaw, (int)w.ref.size(), true);
    int bad = 0, jumps = 0;
    int32_t px = x0, py = y0;
    int64_t ux = x0, uy = y0;                                          // unwrapped
    for (size_t i = 0; i < tr.size(); i++) {
        const Step &a = tr[i], &b = w.ref[i];
        if (((a.x - ox) & 0xffff) != b.x || ((a.y - oy) & 0xffff) != b.y || a.z != b.z || a.yaw != b.yaw || a.health != b.health) {
            if (bad++ == 0)
                std::printf("FAIL %s %s (%x,%x) yaw %x step %zu: tiled (%x,%x,%d,%x) vs 256 (%x,%x,%d,%x)\n", w.name, what,
                            w.x0, w.y0, w.yaw, i, a.x, a.y, a.z, a.yaw, b.x, b.y, b.z, b.yaw);
        }
        const int32_t dx = world_signed(a.x - px), dy = world_signed(a.y - py);
        if (dx > 0x100 || dx < -0x100 || dy > 0x100 || dy < -0x100) jumps++;
        ux += dx; uy += dy;
        px = a.x; py = a.y;
    }
    if (bad) g_fail++;
    CHECK_EQ(jumps, 0);
    // it moved on into the next tile / across the seam (the unwrapped track ends past the start tile)
    const int64_t end = w.yaw == 0x200 ? ux : uy, start = w.yaw == 0x200 ? x0 : y0;
    const int64_t tile_start = start >> 16, tile_end = end >> 16;
    CHECK(tile_end == tile_start + 1);
    const int32_t fin = w.yaw == 0x200 ? tr.back().x : tr.back().y;
    CHECK_EQ(fin, world_wrap((int32_t)end));
    std::printf("  %-6s %-28s start (%6x,%6x) -> end (%6x,%6x), %zu steps%s\n", w.name, what, x0, y0, tr.back().x, tr.back().y,
                tr.size(), bad ? " MISMATCH" : "");
}

// ---- 4. AI ------------------------------------------------------------------------------------------------
// Fake player-list Things (castles of another owner) for the finder / site tests; not linked into cells.
struct FakeList {
    uint32_t saved_head;
    std::vector<Thing *> made;
    FakeList() : saved_head(g_cfg->player_list) { g_cfg->player_list = 0; }
    Thing *add(int32_t x, int32_t y, uint16_t owner) {
        Thing *f = thing_alloc();
        if (!f) return nullptr;
        f->cls = 3;
        f->type = 2;
        f->owner = owner;
        f->x = x;
        f->y = y;
        f->z = 0;
        f->flags &= ~4u;
        f->next = (uint16_t)g_cfg->player_list;
        g_cfg->player_list = thing_index(f);
        made.push_back(f);
        return f;
    }
    ~FakeList() {
        g_cfg->player_list = saved_head;
        for (Thing *f : made) { f->cls = 0; thing_mark_delete(f); }
    }
};

static void site_test(Thing *w, uint16_t other_owner, const char *what) {
    PlayerBlock *P = player_block(w);
    const uint16_t castle = P->castle, spell = P->spell_thing[0x10];
    const Pos home = w->home;
    Thing *s = thing_alloc();
    CHECK(s != nullptr);
    if (!s) return;
    s->cls = 12;
    s->mana_total = 0;
    s->cast_ticks = 0;
    P->castle = 0;
    P->spell_thing[0x10] = thing_index(s);
    std::set<std::pair<int, int>> quads;
    int sites = 0, odd = 0;
    const int32_t quad = g_world.units / 4;
    {
        FakeList fl;
        for (int k = 0; k < 64; k++) {
            if (!ai_find_castle_site(w)) break;
            const int32_t x = w->home.x, y = w->home.y;
            const int qx = x / quad, qy = y / quad;
            const int32_t rx = x - qx * quad, ry = y - qy * quad;
            const int32_t centre = (g_world.size / 8 - 1) << 8;
            if (!((rx == 0 && ry == 0) || (rx == centre && ry == centre))) odd++;
            quads.insert({qx, qy});
            sites++;
            fl.add(x, y, other_owner);
        }
    }
    P->castle = castle;
    P->spell_thing[0x10] = spell;
    w->home = home;
    s->cls = 0;
    thing_mark_delete(s);
    std::printf("  castle sites (%s): %d sites in %zu quadrants of %d cells\n", what, sites, quads.size(), g_world.size / 4);
    CHECK_EQ(quads.size(), 16u);
    CHECK_EQ(odd, 0);
    CHECK(sites >= 16 && sites <= 32);
}

static Thing *find_ai_wizard() {
    for (int i = 1; i < thing_pool_slots(); i++) {
        Thing *t = thing_at((unsigned)i);
        if (t->cls == 3 && t->type == 1) return t;
    }
    return nullptr;
}

// ---- 5. determinism ----------------------------------------------------------------------------------------
static std::vector<uint32_t> tiled_run(int ticks, int *moved_out) {
    std::vector<uint32_t> sums;
    if (!sim_load_level(38)) { g_fail++; return sums; }
    CHECK(world_widen_tiled(kN));
    int moved = 0;
    for (int i = 1; i < thing_pool_slots(); i++) {
        Thing *t = thing_at((unsigned)i);
        if (t->cls != 5 || t->child != 0 || t->parent != 0 || !(t->flags & 4)) continue;
        Pos p = *thing_pos(t);
        if (i % 3 == 1) { p.x = p.x + 0x10000; }                        // one tile east
        else if (i % 3 == 2) { p.x = p.x + 3 * 0x10000; p.y = p.y + 3 * 0x10000; }   // the far corner tile: next to the seam
        else continue;
        thing_move_to(t, &p);
        moved++;
    }
    *moved_out = moved;
    for (int k = 0; k < ticks; k++) {
        game_tick_sim();
        sums.push_back(net_state_checksum());
    }
    int bad = 0, beyond = 0;
    for (int i = 1; i < thing_pool_slots(); i++) {
        const Thing *t = thing_at((unsigned)i);
        if (t->cls == 0 || !(t->flags & 4)) continue;
        if (!in_own_cell_list(t)) bad++;
        if (t->x >= 0x10000 || t->y >= 0x10000) beyond++;
    }
    std::printf("  run: %d creatures moved, %d ticks, %d Things beyond the first tile at the end, checksum %08x\n", moved,
                ticks, beyond, sums.empty() ? 0u : sums.back());
    CHECK_EQ(bad, 0);
    CHECK(beyond > 0);
    return sums;
}

int main(int argc, char **argv) {
    mc_install_crash_handler();
    setvbuf(stdout, nullptr, _IONBF, 0);
    const char *game = argc > 1 ? argv[1] : MC_DEFAULT_GAME_DIR;
    if (!sim_init(game)) { std::printf("SKIP: no game data in %s\n", game); return 0; }
    sim_register_gameplay();
    g_hook_player_local_input = nullptr;
    if (!sim_load_level(38)) { std::printf("FAIL: level 38 did not load\n"); return 1; }
    CHECK_EQ(g_world.size, 256);
    for (int k = 0; k < 4; k++) game_tick_sim();       // the AI wizards are in play after the first ticks

    // 1. the 256 reference values
    std::vector<Probe> ref = probe_points();
    probe_fill(ref, 0, 0);

    // 2. the creatures and their 256 walks
    Pos p0 = make_pos(0x8080, 0x8080);
    Thing *walker = thing_create(&p0, 5, 4);        // archer
    Thing *flyer = thing_create(&p0, 5, 2);         // bee
    CHECK(walker && flyer);
    if (!walker || !flyer) return 1;
    g_walker0 = *walker;
    g_flyer0 = *flyer;
    std::vector<WalkCase> walks = find_walks(walker, g_walker0, "archer");
    std::vector<WalkCase> fw = find_walks(flyer, g_flyer0, "bee");
    walks.insert(walks.end(), fw.begin(), fw.end());
    std::printf("walk references: %zu (archer + bee, east and south across x / y = 65535)\n", walks.size());
    CHECK(walks.size() >= 4);

    Thing *wiz = find_ai_wizard();
    CHECK(wiz != nullptr);
    if (wiz) site_test(wiz, (uint16_t)(wiz->owner + 1), "256 world");

    // ---- the 1024 world
    CHECK(world_widen_tiled(kN));
    CHECK_EQ(g_world.size, kN);
    CHECK_EQ(g_world.units, kN * 256);

    // 1.
    {
        int bad_slope = 0, bad_flat = 0, n = 0;
        const int32_t offs[][2] = {{0x10000, 0}, {0, 0x20000}, {0x30000, 0x30000}, {0x20000, 0x10000}};
        for (const auto &o : offs) {
            std::vector<Probe> v = probe_points();
            probe_fill(v, o[0], o[1]);
            for (size_t i = 0; i < v.size(); i++, n++) {
                if (v[i].slope != ref[i].slope) bad_slope++;
                if (v[i].flat != ref[i].flat) bad_flat++;
            }
        }
        std::printf("terrain_slope_at: %d probes in 4 tiles, %d differ\n", n, bad_slope);
        CHECK_EQ(bad_slope, 0);
        std::printf("INFO terrain_rect_is_flat: %d of %d probes differ%s\n", bad_flat, n,
                    bad_flat ? " (terrain_rect_height_range, level_features.cpp, is still 256-only)" : "");
    }

    // 2.
    std::printf("walks in the %d world:\n", kN);
    const int32_t top = (kN - 1) * 256 >> 16 << 16;                    // 0x30000: the last tile
    for (const WalkCase &w : walks) {
        check_walk(w, 0, 0, "old boundary 65535->65536");
        if (w.yaw == 0x200) check_walk(w, top, 0x10000, "seam N*256->0 (x)");
        else check_walk(w, 0x20000, top, "seam N*256->0 (y)");
        check_walk(w, 0x10000, 0x20000, "inside tile (1,2)");
    }

    // 3. attack across the seam
    {
        const int32_t y = walks.empty() ? 0x4080 : walks[0].y0;
        Pos a = make_pos(g_world.units - 0x180, y), b = make_pos(0x180, y);
        thing_unlink_cell(walker);
        std::memcpy(reinterpret_cast<uint8_t *>(walker) + 0x18, reinterpret_cast<const uint8_t *>(&g_walker0) + 0x18, sizeof(Thing) - 0x18);
        walker->flags &= ~4u;
        thing_link_cell(walker, &a);
        thing_unlink_cell(flyer);
        thing_link_cell(flyer, &b);
        flyer->health = 1000;
        flyer->flags &= ~0x400u;
        walker->target = thing_index(flyer);
        walker->state = 0x1a;                                           // archer base + 2
        walker->yaw = 0x200;
        walker->speed_cur = 0;
        walker->speed_base = 0;
        walker->health = 1000;
        int kept = 0, aimed = 0;
        for (int k = 0; k < 64; k++) {
            walker->tick = (uint8_t)k;
            walker->damage_slots[0].attacker = 0;
            creature_attack_target(walker, 0x18, creature_attack_arrow);
            if (walker->state == 0x1a && walker->target == thing_index(flyer)) kept++;
            if ((k & 3) == 0 && walker->target_yaw == 0x200) aimed++;
        }
        std::printf("attack across the seam: target kept %d / 64 ticks, aimed east %d / 16, distance %d\n", kept, aimed,
                    pos_dist_xyz(thing_pos(walker), thing_pos(flyer)));
        CHECK_EQ(kept, 64);
        CHECK_EQ(aimed, 16);
    }

    // 4. AI wizard
    if (wiz) {
        const Pos saved = *thing_pos(wiz);
        const uint16_t saved_target = wiz->target, saved_sig = wiz->unk94;
        const uint16_t other = (uint16_t)(wiz->owner + 1);
        Pos at = make_pos(100 << 8 | 0x80, 50 << 8 | 0x80);
        thing_move_to(wiz, &at);
        {
            FakeList fl;
            Thing *tgt = fl.add(500 << 8 | 0x80, 80 << 8 | 0x80, other);             // 400 cells east, 30 south
            CHECK(tgt != nullptr);
            if (tgt) {
                wiz->target = thing_index(tgt);
                wiz->unk94 = thing_signature(tgt);
                ai_mode4_approach_target(wiz);
                const int want = math_atan2_w(400 << 8, 30 << 8);
                const int alias = math_atan2((400 << 8) - 0x20000, 30 << 8);   // (int16_t)(400 << 8): the 16-bit alias
                std::printf("AI wizard to a target 400 cells away: target_yaw %#x (true angle %#x, 256-world alias %#x)\n",
                            wiz->target_yaw, want, alias);
                CHECK_EQ(wiz->target_yaw, want);
                CHECK(want != alias);
                CHECK_EQ(pos_dist_xy(thing_pos(wiz), thing_pos(tgt)), world_isqrt((int64_t)(400 << 8) * (400 << 8) + (int64_t)(30 << 8) * (30 << 8)));
            }
        }
        {
            FakeList fl;
            Thing *a300 = fl.add((100 + 300) << 8 | 0x80, 50 << 8 | 0x80, other);
            Thing *a200 = fl.add(world_wrap(-(100 << 8) + 0x80), 50 << 8 | 0x80, other);
            Thing *nearest = player_find_nearest_by_type(wiz, 2);
            std::printf("nearest castle: %s (300 cells east vs 200 cells west)\n",
                        nearest == a200 ? "200 cells" : nearest == a300 ? "300 cells" : "?");
            CHECK(nearest == a200);
            CHECK(player_find_nearest_by_type(wiz, 0xff) == a200);
        }
        site_test(wiz, other, "1024 world");
        wiz->target = saved_target;
        wiz->unk94 = saved_sig;
        thing_move_to(wiz, &saved);
    }

    // 5. determinism
    {
        const int ticks = argc > 2 ? std::atoi(argv[2]) : 3000;
        int m1 = 0, m2 = 0;
        std::vector<uint32_t> a = tiled_run(ticks, &m1);
        std::vector<uint32_t> b = tiled_run(ticks, &m2);
        CHECK_EQ(m1, m2);
        CHECK(a.size() == b.size());
        size_t first = a.size();
        for (size_t i = 0; i < a.size() && i < b.size(); i++) if (a[i] != b[i]) { first = i; break; }
        if (first != a.size()) { std::printf("FAIL: checksums differ from tick %zu\n", first + 1); g_fail++; }
        else std::printf("determinism: %zu ticks, identical checksum sequences\n", a.size());
    }

    std::printf(g_fail ? "world_creatures_test: %d failures\n" : "world_creatures_test: OK\n", g_fail);
    return g_fail ? 1 : 0;
}
