// Unit test for thing.cpp: pool / cell-list invariants, dispatch binding, the angle helpers, and a
// structural check of the port's Thing layout against the engine's own snapshot of level 38
// (movie/gam00000.dat = raw GameState, movie/map00000.dat = maps + cell index): after
// thing_relink_snapshot every cell list, per-thing link and descriptor index must be consistent.
// argv[1] = game dir.
#define _CRT_SECURE_NO_WARNINGS
#include "thing.h"
#include "mc_math.h"
#include "mcfile.h"
#include "crash_handler.h"
#include <cstdio>
#include <cstring>
#include <vector>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static int s_updates = 0;
static void test_update(Thing *t) { s_updates++; t->aux++; }
static Thing *test_create(const Pos *pos) {
    Thing *t = thing_alloc();
    if (!t) return nullptr;
    t->cls = 2; t->type = 0; t->state = 0;
    thing_set_sprite(t, 0);
    thing_link_cell(t, pos);
    return t;
}

static void test_pool() {
    thing_pool_reset();
    CHECK(thing_free_count() == 999);
    Thing *a = thing_alloc(), *b = thing_alloc(), *c = thing_alloc();
    CHECK(thing_index(a) == 1 && thing_index(b) == 2 && thing_index(c) == 3);
    CHECK(a->max_health == 300 && a->flags == 8 && a->speed_cur == 0x10 && a->damage == 100);
    CHECK(a->owner == 1 && a->filter_cls == 0xff && a->impact_cls == 10 && a->timer_a == 0xfa && a->tick == 1);
    a->cls = b->cls = c->cls = 2;
    Pos p{0x1234, 0x5678, 100};
    thing_link_cell(a, &p); thing_link_cell(b, &p); thing_link_cell(c, &p);
    uint16_t cell = mc_cell_of(p.x, p.y);
    CHECK(cell == 0x5612);
    CHECK(g_cell_things[cell] == 3 && c->cell_next == 2 && b->cell_next == 1 && a->cell_next == 0);
    CHECK(a->cell_prev == 2 && b->cell_prev == 3 && c->cell_prev == 0);
    thing_unlink_cell(b);
    CHECK(c->cell_next == 1 && a->cell_prev == 3 && !(b->flags & 4));
    Pos q{0x1300, 0x5678, 0};
    CHECK(thing_move_to(c, &q) == 1 && g_cell_things[cell] == 1 && g_cell_things[mc_cell_of(q.x, q.y)] == 3);
    Pos q2{0x13ff, 0x56ff, 7};
    CHECK(thing_move_to(c, &q2) == 0 && c->x == 0x13ff && c->z == 7);
    thing_free(a); thing_free(c);
    CHECK(g_cell_things[cell] == 0 && thing_free_count() == 998);
    CHECK(thing_alloc() == c);         // LIFO
    // exhaust: nothing recyclable -> null
    thing_pool_reset();
    for (int i = 0; i < 999; i++) { Thing *t = thing_alloc(); CHECK(t != nullptr); if (t) t->cls = 2; }
    CHECK(thing_alloc() == nullptr);
    thing_at(500)->flags |= 0x20000;
    models_initialise();
    CHECK(g_state->free_top == -1 && g_state->active_top == 0);
    Thing *r = thing_alloc();
    CHECK(r == thing_at(500) && r->cls == 0 && g_state->active_top == -1);
    std::memset(g_state->things, 0, sizeof g_state->things);
    std::memset(g_cell_things, 0, g_world.cells * sizeof(uint16_t));
    thing_pool_reset();
}

