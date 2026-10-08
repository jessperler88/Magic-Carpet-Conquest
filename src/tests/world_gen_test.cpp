// Round 11 session 2 (B4 + B5): the generated world of a mode run (worldgen.h).
//
//  1. rts runs at 1024 (4 players, rot4), 512 (2, point), 256 with MODE_PARAM_GENERATE (2, mirror) and 2048
//     (8, dihedral): the world has the size, every player has a start record and spawns on its seat's plateau,
//     the seats' plateaus have one height, the records fit the 1999 budget; generation time per size;
//  2. symmetry: the share of cells whose height equals the height at every image under the group (the original's
//     passes are not symmetric - in-place sweeps, scan orders, the texture RNG - so this is a measure, > 50 %);
//  3. portals: linked pairs, both gates exist as effect 0x22 Things with their destination next to the other end;
//  4. trigger areas: a switch Thing per trigger whose payload records (its DisId) wait in level.things; a computer
//     wizard flown into one fires it (the mode rule: bots fire switches) and the payload is spawned;
//  5. determinism: the same params build identical maps and records, and 600 ticks of the 1024 world run twice
//     give identical net_state_checksum sequences;
//  6. world_gen_<size>.png in the scratch dir (heights, water, features) to look at;
//  7. scenery / terrain art (check_scenery): stone circles, spell urns, palm / oak woods, peaks, snow.
#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "sim.h"
#include "mode.h"
#include "mode_level.h"
#include "worldgen.h"
#include "thing.h"
#include "player.h"
#include "net.h"
#include "world.h"
#include "mc_globals.h"
#include "../mcport/screenshot.h"
#include "crash_handler.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static std::string g_game;
static fs::path g_out;

static Thing *wizard(int p) { return thing_at(thing_wrap(g_state->players[p & 7].thing)); }

static uint32_t fnv(const void *p, size_t n, uint32_t h = 2166136261u) {
    const uint8_t *b = static_cast<const uint8_t *>(p);
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; }
    return h;
}
static uint32_t world_hash() {
    uint32_t h = fnv(g_map_height, g_world.cells);
    h = fnv(g_map_type, g_world.cells, h);
    h = fnv(g_map_flags, g_world.cells, h);
    h = fnv(g_map_light, g_world.cells, h);
    return h;
}

static bool start(const ModeParams &mp, double *ms = nullptr) {
    g_cfg->flags = 0;
    g_cfg->paused = 0;
    if (!mode_start_run(g_game.c_str(), GAME_MODE_CONQUEST, mp)) return false;
    const auto t0 = std::chrono::steady_clock::now();
    const bool ok = sim_load_level(MODE_LEVEL_INDEX);
    if (ms) *ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    for (int i = 0; ok && i < 8 && g_state->players[0].thing == 0; i++) game_tick_sim();   // the players spawn
    if (std::getenv("WG_DEBUG"))
        for (int p = 0; p < 8; p++)
            std::printf("  dbg p%d thing %u active %d comp %d count %d start %d,%d\n", p, (unsigned)g_state->players[p].thing,
                        g_state->players[p].active, g_state->players[p].is_computer, g_state->player_count,
                        (int)world_start_pos(p).x >> 8, (int)world_start_pos(p).y >> 8);
    return ok;
}

