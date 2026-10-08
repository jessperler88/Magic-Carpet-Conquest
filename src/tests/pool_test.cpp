#define _CRT_SECURE_NO_WARNINGS
// Thing pool without the 1000 limit (port round 7, task C; docs/analysis/port_pool.md).
//  1. Allocation beyond 1000 (PortSettings::thing_slots = 4000): order 1, 2, .. 3999, null when full, LIFO
//     reuse across the GameState / extension boundary, recycling of flagged Things in the extension.
//  2. The original's order: one pseudo-random alloc / free / recycle script on a 1000- and a 4000-slot pool -
//     identical indices and an identical GameState (free / recyclable stacks included) as long as the
//     1000-slot pool has room; a 1000-slot image loaded into a 4000-slot pool (thing_pool_ext_reset).
//  3. Generation (game data): levels 39 (its terrain effects run out of the 1000 slots 680 times) and 49
//     generated with 1000 and 4000 slots: identical maps, cell lists and GameState; the 4000 slots are in
//     place once the level has started (thing_pool_reset keeps 1000 for the generation).
//  4. Flood: level 49, whose 1000 slots are full from tick 2 on (port_reference2.md). With 1000 slots
//     allocations fail and a killed creature leaves no mana ball (thing_drop_mana_ball_25fe0); with 4000
//     slots nothing fails and the ball appears. The same 4000-slot run twice: identical per-tick checksums
//     (net_state_checksum covers the extension). Winning: level 2 played by the computer for player 0 with
//     the map crowded by standing stones until 1000 slots are full - not won in 10000 ticks with 1000 slots,
//     won (tick 7691) with 4000.
//  5. Movie 0 (mvi00000.dat, 8550 ticks) with 1000 and 4000 slots: the GameState is identical after every
//     tick up to the tick in which the 1000-slot pool first has no free slot (1069 of the playback =
//     reference tick 1481), and only from there on differs.
// Measurement mode (not part of ctest): pool_test <game dir> peak <ticks> <slots> [level ...] runs each
// level with idle local input (the AI wizards play) and prints the peak number of live Things, the tick of
// the peak, the allocations that failed and the time per tick; pool_test <game dir> win <ticks> <level>
// [stones] runs the winning scenario of 3. on any level (stones -1 = until the 1000-slot pool is full).
#include "sim.h"
#include "thing.h"
#include "player.h"
#include "settings.h"
#include "net.h"
#include "effects.h"
#include "projectiles.h"
#include "demo.h"
#include "mc_globals.h"
#include "crash_handler.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)


static void use_slots(int n) {
    g_settings.thing_slots = n;
    thing_pool_force_slots(0);
}
static void clear_pool() {
    std::memset(g_state->things, 0, sizeof g_state->things);
    std::memset(g_state->free_list, 0, sizeof g_state->free_list);         // stale entries above the tops
    std::memset(g_state->active_list, 0, sizeof g_state->active_list);
    std::memset(g_cell_things, 0, sizeof(uint16_t) * MC_MAP_CELLS);
    thing_pool_reset();             // the original's 1000 slots (level generation) ..
    CHECK(thing_pool_slots() == MC_THING_SLOTS);
    thing_pool_ext_reset();         // .. and the wanted size, as switch_activate(0) / a snapshot load set it
}

