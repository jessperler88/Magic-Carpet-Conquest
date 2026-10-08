// Unit test for creature_common.cpp / creatures.cpp (class 5). argv[1] = game dir; argv[2] = "scan"
// additionally prints the creature types of every campaign level at level start.
//
//  1. replay: level 38 is generated and run for 412 ticks with empty command packets (the recording
//     player hovered at the start position until the demo recorder took its snapshot, see
//     docs/analysis/port_player.md), then every creature slot is compared byte for byte with
//     movie/gam00000.dat. Creatures that never met an unported subsystem (AI wizards, castles,
//     projectiles, spells) must be identical: position, heading, per-thing RNG, timers, state.
//  2. construction tests of the shared functions (hand-computed cases from the disassembly).
//  3. invariants on the snapshot's 169 creatures.
//  4. smoke runs: the movie from the snapshot and a few other levels (dragons, vultures, bees, worms).
#include "sim.h"
#include "creatures.h"
#include "level_features.h"
#include "player.h"
#include "demo.h"
#include "mc_math.h"
#include "crash_handler.h"
#include <cstdio>
#include <cstring>
#include <vector>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)
#define CHECK_EQ(a, b) do { long long va_ = (long long)(a), vb_ = (long long)(b); if (va_ != vb_) { \
    std::printf("FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, va_, vb_); g_fail++; } } while (0)

static const char *g_game_dir = nullptr;

static bool is_creature(const Thing *t) { return t->cls == 5; }
static bool is_wizard_castle(const Thing *t) { return t->cls == 10 && t->type == 0x2d; }

static void census(const char *what) {
    int by_state[128] = {}, n = 0;
    for (int i = 1; i < MC_THING_SLOTS; i++) {
        const Thing *t = thing_at(i);
        if (!is_creature(t)) continue;
        n++;
        by_state[t->state & 127]++;
    }
    std::printf("%s: %d creatures, state:count", what, n);
    for (int s = 0; s < 128; s++) if (by_state[s]) std::printf(" %d:%d", s, by_state[s]);
    std::printf("\n");
}

// Every live creature is in a state of its type and is linked into the cell list of its position.
static void check_pool(const char *what) {
    int bad_state = 0, bad_link = 0, n = 0;
    for (int i = 1; i < MC_THING_SLOTS; i++) {
        const Thing *t = thing_at(i);
        if (!is_creature(t)) continue;
        n++;
        int base = t->type * 6;
        bool ok = (t->state >= base && t->state <= base + 5) || t->state == 0x78 ||
                  (t->type == 0xc && t->state == 0x4f);              // a builder that founded its castle
        if (!ok) { if (bad_state++ < 5) std::printf("  %s: thing %d type %d in state %d\n", what, i, t->type, t->state); }
        if (t->flags & 4) {
            unsigned cell = ((unsigned)(t->y >> 8) << 8) | (t->x >> 8);
            bool found = false;
            int guard = 0;
            for (unsigned k = g_cell_things[cell]; k != 0 && guard < MC_THING_SLOTS; k = thing_at(k)->cell_next, guard++)
                if (k == (unsigned)i) { found = true; break; }
            if (!found) { if (bad_link++ < 5) std::printf("  %s: thing %d not in the list of cell %04x\n", what, i, cell); }
        }
    }
    CHECK_EQ(bad_state, 0);
    CHECK_EQ(bad_link, 0);
    CHECK(n > 0);
}

// ---- 1. replay of the 412 ticks before the snapshot -----------------------------------------------

static bool same_creature(const Thing *a, const Thing *b) {
    // everything but the list links (next, cell_next, cell_prev), which depend on the other things
    const uint8_t *pa = reinterpret_cast<const uint8_t *>(a), *pb = reinterpret_cast<const uint8_t *>(b);
    return std::memcmp(pa + 4, pb + 4, 0x10) == 0 && std::memcmp(pa + 0x18, pb + 0x18, sizeof(Thing) - 0x18) == 0;
}

// The recording was made by a build whose wizard castles hold w * h >> 2 inhabitants (retail:
// >> 4, effect_wizard_init_35090; all 17 castles of the snapshot show it, port_features.md). With
// `recording_build` the test applies that rule to every wizard castle before each tick.
static void apply_recording_capacity() {
    for (int i = 1; i < MC_THING_SLOTS; i++) {
        Thing *t = thing_at(i);
        if (!is_wizard_castle(t)) continue;
        const CastleFootprint *fp = castle_footprint(t->castle_size);
        t->speed_base = (int16_t)((fp->w * 2 * fp->h * 2) >> 4);
    }
}

static int replay_to_snapshot(const std::vector<Thing> &snap, bool recording_build, bool verbose) {
    if (!sim_load_level(38)) { std::printf("level 38 failed to load\n"); g_fail++; return 0; }
    if (verbose) census("level 38 start");
    int start_by_type[20] = {};
    for (int i = 1; i < MC_THING_SLOTS; i++)
        if (is_creature(thing_at(i)) && thing_at(i)->type < 20) start_by_type[thing_at(i)->type]++;
    CHECK_EQ(start_by_type[9], 157);
    CHECK_EQ(start_by_type[0xc], 4);
    CHECK_EQ(start_by_type[0xd], 50);
    CHECK_EQ(start_by_type[0xe], 10);
    for (int tick = 0; tick < 412; tick++) {
        if (recording_build) apply_recording_capacity();
        game_tick_sim();
    }
    if (recording_build) apply_recording_capacity();
    check_pool("replay");

    int same = 0, total = 0, same_type[20] = {}, total_type[20] = {};
    int gone_both = 0, only_port = 0, only_snap = 0, differ = 0, differ_rng = 0;
    for (int i = 1; i < MC_THING_SLOTS; i++) {
        const Thing *a = thing_at(i), *b = &snap[i];
        if (!is_creature(a) && !is_creature(b)) continue;
        total++;
        int ty = is_creature(b) ? b->type : a->type;
        if (ty < 20) total_type[ty]++;
        if (is_creature(a) && is_creature(b) && same_creature(a, b)) {
            same++;
            if (ty < 20) same_type[ty]++;
            continue;
        }
        if (!is_creature(a)) only_snap++;
        else if (!is_creature(b)) only_port++;
        else {
            differ++;
            if (a->rng != b->rng) differ_rng++;
            if (verbose && differ <= 6)
                std::printf("  thing %d type %d: port state %d aux %d target %d pos %04x,%04x | snapshot state %d aux %d target %d pos %04x,%04x\n",
                            i, a->type, a->state, a->aux, a->target, (unsigned)a->x, (unsigned)a->y, b->state, b->aux, b->target,
                            (unsigned)b->x, (unsigned)b->y);
        }
    }
    // start creatures that are gone in both worlds (townies that walked into a castle, converted villagers)
    {
        std::vector<Thing> now(g_state->things, g_state->things + MC_THING_SLOTS);
        sim_load_level(38);
        for (int i = 1; i < MC_THING_SLOTS; i++)
            if (is_creature(thing_at(i)) && !is_creature(&now[i]) && !is_creature(&snap[i])) gone_both++;
        std::memcpy(g_state->things, now.data(), sizeof(Thing) * MC_THING_SLOTS);   // only read below
    }
    std::printf("replay (%s capacity): %d of %d creature slots identical to the snapshot [skeleton %d/%d, builder %d/%d, townie %d/%d, trader %d/%d, archer %d/%d];"
                " %d start creatures gone in both; different: %d (%d with another RNG state), only in the port %d, only in the snapshot %d\n",
                recording_build ? "recording-build" : "retail", same, total, same_type[9], total_type[9], same_type[0xc], total_type[0xc],
                same_type[0xd], total_type[0xd], same_type[0xe], total_type[0xe], same_type[4], total_type[4],
                gone_both, differ, differ_rng, only_port, only_snap);

    // wizard castles: the ones the builders founded during the 412 ticks must be the snapshot's
    int twins = 0, snap_castles = 0, port_castles = 0, aux_equal = 0;
    for (int i = 1; i < MC_THING_SLOTS; i++) if (is_wizard_castle(thing_at(i))) port_castles++;
    for (int i = 1; i < MC_THING_SLOTS; i++) {
        const Thing *b = &snap[i];
        if (!is_wizard_castle(b)) continue;
        snap_castles++;
        for (int j = 1; j < MC_THING_SLOTS; j++) {
            const Thing *a = thing_at(j);
            if (!is_wizard_castle(a) || a->x != b->x || a->y != b->y) continue;
            if (a->castle_size == b->castle_size && a->ext_x == b->ext_x && a->ext_y == b->ext_y && a->z == b->z) {
                twins++;
                if (a->aux == b->aux) aux_equal++;
            }
            break;
        }
    }
    std::printf("  wizard castles: snapshot %d, port %d; %d of the snapshot's have a twin in the port (position, size, height), %d with the same head count\n",
                snap_castles, port_castles, twins, aux_equal);
    CHECK_EQ(twins, snap_castles);
    if (recording_build) CHECK(port_castles > 19);      // 19 level castles + the builders' new ones
    return same;
}

// ---- 2. construction tests ------------------------------------------------------------------------

static Thing *find_creature(int type, int state) {
    for (int i = 1; i < MC_THING_SLOTS; i++) {
        Thing *t = thing_at(i);
        if (is_creature(t) && t->type == type && (state < 0 || t->state == state)) return t;
    }
    return nullptr;
}

static int count_class(int cls) {
    int n = 0;
    for (int i = 1; i < MC_THING_SLOTS; i++) if (thing_at(i)->cls == cls) n++;
    return n;
}

static int s_mana_drops = 0;
static int32_t s_mana_dropped = 0;
static void test_drop_mana_hook(Thing *t) { s_mana_drops++; s_mana_dropped = t->mana; }

static void test_pure() {
    // terrain_slope_at: h00 = 10, h10 = 14, h01 = 11, h11 = 20 -> max(|10 + 11 - 14 - 20|, |14 + 10 - 11 - 20|) = 13
    {
        unsigned c00 = mc_cell(10, 20), c10 = mc_cell(11, 20), c01 = mc_cell(10, 21), c11 = mc_cell(11, 21);
        uint8_t s00 = g_map_height[c00], s10 = g_map_height[c10], s01 = g_map_height[c01], s11 = g_map_height[c11];
        g_map_height[c00] = 10; g_map_height[c10] = 14; g_map_height[c01] = 11; g_map_height[c11] = 20;
        Pos p{ 0x0a80, 0x1480, 0 };
        CHECK_EQ(terrain_slope_at(&p), 13);
        g_map_height[c11] = 15;                 // |10 + 11 - 14 - 15| = 8, |14 + 10 - 11 - 15| = 2
        CHECK_EQ(terrain_slope_at(&p), 8);
        g_map_height[c00] = s00; g_map_height[c10] = s10; g_map_height[c01] = s01; g_map_height[c11] = s11;
    }
    // creature_wander_turn, seed 0: r1 = 0x24df = 9439, 9439 % 157 = 19 -> / 79 = 0 -> direction -1;
    // r2 = 9439 * 0x24a1 + 0x24df = 88518942, & 0xff = 30 -> amount 0x55 + 30 = 115; 0 - 115 = 0x78d
    {
        Thing t{};
        creature_wander_turn(&t);
        CHECK_EQ(t.rng, 88518942u);
        CHECK_EQ(t.target_yaw, 0x78d);
        // seed 1: r1 = 0x24a1 + 0x24df = 18816, % 157 = 133 -> / 79 = 1 -> direction +1
        Thing u{};
        u.rng = 1;
        u.target_yaw = 0x7f0;
        creature_wander_turn(&u);
        uint32_t r2 = mc_lcg(mc_lcg(1));
        CHECK_EQ(u.rng, r2);
        CHECK_EQ(u.target_yaw, (0x7f0 + 0x55 + (r2 & 0xff)) & 0x7ff);
    }
    // creature_apply_damage (no segment chain)
    {
        Thing t{};
        t.health = 1000;
        t.damage_slots[0].amount = 50;
        t.damage_slots[0].attacker = 7;
        t.last_attacker = 99;
        CHECK_EQ(creature_apply_damage(&t), 0);             // asleep: nothing is consumed
        CHECK_EQ(t.health, 1000);
        CHECK_EQ(t.last_attacker, 99);
        t.timer_a = 3;
        CHECK_EQ(creature_apply_damage(&t), 1);
        CHECK_EQ(t.health, 950);
        CHECK_EQ(t.last_attacker, 7);
        CHECK_EQ(t.damage_slots[0].attacker, 0);
        CHECK_EQ(t.damage_slots[0].amount, 50);             // the amount stays in the slot
        CHECK_EQ(creature_apply_damage(&t), 0);             // no attacker recorded: last_attacker cleared
        CHECK_EQ(t.last_attacker, 0);
        t.damage_slots[0].amount = 2000;
        t.damage_slots[0].attacker = 9;
        CHECK_EQ(creature_apply_damage(&t), 2);
        CHECK_EQ(t.health, -1050);
        CHECK_EQ(t.killer, 9);
        t.timer_a = 0;                                      // dead things report 2 even asleep
        t.killer = 0;
        CHECK_EQ(creature_apply_damage(&t), 2);
        CHECK_EQ(t.killer, 9);                              // = last_attacker
    }
    // creature_proximity_wake_timer
    {
        const Thing *local = thing_at(g_state->players[g_state->local_player].thing);
        Thing t{};
        t.timer_a = 5;
        creature_proximity_wake_timer(&t);
        CHECK_EQ(t.timer_a, 4);
        t.timer_a = 0;
        t.timer_b = 3;
        creature_proximity_wake_timer(&t);
        CHECK_EQ(t.timer_b, 2);
        CHECK_EQ(t.timer_a, 0);
        t.timer_b = 0;
        t.x = (uint16_t)(local->x + 0x400);
        t.y = (uint16_t)(local->y - 0x300);
        creature_proximity_wake_timer(&t);
        CHECK_EQ(t.timer_a, 0x10);
        CHECK_EQ(t.cast_ticks, 0x300);                      // |dy|, not the distance (0x500)
        t.timer_a = 0;
        t.cast_ticks = -1;
        t.x = (uint16_t)(local->x + 0x1800);                // d^2 = 0x2400000: not below the limit
        t.y = local->y;
        creature_proximity_wake_timer(&t);
        CHECK_EQ(t.timer_a, 0);
        CHECK_EQ(t.cast_ticks, -1);
        t.x = (uint16_t)(local->x + 0x17ff);
        creature_proximity_wake_timer(&t);
        CHECK_EQ(t.timer_a, 0x10);
        CHECK_EQ(t.cast_ticks, 0);
    }
    // castle_size_half_extents = thing_set_castle_extents + 0x80, terrain_rect_is_flat
    {
        for (unsigned size = 0x19; size <= 0x20; size++) {
            Thing c{};
            thing_set_castle_extents(&c, (int)size);
            uint16_t ex = 0, ey = 0;
            castle_size_half_extents(size, &ex, &ey);
            CHECK_EQ(ex, c.ext_x + 0x80);
            CHECK_EQ(ey, c.ext_y + 0x80);
        }
        // a 4 x 4 block around cell (40, 50); x + y is even after the centring, so no shift
        Pos p{ 0x2880, 0x3280, 0 };
        int range = terrain_rect_height_range(38, 48, 4, 4);
        CHECK_EQ(terrain_rect_is_flat(&p, 4, 4, (unsigned)range + 1), 1);
        CHECK_EQ(terrain_rect_is_flat(&p, 4, 4, (unsigned)range), 0);
        Pos q{ 0x2980, 0x3280, 0 };                         // 39 + 48 is odd: the block starts at x = 40
        range = terrain_rect_height_range(40, 48, 4, 4);
        CHECK_EQ(terrain_rect_is_flat(&q, 4, 4, (unsigned)range + 1), 1);
        CHECK_EQ(terrain_rect_is_flat(&q, 4, 4, (unsigned)range), 0);
    }
}

static void test_states() {
    Thing *player = thing_at(g_state->players[g_state->local_player].thing);
    uint16_t player_idx = thing_index(player);
    Thing *townie = find_creature(0xd, 0x4f), *other = find_creature(0xe, 0x55);
    CHECK(townie && other);
    if (!townie || !other) return;
    const MoveDesc *d = mc_move_desc(townie->desc);
    int period = (int16_t)d->think_period;
    std::printf("townie desc: turn %d slope %d clearance %d..%d z step %d think %d sight %d fov %d; skeleton think %d sight %d fov %d\n",
                d->turn_min, (int16_t)d->unk10, d->clear_hi, d->clear_lo, d->z_step, period, (int16_t)d->sight_radius, (int16_t)d->fov,
                (int16_t)mc_move_desc(find_creature(9, -1)->desc)->think_period, (int16_t)mc_move_desc(find_creature(9, -1)->desc)->sight_radius,
                (int16_t)mc_move_desc(find_creature(9, -1)->desc)->fov);
    CHECK(period > 0);

    // creature_idle_seek_leader / creature_ai_step / creature_attack_target: the damage branches
    Thing saved = *townie;
    auto reset = [&]() { uint16_t cn = townie->cell_next, cp = townie->cell_prev; *townie = saved; townie->cell_next = cn; townie->cell_prev = cp; };
    townie->timer_a = 1;
    townie->damage_slots[0] = { 10, thing_index(other) };          // hit by a creature: ignored
    creature_idle_seek_leader(townie, 0x4e);
    CHECK_EQ(townie->state, 0x4f);
    CHECK_EQ(townie->health, saved.health - 10);
    townie->damage_slots[0] = { 10, player_idx };                  // hit by a class-3 thing: attack it
    creature_idle_seek_leader(townie, 0x4e);
    CHECK_EQ(townie->state, 0x50);
    CHECK_EQ(townie->target, player_idx);
    reset();
    townie->timer_a = 1;
    townie->damage_slots[0] = { 5000, player_idx };
    creature_ai_step(townie, 0x4e);
    CHECK_EQ(townie->state, 0x52);
    CHECK_EQ(townie->killer, player_idx);
    CHECK_EQ(townie->x, saved.x);                                  // no move on the tick of death
    reset();
    townie->timer_a = 1;
    townie->damage_slots[0] = { 10, player_idx };
    townie->target = thing_index(other);
    CHECK_EQ(creature_attack_target(townie, 0x4e, creature_attack_melee), 0);
    CHECK_EQ(townie->state, 0x4f);                                 // retargets, state untouched
    CHECK_EQ(townie->target, player_idx);
    reset();

    // creature_attack_target: target dead -> base + 1; out of sight on a think tick -> base + 1; in
    // range -> the callback (melee: pending damage = the creature's damage value)
    townie->timer_a = 0;
    townie->state = 0x50;
    townie->target = thing_index(other);
    int32_t other_health = other->health;
    other->health = -1;
    CHECK_EQ(creature_attack_target(townie, 0x4e, creature_attack_melee), 0);
    CHECK_EQ(townie->state, 0x4f);
    other->health = other_health;
    reset();
    {
        Pos here = *thing_pos(townie);
        Thing probe = *other;                                      // an unlinked stand-in next to the townie
        probe.x = (uint16_t)(here.x + 0x80); probe.y = here.y; probe.z = here.z;
        std::memset(probe.damage_slots, 0, sizeof probe.damage_slots);
        CHECK_EQ(creature_attack_melee(townie, &probe), 1);
        CHECK_EQ(probe.damage_slots[0].amount, townie->damage);
        CHECK_EQ(probe.damage_slots[0].attacker, townie->owner);
        probe.x = (uint16_t)(here.x + 0x400);
        std::memset(probe.damage_slots, 0, sizeof probe.damage_slots);
        CHECK_EQ(creature_attack_melee(townie, &probe), 0);
        CHECK_EQ(probe.damage_slots[0].amount, 0);
    }

    // creature_move_step: one step of speed_cur along the yaw, yaw turns by at most desc+2
    {
        int moved = 0, turned = 0, n = 0;
        for (int i = 1; i < MC_THING_SLOTS; i++) {
            Thing *t = thing_at(i);
            if (!is_creature(t) || t->state == 0x36) continue;
            Thing before = *t;
            const MoveDesc *td = mc_move_desc(t->desc);
            CHECK_EQ(creature_move_step(t), 1);
            n++;
            if (t->health < 0) { t->health = before.health; continue; }
            int dx = (int16_t)(t->x - before.x), dy = (int16_t)(t->y - before.y);
            if (dx * dx + dy * dy > (before.speed_cur + 2) * (before.speed_cur + 2))
                std::printf("  move: thing %d type %d speed %d moved %d,%d yaw %03x -> %03x\n", i, t->type, before.speed_cur, dx, dy, before.yaw, t->yaw);
            CHECK(dx * dx + dy * dy <= (before.speed_cur + 2) * (before.speed_cur + 2));
            if (dx || dy) moved++;
            // the terrain where it stands now is allowed, unless it stayed inside its cell
            if ((t->x >> 8) != (before.x >> 8) || (t->y >> 8) != (before.y >> 8))
                CHECK_EQ(creature_check_terrain(t, thing_pos(t), 1), 0u);
            // heading: either one of the three detours, or a turn of at most desc+2 toward target_yaw
            int turn = angle_diff(before.yaw, t->yaw) & 0xffff;
            if (turn <= td->turn_min) {
                CHECK((angle_diff(t->yaw, t->target_yaw) & 0xffff) <= (angle_diff(before.yaw, before.target_yaw) & 0xffff));
            }
            if (t->yaw != before.yaw) turned++;
        }
        std::printf("creature_move_step on %d creatures: %d moved, %d changed heading\n", n, moved, turned);
        CHECK(moved > 50);
    }

    // creature_die: segments, kill credit for a human wizard (dragon yes, skeleton no)
    {
        Thing *dragon = thing_create(thing_pos(townie), 5, 0);
        CHECK(dragon != nullptr);
        if (dragon) {
            int segs = 0;
            for (Thing *s = thing_at(dragon->child); s != thing_at(0); s = thing_at(s->child)) segs++;
            CHECK_EQ(segs, 16);
            int32_t kills = player_block(player)->kills;
            dragon->killer = player_idx;
            dragon->owner = thing_index(dragon);
            creature_die(dragon, 0);
            CHECK_EQ(dragon->state, 5);
            for (Thing *s = thing_at(dragon->child); s != thing_at(0); s = thing_at(s->child)) CHECK_EQ(s->state, 5);
            CHECK_EQ(player_block(player)->kills, kills + 1);
            // a segment with less health passes it (and its attacker) to the head
            dragon->state = 1;
            dragon->timer_a = 1;
            Thing *third = thing_at(thing_at(thing_at(dragon->child)->child)->child);
            third->health = dragon->health - 100;
            third->last_attacker = 77;
            CHECK_EQ(creature_apply_damage(dragon), 1);
            CHECK_EQ(dragon->health, third->health);
            CHECK_EQ(dragon->last_attacker, 77);
            // creature_segment_update: awake segments trail the one in front by `speed`
            Thing *first = thing_at(dragon->child);
            first->timer_a = 1;
            creature_segment_update(first);
            CHECK(pos_dist_xyz(thing_pos(first), thing_pos(thing_at(first->parent))) <= first->speed + 2);
            CHECK(!(first->flags & 0x400));
            // wake timer: the head's count is copied to the chain, a reload gives the chain + 2
            dragon->timer_a = 9;
            creature_proximity_wake_timer(dragon);
            CHECK_EQ(first->timer_a, 8);
            CHECK_EQ(third->timer_a, 8);
            // delete the dragon again
            thing_mark_delete(dragon);
            for (Thing *s = thing_at(dragon->child); s != thing_at(0); s = thing_at(s->child)) thing_mark_delete(s);
            player_block(player)->kills = kills;
        }
        Thing *skel = find_creature(9, -1);
        Thing ssaved = *skel;
        int32_t kills = player_block(player)->kills;
        skel->killer = player_idx;
        creature_die(skel, 0x36);
        CHECK_EQ(skel->state, 0x3b);
        CHECK_EQ(player_block(player)->kills, kills);
        skel->state = ssaved.state;
        skel->killer = ssaved.killer;
    }

    // creature_dead_drop_mana: only on every 8th tick; mana ball hook, death effect, delete
    {
        auto saved_hook = g_hook_thing_drop_mana_ball;
        g_hook_thing_drop_mana_ball = test_drop_mana_hook;
        s_mana_drops = 0;
        Thing *victim = find_creature(0xd, 0x4f);
        victim->mana = 1234;
        victim->tick = 3;
        int effects = count_class(10);
        creature_dead_drop_mana(victim);
        CHECK_EQ(s_mana_drops, 0);
        CHECK(!(victim->flags & 0x400));
        victim->tick = 8;
        creature_dead_drop_mana(victim);
        CHECK_EQ(s_mana_drops, 1);
        CHECK_EQ(s_mana_dropped, 1234);
        CHECK(victim->flags & 0x400);
        CHECK_EQ(count_class(10), effects + 1);
        g_hook_thing_drop_mana_ball = saved_hook;
    }
}

static Thing *newest_projectile(const std::vector<uint8_t> &before) {
    for (int i = 1; i < MC_THING_SLOTS; i++)
        if (thing_at(i)->cls == 9 && before[(size_t)i] != 9) return thing_at(i);
    return nullptr;
}

static void test_attacks() {
    Thing *player = thing_at(g_state->players[g_state->local_player].thing);
    Thing *t = find_creature(0xe, 0x55);
    CHECK(t != nullptr);
    if (!t) return;
    t->target = thing_index(player);
    t->filter_cls = 3;
    t->filter_type = 0xff;
    struct Case { CreatureAttackFn fn; int type, damage, desc, impact_type; const char *name; };
    const Case cases[] = {
        { creature_attack_fire,         0,   500,  6, 0,    "193f0" },
        { creature_attack_arrow,        0xd, 250, -1, -1,   "194a0" },
        { skeleton_attack_fire,         0xd, 400, -1, -1,   "19550" },
        { creature_attack_fire_troll,   0xe, 780,  6, 0,    "19940" },
        { creature_attack_fire_griffon, 9,   4000, 6, 0x17, "199f0" },
        { creature_attack_fire_homing_desc, 0, -1, 2, 0,    "19a90" },
    };
    for (const Case &c : cases) {
        std::vector<uint8_t> before(MC_THING_SLOTS);
        for (int i = 0; i < MC_THING_SLOTS; i++) before[(size_t)i] = thing_at(i)->cls;
        CHECK_EQ(c.fn(t, player), 1);
        Thing *p = newest_projectile(before);
        CHECK(p != nullptr);
        if (!p) continue;
        CHECK_EQ(p->type, c.type);
        if (c.damage >= 0) CHECK_EQ(p->damage, c.damage);
        if (c.desc >= 0) CHECK_EQ(p->desc, (uint32_t)c.desc);
        if (c.impact_type >= 0) { CHECK_EQ(p->impact_cls, 10); CHECK_EQ(p->impact_type, c.impact_type); }
        CHECK_EQ(p->owner, t->owner);
        CHECK_EQ(p->target, t->target);
        CHECK_EQ(p->yaw, (uint16_t)pos_angle_to(thing_pos(t), thing_pos(player)));
        CHECK_EQ(p->pitch, (uint16_t)pos_pitch_to(thing_pos(t), thing_pos(player)));
        // 19a90 moves its projectile back onto the creature (thing_move_to after the raise)
        CHECK_EQ(p->z, c.fn == creature_attack_fire_homing_desc ? t->z : t->z + t->ext_h);
        CHECK_EQ(p->x, t->x);
        if (c.fn == creature_attack_fire_griffon) { CHECK_EQ(p->filter_cls, player->filter_cls); CHECK_EQ(p->filter_type, player->filter_type); }
        else                                      { CHECK_EQ(p->filter_cls, 3); CHECK_EQ(p->filter_type, 0xff); }
        if (c.fn == creature_attack_arrow) CHECK_EQ(p->sprite, 0xc3);
        if (c.fn == skeleton_attack_fire)  CHECK_EQ(p->sprite, 0xcb);
        thing_free(p);
    }
    // skeleton with a mana owner: 600
    {
        std::vector<uint8_t> before(MC_THING_SLOTS);
        for (int i = 0; i < MC_THING_SLOTS; i++) before[(size_t)i] = thing_at(i)->cls;
        uint16_t saved = t->mana_owner;
        t->mana_owner = 5;
        CHECK_EQ(skeleton_attack_fire(t, player), 1);
        Thing *p = newest_projectile(before);
        CHECK(p && p->damage == 600);
        if (p) thing_free(p);
        t->mana_owner = saved;
    }
    // creature_attack_volley: mana == total -> 7 shots wanted, clamped to 5; the kind is
    // (rng % 700) / 100: 0 -> 5 x type 0 (damage 400, descriptors 6..2), 1..2 -> 4 x type 9 (800),
    // 3..6 -> 1 x type 3 (8000, descriptor 3)
    {
        int32_t saved_mana = t->mana, saved_total = t->mana_total;
        uint32_t saved_rng = t->rng;
        t->mana = 4000;
        t->mana_total = 4000;
        int seen[3] = {};
        for (uint32_t seed = 1; seed < 40; seed++) {
            std::vector<uint8_t> before(MC_THING_SLOTS);
            for (int i = 0; i < MC_THING_SLOTS; i++) before[(size_t)i] = thing_at(i)->cls;
            t->rng = seed;
            unsigned kind = (mc_lcg(seed) % 700u) / 100u;
            CHECK_EQ(creature_attack_volley(t, player), 1);
            CHECK_EQ(t->rng, mc_lcg(seed));
            int n = 0, type = -1, damage = -1;
            uint32_t desc_lo = 99, desc_hi = 0;
            for (int i = 1; i < MC_THING_SLOTS; i++) {
                Thing *p = thing_at(i);
                if (p->cls != 9 || before[(size_t)i] == 9) continue;
                n++;
                type = p->type;
                damage = p->damage;
                if (p->desc < desc_lo) desc_lo = p->desc;
                if (p->desc > desc_hi) desc_hi = p->desc;
                CHECK_EQ(p->owner, t->owner);
                CHECK_EQ(p->z, t->z + t->ext_h);
                thing_free(p);
            }
            if (kind == 0)      { seen[0]++; CHECK_EQ(n, 5); CHECK_EQ(type, 0); CHECK_EQ(damage, 400); CHECK_EQ(desc_lo, 2u); CHECK_EQ(desc_hi, 6u); }
            else if (kind <= 2) { seen[1]++; CHECK_EQ(n, 4); CHECK_EQ(type, 9); CHECK_EQ(damage, 800); CHECK_EQ(desc_lo, 2u); CHECK_EQ(desc_hi, 5u); }
            else                { seen[2]++; CHECK_EQ(n, 1); CHECK_EQ(type, 3); CHECK_EQ(damage, 8000); CHECK_EQ(desc_lo, 3u); }
        }
        CHECK(seen[0] > 0 && seen[1] > 0 && seen[2] > 0);
        // no mana: one shot of kind 0, no RNG draw
        t->mana = 0;
        t->rng = 5;
        std::vector<uint8_t> before(MC_THING_SLOTS);
        for (int i = 0; i < MC_THING_SLOTS; i++) before[(size_t)i] = thing_at(i)->cls;
        CHECK_EQ(creature_attack_volley(t, player), 1);
        CHECK_EQ(t->rng, 5u);
        int n = 0;
        for (int i = 1; i < MC_THING_SLOTS; i++)
            if (thing_at(i)->cls == 9 && before[(size_t)i] != 9) { n++; thing_free(thing_at(i)); }
        CHECK_EQ(n, 1);
        t->mana = saved_mana; t->mana_total = saved_total; t->rng = saved_rng;
    }
}

static void test_skeleton() {
    Thing *townie = find_creature(0xd, 0x4f);
    CHECK(townie != nullptr);
    if (!townie) return;
    ThingUpdateFn s54 = thing_update_fn(5, 54), s55 = thing_update_fn(5, 55);
    CHECK(s54 && s55);
    if (!s54 || !s55) return;
    // rising (state 54): aux = index % 10 + 0x1d at creation; sprite 0xed when it reaches 0x10; one
    // call after it reached 0 the skeleton walks (state 55, sprite 0xc9, 400 ticks to the first pose)
    Pos pos = *thing_pos(townie);
    pos.x = (uint16_t)(pos.x + 0x200);
    Thing *sk = thing_create(&pos, 5, 9);
    CHECK(sk != nullptr);
    if (!sk) return;
    CHECK_EQ(sk->state, 54);
    int aux0 = sk->aux;
    CHECK_EQ(aux0, thing_index(sk) % 10 + 0x1d);
    CHECK_EQ(sk->sprite, 0xdc);
    for (int k = 0; k < aux0; k++) {
        s54(sk);
        sk->tick++;
        CHECK_EQ(sk->state, 54);
        if (sk->aux == 0x10) CHECK_EQ(sk->sprite, 0xed);
        if (sk->aux > 0x10) CHECK_EQ(sk->sprite, 0xdc);
    }
    CHECK_EQ(sk->aux, 0);
    s54(sk);
    CHECK_EQ(sk->state, 55);
    CHECK_EQ(sk->aux, 0x190);
    CHECK_EQ(sk->sprite, 0xc9);
    CHECK_EQ(sk->speed_cur, sk->speed_base);
    CHECK_EQ(sk->filter_cls, 3);
    CHECK_EQ(sk->filter_type, 0xff);
    CHECK_EQ(sk->castle_size, 0);

    // state 55: awake skeletons hold the countdown at 400; asleep it runs out into the summoning pose
    game_tick_sim();                       // rebuilds the creature lists (the new skeleton included)
    sk->timer_a = 0;
    sk->timer_b = 200;                     // keep the wake timer from re-arming it
    sk->aux = 3;
    sk->castle_size = 0;
    sk->state = 55;
    s55(sk); sk->tick++;
    CHECK_EQ(sk->aux, 2);
    s55(sk); sk->tick++;
    s55(sk); sk->tick++;
    CHECK_EQ(sk->aux, 0);
    CHECK_EQ(sk->castle_size, 1);
    CHECK_EQ(sk->sprite, 0xf5);

    // the pose converts the nearest townie within 0x600 on the think tick whose turn is the townie list
    const MoveDesc *d = mc_move_desc(sk->desc);
    int period = (int16_t)d->think_period;
    CHECK(period > 0 && period * 2 < 256);
    Pos at = *thing_pos(townie);
    at.x = (uint16_t)(at.x + 0x100);
    thing_move_to(sk, &at);
    sk->z = townie->z;
    sk->owner = thing_index(thing_at(g_state->players[g_state->local_player].thing));
    int skeletons = 0;
    for (int i = 1; i < MC_THING_SLOTS; i++) if (is_creature(thing_at(i)) && thing_at(i)->type == 9) skeletons++;
    sk->tick = (uint8_t)(period * 2);      // (tick / period) % 3 == 2: townies
    sk->tick = (uint8_t)(sk->tick + 1);    // not a think tick: nothing
    skeleton_convert_villager(sk);
    CHECK(!(townie->flags & 0x400));
    sk->tick = (uint8_t)period;            // builders' turn: the townie is safe (no builder that near)
    skeleton_convert_villager(sk);
    CHECK(!(townie->flags & 0x400));
    sk->tick = (uint8_t)(period * 2);
    skeleton_convert_villager(sk);
    CHECK(townie->flags & 0x400);
    int after = 0;
    Thing *fresh = nullptr;
    for (int i = 1; i < MC_THING_SLOTS; i++) {
        Thing *o = thing_at(i);
        if (!is_creature(o) || o->type != 9) continue;
        after++;
        if (o->state == 54 && o->x == townie->x && o->y == townie->y) fresh = o;
    }
    CHECK_EQ(after, skeletons + 1);
    CHECK(fresh != nullptr);
    if (fresh) CHECK_EQ(fresh->owner, sk->owner);
    // an awake skeleton breaks the pose off: 50 ticks of cool-down, then the normal sprite and 400
    sk->timer_a = 1;
    skeleton_convert_villager(sk);
    CHECK_EQ(sk->aux, -50);
    sk->timer_a = 0;
    for (int k = 0; k < 49; k++) skeleton_convert_villager(sk);
    CHECK_EQ(sk->aux, -1);
    CHECK_EQ(sk->castle_size, 1);
    skeleton_convert_villager(sk);
    CHECK_EQ(sk->aux, 0x190);
    CHECK_EQ(sk->castle_size, 0);
    CHECK_EQ(sk->sprite, 0xc9);
    // damage in the pose: attack whoever it was (state 56), the next state-55 call sets the attack sprite
    sk->castle_size = 1;
    sk->timer_a = 1;
    Thing *enemy = find_creature(0xe, 0x55);
    sk->damage_slots[0] = { 10, thing_index(enemy) };
    skeleton_convert_villager(sk);
    CHECK_EQ(sk->state, 56);
    CHECK_EQ(sk->target, thing_index(enemy));
}

// Archer (state 25 / 26): only a "wanted" wizard (P+0x210 != 0) is attacked; shooting keeps it wanted.
static void test_archer() {
    Thing *player = thing_at(g_state->players[g_state->local_player].thing);
    ThingUpdateFn s25 = thing_update_fn(5, 25), s26 = thing_update_fn(5, 26);
    CHECK(s25 && s26);
    if (!s25 || !s26) return;
    Pos pos = *thing_pos(player);
    pos.x = (uint16_t)(pos.x + 0x300);
    pos.z = (int16_t)terrain_height_at(&pos);
    Thing *a = thing_create(&pos, 5, 4);
    CHECK(a != nullptr);
    if (!a) return;
    CHECK_EQ(a->state, 0x19);
    game_tick_sim();                        // the lists now hold the archer
    auto face = [&]() {
        a->tick = 0;                        // a think tick of every period
        a->timer_a = 0;
        a->yaw = a->target_yaw = (uint16_t)pos_angle_to(thing_pos(a), thing_pos(player));
    };
    face();
    a->target = 0;
    player_block(player)->timer210 = 0;
    s25(a);
    CHECK_EQ(a->state, 0x19);               // an innocent wizard is left alone
    CHECK_EQ(a->target, 0);
    face();
    player_block(player)->timer210 = 7;
    uint32_t rng = mc_lcg(mc_lcg(mc_lcg(a->rng)));      // wander turn (2 draws) + the sprite draw
    s25(a);
    CHECK_EQ(a->state, 0x1a);
    CHECK_EQ(a->target, thing_index(player));
    CHECK_EQ(a->rng, rng);
    CHECK_EQ(a->speed_cur, 0);
    CHECK_EQ(a->sprite, rng % 20u > 10 ? 1 : 0xce);
    CHECK_EQ(a->filter_cls, 3);
    CHECK_EQ(a->filter_type, player->type);
    // state 26: an arrow on the think tick, and the wizard is wanted for another 200 ticks
    int arrows = 0;
    for (int i = 1; i < MC_THING_SLOTS; i++) if (thing_at(i)->cls == 9 && thing_at(i)->type == 0xd) arrows++;
    face();
    s26(a);
    CHECK_EQ(a->state, 0x1a);
    int after = 0;
    for (int i = 1; i < MC_THING_SLOTS; i++) if (thing_at(i)->cls == 9 && thing_at(i)->type == 0xd) after++;
    CHECK_EQ(after, arrows + 1);
    CHECK_EQ(player_block(player)->timer210, 200);
    // the target dies: back to walking (sprite 0, base speed, filter class 3 / any)
    int32_t health = player->health;
    player->health = -1;
    face();
    s26(a);
    player->health = health;
    CHECK_EQ(a->state, 0x19);
    CHECK_EQ(a->speed_cur, a->speed_base);
    CHECK_EQ(a->sprite, 0);
    CHECK_EQ(a->filter_type, 0xff);
    player_block(player)->timer210 = 0;
}

// ---- 3. the snapshot's creatures --------------------------------------------------------------------

static void test_snapshot_fields(const std::vector<Thing> &snap) {
    const Thing *local = &snap[g_state->players[g_state->local_player].thing];
    int n = 0, awake = 0, dy_exact = 0, dy_near = 0, skel_walk = 0, skel_pose = 0, skel_attack = 0, hold = 0;
    for (int i = 1; i < MC_THING_SLOTS; i++) {
        const Thing *t = &snap[i];
        if (!is_creature(t)) continue;
        n++;
        int base = t->type * 6;
        CHECK((t->state >= base && t->state <= base + 5) || (t->type == 0xc && t->state == 0x4f));
        CHECK(t->timer_a <= 0x10 || t->timer_a == 0xfa || t->state == 0x19);      // wake reload value / dead marker
        CHECK_EQ(t->timer_b, 0);
        if (t->timer_a != 0 && t->timer_a <= 0x10) {
            // cast_ticks = |dy| to the local player when the gate was re-armed (16 - timer_a ticks ago)
            awake++;
            int dy = (int16_t)(local->y - t->y);
            if (dy < 0) dy = -dy;
            int slack = (0x11 - t->timer_a) * (t->speed_base > 0 ? t->speed_base : 0) + 1;
            if (t->cast_ticks == dy) dy_exact++;
            if (t->cast_ticks >= dy - slack && t->cast_ticks <= dy + slack) dy_near++;
            int dx = (int16_t)(local->x - t->x);
            CHECK((int64_t)dx * dx + (int64_t)dy * dy < 0x2400000 + 2 * 0x1800 * slack);
        }
        if (t->type == 9) {
            // skeleton poses: walking 0xc9 at base speed, summoning 0xf5 with the flag, attacking 0xca standing
            if (t->state == 55 && t->castle_size == 0) {
                skel_walk++;
                CHECK_EQ(t->sprite, 0xc9);
                CHECK_EQ(t->speed_cur, t->speed_base);
                CHECK(t->aux >= 0 && t->aux <= 0x190);
                CHECK_EQ(t->filter_cls, 3);
                CHECK_EQ(t->filter_type, 0xff);
                if (t->timer_a != 0) { hold++; CHECK_EQ(t->aux, 0x190); }
            } else if (t->state == 55) {
                skel_pose++;
                CHECK_EQ(t->castle_size, 1);
                CHECK_EQ(t->sprite, 0xf5);
                CHECK(t->aux <= 0 && t->aux >= -50);
            } else if (t->state == 56) {
                skel_attack++;
                CHECK_EQ(t->sprite, 0xca);
                CHECK_EQ(t->speed_cur, 0);
                const Thing *target = &snap[t->target];
                CHECK_EQ(t->filter_cls, target->cls);
                CHECK_EQ(t->filter_type, target->type);
                CHECK(target->owner != t->owner);
            }
        }
        if (t->type == 0xc && t->state == 74) {
            const Thing *w = &snap[t->target];
            CHECK(is_wizard_castle(w));
            CHECK(t->aux >= 0 && t->aux <= 10);
        }
        if (t->type == 0xe) {
            CHECK_EQ(t->state, 85);
            if (t->target != 0) { CHECK(is_wizard_castle(&snap[t->target])); CHECK_EQ(t->speed_cur, t->speed_base); }
            else CHECK(t->speed_cur == t->speed_base || t->speed_cur == (int16_t)t->turn_rate);
        }
    }
    std::printf("snapshot: %d creatures; %d awake, cast_ticks == |dy to the local player| for %d of them (%d within the distance walked since);"
                " skeletons: %d walking (%d awake with the pose countdown held at 400), %d in the summoning pose, %d attacking\n",
                n, awake, dy_exact, dy_near, skel_walk, hold, skel_pose, skel_attack);
    CHECK_EQ(n, 169);
    CHECK_EQ(dy_near, awake);
}

// ---- 4. smoke runs --------------------------------------------------------------------------------

static void smoke_level(int level, int ticks) {
    if (!sim_load_level(level)) { std::printf("level %d failed to load\n", level); g_fail++; return; }
    char what[64];
    std::snprintf(what, sizeof what, "level %d start", level);
    census(what);
    for (int tick = 1; tick <= ticks; tick++) {
        game_tick_sim();
        if (tick % 500 == 0 || tick == ticks) {
            std::snprintf(what, sizeof what, "level %d tick %d", level, tick);
            census(what);
            check_pool(what);
        }
    }
}

static void scan_levels() {
    for (int level = 0; level < 80; level++) {
        if (!sim_load_level(level)) break;
        int by_type[20] = {};
        for (int i = 1; i < MC_THING_SLOTS; i++)
            if (is_creature(thing_at(i)) && thing_at(i)->state != 0x78 && thing_at(i)->type < 20) by_type[thing_at(i)->type]++;
        std::printf("level %2d types:", level);
        for (int k = 0; k < 20; k++) if (by_type[k]) std::printf(" %d:%d", k, by_type[k]);
        std::printf("\n");
    }
}

int main(int argc, char **argv) {
    mc_install_crash_handler();
    g_game_dir = argc > 1 ? argv[1] : MC_DEFAULT_GAME_DIR;
    if (!sim_init(g_game_dir)) { std::printf("sim_init failed\n"); return 2; }
    creatures_register_handlers();
    CHECK(g_hook_creature_wake_tick == creature_wake_tick);
    for (int state = 0; state <= 30; state++) CHECK(thing_update_fn(5, state) != nullptr);
    for (int state = 54; state <= 60; state++) CHECK(thing_update_fn(5, state) != nullptr);
    for (int state = 72; state <= 90; state++) CHECK(thing_update_fn(5, state) != nullptr);
    CHECK(thing_update_fn(5, 0x78) != nullptr);

    if (argc > 2 && std::strcmp(argv[2], "scan") == 0) scan_levels();

    if (!sim_load_snapshot("movie/gam00000.dat", "movie/map00000.dat")) { std::printf("snapshot failed to load\n"); return 2; }
    census("snapshot");
    check_pool("snapshot");
    std::vector<Thing> snap(g_state->things, g_state->things + MC_THING_SLOTS);
    CHECK_EQ(g_state->players[0].tick, 413u);
    test_snapshot_fields(snap);

    // 1. replay
    int same_retail = replay_to_snapshot(snap, false, false);
    int same_recording = replay_to_snapshot(snap, true, true);
    census("level 38 after 412 ticks");
    CHECK(same_retail >= 120);
    CHECK(same_recording >= 125);

    // 2. construction tests on a fresh level 38 (one tick: the players spawn on their first command)
    if (!sim_load_level(38)) { std::printf("level 38 failed to load\n"); return 2; }
    game_tick_sim();
    CHECK(g_state->players[g_state->local_player].thing != 0);
    test_pure();
    test_states();
    test_attacks();
    if (!sim_load_level(38)) { std::printf("level 38 failed to load\n"); return 2; }
    game_tick_sim();
    test_skeleton();
    if (!sim_load_level(38)) { std::printf("level 38 failed to load\n"); return 2; }
    game_tick_sim();
    test_archer();

    // 4. smoke: the movie from the snapshot, then other levels
    thing_dispatch_reset_stats();
    if (demo_open(g_game_dir, 0)) {
        int ticks = 0;
        while (ticks < 2500 && demo_step()) {
            ticks++;
            if (ticks % 500 == 0) {
                char what[64];
                std::snprintf(what, sizeof what, "movie tick %d", ticks);
                census(what);
                check_pool(what);
            }
        }
        CHECK_EQ(ticks, 2500);
        demo_close();
    } else {
        std::printf("movie/mvi00000.dat missing\n");
        g_fail++;
    }
    static const int kLevels[] = { 0, 4, 6, 11 };
    for (int level : kLevels) smoke_level(level, 2000);
    std::printf("handlers dispatched without a port during the smoke runs:\n");
    thing_dispatch_report(stdout);

    std::printf("%s: %d failure(s)\n", g_fail ? "FAILED" : "OK", g_fail);
    return g_fail ? 1 : 0;
}