static void test_math() {
    CHECK(math_atan2(0, -100) == 0);
    CHECK(math_atan2(100, 0) == 0x200);
    CHECK(math_atan2(0, 100) == 0x400);
    CHECK(math_atan2(-100, 0) == 0x600);
    CHECK(math_atan2(100, -100) == 0x100);
    CHECK(math_atan2(100, 100) == 0x300);
    CHECK(math_atan2(-100, 100) == 0x500);
    CHECK(math_atan2(-100, -100) == 0x700);
    CHECK(math_atan2(0, 0) == 0);
    CHECK(angle_diff(0x10, 0x7f0) == 0x20 && angle_diff(0, 0x400) == 0x400 && angle_diff(0x100, 0x300) == 0x200);
    CHECK(angle_turn_dir(0x10, 0x7f0) == -1 && angle_turn_dir(0x7f0, 0x10) == 1 && angle_turn_dir(5, 5) == 0);
    CHECK(angle_turn_step(0, 0x100, 0, 0x10) == 0x10 && angle_turn_step(0, 0x7fc, 0, 0x10) == -4);
    Pos p{0x8000, 0x8000, 0};
    math_rotate_offset(&p, 0, 0, 0x100);
    CHECK(p.x == 0x8000 && p.y == 0x7f00);
    math_rotate_offset(&p, 0x200, 0, 0x100);
    CHECK(p.x == 0x8100 && p.y == 0x7f00);
    Pos a{100, 100, 0}, b{103, 104, 0};
    CHECK(pos_dist_xy(&a, &b) == 5 && pos_dist_sq_xy(&a, &b) == 25 && pos_dist_manhattan(&a, &b) == 7);
    CHECK(pos_angle_to(&a, &b) > 0x200 && pos_angle_to(&a, &b) < 0x400);
    int16_t ea[4] = {0, 10, 10, 10}, eb[4] = {0, 5, 5, 5};
    Pos c{114, 100, 0}, d{115, 100, 0};
    CHECK(math_bbox_overlap(&a, ea, &c, eb) == 1 && math_bbox_overlap(&a, ea, &d, eb) == 0);
}

static void test_dispatch() {
    // class 2 state 0 = scenery_tree_update_43ba0, Table B class 2 type 0 = 0x35e60 (tree)
    CHECK(thing_register_update(0x43ba0, test_update) == 1);
    CHECK(thing_register_create(0x35e60, test_create) == 1);
    CHECK(thing_register_update(0x12345, test_update) == 0);
    thing_pool_reset();
    Pos p{0x2080, 0x3080, 0};
    Thing *t = thing_create(&p, 2, 0);
    CHECK(t && thing_index(t) == 1 && g_cell_things[0x3020] == 1);
    CHECK(thing_create(&p, 2, 99) == nullptr && thing_create(&p, 4, 0) == nullptr);
    Thing *pl = thing_alloc();       // a fake player thing to check the list pass
    pl->cls = 3; pl->state = 3; pl->health = 1;
    Thing *cr = thing_alloc();
    cr->cls = 5; cr->type = 9; cr->state = 54; cr->health = 1;
    uint8_t tick0 = t->tick;
    s_updates = 0;
    thing_dispatch_reset_stats();
    thing_update_all();
    CHECK(s_updates == 1 && t->aux == 1 && t->tick == (uint8_t)(tick0 + 1));
    CHECK(g_cfg->player_list == thing_index(pl) && g_cfg->creature_lists[9] == thing_index(cr));
    CHECK(thing_dispatch_report(nullptr) == 2);          // the player and skeleton handlers are not ported here
    thing_mark_delete(t);
    thing_update_all();
    CHECK(t->cls == 0 && g_cell_things[0x3020] == 0 && s_updates == 1);
    // bad state -> deleted on the following tick
    cr->state = 125;
    thing_update_all();
    CHECK(cr->flags & 0x400);
    thing_update_all();
    CHECK(cr->cls == 0);
    std::memset(g_state->things, 0, sizeof g_state->things);
    std::memset(g_cell_things, 0, g_world.cells * sizeof(uint16_t));
    thing_pool_reset();
}