// ---- 1. allocation beyond 1000 ------------------------------------------------------------------------
static void test_beyond() {
    use_slots(4000);
    clear_pool();
    CHECK(thing_pool_slots() == 4000 && thing_pool_ext_count() == 3000);
    CHECK(thing_free_count() == 3999);
    CHECK(g_state->free_top == 998);               // GameState part = the original's stack, bottom in the extension
    bool order = true;
    for (int i = 1; i < 4000; i++) {
        Thing *t = thing_alloc();
        if (!t || thing_index(t) != i || thing_at((unsigned)i) != t) { order = false; break; }
        t->cls = 2;
        if (!thing_in_pool(t)) order = false;
    }
    CHECK(order);
    const uint32_t f0 = g_thing_alloc_failures;
    CHECK(thing_alloc() == nullptr);
    CHECK(g_thing_alloc_failures == f0 + 1);
    CHECK(thing_free_count() == 0 && thing_pool_live_count() == 3999);
    CHECK(!thing_in_pool(thing_at(0)) && thing_in_pool(thing_at(999)) && thing_in_pool(thing_at(1000)) &&
          thing_in_pool(thing_at(3999)));
    // LIFO across the boundary
    thing_free(thing_at(2500)); thing_free(thing_at(7)); thing_free(thing_at(1000));
    CHECK(thing_free_count() == 3);
    CHECK(thing_alloc() == thing_at(1000) && thing_alloc() == thing_at(7) && thing_alloc() == thing_at(2500));
    thing_at(1000)->cls = thing_at(7)->cls = thing_at(2500)->cls = 2;
    // cell lists through the extension
    Pos p{0x4080, 0x2080, 0};
    thing_link_cell(thing_at(3999), &p); thing_link_cell(thing_at(5), &p); thing_link_cell(thing_at(1500), &p);
    const uint16_t cell = mc_cell_of(p.x, p.y);
    CHECK(g_cell_things[cell] == 1500 && thing_at(1500)->cell_next == 5 && thing_at(5)->cell_next == 3999);
    thing_unlink_cell(thing_at(5));
    CHECK(thing_at(1500)->cell_next == 3999 && thing_at(3999)->cell_prev == 1500);
    // recycling: flagged Things on the recyclable stack, positions >= 1000 in the extension
    for (int i = 1; i < 4000; i += 3) thing_at((unsigned)i)->flags |= 0x20000;    // 1333 recyclable
    models_initialise();
    CHECK(thing_free_count() == 0 && g_state->active_top == 1332);
    bool rec = true;
    for (int k = 0; k < 1333; k++) {
        Thing *t = thing_alloc();
        if (!t || thing_index(t) != 1 + 3 * k) { rec = false; break; }
        t->cls = 2;
    }
    CHECK(rec);
    CHECK(g_state->active_top == -1 && thing_alloc() == nullptr);
    // the faithful pool again
    use_slots(1000);
    clear_pool();
    CHECK(thing_pool_slots() == 1000 && thing_pool_ext_count() == 0 && thing_free_count() == 999 && g_state->free_top == 998);
    // the setting is clamped
    use_slots(500);   clear_pool(); CHECK(thing_pool_slots() == 1000);
    use_slots(70000); clear_pool(); CHECK(thing_pool_slots() == MC_THING_SLOTS_MAX);
    thing_pool_force_slots(2000); clear_pool(); CHECK(thing_pool_slots() == 2000);
    use_slots(1000); clear_pool();
}

