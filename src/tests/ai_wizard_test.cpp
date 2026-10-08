// Unit / integration test for ai_wizard.cpp (the computer-controlled wizard, class 3 type 1 state 1).
// argv[1] = game dir.
//
//  1. by construction: the extracted cooldown table, the pure helpers (aim tolerance, think period,
//     signature, rand), threat drift, spell readiness / casting, the threat recorder, dodging, the
//     approach distances, the finders and the flight model, all on the engine's own snapshot
//     (movie/gam00000.dat, 413 ticks into level 38) as the source of real AI wizards;
//  2. the snapshot's three AI players (1..3): every AI field must be producible (modes, target
//     signatures, cooldown bounds, threat values);
//  3. test_replay_to_snapshot of tests/sim_test.cpp, run without and with the AI handler: slot counts
//     per class and the PlayerBlock / Thing fields of players 1..3 against the snapshot;
//  4. the shipped movie (8551 ticks) from the snapshot with the AI active: no crash, the wizards move
//     and cast, pool consistency, dispatch report.
#include "sim.h"
#include "ai_wizard.h"
#include "player.h"
#include "demo.h"
#include "level_features.h"
#include "gen/ai_wizard_tables.h"
#include "crash_handler.h"
#include <cstdio>
#include <cstring>
#include <vector>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)
#define CHECK_EQ(a, b) do { long long va_ = (long long)(a), vb_ = (long long)(b); if (va_ != vb_) { \
    std::printf("FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, va_, vb_); g_fail++; } } while (0)

static const char *kClassName[14] = { "-", "c1", "scenery", "player", "c4", "creature", "c6", "c7", "c8",
                                      "projectile", "effect", "switch", "spell", "c13" };

static bool live(const Thing &t) { return t.cls != 0 && t.cls < 14; }

static bool same_thing(const Thing &a, const Thing &b) {
    const uint8_t *pa = reinterpret_cast<const uint8_t *>(&a), *pb = reinterpret_cast<const uint8_t *>(&b);
    return std::memcmp(pa + 4, pb + 4, 0x10) == 0 && std::memcmp(pa + 0x18, pb + 0x18, sizeof(Thing) - 0x18) == 0;
}

struct Census { int n[14] = {}; int total = 0; };
static Census census(const Thing *things) {
    Census c;
    for (int i = 1; i < MC_THING_SLOTS; i++)
        if (live(things[i])) { c.n[things[i].cls]++; c.total++; }
    return c;
}
static void print_census(const char *what, const Census &c) {
    std::printf("%-30s %4d things:", what, c.total);
    for (int k = 1; k < 14; k++) if (c.n[k]) std::printf(" %s %d", kClassName[k], c.n[k]);
    std::printf("\n");
}

static void check_pool(const char *what) {
    std::vector<int> seen(MC_THING_SLOTS, 0);
    int bad = 0;
    for (int c = 0; c < MC_MAP_CELLS; c++) {
        int guard = 0; unsigned prev = 0;
        for (unsigned i = g_cell_things[c]; i != 0; i = thing_at(i)->cell_next) {
            if (i >= MC_THING_SLOTS || ++guard > MC_THING_SLOTS) { bad++; break; }
            const Thing *t = thing_at(i);
            if (t->cls == 0 || !(t->flags & 4) || t->cell_prev != prev) bad++;
            seen[i]++;
            prev = i;
        }
    }
    for (int i = 1; i < MC_THING_SLOTS; i++) {
        const Thing *t = thing_at(i);
        if (t->cls != 0 && (t->flags & 4) && seen[i] != 1) bad++;
        if (seen[i] > 1) bad++;
    }
    if (bad) { std::printf("FAIL pool consistency (%s): %d problems\n", what, bad); g_fail++; }
}

struct Snapshot {
    std::vector<Thing> things;
    PlayerRec players[8];
};

// The per-tick Config lists as thing_update_all builds them (the AI walks player_list,
// projectile_list, mana_ball_list and creature_lists); a freshly loaded snapshot has none.
static void rebuild_lists() {
    int tails[24] = {};                                  // thing index of the last element per list
    std::memset(g_cfg->creature_lists, 0, sizeof g_cfg->creature_lists);
    g_cfg->player_list = g_cfg->projectile_list = g_cfg->mana_ball_list = g_cfg->wizard_list = 0;
    // slots 0..19 = creature lists, 20 players, 21 projectiles, 22 mana balls, 23 wizard effects
    auto append = [&](int slot, int i) {
        if (!tails[slot]) {
            uint32_t v = (uint32_t)i;
            if (slot < 20) g_cfg->creature_lists[slot] = v;
            else if (slot == 20) g_cfg->player_list = v;
            else if (slot == 21) g_cfg->projectile_list = v;
            else if (slot == 22) g_cfg->mana_ball_list = v;
            else g_cfg->wizard_list = v;
        } else {
            thing_at((unsigned)tails[slot])->next = (uint32_t)i;
        }
        thing_at((unsigned)i)->next = 0;
        tails[slot] = i;
    };
    for (int i = 1; i < MC_THING_SLOTS; i++) {
        Thing *t = thing_at((unsigned)i);
        if (t->cls == 3 && t->health >= 0 && !(t->flags & 0x10)) append(20, i);
        else if (t->cls == 5 && t->health >= 0 && t->state != 0x78 && t->type < 20) append(t->type, i);
        else if (t->cls == 9) append(21, i);
        else if (t->cls == 10 && (t->type == 0x27 || t->type == 0x28)) append(22, i);
        else if (t->cls == 10 && t->type == 0x2d) append(23, i);
    }
}

static bool load_snapshot(Snapshot &s) {
    if (!sim_load_snapshot("movie/gam00000.dat", "movie/map00000.dat")) return false;
    s.things.assign(g_state->things, g_state->things + MC_THING_SLOTS);
    std::memcpy(s.players, g_state->players, sizeof s.players);
    rebuild_lists();
    return true;
}

static Thing *ai_wizard(int player) { return thing_at(g_state->players[player].thing % MC_THING_SLOTS); }
static PlayerBlock *P_of(int player) { return &g_state->players[player].blk; }

static void noop_update(Thing *) {}

// ---- 1. by construction --------------------------------------------------------------------------------
static void test_pure() {
    std::printf("-- pure helpers\n");
    // DAT_000938c4: the 24 cooldown reloads.
    CHECK_EQ(g_ai_spell_cooldown_reload[0], 2);       // fireball
    CHECK_EQ(g_ai_spell_cooldown_reload[1], 1);       // heal
    CHECK_EQ(g_ai_spell_cooldown_reload[2], 32);      // speed-up
    CHECK_EQ(g_ai_spell_cooldown_reload[3], 10);      // possession
    CHECK_EQ(g_ai_spell_cooldown_reload[7], 4);       // meteor
    CHECK_EQ(g_ai_spell_cooldown_reload[8], 400);     // volcano
    CHECK_EQ(g_ai_spell_cooldown_reload[0xf], 1);     // lightning
    CHECK_EQ(g_ai_spell_cooldown_reload[0x10], 40);   // castle
    CHECK_EQ(g_ai_spell_cooldown_reload[0x11], 600);  // skeleton army
    // ((255 - acc) / 4 + 20) degrees in 2048 units.
    CHECK_EQ(ai_aim_tolerance(255), 113);             // 20 deg
    CHECK_EQ(ai_aim_tolerance(0), 472);               // 83 deg
    CHECK_EQ(ai_aim_tolerance(128), 290);             // (31 + 20) deg
    CHECK_EQ(ai_think_period(0), 64);
    CHECK_EQ(ai_think_period(255), 1);
    CHECK_EQ(ai_think_period(128), 32);
    // thing_signature: cls * 0x80 + type + owner in 16 bits.
    Thing t{}; t.cls = 3; t.type = 1; t.owner = 5;
    CHECK_EQ(thing_signature(&t), 3 * 128 + 1 + 5);
    t.cls = 10; t.type = 0x27; t.owner = 900;
    CHECK_EQ(thing_signature(&t), 10 * 128 + 0x27 + 900);
    t.cls = 0xff; t.type = 0xff; t.owner = 1;         // sign-extended bytes
    CHECK_EQ(thing_signature(&t), (uint16_t)(-128 - 1 + 1));
    // ANSI rand() from seed 1: 16838, 5758, 10113 (the textbook sequence).
    g_ai_rand_seed = 1;
    CHECK_EQ(ai_rand(), 16838);
    CHECK_EQ(ai_rand(), 5758);
    CHECK_EQ(ai_rand(), 10113);
    g_ai_rand_seed = 1;
}

// Threat drift of player_ai_wizard_tick on a real AI wizard of the snapshot.
static void test_threat_drift(const Snapshot &snap) {
    std::printf("-- threat drift\n");
    Thing *w = ai_wizard(1);
    PlayerBlock *P = P_of(1);
    int aggr = P->ai_aggression;
    uint16_t *th = player_threat(P, 0);
    th[0] = 0x5000; th[1] = 0;                       // below neutral, no grudge
    uint16_t *th1 = player_threat(P, 1); th1[0] = 0x601a; th1[1] = 0;    // below, clamps
    uint16_t *th2 = player_threat(P, 2); th2[0] = 0x7000; th2[1] = 0;    // above, decays
    uint16_t *th3 = player_threat(P, 3); th3[0] = 0x7000; th3[1] = 1;    // above, grudge holds it
    uint16_t *th4 = player_threat(P, 4); th4[0] = 0x6020; th4[1] = 0;    // above, clamps
    uint16_t *th5 = player_threat(P, 5); th5[0] = 0x601f; th5[1] = 0;    // neutral: untouched
    int16_t cd_before = (int16_t)P->spell_cooldown[0];
    P->spell_cooldown[0] = 5; P->spell_cooldown[1] = 0; P->spell_cooldown[2] = 1;
    P->ai_burst = -3;
    player_ai_wizard_tick(w);
    CHECK_EQ(th[0], 0x5000 + aggr + 1);
    CHECK_EQ(th1[0], 0x601f);
    CHECK_EQ(th2[0], 0x7000 - (0x100 - aggr));
    CHECK_EQ(th3[0], 0x7000);
    CHECK_EQ(th4[0], 0x601f);
    CHECK_EQ(th5[0], 0x601f);
    CHECK_EQ(P->spell_cooldown[0], 4);
    CHECK_EQ(P->spell_cooldown[1], 0);
    CHECK_EQ(P->spell_cooldown[2], 0);
    CHECK_EQ(P->ai_burst, -2);
    (void)cd_before;
    // Regeneration outside the castle: mana_cost = max(total / 2000, 100), health_regen = max / 500.
    CHECK_EQ(w->mana_cost, w->mana_total / 2000 < 100 ? 100 : w->mana_total / 2000);
    CHECK_EQ(P->health_regen, w->max_health / 500);
    (void)snap;
}

// ai_spawn_spells / ai_spell_ready / ai_cast_spell on the snapshot's AI wizard 1 (it owns fireball,
// possession, lightning and the castle spell; the others are acquired here through the want timers).
static void test_spells(const Snapshot &snap) {
    std::printf("-- spell acquisition, readiness and casting\n");
    Thing *w = ai_wizard(1);
    PlayerBlock *P = P_of(1);
    Thing *human = ai_wizard(0);
    std::printf("   wizard 1 owns spells:");
    for (int s = 0; s < 24; s++) if (ai_get_spell_thing(w, s)) std::printf(" %d", s);
    std::printf("\n");
    CHECK(ai_get_spell_thing(w, 0) && ai_get_spell_thing(w, 3) && ai_get_spell_thing(w, 0xf) && ai_get_spell_thing(w, 0x10));
    // Acquisition: a want timer of 3 creates the spell on its third tick, in the first free book slot.
    static const int kWanted[] = { 1, 2, 4, 0xe, 7, 8, 0x11 };
    int free_slots = 0;
    for (int k = 0; k < 24; k++) if (P->spell_slot[k] == 0) free_slots++;
    for (int id : kWanted) P->ai_want_spell[id] = 3;
    P->ai_want_spell[0] = 3;                            // owned already: the timer must not run
    int spells_before = census(g_state->things).n[12];
    ai_spawn_spells(w);
    CHECK_EQ(P->ai_want_spell[1], 2);
    CHECK_EQ(P->ai_want_spell[0], 3);
    CHECK_EQ(census(g_state->things).n[12], spells_before);
    ai_spawn_spells(w);
    ai_spawn_spells(w);
    CHECK_EQ(census(g_state->things).n[12], spells_before + 7);
    CHECK_EQ(P->ai_want_spell[1], 0);
    int filled = 0;
    for (int k = 0; k < 24; k++) {
        int idx = P->spell_slot[k];
        if (idx <= 0 || idx >= MC_THING_SLOTS) continue;
        const Thing *s = thing_at((unsigned)idx);
        bool is_new = false;
        for (int id : kWanted) if (s->type == id) is_new = true;
        if (!is_new) continue;
        filled++;
        CHECK_EQ(s->cls, 12);
        CHECK_EQ(s->caster, thing_index(w));
        CHECK((s->flags & 1) != 0);
        CHECK_EQ(s->x, w->x); CHECK_EQ(s->y, w->y);
    }
    CHECK_EQ(filled, 7);
    CHECK_EQ(free_slots - 7, [&] { int n = 0; for (int k = 0; k < 24; k++) if (P->spell_slot[k] == 0) n++; return n; }());
    player_rebuild_spell_index(w);                      // what the next tick does (P.spell_thing by spell id)
    for (int id : kWanted) CHECK(ai_get_spell_thing(w, id) != nullptr);
    Thing *heal = ai_get_spell_thing(w, 1), *fire = ai_get_spell_thing(w, 0), *shield = ai_get_spell_thing(w, 4),
          *speed = ai_get_spell_thing(w, 2), *castle_spell = ai_get_spell_thing(w, 0x10);
    CHECK(heal && fire && shield && speed && castle_spell);
    if (!(heal && fire && shield && speed && castle_spell)) return;
    for (int s = 0; s < 24; s++) P->spell_cooldown[s] = 0;
    w->mana = w->mana_total;

    // Heal (generic rung 0x14936): cooldown, mana.
    CHECK_EQ(ai_spell_ready(w, 1), 1);
    P->spell_cooldown[1] = 1;
    CHECK_EQ(ai_spell_ready(w, 1), 0);
    P->spell_cooldown[1] = 0;
    w->mana = heal->mana_total - 1;
    CHECK_EQ(ai_spell_ready(w, 1), 0);
    w->mana = w->mana_total;
    CHECK_EQ(ai_cast_spell(w, 1), 1);
    CHECK_EQ(heal->cast_ticks, heal->duration);
    CHECK_EQ(P->spell_cooldown[1], g_ai_spell_cooldown_reload[1]);
    heal->cast_ticks = 0; P->spell_cooldown[1] = 0;

    // Speed-up (0x1465f): no cooldown test at all.
    P->spell_cooldown[2] = 100;
    CHECK_EQ(ai_spell_ready(w, 2), 1);
    CHECK_EQ(ai_cast_spell(w, 2), 1);
    CHECK_EQ(speed->cast_ticks, speed->duration);
    CHECK_EQ(P->spell_cooldown[2], 32);
    speed->cast_ticks = 0; P->spell_cooldown[2] = 0;

    // Shield (0x14691): refused while mid-cast.
    shield->cast_ticks = 3;
    CHECK_EQ(ai_spell_ready(w, 4), 0);
    shield->cast_ticks = 0;
    CHECK_EQ(ai_spell_ready(w, 4), 1);

    // Fireball (0x148a3 / 0x14287): aim within the accuracy tolerance, cast needs < 0xaa and a burst
    // counter >= 0; 8 casts in a row start the recovery.
    w->target = thing_index(human);
    w->unk94 = thing_signature(human);
    int tol = ai_aim_tolerance(P->ai_accuracy);
    w->yaw = 0x100; w->target_yaw = (uint16_t)(0x100 + tol);
    CHECK_EQ(ai_spell_ready(w, 0), 0);
    w->target_yaw = (uint16_t)(0x100 + tol - 1);
    CHECK_EQ(ai_spell_ready(w, 0), 1);
    w->target_yaw = 0x100 + 0xaa;
    if (tol > 0xaa) { CHECK_EQ(ai_spell_ready(w, 0), 1); CHECK_EQ(ai_cast_spell(w, 0), 0); }  // ready but not castable
    w->target_yaw = 0x100;
    P->ai_burst = 0;
    int expected_pitch = pos_pitch_to(thing_pos(w), thing_pos(human));
    int casts = 0;
    for (int i = 0; i < 8; i++) {
        P->spell_cooldown[0] = 0;
        casts += ai_cast_spell(w, 0);
    }
    CHECK_EQ(casts, 8);
    CHECK_EQ(fire->cast_ticks, fire->duration);
    CHECK_EQ(w->pitch, (uint16_t)expected_pitch);
    CHECK_EQ(P->spell_cooldown[0], 2);
    CHECK_EQ(P->ai_burst, (P->ai_reaction - 255) / 8 - 1);
    P->spell_cooldown[0] = 0;
    CHECK_EQ(ai_cast_spell(w, 0), 0);                  // recovering
    CHECK_EQ((w->flags & 0x100), 0u);
    fire->cast_ticks = 0;

    // Castle spell with a castle (0x14783): site must be clear and the aim right; the cast is a
    // normal cast. Without a castle: founds one at Thing.home.
    Thing *castle = thing_at(P->castle % MC_THING_SLOTS);
    CHECK(P->castle != 0 && castle->cls == 3 && castle->type == 2);
    castle_spell->cast_ticks = 0;
    w->target_yaw = w->yaw;
    std::printf("   castle spell: mana_total %d (wizard mana %d / %d)\n", castle_spell->mana_total, w->mana, w->mana_total);
    w->mana = castle_spell->mana_total;             // ready needs Thing.mana >= the spell's mana_total
    w->mana_total = w->mana > w->mana_total ? w->mana : w->mana_total;   // ai_castle_spell_ready reads mana_total
    int clear = castle_footprint_clear(castle);
    std::printf("   castle %d level %d: footprint clear for an upgrade = %d\n", P->castle, castle->aux, clear);
    CHECK_EQ(ai_spell_ready(w, 0x10), clear ? 1 : 0);
    CHECK_EQ(ai_castle_spell_ready(w), clear ? 1 : 0);
    castle_spell->cast_ticks = 1;
    CHECK_EQ(ai_spell_ready(w, 0x10), 0);
    castle_spell->cast_ticks = 0;
    uint16_t saved_castle = P->castle;
    P->castle = 0;
    w->mana = castle_spell->mana_total - 1;
    CHECK_EQ(ai_spell_ready(w, 0x10), 0);
    w->mana = castle_spell->mana_total;
    CHECK_EQ(ai_spell_ready(w, 0x10), 1);
    Pos site = { (uint16_t)0x4000, (uint16_t)0x4000, 0 };
    w->home = site;
    int before = census(g_state->things).n[3];
    CHECK_EQ(ai_cast_spell(w, 0x10), 1);
    CHECK(P->castle != 0 && P->castle != saved_castle);
    Thing *nc = thing_at(P->castle % MC_THING_SLOTS);
    CHECK_EQ(nc->cls, 3); CHECK_EQ(nc->type, 2); CHECK_EQ(nc->owner, w->owner);
    CHECK_EQ(nc->x & 0xff00, 0x4000); CHECK_EQ(nc->y & 0xff00, 0x4000);
    CHECK_EQ(census(g_state->things).n[3], before + 1);
    CHECK_EQ(castle_spell->cast_ticks, 0);              // founding is not a cast
    CHECK_EQ(P->spell_cooldown[0x10], 0);
    thing_mark_delete(nc);
    P->castle = saved_castle;

    // Spells above 0x11: ai_spell_ready accepts them (generic rung), ai_cast_spell refuses them.
    if (Thing *s19 = ai_get_spell_thing(w, 0x13)) {
        CHECK_EQ(ai_spell_ready(w, 0x13), 1);
        CHECK_EQ(ai_cast_spell(w, 0x13), 0);
        CHECK_EQ(s19->cast_ticks, 0);
    }
    CHECK_EQ(ai_spell_ready(w, 0x18), 0);
    CHECK_EQ(ai_can_afford_spell(w, 0x17), 0);          // not owned -> 0 (guarded null in the port)

    // Conserve flag of the attack spell choice: below a quarter of the total nothing is chosen.
    w->mana = w->mana_total / 4 - 1;
    CHECK_EQ(ai_choose_attack_spell(w), 0xff);
    CHECK_EQ(P->ai_conserve, 1);
    w->mana = w->mana_total / 4 + 6000 - 1;
    if (w->mana_total / 4 + 6000 < w->mana_total) { ai_choose_attack_spell(w); CHECK_EQ(P->ai_conserve, 1); }
    w->mana = w->mana_total;
    // rand() is drawn only while the target casts rebound (0x14dc0; per-tick reference, tick 553).
    uint32_t seed0 = g_ai_rand_seed;
    int pick = ai_choose_attack_spell(w);
    CHECK_EQ(P->ai_conserve, 0);
    if (!ai_spell_in_progress(thing_at(w->target % MC_THING_SLOTS), 0xe)) CHECK_EQ(g_ai_rand_seed, seed0);
    std::printf("   attack spell chosen with full mana facing the human: 0x%x; castle attack: 0x%x\n", pick,
                ai_choose_castle_attack_spell(w));
    CHECK(pick == 0x11 || pick == 8 || pick == 7 || pick == 0 || pick == 0xf || pick == 0xff);
    CHECK_EQ(ai_has_any_attack_spell(w), 1);
    (void)snap;
}

// ai_record_threat_from_projectiles with synthetic projectiles.
static void test_threat_recorder() {
    std::printf("-- threat recorder\n");
    Thing *human = ai_wizard(0);
    PlayerBlock *P1 = P_of(1);
    Thing *castle1 = thing_at(P1->castle % MC_THING_SLOTS);
    CHECK(castle1->type == 2);
    uint16_t *th = player_threat(P1, P_of(0)->player_no);
    th[0] = 0x601f; th[1] = 0;
    Pos pos = *thing_pos(human);
    Thing *proj = thing_create(&pos, 9, 0);                  // fireball fired by the human at castle 1
    CHECK(proj != nullptr);
    if (!proj) return;
    proj->owner = human->owner;
    proj->target = thing_index(castle1);
    g_cfg->projectile_list = thing_index(proj);
    proj->next = 0;
    ai_record_threat_from_projectiles();
    CHECK_EQ(th[0], 0x601f + 1000);
    CHECK_EQ(th[1], 0);
    CHECK((proj->flags & 0x2000) != 0);
    ai_record_threat_from_projectiles();                     // scored once only
    CHECK_EQ(th[0], 0x601f + 1000);
    // A castle seed (type 0xa, the castle spell's projectile) adds nothing but is marked (per-tick
    // reference, tick 867: player 3 upgrading its own castle).
    proj->flags &= ~0x2000u; proj->type = 0xa;
    ai_record_threat_from_projectiles();
    CHECK_EQ(th[0], 0x601f + 1000);
    CHECK((proj->flags & 0x2000) != 0);
    proj->type = 0;
    // A meteor (type 3) at the castle: +5000, and the grudge when the threshold (50000 for a castle:
    // the castle's own player block is the dummy block with aggression 0) is passed.
    proj->flags &= ~0x2000u;
    proj->type = 3;
    th[0] = 46000;
    ai_record_threat_from_projectiles();
    CHECK_EQ(th[0], 51000);
    CHECK_EQ(th[1], 1);
    // Saturation at 0xffff.
    proj->flags &= ~0x2000u; th[0] = 0xff00; th[1] = 0;
    ai_record_threat_from_projectiles();
    CHECK_EQ(th[0], 0xffff);
    // The wizard itself as target: +500 / +3000, no grudge.
    Thing *w1 = ai_wizard(1);
    proj->flags &= ~0x2000u; proj->type = 0; proj->target = thing_index(w1); th[0] = 0x601f; th[1] = 0;
    ai_record_threat_from_projectiles();
    CHECK_EQ(th[0], 0x601f + 500);
    CHECK_EQ(th[1], 0);
    proj->flags &= ~0x2000u; proj->type = 0xb;
    ai_record_threat_from_projectiles();
    CHECK_EQ(th[0], 0x601f + 500 + 3000);
    // A possession shot at a mana ball owned by player 1: + mana / 4.
    Pos bp = *thing_pos(w1);
    Thing *ball = thing_create(&bp, 10, 0x27);
    CHECK(ball != nullptr);
    if (ball) {
        ball->mana = 4003; ball->mana_owner = w1->owner;
        proj->flags &= ~0x2000u; proj->type = 1; proj->target = thing_index(ball); th[0] = 1000;
        ai_record_threat_from_projectiles();
        CHECK_EQ(th[0], 1000 + 1000);
        proj->flags &= ~0x2000u; proj->type = 0;                 // only type 1 scores on balls
        ai_record_threat_from_projectiles();
        CHECK_EQ(th[0], 2000);
        thing_mark_delete(ball);
    }
    // A projectile without a player shooter (creature arrow) is ignored and not marked.
    proj->flags &= ~0x2000u; proj->type = 13; proj->owner = 0; proj->target = thing_index(castle1); th[0] = 0x601f;
    ai_record_threat_from_projectiles();
    CHECK_EQ(th[0], 0x601f);
    CHECK_EQ((proj->flags & 0x2000), 0u);

    // Dodge / counter: the projectile homes on wizard 1 within 20 cells -> found, strafe 0x50, shield.
    proj->owner = human->owner; proj->type = 0; proj->target = w1->owner;
    Pos near_pos = *thing_pos(w1); near_pos.x = (uint16_t)(near_pos.x + 0x300);
    thing_move_to(proj, &near_pos);
    CHECK(ai_find_incoming_projectile(w1) == proj);
    PlayerBlock *P = P_of(1);
    P->strafe_speed = 0;
    ai_set_dodge_steer(w1, proj);
    CHECK_EQ(P->strafe_speed, 0x50);
    for (int s = 0; s < 24; s++) P->spell_cooldown[s] = 0;
    w1->mana = w1->mana_total;
    P->ai_want_spell[4] = 1; P->ai_want_spell[0xe] = 1;     // give it shield and rebound
    ai_spawn_spells(w1);
    player_rebuild_spell_index(w1);
    Thing *shield = ai_get_spell_thing(w1, 4), *rebound = ai_get_spell_thing(w1, 0xe);
    CHECK(shield && rebound);
    if (shield) shield->cast_ticks = 0;
    if (rebound) rebound->cast_ticks = 0;
    ai_counter_projectile(w1, proj);
    if (rebound) { CHECK_EQ(rebound->cast_ticks, rebound->duration); rebound->cast_ticks = 0; }
    else if (shield) { CHECK_EQ(shield->cast_ticks, shield->duration); shield->cast_ticks = 0; }
    if (shield) {
        proj->type = 9;                                      // lightning: shield only
        ai_counter_projectile(w1, proj);
        CHECK_EQ(shield->cast_ticks, shield->duration);
        shield->cast_ticks = 0;
        proj->type = 5;                                      // no reaction
        ai_counter_projectile(w1, proj);
        CHECK_EQ(shield->cast_ticks, 0);
    }
    Pos far_pos = *thing_pos(w1); far_pos.x = (uint16_t)(far_pos.x + 0x1500);   // 21 cells
    thing_move_to(proj, &far_pos);
    CHECK(ai_find_incoming_projectile(w1) == nullptr);
    far_pos.x = (uint16_t)(thing_pos(w1)->x + 0x1000);         // 16 cells: found but beyond the 4-cell counter range
    thing_move_to(proj, &far_pos);
    CHECK(ai_find_incoming_projectile(w1) == proj);
    proj->type = 0;
    if (shield) { ai_counter_projectile(w1, proj); CHECK_EQ(shield->cast_ticks, 0); }
    thing_mark_delete(proj);
}

// ai_approach_target, the finders, ai_find_castle_site and ai_wizard_move.
static void test_movement() {
    std::printf("-- approach / finders / flight\n");
    Thing *w = ai_wizard(1);
    PlayerBlock *P = P_of(1);
    Thing *human = ai_wizard(0);
    int d = pos_dist_xyz(thing_pos(w), thing_pos(human));
    P->target_speed = 0; P->accelerating = 0;
    // Make speed-up unavailable so the far branch cruises.
    uint16_t saved_speed_idx = P->spell_thing[2];
    P->spell_thing[2] = 0;
    CHECK_EQ(ai_approach_target(w, human, d, d + 1), 1);        // within near: stop
    CHECK_EQ(P->target_speed, 0); CHECK_EQ(P->accelerating, 1);
    CHECK_EQ(ai_approach_target(w, human, d - 1, d + 1), 0);    // between: cruise
    CHECK_EQ(P->target_speed, w->speed_base); CHECK_EQ(P->accelerating, 1);
    P->target_speed = 0;
    CHECK_EQ(ai_approach_target(w, human, d - 2, d - 1), 0);    // beyond far without speed-up: cruise
    CHECK_EQ(P->target_speed, w->speed_base);
    P->spell_thing[2] = saved_speed_idx;
    Thing *speed = ai_get_spell_thing(w, 2);
    if (speed) {
        speed->cast_ticks = 0; w->mana = w->mana_total; P->target_speed = 0;
        CHECK_EQ(ai_approach_target(w, human, d - 2, d - 1), 0);    // beyond far with speed-up: cast, no cruise
        CHECK_EQ(speed->cast_ticks, speed->duration);
        CHECK_EQ(P->target_speed, 0);
        CHECK_EQ(ai_approach_target(w, human, d - 2, d - 1), 0);    // running: nothing
        CHECK_EQ(P->target_speed, 0);
        speed->cast_ticks = 0;
    }
    // Null target: distance to Thing.home.
    w->home = *thing_pos(w); w->home.x = (uint16_t)(w->home.x + 0x500);
    CHECK_EQ(ai_approach_target(w, nullptr, 0x4ff, 0x2000), 0);
    CHECK_EQ(ai_approach_target(w, nullptr, 0x500, 0x2000), 1);

    // Finders.
    CHECK(player_find_nearest_by_type(w, 1) == nullptr);
    CHECK(player_find_nearest_by_type(w, 4) == nullptr);
    Thing *any = player_find_nearest_by_type(w, 0xff);
    Thing *hum = player_find_nearest_by_type(w, 0);
    CHECK(hum == human);
    CHECK(any != nullptr && any->owner != w->owner);
    Thing *c = player_find_nearest_by_type(w, 2);
    CHECK(c != nullptr && c->type == 2 && c->owner != w->owner);
    if (c) {
        Thing *c2 = player_find_nearest_castle_excl(w, c);
        CHECK(c2 == nullptr || (c2->type == 2 && c2->owner != c->owner && c2->owner != w->owner));
        // The nearest castle by brute force over the pool.
        Thing *bf = nullptr; uint32_t bd = 0xffffffffu;
        for (int i = 1; i < MC_THING_SLOTS; i++) {
            Thing *t = thing_at((unsigned)i);
            if (t->cls != 3 || t->type != 2 || t->owner == w->owner || t->health < 0 || (t->flags & 0x10)) continue;
            uint32_t dd = (uint32_t)pos_dist_sq_xy(thing_pos(w), thing_pos(t));
            if (dd < bd) { bd = dd; bf = t; }
        }
        CHECK(bf == c);
    }
    Thing *wz = player_find_nearest_wizard_excl(w, human);
    CHECK(wz != nullptr && wz->type == 1 && wz->owner != w->owner && wz->owner != human->owner);

    // Target validity: the signature pins class, type and owner.
    w->target = thing_index(human); w->unk94 = thing_signature(human);
    CHECK_EQ(ai_target_valid(w, human), 1);
    if (c) CHECK_EQ(ai_target_valid(w, c), 0);

    // ai_find_castle_site: without a castle and with the (affordable) castle spell the first grid
    // point without a castle within 0x3000 is taken; with a castle nothing happens.
    Pos home0 = w->home;
    CHECK_EQ(ai_find_castle_site(w), 0);
    uint16_t saved_castle = P->castle;
    P->castle = 0;
    w->mana_total = 1000000;
    int found = ai_find_castle_site(w);
    CHECK_EQ(found, 1);
    if (found) {
        std::printf("   castle site for wizard 1 at (%d,%d): cell %d,%d (wizard at cell %d,%d)\n", (int)w->home.x, (int)w->home.y,
                    w->home.x >> 8, w->home.y >> 8, w->x >> 8, w->y >> 8);
        CHECK((w->home.x & 0x3fff) == 0 || (w->home.x & 0xff) == 0);
        // No castle within 0x3000 (Chebyshev) of the site.
        Thing probe = *thing_at(0);
        probe.x = w->home.x; probe.y = w->home.y; probe.owner = w->owner;
        Thing *nc = player_find_nearest_by_type(&probe, 2);
        CHECK(nc == nullptr || pos_dist_chebyshev_xy(thing_pos(nc), thing_pos(&probe)) > 0x3000);
        // The first quadrant corner tested is the wizard's own; it is taken iff it is free.
        probe.x = (uint16_t)(((int16_t)w->x / 0x4000 & 3) << 14); probe.y = (uint16_t)(((int16_t)w->y / 0x4000 & 3) << 14);
        Thing *nc0 = player_find_nearest_by_type(&probe, 2);
        bool first_free = !nc0 || pos_dist_chebyshev_xy(thing_pos(nc0), thing_pos(&probe)) > 0x3000;
        CHECK_EQ(first_free, w->home.x == probe.x && w->home.y == probe.y);
    }
    P->castle = saved_castle;
    w->home = home0;

    // Flight model: one step from rest toward target_yaw.
    const MoveDesc *desc = mc_move_desc(w->desc);
    std::printf("   wizard desc %u: yaw step min %d max %d, clearance %d..%d, z step %d\n", w->desc, (int16_t)desc->unk4,
                (int16_t)desc->turn_min, (int16_t)desc->clear_hi, (int16_t)desc->clear_lo, (int16_t)desc->z_step);
    w->yaw = 0; w->target_yaw = 0x100; w->speed_cur = 0; P->target_speed = 0x50; P->strafe_speed = 0x50;
    Pos p0 = *thing_pos(w);
    int rate = (255 - P->ai_reaction) / 16 + 8;
    int step = 0x100 / rate;
    if (step > (int16_t)desc->turn_min) step = (int16_t)desc->turn_min;
    if (step < (int16_t)desc->unk4) step = (int16_t)desc->unk4;
    ai_wizard_move(w);
    CHECK_EQ(w->yaw, step);
    CHECK_EQ(w->speed_cur, 16);
    CHECK_EQ(P->strafe_speed, 0x4c);
    // speed 0 and strafe 0x50 at yaw + 0x200 (east): the position moved +0x50 in x (yaw 0 = -y, 0x200 = +x).
    CHECK_EQ((int16_t)(w->x - p0.x), 0x50);
    CHECK_EQ((int16_t)(w->y - p0.y), 0);
    // Snap: a small remaining difference is reached exactly, in both directions.
    // (difference 8 / divisor -> 0, raised to the descriptor minimum 5: no snap yet)
    w->yaw = 0x7f0; w->target_yaw = 0x7f8; P->strafe_speed = 0;
    ai_wizard_move(w);
    CHECK_EQ(w->yaw, 0x7f5);
    ai_wizard_move(w);
    CHECK_EQ(w->yaw, 0x7f8);                           // 0x7fa would pass the target: snapped
    w->yaw = 0x100; w->target_yaw = 0xfc;
    ai_wizard_move(w);
    CHECK_EQ(w->yaw, 0xfc);
    // Speed converges on target_speed by 16 per step, strafe decays by 4 toward 0.
    w->speed_cur = 0x48; P->target_speed = 0x50; P->strafe_speed = -2;
    ai_wizard_move(w);
    CHECK_EQ(w->speed_cur, 0x58);
    CHECK_EQ(P->strafe_speed, 2);
    ai_wizard_move(w);
    CHECK_EQ(w->speed_cur, 0x48);
    CHECK_EQ(P->strafe_speed, -2);

    // Mode handlers that end *without* the hover tail (per-tick reference, tick 1255): mode 8 while
    // the burst counter recovers (0x12b15 jl 0x12bc6), mode 7 on a non-think tick (0x129f8 jne 0x12a70).
    const int zstep = (int16_t)desc->z_step;
    w->x = human->x; w->y = (uint16_t)(human->y + 0x100); w->z = (int16_t)(human->z + 0x300);
    w->target = thing_index(human); w->unk94 = thing_signature(human);
    P->ai_burst = -3;
    int16_t z0 = w->z;
    CHECK_EQ(ai_mode8_attack_wizard(w), 1);
    CHECK_EQ(w->z, z0);                                  // no hover while recovering
    CHECK_EQ(P->target_speed, 0);                        // (approach stopped it: in range)
    P->ai_burst = 0;
    int32_t saved_mana = w->mana;
    w->mana = 0;                                         // nothing castable -> hover toward target z + 0x200
    CHECK_EQ(ai_mode8_attack_wizard(w), 1);
    CHECK_EQ(w->z, z0 + zstep);                          // z_step is negative: above the hover height -> sinks
    w->mana = saved_mana;
    if (c) {
        w->x = c->x; w->y = (uint16_t)(c->y + 0x100); w->z = (int16_t)(c->z + 0x300);
        w->target = thing_index(c); w->unk94 = thing_signature(c);
        int period = ai_think_period(P->ai_reaction);
        w->tick = (uint8_t)(period * 2 + 1);             // not a think tick
        z0 = w->z;
        CHECK_EQ(ai_mode7_attack_castle(w), 1);
        CHECK_EQ(w->z, z0);
        w->tick = (uint8_t)(period * 2);                 // think tick, nothing castable -> hover
        w->mana = 0;
        CHECK_EQ(ai_mode7_attack_castle(w), 1);
        CHECK_EQ(w->z, z0 + zstep);
        w->mana = saved_mana;
    }

    // ai_goal_attack_castle_13000 (per-tick reference, tick 8498): a hostile player's castle is a
    // target when *its owner* is more than 0x1e00 away from it (0x130e2..0x130ec), wherever this
    // wizard is. Wizard 1 against player 2's castle, with its own castle empty so the mana rule fails.
    if (c) {
        Thing *owner = thing_at(c->owner % MC_THING_SLOTS);
        Thing *own_castle = thing_at(P->castle % MC_THING_SLOTS);
        int32_t saved_own_mana = own_castle->mana;
        own_castle->mana = 0;
        uint16_t &th = *player_threat(P, player_block(owner)->player_no);
        uint16_t saved_th = th;
        th = 0xffff;                                     // hostile
        uint16_t ox = owner->x, oy = owner->y;
        w->x = c->x; w->y = (uint16_t)(c->y + 0x800);   // this wizard close to the castle in both cases
        owner->x = c->x; owner->y = (uint16_t)(c->y + 0x1000);   // owner at home: dist^2 0x1000000
        w->target = 0;
        CHECK_EQ(ai_goal_attack_castle(w), 0);
        owner->y = (uint16_t)(c->y + 0x2000);            // owner away: dist^2 0x4000000 > 0x3840000
        CHECK_EQ(ai_goal_attack_castle(w), 1);
        CHECK_EQ(w->target, thing_index(c));
        CHECK_EQ(w->unk94, thing_signature(c));
        th = 0x601f;                                     // not hostile, own castle poorer -> no target
        w->target = 0;
        CHECK_EQ(ai_goal_attack_castle(w), 0);
        th = saved_th; owner->x = ox; owner->y = oy; own_castle->mana = saved_own_mana;
    }
}

// ---- 2. the snapshot's AI players ----------------------------------------------------------------------
static const char *mode_name(int m) {
    switch (m) {
    case 0: return "choose"; case 1: return "upgrade castle"; case 3: return "fly to site"; case 4: return "approach";
    case 6: return "collect mana"; case 7: return "attack castle"; case 8: return "attack wizard"; case 9: return "attack type3";
    case 0xb: return "return home"; case 0xc: return "idle"; case 0xd: return "hunt creature"; default: return "?";
    }
}

static void test_snapshot_fields(const Snapshot &snap) {
    std::printf("-- snapshot AI players (tick %u)\n", snap.players[0].tick);
    for (int p = 1; p <= 3; p++) {
        const PlayerRec &r = snap.players[p];
        const PlayerBlock &P = r.blk;
        const Thing &w = snap.things[r.thing % MC_THING_SLOTS];
        CHECK_EQ(r.is_computer, 1);
        CHECK_EQ(w.cls, 3); CHECK_EQ(w.type, 1); CHECK_EQ(w.state, 1);
        std::printf("   player %d thing %d: mode 0x%x (%s), target %d sig 0x%x, home (%d,%d,%d), cell %d,%d z %d yaw %d\n",
                    p, r.thing, P.ai_mode, mode_name(P.ai_mode), w.target, w.unk94, (int)w.home.x, (int)w.home.y, w.home.z,
                    w.x >> 8, w.y >> 8, w.z, w.yaw);
        std::printf("      aggr %d acc %d react %d; burst %d conserve %d; target_speed %d accel %d strafe %d; invuln %d regen %d; "
                    "castle %d level %d; mana %d / %d, health %d / %d, aim %d\n",
                    P.ai_aggression, P.ai_accuracy, P.ai_reaction, P.ai_burst, P.ai_conserve, P.target_speed, P.accelerating,
                    P.strafe_speed, P.invuln_timer, P.health_regen, P.castle, P.castle_level, w.mana, w.mana_total, w.health,
                    w.max_health, P.aim_charge);
        std::printf("      threat:");
        for (int k = 0; k < 8; k++) {
            const uint16_t *th = reinterpret_cast<const uint16_t *>(reinterpret_cast<const uint8_t *>(&P) + 0x1cc + 8 * k);
            std::printf(" %04x/%d", th[0], th[1]);
        }
        std::printf("\n      cooldown:");
        for (int s = 0; s < 24; s++) if (P.spell_cooldown[s]) std::printf(" [%d]=%d", s, P.spell_cooldown[s]);
        std::printf("  want:");
        for (int s = 0; s < 24; s++) if (P.ai_want_spell[s]) std::printf(" [%d]=%d", s, P.ai_want_spell[s]);
        std::printf("\n");
        // Every value must be producible by the translated code.
        int m = P.ai_mode;
        CHECK(m == 0 || m == 1 || m == 3 || m == 4 || m == 6 || m == 7 || m == 8 || m == 9 || m == 0xb || m == 0xc || m == 0xd);
        for (int s = 0; s < 24; s++) CHECK(P.spell_cooldown[s] <= g_ai_spell_cooldown_reload[s]);
        CHECK(P.ai_burst < 8);
        CHECK(P.ai_burst >= (P.ai_reaction - 255) / 8 - 1);
        CHECK(P.ai_conserve == 0 || P.ai_conserve == 1);
        CHECK(P.target_speed == 0 || P.target_speed == w.speed_base);
        CHECK(P.accelerating == 0 || P.accelerating == 1);
        CHECK(P.strafe_speed >= -0x50 && P.strafe_speed <= 0x50);
        CHECK(P.health_regen == w.max_health / 500 || P.health_regen == w.max_health / 200);
        CHECK(w.mana_cost == (w.mana_total / 2000 < 100 ? 100 : w.mana_total / 2000) ||
              w.mana_cost == (w.mana_total / 200 < 1000 ? 1000 : w.mana_total / 200));
        // The target's signature is the signature of the Thing in the slot (or the slot was reused).
        if (w.target != 0) {
            const Thing &tg = snap.things[w.target % MC_THING_SLOTS];
            bool valid = thing_signature(&tg) == w.unk94;
            std::printf("      target %d is class %d type %d owner %d state %d: signature %s\n", w.target, tg.cls, tg.type,
                        tg.owner, tg.state, valid ? "matches" : "stale");
        }
        for (int k = 0; k < 8; k++) {
            const uint16_t *th = reinterpret_cast<const uint16_t *>(reinterpret_cast<const uint8_t *>(&P) + 0x1cc + 8 * k);
            CHECK(th[1] == 0 || th[1] == 1);
        }
    }
}

// ---- 3. the 412 ticks before the snapshot ----------------------------------------------------------------
struct FieldCmp { const char *name; bool same[3]; };

static void compare_ai_players(const Snapshot &snap, const char *label) {
    struct F { const char *name; long long (*get)(const PlayerBlock &, const Thing &); };
    static const F fields[] = {
        { "ai_mode",       [](const PlayerBlock &P, const Thing &) -> long long { return P.ai_mode; } },
        { "target_speed",  [](const PlayerBlock &P, const Thing &) -> long long { return P.target_speed; } },
        { "accelerating",  [](const PlayerBlock &P, const Thing &) -> long long { return P.accelerating; } },
        { "strafe_speed",  [](const PlayerBlock &P, const Thing &) -> long long { return P.strafe_speed; } },
        { "invuln_timer",  [](const PlayerBlock &P, const Thing &) -> long long { return P.invuln_timer; } },
        { "health_regen",  [](const PlayerBlock &P, const Thing &) -> long long { return P.health_regen; } },
        { "aim_charge",    [](const PlayerBlock &P, const Thing &) -> long long { return P.aim_charge; } },
        { "countdown15f",  [](const PlayerBlock &P, const Thing &) -> long long { return P.countdown15f; } },
        { "ai_burst",      [](const PlayerBlock &P, const Thing &) -> long long { return P.ai_burst; } },
        { "ai_conserve",   [](const PlayerBlock &P, const Thing &) -> long long { return P.ai_conserve; } },
        { "castle",        [](const PlayerBlock &P, const Thing &) -> long long { return P.castle; } },
        { "castle_level",  [](const PlayerBlock &P, const Thing &) -> long long { return P.castle_level; } },
        { "P.mana",        [](const PlayerBlock &P, const Thing &) -> long long { return P.mana; } },
        { "kills",         [](const PlayerBlock &P, const Thing &) -> long long { return P.kills; } },
        { "shots/hits",    [](const PlayerBlock &P, const Thing &) -> long long { return P.shots * 100000LL + P.hits; } },
        { "threat[8]",     [](const PlayerBlock &P, const Thing &) -> long long {
              long long h = 0; for (int k = 0; k < 8; k++) h = h * 65537 + *player_threat(const_cast<PlayerBlock *>(&P), k); return h; } },
        { "grudge[8]",     [](const PlayerBlock &P, const Thing &) -> long long {
              long long h = 0; for (int k = 0; k < 8; k++) h = h * 3 + *(player_threat(const_cast<PlayerBlock *>(&P), k) + 1); return h; } },
        { "cooldown[24]",  [](const PlayerBlock &P, const Thing &) -> long long {
              long long h = 0; for (int k = 0; k < 24; k++) h = h * 1009 + P.spell_cooldown[k]; return h; } },
        { "want[24]",      [](const PlayerBlock &P, const Thing &) -> long long {
              long long h = 0; for (int k = 0; k < 24; k++) h = h * 1009 + P.ai_want_spell[k]; return h; } },
        { "spell_thing[24]", [](const PlayerBlock &P, const Thing &) -> long long {
              long long h = 0; for (int k = 0; k < 24; k++) h = h * 1009 + P.spell_thing[k]; return h; } },
        { "T.x,y",         [](const PlayerBlock &, const Thing &t) -> long long { return t.x * 65536LL + t.y; } },
        { "T.z",           [](const PlayerBlock &, const Thing &t) -> long long { return t.z; } },
        { "T.yaw",         [](const PlayerBlock &, const Thing &t) -> long long { return t.yaw; } },
        { "T.pitch",       [](const PlayerBlock &, const Thing &t) -> long long { return t.pitch; } },
        { "T.target_yaw",  [](const PlayerBlock &, const Thing &t) -> long long { return t.target_yaw; } },
        { "T.target/sig",  [](const PlayerBlock &, const Thing &t) -> long long { return t.target * 65536LL + t.unk94; } },
        { "T.home",        [](const PlayerBlock &, const Thing &t) -> long long { return (t.home.x * 65536LL + t.home.y) * 65536LL + (uint16_t)t.home.z; } },
        { "T.mana",        [](const PlayerBlock &, const Thing &t) -> long long { return t.mana; } },
        { "T.mana_total",  [](const PlayerBlock &, const Thing &t) -> long long { return t.mana_total; } },
        { "T.mana_cost",   [](const PlayerBlock &, const Thing &t) -> long long { return t.mana_cost; } },
        { "T.health",      [](const PlayerBlock &, const Thing &t) -> long long { return t.health; } },
        { "T.speed_cur",   [](const PlayerBlock &, const Thing &t) -> long long { return t.speed_cur; } },
        { "T.flags",       [](const PlayerBlock &, const Thing &t) -> long long { return t.flags; } },
        { "T.state",       [](const PlayerBlock &, const Thing &t) -> long long { return t.state; } },
    };
    std::printf("AI players 1..3 against the snapshot (%s):\n", label);
    int total = 0, same_all = 0;
    for (const F &f : fields) {
        std::printf("  %-16s", f.name);
        for (int p = 1; p <= 3; p++) {
            const PlayerBlock &S = snap.players[p].blk, &O = g_state->players[p].blk;
            const Thing &st = snap.things[snap.players[p].thing % MC_THING_SLOTS];
            const Thing &ot = g_state->things[g_state->players[p].thing % MC_THING_SLOTS];
            bool same = f.get(S, st) == f.get(O, ot);
            total++; same_all += same;
            std::printf(" %s", same ? "  =  " : "  x  ");
        }
        std::printf("\n");
    }
    std::printf("  %d of %d fields equal\n", same_all, total);
    for (int p = 1; p <= 3; p++) {
        const Thing &st = snap.things[snap.players[p].thing % MC_THING_SLOTS];
        const Thing &ot = g_state->things[g_state->players[p].thing % MC_THING_SLOTS];
        std::printf("  player %d: port mode 0x%x cell %d,%d z %d mana %d health %d | snapshot mode 0x%x cell %d,%d z %d mana %d health %d\n",
                    p, g_state->players[p].blk.ai_mode, ot.x >> 8, ot.y >> 8, ot.z, ot.mana, ot.health,
                    snap.players[p].blk.ai_mode, st.x >> 8, st.y >> 8, st.z, st.mana, st.health);
    }
}

static void test_replay_to_snapshot(const Snapshot &snap, bool with_ai, int same_out[14]) {
    void (*saved_input)() = g_hook_player_local_input;
    g_hook_player_local_input = nullptr;            // nothing is known about the inputs before the recording
    g_video_mode_flags = 1;                         // the recording ran in 320x200 (level_features.h castle_footprint)
    g_cfg->flags = 0; g_cfg->paused = 0;
    if (with_ai) {
        ai_wizard_register_handlers();
    } else {
        thing_register_update(0x11de0, noop_update);
        g_hook_ai_record_threat = nullptr;
    }
    g_ai_rand_seed = 1;
    CHECK(sim_load_level(38));
    thing_dispatch_reset_stats();
    for (int guard = 0; g_state->players[0].tick < 389 && guard < 1000; guard++) game_tick_sim();
    g_state->commands[0].cmd = 0x1e; g_state->commands[0].arg = 1;   // "access all spells" on tick 390
    game_tick_sim();
    for (int guard = 0; g_state->players[0].tick < 412 && guard < 1000; guard++) game_tick_sim();
    check_pool(with_ai ? "replay with AI" : "replay without AI");

    char label[64];
    std::snprintf(label, sizeof label, "port, level 38 + 412 ticks, AI %s", with_ai ? "on" : "off");
    print_census("snapshot", census(snap.things.data()));
    print_census(label, census(g_state->things));
    int same[14] = {}, same_kind[14] = {}, total[14] = {};
    for (int i = 1; i < MC_THING_SLOTS; i++) {
        const Thing &s = snap.things[i], &o = g_state->things[i];
        if (!live(s)) continue;
        total[s.cls]++;
        if (o.cls == s.cls && o.type == s.type) same_kind[s.cls]++;
        if (same_thing(s, o)) same[s.cls]++;
    }
    int all_same = 0, all_total = 0;
    std::printf("slot-by-slot against the snapshot (identical bytes except list links / same class+type / snapshot things):\n");
    for (int k = 1; k < 14; k++) {
        if (!total[k]) continue;
        std::printf("  %-10s %3d / %3d / %3d\n", kClassName[k], same[k], same_kind[k], total[k]);
        all_same += same[k]; all_total += total[k];
        same_out[k] = same[k];
    }
    std::printf("  %-10s %3d /     / %3d\n", "all", all_same, all_total);
    same_out[0] = all_same;
    int missing = thing_dispatch_report(nullptr);
    std::printf("handlers dispatched without a port: %d\n", missing);
    if (missing) thing_dispatch_report(stdout);
    compare_ai_players(snap, with_ai ? "AI on" : "AI off");
    if (with_ai) {
        CHECK(same[2] == total[2]);                 // every tree
        CHECK(same[5] >= 120);                      // creatures (the sim_test floor)
    }
    g_video_mode_flags = 8;
    g_hook_player_local_input = saved_input;
}

// ---- 4. the shipped recording with the AI active -------------------------------------------------------
static void test_movie(const char *game_dir) {
    std::printf("-- movie 0 from the snapshot with the AI active\n");
    ai_wizard_register_handlers();
    g_ai_rand_seed = 1;
    g_cfg->flags = 0; g_cfg->paused = 0;
    uint16_t saved_mode = g_video_mode_flags;
    sim_prepare_movie();
    if (!demo_open(game_dir, 0)) { std::printf("movie 0 missing, playback skipped\n"); g_video_mode_flags = saved_mode; return; }
    thing_dispatch_reset_stats();
    int ticks = 0;
    bool more = true;
    struct Stats { int moved = 0, cast_ticks = 0, casts = 0, modes[16] = {}, deaths = 0, respawns = 0, dodges = 0, castles_founded = 0;
                   long long path = 0; int min_cell_x = 256, max_cell_x = -1, min_cell_y = 256, max_cell_y = -1; uint16_t last_cast_state[24] = {}; } st[4];
    Pos last[4] = {};
    uint8_t last_state[4] = {};
    uint16_t last_castle[4] = {};
    int proj_by_ai = 0;
    std::vector<uint8_t> seen_proj(MC_THING_SLOTS, 0);
    while (more && ticks < 20000) {
        more = demo_step();
        ticks++;
        if (ticks == 1) {
            for (int p = 1; p <= 3; p++) { last[p] = *thing_pos(ai_wizard(p)); last_state[p] = ai_wizard(p)->state; last_castle[p] = P_of(p)->castle; }
        }
        for (int p = 1; p <= 3 && p < g_state->player_count; p++) {
            Thing *w = ai_wizard(p);
            PlayerBlock *P = P_of(p);
            Stats &s = st[p];
            s.modes[P->ai_mode & 15]++;
            if (w->x != last[p].x || w->y != last[p].y) {
                s.moved++;
                int dx = (int16_t)(w->x - last[p].x), dy = (int16_t)(w->y - last[p].y);
                if (dx < 0) dx = -dx; if (dy < 0) dy = -dy;
                if (dx < 0x1000 && dy < 0x1000) s.path += dx + dy;
            }
            int cx = w->x >> 8, cy = w->y >> 8;
            if (cx < s.min_cell_x) s.min_cell_x = cx; if (cx > s.max_cell_x) s.max_cell_x = cx;
            if (cy < s.min_cell_y) s.min_cell_y = cy; if (cy > s.max_cell_y) s.max_cell_y = cy;
            last[p] = *thing_pos(w);
            bool casting = false;
            for (int k = 0; k < 24; k++) {
                Thing *sp = ai_get_spell_thing(w, k);
                int ct = sp ? sp->cast_ticks : 0;
                if (ct > 0) casting = true;
                if (ct > 0 && s.last_cast_state[k] == 0) s.casts++;   // the handler already counted the launch tick down
                s.last_cast_state[k] = (uint16_t)(ct > 0 ? 1 : 0);
            }
            if (casting) s.cast_ticks++;
            if (w->state != last_state[p]) {
                if (w->state == 2) s.deaths++;
                if (w->state == 1 && last_state[p] != 1) s.respawns++;
                last_state[p] = w->state;
            }
            if (P->strafe_speed == 0x50) s.dodges++;
            if (P->castle != 0 && last_castle[p] == 0) s.castles_founded++;
            last_castle[p] = P->castle;
        }
        for (int i = 1; i < MC_THING_SLOTS; i++) {
            const Thing *t = thing_at(i);
            if (t->cls != 9) { seen_proj[i] = 0; continue; }
            if (seen_proj[i]) continue;
            seen_proj[i] = 1;
            for (int p = 1; p <= 3; p++) if (t->owner == ai_wizard(p)->owner) proj_by_ai++;
        }
        if (ticks == 1 || ticks % 2000 == 0 || !more) {
            char label[64];
            std::snprintf(label, sizeof label, "movie tick %d", ticks);
            print_census(label, census(g_state->things));
            check_pool(label);
        }
    }
    g_video_mode_flags = saved_mode;
    std::printf("movie 0: %d ticks played, %ld / %ld packets\n", ticks, demo_packets_read(), demo_packets_total());
    CHECK(demo_packets_read() == demo_packets_total());
    CHECK_EQ(ticks, 8551);
    for (int p = 0; p < g_state->player_count && p < 8; p++) {
        const PlayerRec &r = g_state->players[p];
        const Thing *t = thing_at(r.thing % MC_THING_SLOTS);
        std::printf("  player %d: thing %d at cell %d,%d z=%d health %d / %d, mana %d, state %d, mode 0x%x\n", p, r.thing,
                    t->x >> 8, t->y >> 8, t->z, (int)t->health, (int)t->max_health, (int)t->mana, t->state, r.blk.ai_mode);
    }
    for (int p = 1; p <= 3; p++) {
        const Stats &s = st[p];
        std::printf("  AI %d: moved in %d ticks, path %lld units (%lld cells), cells x %d..%d y %d..%d, %d casts started, casting in %d ticks, "
                    "%d deaths, %d respawns, dodging in %d ticks, %d castles founded\n", p, s.moved, s.path, s.path / 256,
                    s.min_cell_x, s.max_cell_x, s.min_cell_y, s.max_cell_y, s.casts, s.cast_ticks, s.deaths, s.respawns, s.dodges,
                    s.castles_founded);
        std::printf("        modes:");
        for (int m = 0; m < 16; m++) if (s.modes[m]) std::printf(" 0x%x:%d", m, s.modes[m]);
        std::printf("\n");
        CHECK(s.moved > 100);
        CHECK(s.casts > 0);
    }
    std::printf("  projectiles owned by AI wizards during the movie: %d\n", proj_by_ai);
    int missing = thing_dispatch_report(nullptr);
    std::printf("handlers dispatched without a port during the movie: %d\n", missing);
    if (missing) thing_dispatch_report(stdout);
    demo_close();
}

int main(int argc, char **argv) {
    mc_install_crash_handler();
    setvbuf(stdout, nullptr, _IONBF, 0);
    const char *game_dir = argc > 1 ? argv[1] : MC_DEFAULT_GAME_DIR;
    if (!sim_init(game_dir)) { std::printf("sim_init failed\n"); return 2; }
    sim_register_gameplay();
    CHECK_EQ(thing_register_update(0x11de0, player_type1_s1_update), 1);
    CHECK(g_hook_ai_record_threat == ai_record_threat_from_projectiles);

    Snapshot snap;
    if (!load_snapshot(snap)) { std::printf("snapshot failed to load\n"); return 2; }

    test_pure();
    test_snapshot_fields(snap);
    test_threat_drift(snap);
    CHECK(load_snapshot(snap));
    test_spells(snap);
    CHECK(load_snapshot(snap));
    test_threat_recorder();
    CHECK(load_snapshot(snap));
    test_movement();

    int same_off[14] = {}, same_on[14] = {};
    CHECK(load_snapshot(snap));
    test_replay_to_snapshot(snap, false, same_off);
    test_replay_to_snapshot(snap, true, same_on);
    std::printf("identical slots per class, AI off -> on:");
    for (int k = 1; k < 14; k++) if (same_off[k] || same_on[k]) std::printf(" %s %d -> %d", kClassName[k], same_off[k], same_on[k]);
    std::printf("; all %d -> %d\n", same_off[0], same_on[0]);

    test_movie(game_dir);

    std::printf("%s: %d failure(s)\n", g_fail ? "FAILED" : "OK", g_fail);
    return g_fail ? 1 : 0;
}