static void write_png(const char *name) {
    const int n = g_world.size, scale = n > 1024 ? n / 1024 : 1, w = n / scale;
    std::vector<uint8_t> rgb((size_t)w * w * 3);
    for (int y = 0; y < w; y++)
        for (int x = 0; x < w; x++) {
            const uint32_t c = world_cell(x * scale, y * scale);
            const int h = g_map_height[c], cls = g_map_flags[c] & 7;
            uint8_t *p = &rgb[((size_t)y * w + x) * 3];
            if (cls == 0) { p[0] = 20; p[1] = (uint8_t)(60 + h / 2); p[2] = (uint8_t)(140 + h / 3); }
            else {
                const int l = 50 + h, sh = (int)g_map_light[c] - 0x20;
                int v = l + sh * 6;
                v = v < 0 ? 0 : (v > 255 ? 255 : v);
                p[0] = (uint8_t)(v * (cls == 6 || cls == 1 ? 9 : 7) / 10); p[1] = (uint8_t)v; p[2] = (uint8_t)(v * 6 / 10);
            }
        }
    auto dot = [&](int cx, int cy, uint8_t r, uint8_t g, uint8_t b, int rad) {
        for (int dy = -rad; dy <= rad; dy++)
            for (int dx = -rad; dx <= rad; dx++) {
                const int x = ((cx / scale + dx) % w + w) % w, y = ((cy / scale + dy) % w + w) % w;
                uint8_t *p = &rgb[((size_t)y * w + x) * 3];
                p[0] = r; p[1] = g; p[2] = b;
            }
    };
    for (const WorldFeature &f : worldgen_last_layout().features) {
        switch (f.kind) {
        case WFEAT_SPAWN:   dot(f.x, f.y, 255, 255, 255, 6); break;
        case WFEAT_SPRING:  dot(f.x, f.y, 40, 220, 255, 3); break;
        case WFEAT_LAIR:    dot(f.x, f.y, 255, 40 + 40 * (5 - f.grade), 0, 3); break;
        case WFEAT_TRIGGER: dot(f.x, f.y, 255, 0, 255, 2); break;
        case WFEAT_PORTAL:  dot(f.x, f.y, 255, 255, 0, 3); break;
        case WFEAT_TOWN:    dot(f.x, f.y, 160, 80, 20, 4); break;
        case WFEAT_TREES:   dot(f.x, f.y, 0, 90, 0, 1); break;
        default: break;
        }
    }
    const std::string path = (g_out / name).string();
    CHECK(shot_write_png(path.c_str(), w, w, rgb.data()));
    std::printf("  wrote %s\n", path.c_str());
}

// share of cells (sampled) whose height equals the height of all its images
static int symmetry_pct(int sym) {
    if (sym == WORLD_SYM_NONE) return 100;
    const int n = g_world.size;
    int same = 0, total = 0;
    for (int y = 0; y < n; y += 3)
        for (int x = 0; x < n; x += 3) {
            const uint8_t h = g_map_height[world_cell(x, y)];
            int imgs[8][2], k = 0;
            imgs[k][0] = -x; imgs[k][1] = -y; k++;
            if (sym == WORLD_SYM_MIRROR) { k = 0; imgs[k][0] = -x; imgs[k][1] = y; k++; }
            if (sym == WORLD_SYM_ROT4 || sym == WORLD_SYM_DIHEDRAL) { imgs[k][0] = -y; imgs[k][1] = x; k++; imgs[k][0] = y; imgs[k][1] = -x; k++; }
            if (sym == WORLD_SYM_DIHEDRAL) { imgs[k][0] = y; imgs[k][1] = x; k++; imgs[k][0] = -x; imgs[k][1] = y; k++; }
            bool ok = true;
            for (int i = 0; i < k; i++) ok = ok && g_map_height[world_cell(imgs[i][0], imgs[i][1])] == h;
            same += ok;
            total++;
        }
    return same * 100 / total;
}