// ---- 2. the original's order --------------------------------------------------------------------------
struct Step { int idx; std::vector<uint8_t> stacks; };
static std::vector<uint8_t> stack_bytes() {
    const uint8_t *s = reinterpret_cast<const uint8_t *>(g_state);
    std::vector<uint8_t> v(s + offsetof(GameState, free_top), s + offsetof(GameState, free_top) + 4);
    v.insert(v.end(), s + offsetof(GameState, free_list), s + offsetof(GameState, opt_second_surface));
    return v;
}
// A deterministic script: allocate (biased), free a random live Thing, now and then mark Things recyclable
// and rebuild the stacks (models_initialise, as player_spawn / castle_release_excess_mana do).
static std::vector<Step> run_script(int slots, int steps, bool *full_seen) {
    use_slots(slots);
    clear_pool();
    std::vector<Step> out;
    std::vector<int> live;
    uint32_t r = 12345;
    *full_seen = false;
    for (int s = 0; s < steps; s++) {
        r = r * 1103515245u + 12345u;
        const uint32_t op = (r >> 16) % 100;
        int idx = -1;
        if (op < 62 || live.empty()) {
            // stop where the 1000-slot pool has no free slot left (it would recycle or fail from here)
            if (thing_free_count() == 0) { *full_seen = true; break; }
            Thing *t = thing_alloc();
            t->cls = 2; idx = thing_index(t); live.push_back(idx);
        } else if (op < 98) {
            const size_t k = (r >> 4) % live.size();
            thing_free(thing_at((unsigned)live[k]));
            idx = -live[k];
            live[k] = live.back(); live.pop_back();
        } else {
            for (size_t k = 0; k < live.size(); k += 7) thing_at((unsigned)live[k])->flags |= 0x20000;
            models_initialise();
            idx = 0;
        }
        out.push_back({idx, stack_bytes()});
    }
    return out;
}
static void test_order() {
    bool full1 = false, full4 = false;
    std::vector<Step> a = run_script(1000, 50000, &full1);
    CHECK(full1);                                   // the script runs the 1000-slot pool out of free slots
    std::vector<Step> b = run_script(4000, (int)a.size(), &full4);
    CHECK(!full4 && b.size() == a.size());
    size_t same = 0;
    for (size_t i = 0; i < a.size() && i < b.size(); i++) {
        if (a[i].idx != b[i].idx || a[i].stacks != b[i].stacks) {
            size_t k = 0;
            while (k < a[i].stacks.size() && a[i].stacks[k] == b[i].stacks[k]) k++;
            std::printf("order: step %zu differs: Thing %d / %d, stack byte %zu\n", i, a[i].idx, b[i].idx, k);
            break;
        }
        same++;
    }
    std::printf("order: %zu script steps (alloc / free / models_initialise) identical with 1000 and 4000 slots, "
                "up to the 1000-slot pool's last free slot\n", same);
    CHECK(same == a.size() && same > 1000);
    // the next allocation: the extended pool goes on with slot 1000
    Thing *t = thing_alloc();
    CHECK(t && thing_index(t) == 1000);
    use_slots(1000); clear_pool();
}

// A 1000-slot image (as a snapshot / save stores it) in a 4000-slot pool.
static void test_image() {
    use_slots(1000);
    clear_pool();
    for (int i = 0; i < 600; i++) thing_alloc()->cls = 2;
    for (int i = 10; i < 300; i += 4) thing_free(thing_at((unsigned)i));
    std::vector<uint8_t> image(sizeof(GameState));
    std::memcpy(image.data(), g_state, sizeof(GameState));
    std::vector<int> expect;
    while (Thing *t = thing_alloc()) { t->cls = 2; expect.push_back(thing_index(t)); }
    // load the image into an extended pool
    use_slots(4000);
    clear_pool();
    for (int i = 1000; i < 1200; i++) thing_at((unsigned)i)->cls = 5;     // left over from the previous level
    std::memcpy(g_state, image.data(), sizeof(GameState));
    thing_pool_ext_reset();
    CHECK(thing_pool_slots() == 4000 && thing_at(1100)->cls == 0);
    CHECK(thing_free_count() == (int)expect.size() + 3000);
    bool same = true;
    for (int e : expect) { Thing *t = thing_alloc(); if (!t || thing_index(t) != e) { same = false; break; } t->cls = 2; }
    CHECK(same);
    Thing *n = thing_alloc();
    CHECK(n && thing_index(n) == 1000);
    n = thing_alloc();
    CHECK(n && thing_index(n) == 1001);
    use_slots(1000); clear_pool();
}

// ---- 3. a level that fills the pool ---------------------------------------------------------------------
struct LevelRun { int peak = 0; int peak_tick = 0; uint32_t failures = 0; uint32_t hash = 0; int won_tick = 0; int won_player = -1; double us_per_tick = 0; uint32_t load_failures = 0; };