static bool test_snapshot(const char *game_dir) {
    char path[1024];
    mc_blob gam, map;
    mc_path_join(path, sizeof path, game_dir, "movie/gam00000.dat");
    if (!mc_read_file(path, &gam)) { std::printf("snapshot: %s missing, skipped\n", path); return true; }
    mc_path_join(path, sizeof path, game_dir, "movie/map00000.dat");
    if (!mc_read_file(path, &map)) { mc_blob_free(&gam); std::printf("snapshot: %s missing, skipped\n", path); return true; }
    CHECK(gam.len == MC_STATE_IMAGE_SIZE);
    CHECK(map.len >= 0x60000);
    if (gam.len != MC_STATE_IMAGE_SIZE || map.len < 0x60000) return false;
    std::memcpy(g_map_type, map.data, 0x10000);
    std::memcpy(g_map_height, map.data + 0x10000, 0x10000);
    std::memcpy(g_map_light, map.data + 0x20000, 0x10000);
    std::memcpy(g_map_flags, map.data + 0x30000, 0x10000);
    std::memcpy(g_cell_things, map.data + 0x40000, 0x20000);
    CHECK(thing_load_snapshot_image(gam.data, g_state));
    mc_blob_free(&gam); mc_blob_free(&map);

    int live = 0, linked = 0, by_class[16] = {};
    for (int i = 1; i < MC_THING_SLOTS; i++) {
        const Thing &t = g_state->things[i];
        if (!t.cls) continue;
        live++;
        if (t.cls < 16) by_class[t.cls]++;
        if (t.flags & 4) linked++;
        CHECK(t.desc < 30);
        CHECK(t.next < MC_THING_SLOTS);
        if (t.player) {
            uint32_t rel = t.player - (uint32_t)offsetof(GameState, players);
            CHECK(rel % sizeof(PlayerRec) == offsetof(PlayerRec, p) && rel / sizeof(PlayerRec) < 8);
        }
    }
    CHECK(live + thing_free_count() == 999);
    for (int i = 0; i <= g_state->free_top; i++) CHECK(g_state->things[g_state->free_list[i]].cls == 0);
    // every cell list holds exactly the linked things of that cell
    int in_lists = 0;
    std::vector<uint8_t> seen(MC_THING_SLOTS);
    for (int cell = 0; cell < MC_MAP_CELLS; cell++) {
        uint16_t prev = 0;
        for (uint16_t i = g_cell_things[cell]; i; i = g_state->things[i].cell_next) {
            const Thing &t = g_state->things[i];
            if (i >= MC_THING_SLOTS || seen[i]) { CHECK(!"cell list corrupt"); break; }
            seen[i] = 1; in_lists++;
            CHECK(t.cls != 0 && (t.flags & 4));
            CHECK(mc_cell_of(t.x, t.y) == cell);
            CHECK(t.cell_prev == prev);
            prev = i;
        }
    }
    CHECK(in_lists == linked);
    // per-player things point back at their own P block
    for (int p = 0; p < g_state->player_count && p < 8; p++) {
        const Thing &t = g_state->things[g_state->players[p].thing];
        CHECK(t.cls == 3);
        CHECK(t.player == player_block_offset(p));
    }
    CHECK(g_state->things[g_state->players[0].thing].desc == 7);
    // thing_set_sprite reproduces the stored extents and frame counts of the scenery
    int scen = 0, scen_ok = 0;
    for (int i = 1; i < MC_THING_SLOTS; i++) {
        const Thing &t = g_state->things[i];
        if (t.cls != 2) continue;
        Thing probe = t;
        thing_set_sprite(&probe, t.sprite);
        scen++;
        if (probe.ext_z0 == t.ext_z0 && probe.ext_h == t.ext_h && probe.draw_type == t.draw_type) scen_ok++;
    }
    std::printf("snapshot: %d live things (scenery %d, players %d, creatures %d, projectiles %d, effects %d, switches %d, spells %d), "
                "%d linked, %d free; scenery extents reproduced %d/%d\n",
                live, by_class[2], by_class[3], by_class[5], by_class[9], by_class[10], by_class[11], by_class[12],
                linked, thing_free_count(), scen_ok, scen);
    CHECK(scen > 0 && scen_ok * 10 >= scen * 9);
    // one simulation step over the real pool with no handlers bound must not corrupt anything
    thing_dispatch_reset_stats();
    thing_update_all();
    int missing = thing_dispatch_report(nullptr);
    std::printf("snapshot: one thing_update_all over the pool dispatches %d distinct handlers\n", missing);
    CHECK(missing > 10);
    return true;
}

int main(int argc, char **argv) {
    mc_install_crash_handler();
    const char *game_dir = argc > 1 ? argv[1] : MC_DEFAULT_GAME_DIR;
    mc_globals_init();
    CHECK(sprite_table_init_sizes(game_dir));
    test_pool();
    test_math();
    test_dispatch();
    test_snapshot(game_dir);
    if (g_fail) { std::printf("thing_test: %d FAILED\n", g_fail); return 1; }
    std::printf("thing_test: OK\n");
    return 0;
}