static void check_world(const ModeParams &mp, const char *png) {
    double ms = 0;
    CHECK(start(mp, &ms));
    const WorldLayout &L = worldgen_last_layout();
    const WorldGenSettings s = worldgen_settings(mp);
    CHECK(g_world.size == (int)mp.world_size);
    CHECK(L.size == (int)mp.world_size && L.symmetry == s.symmetry);
    CHECK(L.seats >= s.players);
    CHECK(L.records > 0 && L.records <= 1999);
    int count[16] = {};
    for (const WorldFeature &f : L.features) count[f.kind & 15]++;
    int trees = 0, stones = 0, wild = 0;
    for (int k = 1; k < thing_pool_slots(); k++) {
        const Thing *t = thing_at((unsigned)k);
        if (t->cls == 2) (t->type == 0 ? trees : stones)++;
        if (t->cls == 5) wild++;
    }
    std::printf("size %d players %d sym %s: %.0f ms, %d seats, %d records (%d dropped) + %d extra; springs %d lairs %d "
                "triggers %d portal ends %d towns %d groves %d; pool %d, live %d (trees %d, stones %d, creatures %d); "
                "symmetric heights %d %%\n",
                g_world.size, s.players, worldgen_symmetry_name(L.symmetry), ms, L.seats, L.records, L.records_dropped,
                L.extra_records, count[WFEAT_SPRING], count[WFEAT_LAIR], count[WFEAT_TRIGGER], count[WFEAT_PORTAL],
                count[WFEAT_TOWN], count[WFEAT_TREES], thing_pool_slots(), thing_pool_live_count(), trees, stones, wild,
                symmetry_pct(L.symmetry));
    CHECK(thing_pool_slots() >= mode_generated_world_slots(mp.world_size));
    CHECK(level_extra_records_count() == 0);                    // spawned at the level start
    CHECK(trees >= L.extra_records / 3);
    if (s.symmetry != WORLD_SYM_NONE) CHECK(symmetry_pct(L.symmetry) > 50);
    // seats: one plateau height, every player on its seat
    int plateau = -1;
    for (const WorldFeature &f : L.features) {
        if (f.kind != WFEAT_SPAWN) continue;
        // away from the centre: a computer wizard's start castle (castle_level > 0) stamps its footprint there
        const int h = g_map_height[world_cell(f.x - 9, f.y - 9)];
        if (plateau < 0) plateau = h;
        if (h != plateau || h < 0x28) {
            std::printf("  seat %d at %d,%d: height %d (first seat %d), class %d; row:", f.grade, f.x, f.y, h, plateau,
                        g_map_flags[world_cell(f.x, f.y)] & 7);
            for (int d = -14; d <= 14; d += 2) std::printf(" %d", g_map_height[world_cell(f.x + d, f.y)]);
            std::printf("\n");
        }
        CHECK(h == plateau && h >= 0x28);
        CHECK((g_map_flags[world_cell(f.x, f.y)] & 7) != 0);
        if (f.grade < s.players) {
            const Thing *w = wizard(f.grade);
            const int dx = std::abs(world_signed(w->x - (f.x * 256 + 0x80))), dy = std::abs(world_signed(w->y - (f.y * 256 + 0x80)));
            if (dx > 12 * 256 || dy > 12 * 256)
                std::printf("  player %d at cell %d,%d, seat %d,%d\n", f.grade, (int)w->x >> 8, (int)w->y >> 8, f.x, f.y);
            CHECK(dx <= 12 * 256 && dy <= 12 * 256);
        }
    }
    // towns: built during generation (effect 0x2d, the original's villager castle), alive at the start
    int towns = 0;
    for (int k = 1; k < thing_pool_slots(); k++) {
        const Thing *t = thing_at((unsigned)k);
        towns += t->cls == 10 && t->type == 0x2d;
    }
    if (towns != count[WFEAT_TOWN]) std::printf("  towns: %d placed, %d alive\n", count[WFEAT_TOWN], towns);
    CHECK(towns == count[WFEAT_TOWN]);
    // portals: mutual links, gates as Things whose destination is beside the other end
    for (size_t i = 0; i < L.features.size(); i++) {
        const WorldFeature &f = L.features[i];
        if (f.kind != WFEAT_PORTAL) continue;
        CHECK(f.link < L.features.size() && L.features[f.link].link == i);
        const WorldFeature &o = L.features[f.link];
        bool gate = false;
        for (int k = 1; k < thing_pool_slots(); k++) {
            const Thing *t = thing_at((unsigned)k);
            if (t->cls == 10 && t->type == 0x22 && ((int)t->x >> 8) == f.x && ((int)t->y >> 8) == f.y)
                gate = ((int)t->home.x >> 8) == world_wrap_cell(o.x + 3) && ((int)t->home.y >> 8) == world_wrap_cell(o.y + 3);
            if (gate) break;
        }
        CHECK(gate);
    }
    // triggers: the switch exists, the payload waits
    for (const WorldFeature &f : L.features) {
        if (f.kind != WFEAT_TRIGGER) continue;
        bool sw = false;
        for (int k = 1; k < thing_pool_slots() && !sw; k++) {
            const Thing *t = thing_at((unsigned)k);
            sw = t->cls == 11 && t->owner == f.link;
        }
        int waiting = 0;
        for (const ThingInit &r : g_state->level.things) waiting += r.cls != 0 && r.dis_id == f.link;
        CHECK(sw && waiting > 0);
    }
    if (png) write_png(png);
    if (std::getenv("WG_DEBUG"))
        for (const WorldFeature &f : L.features)
            if (f.kind != WFEAT_TREES) std::printf("  feature %d grade %d at %d,%d link %d\n", f.kind, f.grade, f.x, f.y, f.link);
    mode_stop_run();
}