static bool load_level(int level, int slots) {
    use_slots(slots);
    g_cfg->flags = 0; g_cfg->paused = 0;
    return sim_load_level(level);
}
static LevelRun run_level(int level, int slots, int ticks, bool hash) {
    LevelRun r;
    const uint32_t fl = g_thing_alloc_failures;
    if (!load_level(level, slots)) { std::printf("level %d did not load\n", level); g_fail++; return r; }
    const uint32_t f0 = g_thing_alloc_failures;
    r.load_failures = f0 - fl;
    uint32_t h = 2166136261u;
    const auto t0 = std::chrono::steady_clock::now();
    for (int t = 1; t <= ticks; t++) {
        game_tick_sim();
        const int live = thing_pool_slots() - 1 - thing_free_count();     // slots in use (O(1))
        if (live > r.peak) { r.peak = live; r.peak_tick = t; }
        if (hash) { h ^= net_state_checksum(); h *= 16777619u; }
        for (int p = 0; p < 8 && !r.won_tick; p++)          // game_check_level_won_3db20 sets status bit 2
            if (g_state->players[p].active && (g_state->players[p].status & 2)) { r.won_tick = t; r.won_player = p; }
    }
    r.us_per_tick = (double)std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - t0).count() / (ticks > 0 ? ticks : 1);
    r.failures = g_thing_alloc_failures - f0;
    r.hash = h;
    return r;
}

static int count_mana_balls();
static bool g_trace = false;

// Winning a crowded level. The local player is handed to the computer (players_init_records again with
// local_player pointing past the players, so every record is a computer wizard, as the AI wizards of the
// campaign are), then `flood` standing stones (scenery_create_standing_stone_35f90: inert, they stay) are
// spread over the map before the first tick - a stand-in for a crowded late level. The run ends when
// game_check_level_won_3db20 sets player 0's win bit (its castle held more than the level's percentage of the
// world's mana for 16 ticks).
struct WinRun { int stones = 0; int won = 0; uint32_t failures = 0; int peak = 0; };
static WinRun win_scenario(int level, int slots, int flood, int ticks) {
    WinRun r;
    if (!load_level(level, slots)) { g_fail++; return r; }
    const int16_t local = g_state->local_player;
    g_state->local_player = 7;
    players_init_records();
    g_state->local_player = local;
    for (int k = 0; flood < 0 || k < flood; k++) {           // flood < 0: until the pool is full
        Pos p{(uint16_t)(((k % 64) * 4 + 2) << 8 | 0x80), (uint16_t)(((k / 64) * 4 + 2) << 8 | 0x80), 0};
        p.z = (int16_t)terrain_height_at(&p);
        if (!thing_create(&p, 2, 1)) break;
        r.stones++;
    }
    const uint32_t f0 = g_thing_alloc_failures;
    for (int t = 1; t <= ticks; t++) {
        game_tick_sim();
        const int live = thing_pool_slots() - 1 - thing_free_count();
        if (live > r.peak) r.peak = live;
        if (g_state->players[0].status & 2) { r.won = t; break; }
        if (g_trace && t % 1000 == 0) {
            const PlayerBlock *P = player_block(thing_at(g_state->players[0].thing));
            const Thing *c = thing_at(P->castle);
            std::printf("  tick %5d: castle %d mana %d, in transit %d, world %u, win %u%%, free %d\n", t, P->castle,
                        P->castle ? c->mana : 0, P->mana_in_transit, g_cfg->total_mana,
                        (unsigned)g_state->level.win_percent, thing_free_count());
        }
    }
    r.failures = g_thing_alloc_failures - f0;
    return r;
}

// The first live creature (class 5) with mana, or null.
static Thing *find_creature() {
    for (int i = 1; i < thing_pool_slots(); i++) {
        Thing *t = thing_at((unsigned)i);
        if (t->cls == 5 && t->health >= 0 && t->mana > 0) return t;
    }
    return nullptr;
}
static int count_mana_balls() {
    int n = 0;
    for (int i = 1; i < thing_pool_slots(); i++) {
        const Thing *t = thing_at((unsigned)i);
        n += t->cls == 10 && t->type == 0x27;
    }
    return n;
}

// Kill one creature and drop its mana the way creature_die does (thing_drop_mana_ball_25fe0): a ball appears
// only when the pool has room.
static bool kill_creature_drops_ball() {
    Thing *c = find_creature();
    if (!c) return false;
    const int before = count_mana_balls();
    thing_drop_mana_ball(c);
    thing_mark_delete(c);
    return count_mana_balls() == before + 1;
}

// Level generation keeps the original's 1000 slots (level 39's terrain effects run out of them): the
// generated level - the four maps, the cell lists and the GameState - is the same with 1000 and 4000 slots,
// and the 4000-slot pool is in place once the level has started.
static uint32_t fnv_bytes(const void *p, size_t n);
static uint32_t level_hash() {
    uint32_t h = fnv_bytes(g_state, sizeof(GameState));
    h ^= fnv_bytes(g_map_type, g_world.cells) * 3u;
    h ^= fnv_bytes(g_map_height, g_world.cells) * 5u;
    h ^= fnv_bytes(g_map_light, g_world.cells) * 7u;
    h ^= fnv_bytes(g_map_flags, g_world.cells) * 11u;
    h ^= fnv_bytes(g_cell_things, sizeof(uint16_t) * MC_MAP_CELLS) * 13u;
    return h;
}
static void test_generation() {
    for (int level : {39, 49}) {
        load_level(level, 1000);    // the same history for both (the player-record scratch at +0x2c0a)
        const uint32_t f0 = g_thing_alloc_failures;
        load_level(level, 1000);
        const uint32_t gen_fail = g_thing_alloc_failures - f0;
        const uint32_t h1 = level_hash();
        if (g_trace) if (std::FILE *f = std::fopen("gen_1000.gam", "wb")) { std::fwrite(g_state, 1, sizeof(GameState), f); std::fclose(f); }
        load_level(level, 4000);
        const uint32_t h4 = level_hash();
        if (g_trace) if (std::FILE *f = std::fopen("gen_4000.gam", "wb")) { std::fwrite(g_state, 1, sizeof(GameState), f); std::fclose(f); }
        std::printf("level %d generated with 1000 / 4000 slots: %u allocations failed during the generation, %s, pool %d\n",
                    level, gen_fail, h1 == h4 ? "identical" : "DIFFERENT", thing_pool_slots());
        CHECK(h1 == h4 && thing_pool_slots() == 4000);
        if (level == 39) CHECK(gen_fail > 0);
    }
    use_slots(1000);
}

static void test_flood(int ticks) {
    // Faithful: level 49 fills its 1000 slots at once.
    LevelRun f = run_level(49, 1000, ticks, false);
    std::printf("level 49, 1000 slots: peak %d live at tick %d, %u allocations failed in %d ticks\n",
                f.peak, f.peak_tick, f.failures, ticks);
    CHECK(f.peak == 999);
    CHECK(f.failures > 0);
    // run on to a tick that ends with the pool full, then kill a creature: its mana is lost
    for (int t = 0; t < 5000 && thing_free_count() != 0; t++) game_tick_sim();
    CHECK(thing_free_count() == 0);
    CHECK(!kill_creature_drops_ball());
    // Extended: the same level, room for everything.
    LevelRun e = run_level(49, 4000, ticks, true);
    std::printf("level 49, 4000 slots: peak %d live at tick %d, %u allocations failed\n", e.peak, e.peak_tick, e.failures);
    CHECK(e.peak > 999);
    CHECK(e.failures == 0);
    CHECK(kill_creature_drops_ball());
    // Determinism: the same 4000-slot run again.
    LevelRun e2 = run_level(49, 4000, ticks, true);
    std::printf("level 49, 4000 slots, second run: checksum %08x / %08x\n", e.hash, e2.hash);
    CHECK(e.hash == e2.hash && e.peak == e2.peak && e.peak_tick == e2.peak_tick);
    // Winning: level 2 played by the computer for player 0, the map crowded with standing stones until the
    // 1000-slot pool is full. With 1000 slots the level is not won; with 4000 (the same stones) it is.
    const int win_ticks = 10000;
    const WinRun w1 = win_scenario(2, 1000, -1, win_ticks);
    const WinRun w4 = win_scenario(2, 4000, w1.stones, win_ticks);
    std::printf("level 2 + %d stones: 1000 slots: %u allocations failed, won at tick %d; 4000 slots: peak %d in use, "
                "%u failed, won at tick %d\n", w1.stones, w1.failures, w1.won, w4.peak, w4.failures, w4.won);
    CHECK(w1.stones > 600 && w1.failures > 0 && w1.won == 0);
    CHECK(w4.stones == w1.stones && w4.failures == 0 && w4.peak > 999 && w4.won > 0);
    use_slots(1000);
}