// Computer wizards flown into trigger areas fire them (one area per payload type found in a few worlds), and a
// wizard flying into a portal gate comes out beside the other end.
static void check_triggers_and_portals() {
    static const char *const kNames[WTRIG_COUNT] = {"?", "mana", "ambush", "earthquake", "volcano", "storm", "craters",
                                                    "elite", "urn"};
    bool fired[WTRIG_COUNT] = {};
    int portals_ok = 0;
    for (uint32_t seed = 11; seed < 17; seed++) {
        ModeParams mp;
        mp.seed = seed; mp.world_size = 1024; mp.bots = 3; mp.humans = 1;
        mp.world.features = 0x18;                       // 8 trigger areas, 1 portal pair per player
        CHECK(start(mp));
        const WorldLayout L = worldgen_last_layout();
        g_mode.players[1].flags |= MODE_PLAYER_GOD;     // the bot survives volcanoes and ambushes
        Thing *bot = wizard(1);
        CHECK(bot->cls == 3 && bot->type == 1);
        for (const WorldFeature &f : L.features) {
            if (f.kind != WFEAT_TRIGGER || f.grade >= WTRIG_COUNT || fired[f.grade]) continue;
            auto waiting = [&] { int n = 0; for (const ThingInit &r : g_state->level.things) n += r.cls != 0 && r.dis_id == f.link; return n; };
            const int before = waiting();
            const int live0 = thing_pool_live_count();
            for (int i = 0; i < 24 && waiting() == before; i++) {
                Pos p{f.x * 256 + 0x80, f.y * 256 + 0x80, 0};
                p.z = (int16_t)(terrain_height_at(&p) + 0x100);
                thing_move_to(bot, &p);
                game_tick_sim();
            }
            std::printf("trigger DisId %u (%s) at %d,%d: %d records waiting -> %d after a bot flew in; Things %d -> %d\n",
                        f.link, kNames[f.grade], f.x, f.y, before, waiting(), live0, thing_pool_live_count());
            CHECK(before > 0 && waiting() == 0);
            fired[f.grade] = true;
        }
        // the first portal: a wizard just in front of the gate, looking at it
        for (const WorldFeature &f : L.features) {
            if (f.kind != WFEAT_PORTAL) continue;
            const WorldFeature &o = L.features[f.link];
            Thing *gate = nullptr;
            for (int k = 1; k < thing_pool_slots() && !gate; k++) {
                Thing *t = thing_at((unsigned)k);
                if (t->cls == 10 && t->type == 0x22 && ((int)t->x >> 8) == f.x && ((int)t->y >> 8) == f.y) gate = t;
            }
            CHECK(gate != nullptr);
            if (!gate) break;
            Thing *w = wizard(2);
            bool through = false;
            for (int i = 0; i < 8 && !through; i++) {
                Pos p{gate->x, gate->y + 0x60, gate->z};
                thing_move_to(w, &p);
                w->yaw = (uint16_t)(pos_angle_to(thing_pos(w), thing_pos(gate)) & 0x7ff);
                game_tick_sim();
                through = std::abs(world_signed(w->x - (o.x + 3) * 256)) < 3 * 256 &&
                          std::abs(world_signed(w->y - (o.y + 3) * 256)) < 3 * 256;
            }
            std::printf("portal %d,%d -> %d,%d: wizard 2 at cell %d,%d\n", f.x, f.y, o.x, o.y, (int)w->x >> 8, (int)w->y >> 8);
            CHECK(through);
            portals_ok += through;
            break;
        }
        mode_stop_run();
    }
    for (int k = 1; k < WTRIG_COUNT; k++) if (!fired[k]) std::printf("  (payload %s not drawn in these worlds)\n", kNames[k]);
    CHECK(fired[WTRIG_MANA] && fired[WTRIG_AMBUSH]);
    CHECK(portals_ok > 0);
}