// ---- 5. movie 0 with both pool sizes ---------------------------------------------------------------------
// The shipped recording (movie/mvi00000.dat, level 38, ~450 Things) played with 1000 and with 4000 slots
// (forced over the movie's own size after demo_open): the GameState must be identical after every tick -
// the extension is never needed, so nothing may differ. Returns the first differing tick (0 = none);
// `dump_tick` > 0 writes the state after that tick to pool_movie_<slots>.gam (debugging).
static uint32_t fnv_bytes(const void *p, size_t n) {
    uint32_t h = 2166136261u;
    const uint8_t *b = static_cast<const uint8_t *>(p);
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; }
    return h;
}
static std::vector<uint32_t> play_movie0(const char *game_dir, int slots, int dump_tick, int *first_full) {
    std::vector<uint32_t> hashes;
    *first_full = 0;
    g_cfg->flags = 0x100; g_cfg->paused = 0;
    sim_prepare_movie();
    thing_pool_force_slots(slots);
    sim_load_level(38);
    if (!demo_open(game_dir, 0)) return hashes;
    thing_pool_force_slots(slots);
    g_projectile_null_hit_index = 0xfcd9;           // as in the reference run (reference_test)
    for (;;) {
        const uint32_t f0 = g_thing_alloc_failures;
        const bool more = demo_step();
        if (!more) break;
        hashes.push_back(fnv_bytes(g_state, sizeof(GameState)));
        // the first tick in which the pool had no slot for an allocation (or ended with none free)
        if (!*first_full && (g_thing_alloc_failures != f0 || thing_free_count() == 0)) *first_full = (int)hashes.size();
        if ((int)hashes.size() == dump_tick) {
            char name[64];
            std::snprintf(name, sizeof name, "pool_movie_%d.gam", slots);
            if (std::FILE *f = std::fopen(name, "wb")) { std::fwrite(g_state, 1, sizeof(GameState), f); std::fclose(f); }
        }
    }
    demo_close();
    thing_pool_force_slots(0);
    g_projectile_null_hit_index = 0;
    return hashes;
}
static int test_movie(const char *game_dir, int dump_tick) {
    int full1 = 0, full4 = 0;
    const std::vector<uint32_t> a = play_movie0(game_dir, 1000, dump_tick, &full1);
    const std::vector<uint32_t> b = play_movie0(game_dir, 4000, dump_tick, &full4);
    int first = 0;
    for (size_t i = 0; i < a.size() && i < b.size(); i++) if (a[i] != b[i]) { first = (int)i + 1; break; }
    std::printf("movie 0: %zu / %zu ticks with 1000 / 4000 slots; the 1000-slot pool is first full in tick %d (4000: %d); "
                "first differing GameState after tick %d\n", a.size(), b.size(), full1, full4, first);
    CHECK(a.size() > 8000 && a.size() == b.size());
    CHECK(full4 == 0);
    // identical up to the tick in which the original's pool ran out
    CHECK(full1 > 0 ? first == full1 : first == 0);
    return first != full1;
}