// 7. Scenery and terrain art (user, 2026-10-08): stone circles (standing stones only there, lone ones rare; ~60 %
// hold a trigger), spell urns graded by distance (pickups on the ground), woods of one type each (oak woods re-skinned
// by worldgen_apply_woods to MC_SPRITE_DESC_OAK with the palm's extents), taller peaks (above the old 0xc4), snow
// (class 6) only on high ground and steep rock as class 1 (Hidden Worlds' classification).
static void check_scenery() {
    ModeParams mp;
    mp.seed = 49; mp.world_size = 512; mp.bots = 6; mp.humans = 1;
    CHECK(start(mp));
    const WorldLayout &L = worldgen_last_layout();
    int circles = 0, trig_circles = 0, urns[4] = {}, woods[2] = {};
    for (const WorldFeature &f : L.features) {
        if (f.kind == WFEAT_CIRCLE) { circles++; trig_circles += f.grade; }
        if (f.kind == WFEAT_URN && f.grade <= 3) urns[f.grade]++;
        if (f.kind == WFEAT_TREES && f.grade <= 1) woods[f.grade]++;
    }
    // stones: standing stones (type 1) within 5 cells of a circle centre vs elsewhere
    int stones_in = 0, stones_out = 0, oaks = 0, palms = 0, spells_ground = 0;
    for (int i = 1; i < thing_pool_slots(); i++) {
        const Thing *t = thing_at((unsigned)i);
        if (t->cls == 12) spells_ground++;
        if (t->cls != 2) continue;
        if (t->type == 0) { if (t->sprite == MC_SPRITE_DESC_OAK) oaks++; else palms++; }
        if (t->type != 1) continue;
        bool in = false;
        for (const WorldFeature &f : L.features)
            if (f.kind == WFEAT_CIRCLE && world_dist_sq(world_signed(t->x - f.x * 256), world_signed(t->y - f.y * 256)) <= 0x500 * 0x500) in = true;
        (in ? stones_in : stones_out)++;
    }
    int hmax = 0, snow = 0, snow_low = 0, cliff = 0;
    for (uint32_t c = 0; c < g_world.cells; c++) {
        hmax = std::max<int>(hmax, g_map_height[c]);
        const int cls = g_map_flags[c] & 7;
        if (cls == 6) { snow++; if (g_map_height[c] <= g_state->level.gen.snlin) snow_low++; }
        if (cls == 1) cliff++;
    }
    std::printf("scenery 512: %d stone circles (%d with a trigger), standing stones %d in circles / %d alone; urns %d / %d / %d "
                "(%d spells on the ground); woods %d palm / %d oak, trees %d palm / %d oak; peak %d, snow line %d, %d snow cells "
                "(%d below the line), %d rock / cliff edge\n", circles, trig_circles, stones_in, stones_out, urns[1], urns[2],
                urns[3], spells_ground, woods[0], woods[1], palms, oaks, hmax, (int)g_state->level.gen.snlin, snow, snow_low, cliff);
    CHECK(circles >= 8 && trig_circles * 100 >= circles * 35 && trig_circles * 100 <= circles * 85);
    CHECK(stones_in > 8 * circles / 2 && stones_out * 10 <= stones_in);
    CHECK(urns[1] >= urns[2] && urns[2] > 0 && urns[3] > 0 && spells_ground >= urns[1] + urns[2] + urns[3]);
    CHECK(woods[0] > 0 && woods[1] > 0 && oaks > 50 && palms > 50);
    CHECK(mc_sprite_desc(MC_SPRITE_DESC_OAK)->half_z == mc_sprite_desc(0x53)->half_z);
    CHECK(hmax > 0xc4 && snow > 100 && snow_low * 50 <= snow && cliff > 0);   // (spike smoothing lowers a few after)
    mode_stop_run();
}

static void check_determinism() {
    ModeParams mp;
    mp.seed = 5; mp.world_size = 1024; mp.bots = 3; mp.humans = 1;
    std::vector<uint32_t> run[2];
    uint32_t maps[2] = {}, recs[2] = {};
    for (int r = 0; r < 2; r++) {
        CHECK(start(mp));
        maps[r] = world_hash();
        recs[r] = fnv(&g_state->level.things, sizeof g_state->level.things);
        for (int i = 0; i < 600; i++) { game_tick_sim(); run[r].push_back(net_state_checksum()); }
        mode_stop_run();
    }
    CHECK(maps[0] == maps[1] && recs[0] == recs[1]);
    CHECK(run[0] == run[1]);
    std::printf("determinism: maps %08x, 600 ticks %08x .. %08x, identical: %s\n", maps[0], run[0].front(), run[0].back(),
                run[0] == run[1] ? "yes" : "NO");
    // another seed is another world
    mp.seed = 6;
    CHECK(start(mp));
    CHECK(world_hash() != maps[0]);
    mode_stop_run();
}

int main(int argc, char **argv) {
    mc_install_crash_handler();
    g_game = argc > 1 ? argv[1] : MC_DEFAULT_GAME_DIR;
    if (!sim_init(g_game.c_str())) { std::printf("SKIP: no game data in %s\n", g_game.c_str()); return 0; }
    sim_register_gameplay();
    mode_level_register();
    g_hook_player_local_input = nullptr;
    const char *out = std::getenv("MC_WORLD_GEN_OUT");
    g_out = out ? fs::path(out) : fs::temp_directory_path() / "mc_world_gen_test";
    std::error_code ec;
    fs::create_directories(g_out, ec);

    ModeParams mp;
    if (const char *s = std::getenv("WG_SEED")) {       // look at one world: WG_SEED=n [WG_BOTS=b] [WG_SIZE=N]
        mp.seed = (uint32_t)std::strtoul(s, nullptr, 0);
        mp.bots = std::getenv("WG_BOTS") ? (uint32_t)std::atoi(std::getenv("WG_BOTS")) : 3;
        mp.world_size = std::getenv("WG_SIZE") ? (uint32_t)std::atoi(std::getenv("WG_SIZE")) : 1024;
        check_world(mp, "world_gen_seed.png");
        return g_fail ? 1 : 0;
    }
    mp.seed = 1; mp.world_size = 1024; mp.bots = 3; mp.humans = 1;
    check_world(mp, "world_gen_1024.png");
    mp.seed = 2; mp.world_size = 512; mp.bots = 1;
    check_world(mp, "world_gen_512.png");
    mp.seed = 3; mp.world_size = 256; mp.bots = 1; mp.flags = MODE_PARAM_GENERATE; mp.world.symmetry = WORLD_SYM_MIRROR;
    check_world(mp, "world_gen_256.png");
    mp = ModeParams{};
    mp.seed = 4; mp.world_size = 2048; mp.bots = 7; mp.humans = 1;
    check_world(mp, "world_gen_2048.png");
    // every template the seed can pick builds a full world (round 11: template 50's class thresholds gave a world of
    // steep rock with no features before the generator preset)
    for (uint32_t seed : {453406441u, 22041985u, 145629623u, 171188u, 7u, 99u, 1234u, 31337u, 4242u, 900000u}) {
        ModeParams q;
        q.seed = seed; q.world_size = 1024; q.bots = 4; q.humans = 1;
        CHECK(start(q));
        const WorldLayout &L = worldgen_last_layout();
        int count[16] = {}, steep = 0, water = 0;
        for (const WorldFeature &f : L.features) count[f.kind & 15]++;
        for (uint32_t c = 0; c < g_world.cells; c++) {
            const int k = g_map_flags[c] & 7;
            steep += k == 1 || k == 6;
            water += k == 0;
        }
        std::printf("seed %u template %u: water %d %%, steep %d %%, springs %d lairs %d triggers %d portals %d groves %d, "
                    "extra %d\n", seed, g_mode.template_level, (int)((int64_t)water * 100 / g_world.cells),
                    (int)((int64_t)steep * 100 / g_world.cells), count[WFEAT_SPRING], count[WFEAT_LAIR],
                    count[WFEAT_TRIGGER], count[WFEAT_PORTAL] / 2, count[WFEAT_TREES], L.extra_records);
        CHECK(count[WFEAT_SPRING] >= 16 && count[WFEAT_LAIR] >= 16 && count[WFEAT_TREES] >= 100);
        CHECK(steep * 100 / (int64_t)g_world.cells < 40);
        mode_stop_run();
    }
    check_triggers_and_portals();
    check_scenery();
    check_determinism();
    if (g_fail) { std::printf("world_gen_test: %d FAILED\n", g_fail); return 1; }
    std::printf("world_gen_test: all passed\n");
    return 0;
}