// ---- measurement mode ---------------------------------------------------------------------------------
static int peak_mode(int argc, char **argv) {
    const int ticks = argc > 3 ? std::atoi(argv[3]) : 30000;
    const int slots = argc > 4 ? std::atoi(argv[4]) : 1000;
    std::vector<int> levels;
    for (int i = 5; i < argc; i++) levels.push_back(std::atoi(argv[i]));
    if (levels.empty()) for (int l = 40; l <= 49; l++) levels.push_back(l);
    for (int lv : levels) {
        LevelRun r = run_level(lv, slots, ticks, false);
        std::printf("level %2d, %5d slots, %d ticks: peak %5d in use at tick %5d, final %5d live, %u + %u allocations "
                    "failed (load + ticks), won by player %d (tick %d), %.1f us/tick\n",
                    lv, thing_pool_slots(), ticks, r.peak, r.peak_tick, thing_pool_live_count(), r.load_failures,
                    r.failures, r.won_player, r.won_tick, r.us_per_tick);
        if (g_trace) {          // MC_POOL_TRACE: the most common class / type at the end
            static int n[14][256];
            std::memset(n, 0, sizeof n);
            for (int i = 1; i < thing_pool_slots(); i++) {
                const Thing *t = thing_at((unsigned)i);
                if (t->cls != 0 && t->cls < 14) n[t->cls][t->type]++;
            }
            for (int k = 0; k < 4; k++) {
                int bc = 0, bt = 0;
                for (int c = 1; c < 14; c++) for (int ty = 0; ty < 256; ty++) if (n[c][ty] > n[bc][bt]) { bc = c; bt = ty; }
                if (!n[bc][bt]) break;
                std::printf("    class %d type %d: %d\n", bc, bt, n[bc][bt]);
                n[bc][bt] = 0;
            }
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    mc_install_crash_handler();
    setvbuf(stdout, nullptr, _IONBF, 0);
    const char *game_dir = argc > 1 ? argv[1] : MC_DEFAULT_GAME_DIR;
    const bool have_game = sim_init(game_dir);
    if (have_game) sim_register_gameplay();
    else mc_globals_init();
#pragma warning(suppress : 4996)
    g_trace = std::getenv("MC_POOL_TRACE") != nullptr;
    g_hook_player_local_input = nullptr;            // idle local player: the AI wizards play
    if (argc > 2 && std::strcmp(argv[2], "peak") == 0) return have_game ? peak_mode(argc, argv) : 2;
    if (argc > 2 && std::strcmp(argv[2], "movie") == 0 && have_game) return test_movie(game_dir, argc > 3 ? std::atoi(argv[3]) : 0);
    if (argc > 2 && std::strcmp(argv[2], "win") == 0 && have_game) {
        const int ticks = argc > 3 ? std::atoi(argv[3]) : 20000;
        const int level = argc > 4 ? std::atoi(argv[4]) : 0;
        int flood = argc > 5 ? std::atoi(argv[5]) : 0;
        for (int slots : {1000, 4000}) {
            const WinRun r = win_scenario(level, slots, flood, ticks);
            flood = r.stones;                                   // the same flood for the 4000-slot run
            std::printf("level %d (win %u%%), %d slots, %d stones: peak %d in use, %u allocations failed, player 0 "
                        "won at tick %d (world mana %u)\n", level, (unsigned)g_state->level.win_percent, slots,
                        r.stones, r.peak, r.failures, r.won, g_cfg->total_mana);
        }
        return 0;
    }

    test_beyond();
    test_order();
    test_image();
    {   // no network session: the agreement is the local value
        bool differs = true;
        NetGameRules mine{};
        mine.thing_slots = 4000; mine.possession_range_pct = 130;
        const NetGameRules r = net_agree_rules(mine, &differs);
        CHECK(r.thing_slots == 4000 && r.possession_range_pct == 130 && !differs);
    }
    if (have_game) {
        test_generation();
        test_flood(argc > 2 ? std::atoi(argv[2]) : 200);
        test_movie(game_dir, 0);
    }
    else std::printf("SKIP: flood test (no game data in %s)\n", game_dir);
    if (g_fail) { std::printf("pool_test: %d failures\n", g_fail); return 1; }
    std::printf("pool_test: all passed\n");
    return 0;
}
