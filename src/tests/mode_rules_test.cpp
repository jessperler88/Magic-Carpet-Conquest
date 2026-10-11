// Port round 12 task B (docs/analysis/port_r12_rules.md): the match rules v1 of a Conquest run, headless on a small
// generated world (256 cells, MODE_PARAM_GENERATE), scripted by direct state changes between ticks and debug packets.
//
//  1. sides: same_side / mode_same_side_records without teams (exactly a == b) and with 2 v 2 teams (the wizards,
//     their castles, projectiles and effects; wild creatures are nobody's ally);
//  2. allies: area damage, thing_try_damage and the projectile collision search skip an ally, hit an enemy;
//     a wizard inside an ally's castle does not desecrate it;
//  3. respawn: a human with a castle respawns at the castle only after (respawn_s + respawn_level_s * level) and the
//     respawn command (0xf); a bot respawns by itself when the timer runs out;
//  4. elimination: a dead wizard without a castle is eliminated (and stays dead), the command 0xf does nothing;
//  5. desecration: an enemy wizard in the courtyard channels; leaving resets; the full channel razes the castle
//     (every level, mana spilled), castles_razed++; the owner, alive, is castle-less (not eliminated);
//  6. win conditions: elimination, mana dominance, timer, debug win / lose, MATCH_WIN_NONE; the level ends for every
//     record on one tick (winners status 10, the rest 8) MODE_MATCH_END_DELAY_TICKS after the end; places, stats;
//  7. determinism: the same scripted run twice gives the same checksums every tick.
#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "sim.h"
#include "mode.h"
#include "mode_level.h"
#include "mode_rules.h"
#include "thing.h"
#include "player.h"
#include "spatial.h"
#include "net.h"
#include "world.h"
#include "debug_cmd.h"
#include "mc_globals.h"
#include "crash_handler.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static std::string g_game;
static constexpr int TPS = MODE_TICKS_PER_SECOND;

static ModeParams params(uint32_t seed, uint32_t bots) {
    ModeParams p;
    p.seed = seed;
    p.bots = bots;
    p.humans = 1;
    p.world_size = 256;
    p.flags = MODE_PARAM_DEBUG | MODE_PARAM_GENERATE;
    return p;
}
static bool start(const ModeParams &p) {
    mode_stop_run();
    g_cfg->flags = 0;
    g_cfg->paused = 0;
    if (!mode_start_run(g_game.c_str(), GAME_MODE_CONQUEST, p)) return false;
    return sim_load_level(MODE_LEVEL_INDEX);
}
static Thing *wizard(int p) { return thing_at(thing_wrap(g_state->players[p].thing)); }
static Thing *castle(int p) {
    const unsigned c = g_state->players[p].blk.castle;
    return c ? thing_at(thing_wrap(c)) : nullptr;
}
static ModePlayer &mp(int p) { return g_mode.players[p]; }
static void god(int p, bool on) {
    if (on) mp(p).flags |= MODE_PLAYER_GOD; else mp(p).flags &= ~MODE_PLAYER_GOD;
}
static void tick(int n = 1) { for (int i = 0; i < n; i++) game_tick_sim(); }
// The wizard dies: out of its castle (inside it a wizard is invulnerable), invulnerability off, health -1 (a computer
// wizard dies through player_apply_hits, which returns 2 for health < 0).
static void place(Thing *t, const Pos &at, int above = 0x100);
static void kill(int p) {
    god(p, false);
    Thing *w = wizard(p);
    Pos away = *thing_pos(w);
    away.x = away.x + 0x2000;
    place(w, away);
    player_block(w)->invuln_timer = 0;
    w->health = -1;
}
// Destroys the castle of p level by level (the game's own destruction: health -1 each tick).
static void drop_castle(int p) {
    for (int i = 0; i < 600 && castle(p); i++) {
        castle(p)->health = -1;
        game_tick_sim();
    }
}
// Ticks until the wizard of p lies dead (state 3); -1 when it does not within the limit.
static int wait_dead(int p, int limit = 400) {
    for (int i = 0; i < limit; i++) {
        if (wizard(p)->state == 3) return i;
        tick();
    }
    return -1;
}
static void place(Thing *t, const Pos &at, int above) {
    Pos q = at;
    q.z = (int16_t)(terrain_height_at(&q) + above);
    thing_move_to(t, &q);
}

// A castle of `levels` for player p at its wizard's position, built as player_spawn builds a computer wizard's start
// castle (one footprint stamp per level, aux = levels - 1, the build sequencer adds the last level), then run until it
// stands (state 4).
static Thing *give_castle(int p, int levels, int32_t mana = -1) {
    Thing *t = wizard(p);
    PlayerBlock *P = &g_state->players[p].blk;
    Thing *c = thing_create(thing_pos(t), 3, 2);
    if (!c) return nullptr;
    c->owner = t->owner;
    P->castle = thing_index(c);
    for (int lvl = 0; lvl < levels; lvl++) {
        Thing *s0 = thing_at(0);
        s0->x = c->home.x; s0->y = c->home.y; s0->z = c->home.z;
        s0->type = 0;
        s0->aux = 0;
        s0->owner = c->owner;
        s0->castle_size = (uint8_t)lvl;
        if (g_hook_castle_stamp_footprint) g_hook_castle_stamp_footprint(s0);
    }
    c->aux = (int16_t)(levels - 1);
    if (g_hook_thing_set_castle_extents) g_hook_thing_set_castle_extents(c, c->aux);
    castle_set_level_stats(c);
    c->mana = c->mana_total;
    for (int i = 0; i < 200 && c->state != 4; i++) tick();
    if (mana >= 0) c->mana = mana;
    return c;
}

// ---- 1 + 2: sides and allies ------------------------------------------------------------------------------------
static void test_sides() {
    // without teams
    ModeParams pa = params(11, 3);
    pa.match.win = MATCH_WIN_NONE;
    CHECK(start(pa));
    tick(2);
    CHECK(!g_mode_teams);
    for (int p = 0; p < 4; p++) {
        CHECK(mp(p).flags & MODE_PLAYER_IN_MATCH);
        CHECK(((mp(p).flags & MODE_PLAYER_BOT) != 0) == (p != 0));
        CHECK(mp(p).side == p);
    }
    CHECK(!(mp(4).flags & MODE_PLAYER_IN_MATCH));
    const int w0 = g_state->players[0].thing, w1 = g_state->players[1].thing;
    CHECK(same_side(w0, w0) && !same_side(w0, w1));
    CHECK(mode_same_side_records(2, 2) && !mode_same_side_records(0, 1));

    // 2 v 2
    ModeParams pt = params(11, 3);
    pt.match.win = MATCH_WIN_NONE;
    pt.match.team[0] = 1; pt.match.team[1] = 1; pt.match.team[2] = 2; pt.match.team[3] = 2;
    CHECK(start(pt));
    tick(2);
    CHECK(g_mode_teams);
    CHECK(mp(0).side == mp(1).side && mp(2).side == mp(3).side && mp(0).side != mp(2).side);
    CHECK(mode_same_side_records(0, 1) && !mode_same_side_records(1, 2));
    {   // the sides come back with a MODE chunk (savestate / movie snapshot): g_mode_teams is derived, not stored
        std::vector<uint8_t> blob(mode_serialised_size());
        mode_serialise(blob.data());
        g_mode_teams = false;
        g_hook_mode_side_of_owner = nullptr;
        CHECK(mode_deserialise(blob.data(), blob.size()));
        CHECK(g_mode_teams && g_hook_mode_side_of_owner != nullptr);
    }
    for (int p = 0; p < 4; p++) god(p, true);
    Thing *a0 = wizard(0), *a1 = wizard(1), *e2 = wizard(2);
    CHECK(same_side(a0->owner, a1->owner) && !same_side(a0->owner, e2->owner));
    // a creature (wild: owns itself) is nobody's ally
    int wild = 0;
    for (int i = 1; i < thing_pool_slots() && !wild; i++)
        if (thing_at(i)->cls == 5 && thing_at(i)->owner == i) wild = i;
    CHECK(wild != 0);
    CHECK(!same_side(wild, a0->owner) && !same_side(a0->owner, wild));
    // a castle of the ally counts as own
    Thing *c1 = give_castle(1, 1);
    CHECK(c1 && same_side(c1->owner, a0->owner) && !same_side(c1->owner, e2->owner));

    // area damage: an effect of record 1 (ally) next to wizard 0 hurts nobody of team 1, the same effect of record 2 does
    for (int round = 0; round < 2; round++) {
        Thing *x = thing_create(thing_pos(a0), 10, 1);           // a death puff (any effect Thing will do)
        CHECK(x != nullptr);
        if (!x) break;
        x->owner = round == 0 ? a1->owner : e2->owner;
        x->filter_cls = 0xff;
        x->ext_x = 0x200; x->ext_y = 0x200; x->ext_h = 0x400;
        std::memset(a0->damage_slots, 0, sizeof a0->damage_slots);
        thing_area_damage(x, 0, 100);
        CHECK((a0->damage_slots[0].attacker != 0) == (round == 1));
        std::memset(a0->damage_slots, 0, sizeof a0->damage_slots);
        thing_try_damage(x, a0, 0, 100);
        CHECK((a0->damage_slots[0].attacker != 0) == (round == 1));
        std::memset(a0->damage_slots, 0, sizeof a0->damage_slots);
        // a projectile in the wizard: the collision search finds him only for the enemy's shot
        Thing *shot = thing_create(thing_pos(a0), 9, 0);
        CHECK(shot != nullptr);
        if (shot) {
            shot->owner = x->owner;
            shot->filter_cls = 0xff;
            Thing *hit = thing_find_collision(shot);
            CHECK(round == 0 ? (hit == nullptr || !same_side(hit->owner, a0->owner) || hit == shot) : hit != nullptr);
            if (round == 0) CHECK(hit != a0);
            thing_free(shot);
        }
        thing_free(x);
    }
    // the ally's castle: wizard 0 parked inside it does not desecrate
    g_mode.params.match.desecrate_s = 2;
    tick(1);
    for (int i = 0; i < 4 * TPS; i++) { place(a0, *thing_pos(c1)); tick(); }
    CHECK(!(mp(0).flags & MODE_PLAYER_DESECRATING) && mp(0).desecrate_rec1 == 0);
    CHECK(castle(1) == c1 && mode_desecration_pct(1) == 0);
    std::printf("sides: free for all %s, 2 v 2 %s, ally damage / collision / desecration skipped\n",
                "a == b", "sides 0 0 1 1");
}

// ---- 3 + 4: respawn and elimination ----------------------------------------------------------------------------
static void test_respawn() {
    ModeParams p = params(12, 2);
    // no wild creatures and no lairs: the rules are under test, and on this 256-cell world a pack at bot 1's seat
    // piled 21200 damage on the castle the test gives it while it was still being built (gone the tick it stood)
    p.world.wild = 0xff;
    p.world.lairs = 0xff;
    p.match.win = MATCH_WIN_NONE;
    p.match.respawn_s = 3;
    p.match.respawn_level_s = 2;
    CHECK(start(p));
    tick(2);
    for (int q = 0; q < 3; q++) god(q, true);
    Thing *c0 = give_castle(0, 2);
    CHECK(c0 && c0->state == 4 && c0->aux == 2);
    const int want = (3 + 2 * 2) * TPS;
    kill(0);
    CHECK(wait_dead(0) >= 0);
    // the death tick: the hook set the timer, the mode tick of the same tick took one off
    CHECK(mp(0).respawn_ticks == want - 1);
    CHECK(mp(0).deaths == 1);
    CHECK(mode_respawn_seconds(0) == (want - 1 + TPS - 1) / TPS);
    // the respawn command before the timer: nothing
    g_state->commands[0].cmd = 0xf;
    tick();
    CHECK(wizard(0)->state == 3);
    tick(want);
    CHECK(mp(0).respawn_ticks == 0 && mode_respawn_seconds(0) == 0);
    CHECK(wizard(0)->state == 3);                       // a human waits for the command
    g_state->commands[0].cmd = 0xf;
    tick();
    Thing *w0 = wizard(0);
    CHECK(w0->state == 0 && w0->health > 0);
    const int dx = world_signed(w0->x - c0->x), dy = world_signed(w0->y - c0->y);
    CHECK(dx > -0x100 && dx < 0x100 && dy > -0x100 && dy < 0x100);
    std::printf("respawn: human dead %d ticks (level 2 castle), respawned at the castle (%d,%d off) on command 0xf\n", want, dx, dy);

    // a bot respawns by itself when the timer runs out
    Thing *c1 = give_castle(1, 1);
    CHECK(c1 && c1->state == 4);
    kill(1);
    CHECK(wait_dead(1) >= 0);
    const int want1 = (3 + 2 * 1) * TPS;
    CHECK(mp(1).respawn_ticks == want1 - 1);
    int alive_after = -1;
    for (int i = 1; i <= want1 + 5; i++) {
        tick();
        if (wizard(1)->state == 1) { alive_after = i; break; }
    }
    CHECK(alive_after == want1);
    std::printf("respawn: bot respawned %d ticks after its death (want %d)\n", alive_after, want1);

    // elimination: bot 2 has no castle; dead = eliminated, stays dead
    drop_castle(2);                                      // (the bot may have cast its own by now)
    CHECK(castle(2) == nullptr);
    kill(2);
    CHECK(wait_dead(2) >= 0);
    CHECK(mp(2).flags & MODE_PLAYER_ELIMINATED);
    CHECK(mp(2).eliminated_tick == g_mode.tick);
    tick(400);
    CHECK(wizard(2)->state == 3 && (mp(2).flags & MODE_PLAYER_ELIMINATED));
    CHECK(g_state->players[2].active == 1);              // a spectator, not "left the game"
    // a living wizard without a castle is not eliminated; the human without a castle: eliminated, 0xf does nothing
    CHECK(!(mp(1).flags & MODE_PLAYER_ELIMINATED));
    drop_castle(0);                                      // destroy the human's castle level by level
    CHECK(castle(0) == nullptr);
    CHECK(!(mp(0).flags & MODE_PLAYER_ELIMINATED));
    kill(0);
    CHECK(wait_dead(0) >= 0);
    CHECK(mp(0).flags & MODE_PLAYER_ELIMINATED);
    g_state->commands[0].cmd = 0xf;
    tick();
    CHECK(wizard(0)->state == 3 && g_state->players[0].status == 0);
    CHECK(g_mode.match_state == MATCH_RUNNING);          // MATCH_WIN_NONE
    std::printf("elimination: bot 2 and human 0 (castle destroyed first) eliminated, stay dead, no level end\n");
}

// ---- 5: desecration ------------------------------------------------------------------------------------------------
static void test_desecration() {
    ModeParams p = params(13, 2);
    p.match.win = MATCH_WIN_NONE;
    p.match.desecrate_s = 4;
    CHECK(start(p));
    tick(2);
    for (int q = 0; q < 3; q++) god(q, true);
    Thing *c1 = give_castle(1, 2);
    CHECK(c1 && c1->state == 4);
    Thing *w0 = wizard(0);
    const Pos home = *thing_pos(w0);
    // 2 s inside, then out: the channel drains at twice the rate (not lost at once), gone after 1 s
    for (int i = 0; i < 2 * TPS; i++) { place(w0, *thing_pos(c1)); tick(); }
    CHECK((mp(0).flags & MODE_PLAYER_DESECRATING) && mp(0).desecrate_rec1 == 2);
    CHECK(mode_desecration_pct(1) == 50);
    place(w0, home);
    tick();
    CHECK(!(mp(0).flags & MODE_PLAYER_DESECRATING) && mp(0).desecrate_ticks == 2 * TPS - 2 && mp(0).desecrate_rec1 == 2);
    for (int i = 0; i < TPS / 2; i++) { place(w0, home); tick(); }
    CHECK(mp(0).desecrate_ticks > 0 && mp(0).desecrate_ticks < TPS + 2);     // half a second later: about half of it left
    for (int i = 0; i < TPS; i++) { place(w0, home); tick(); }
    CHECK(!(mp(0).flags & MODE_PLAYER_DESECRATING) && mp(0).desecrate_ticks == 0 && mode_desecration_pct(1) == 0);
    // the whole channel
    // the castle's spilled mana: balls owned by record 1's wizard
    const auto owned_ball_mana = [] {
        int64_t n = 0;
        for (int i = 1; i < thing_pool_slots(); i++) {
            const Thing *b = thing_at(i);
            if (b->cls == 10 && b->type == 0x27 && b->mana_owner == g_state->players[1].thing) n += b->mana;
        }
        return n;
    };
    const int64_t balls_before = owned_ball_mana();
    const int32_t castle_mana = c1->mana;
    // one level per completed channel (user, 2026-10-10): the level-2 castle is one level down after 4 s (the
    // channel starts again, the wizard still channelling) and gone after 8 s
    const int levels = c1->aux;
    int done = -1, first_drop = -1;
    for (int i = 1; i <= 5 * TPS * levels; i++) {
        if (castle(1)) place(w0, *thing_pos(c1));
        tick();
        if (!castle(1)) { done = i; break; }
        if (first_drop < 0 && c1->aux < levels) {
            first_drop = i;
            CHECK((mp(0).flags & MODE_PLAYER_DESECRATING) && mp(0).desecrate_ticks == 0 && mp(0).castles_razed == 0);
            CHECK(c1->duration == 5);                   // the rebuild timer of a collapse (it releases the Castle spell)
        }
        // the owner's Castle spell is free again once the rebuild sequence has run (it stuck "busy" without it)
        if (first_drop > 0 && i == first_drop + 3 * TPS) {
            const uint16_t si = g_state->players[1].blk.spell_thing[0x10];
            CHECK(c1->state == 4 && c1->duration == 0);
            if (si != 0) CHECK(thing_at(si)->cast_ticks == 0);
            std::printf("desecration: 3 s after the level fell the castle is in state %d, timer %d, the owner's Castle spell "
                        "(Thing %d) cast_ticks %d\n", c1->state, (int)c1->duration, (int)si, si ? (int)thing_at(si)->cast_ticks : -1);
        }
    }
    CHECK(levels < 2 || first_drop == 4 * TPS);
    CHECK(done == 4 * TPS * levels);
    CHECK(mp(0).castles_razed == 1 && !(mp(0).flags & MODE_PLAYER_DESECRATING));
    CHECK(!(mp(1).flags & MODE_PLAYER_ELIMINATED));      // alive: castle-less
    CHECK(c1->cls == 0 || c1->state != 4 || g_state->players[1].blk.castle == 0);
    const int64_t balls_after = owned_ball_mana();
    std::printf("desecration: 2 s then out -> drained in 1 s; 4 s per level -> first level after %d ticks, the level-%d "
                "castle gone after %d; castle mana %d, record 1's ball mana %lld -> %lld\n", first_drop, levels, done,
                (int)castle_mana, (long long)balls_before, (long long)balls_after);
    CHECK(balls_after > balls_before);
    tick();
    // now dead without a castle: eliminated
    kill(1);
    CHECK(wait_dead(1) >= 0);
    CHECK(mp(1).flags & MODE_PLAYER_ELIMINATED);
    // desecration off
    ModeParams q = params(13, 1);
    q.match.win = MATCH_WIN_NONE;
    q.match.desecrate_s = 0xff;
    CHECK(start(q));
    tick(2);
    god(0, true); god(1, true);
    Thing *d1 = give_castle(1, 1);
    for (int i = 0; i < 30 * TPS; i++) { place(wizard(0), *thing_pos(d1)); tick(); }
    CHECK(castle(1) == d1 && mp(0).desecrate_ticks == 0);
}

// ---- 6: win conditions ------------------------------------------------------------------------------------------
// Runs until the level end statuses are set; checks that every record got its status on the same tick.
static void check_level_end(int winner_side, const char *what) {
    CHECK(g_mode.match_state == MATCH_OVER);
    const uint32_t end = g_mode.match_end_tick;
    for (int i = 0; i < MODE_MATCH_END_DELAY_TICKS + 2; i++) {
        bool any = false;
        for (int p = 0; p < g_state->player_count; p++) any |= g_state->players[p].status != 0;
        if (any) break;
        tick();
    }
    CHECK(g_mode.tick == end + MODE_MATCH_END_DELAY_TICKS);
    int won = 0;
    for (int p = 0; p < g_state->player_count; p++) {
        const uint16_t st = g_state->players[p].status;
        const bool w = winner_side >= 0 && mp(p).side == winner_side;
        CHECK(st == (w ? 10 : 8));
        if (w) { won++; CHECK(mp(p).place == 1); } else CHECK(mp(p).place > 1 || winner_side < 0);
    }
    std::printf("win %s: reason %u winner side %d at tick %u, level end at tick %u for %d records (%d won)\n", what,
                (unsigned)g_mode.end_reason, winner_side, (unsigned)end, (unsigned)g_mode.tick, (int)g_state->player_count, won);
}

static void test_win() {
    // elimination: human (god) vs 2 bots without castles
    {
        ModeParams p = params(14, 2);
        CHECK(start(p));
        tick(2);
        god(0, true);
        drop_castle(1);
        kill(1);
        CHECK(wait_dead(1) >= 0);
        CHECK(mp(1).flags & MODE_PLAYER_ELIMINATED);
        CHECK(g_mode.match_state == MATCH_RUNNING);
        drop_castle(2);
        kill(2);
        CHECK(wait_dead(2) >= 0);
        CHECK(g_mode.end_reason == MATCH_END_ELIMINATION && g_mode.winner_side == mp(0).side);
        CHECK(mp(1).deaths == 1 && mp(2).deaths == 1);
        check_level_end(mp(0).side, "elimination");
        CHECK(mp(1).place != mp(2).place);
        CHECK(mp(2).place == 2);                         // eliminated later ranks better
    }
    // MATCH_WIN_NONE: the same never ends
    {
        ModeParams p = params(14, 2);
        p.match.win = MATCH_WIN_NONE;
        CHECK(start(p));
        tick(2);
        god(0, true);
        kill(1); kill(2);
        CHECK(wait_dead(1) >= 0 && wait_dead(2) >= 0);
        tick(300);
        CHECK(g_mode.match_state == MATCH_RUNNING && g_state->players[0].status == 0);
    }
    // mana dominance: 2 v 1, the team's castle holds the share for the hold time
    {
        ModeParams p = params(15, 2);
        p.match.win = MATCH_WIN_MANA;
        p.match.mana_pct = 5;
        p.match.mana_hold_s = 2;
        p.match.team[0] = 1; p.match.team[1] = 1;
        CHECK(start(p));
        tick(2);
        for (int q = 0; q < 3; q++) god(q, true);
        CHECK(mode_world_mana() > 0);
        Thing *c = give_castle(1, 3);
        CHECK(c != nullptr);
        int over = -1;
        for (int i = 0; i < 20 * TPS && g_mode.match_state == MATCH_RUNNING; i++) {
            if (c->cls == 3) c->mana = c->mana_total;    // keep the castle full
            tick();
            if (g_mode.match_state == MATCH_OVER) over = (int)g_mode.tick;
        }
        const int first_hold = over - (int)mp(0).mana_hold_ticks + 1;
        CHECK(over > 0 && mp(0).mana_hold_ticks == 2u * TPS && mp(1).mana_hold_ticks == 2u * TPS);
        CHECK(mp(2).mana_hold_ticks == 0);
        CHECK(g_mode.end_reason == MATCH_END_MANA && g_mode.winner_side == mp(1).side);
        std::printf("mana: castle %lld of world %lld (%d %%), hold from tick %d, over at %d\n",
                    (long long)mode_side_castle_mana(mp(1).side), (long long)mode_world_mana(), p.match.mana_pct, first_hold, over);
        check_level_end(mp(0).side, "mana (2 v 1)");
    }
    // timer: 1 minute, most castle mana
    {
        ModeParams p = params(16, 2);
        p.match.win = MATCH_WIN_TIMER;
        p.match.time_limit_min = 1;
        CHECK(start(p));
        tick(2);
        for (int q = 0; q < 3; q++) god(q, true);
        CHECK(mode_time_left_seconds() == 60 - 0 || mode_time_left_seconds() == 60);
        Thing *c = give_castle(2, 2);
        CHECK(c != nullptr);
        int64_t m[3] = {};
        while (g_mode.match_state == MATCH_RUNNING && g_mode.tick < 61u * TPS) {
            for (int q = 0; q < 3; q++) m[q] = mode_side_castle_mana(mp(q).side);
            tick();
        }
        CHECK(g_mode.match_state == MATCH_OVER && g_mode.end_reason == MATCH_END_TIMER);
        CHECK(g_mode.match_end_tick == 60u * TPS);
        CHECK(mode_time_left_seconds() == 0);
        int best = 0;
        for (int q = 1; q < 3; q++) if (m[q] > m[best]) best = q;
        // the ranking at the end tick (the mana right before it is close; the winner is the side that ranks first)
        CHECK(g_mode.winner_side == mp(best).side || mode_side_castle_mana(g_mode.winner_side) >= mode_side_castle_mana(mp(best).side));
        std::printf("timer: castle mana %lld / %lld / %lld -> winner side %u\n", (long long)m[0], (long long)m[1],
                    (long long)m[2], (unsigned)g_mode.winner_side);
        check_level_end((int)g_mode.winner_side, "timer");
    }
    // debug win / lose
    {
        ModeParams p = params(17, 2);
        CHECK(start(p));
        tick(2);
        for (int q = 0; q < 3; q++) god(q, true);
        g_state->commands[0] = debug_cmd_level_end(DEBUG_END_WIN, false, 2);
        tick();
        CHECK(g_mode.end_reason == MATCH_END_DEBUG && g_mode.winner_side == mp(2).side);
        check_level_end(mp(2).side, "debug win @2");
        CHECK(start(p));
        tick(2);
        for (int q = 0; q < 3; q++) god(q, true);
        g_state->commands[0] = debug_cmd_level_end(DEBUG_END_LOSE, false, DEBUG_TARGET_SELF);
        tick();
        CHECK(g_mode.end_reason == MATCH_END_DEBUG && g_mode.winner_side != mp(0).side && g_mode.winner_side < 8);
        check_level_end((int)g_mode.winner_side, "debug lose");
    }
}

// ---- 7: stats and determinism ----------------------------------------------------------------------------------
static std::vector<uint32_t> scripted_run() {
    std::vector<uint32_t> v;
    ModeParams p = params(21, 3);
    p.match.team[0] = 1; p.match.team[2] = 1;
    p.match.desecrate_s = 3;
    p.match.respawn_s = 2;
    if (!start(p)) return v;
    for (int i = 0; i < 1500; i++) {
        if (i == 5) { god(0, true); give_castle(0, 1); }
        if (i == 300) kill(1);
        if (i == 400 && castle(3)) { place(wizard(0), *thing_pos(castle(3))); }
        if (i >= 400 && i < 600 && castle(3)) place(wizard(0), *thing_pos(castle(3)));
        game_tick_sim();
        v.push_back(net_state_checksum());
        if (i == 0) {                       // the parts of the first tick, for the message when two runs differ
            static NetChecksumParts first;
            static bool have = false;
            NetChecksumParts now;
            net_state_checksum_parts(&now);
            if (have)
                for (int k = 0; k < NCP_COUNT; k++)
                    if (now.part[k] != first.part[k]) std::printf("determinism: part %s differs at tick 0\n", net_checksum_part_name(k));
            static ModeState mode_first;
            if (have) {
                const uint8_t *x = (const uint8_t *)&mode_first, *y = (const uint8_t *)&g_mode;
                for (size_t o = 0; o < sizeof(ModeState); o++)
                    if (x[o] != y[o]) std::printf("determinism: g_mode byte %zu: %u then %u (players start at %zu, 64 each)\n", o, x[o], y[o],
                                                  offsetof(ModeState, players));
            }
            mode_first = g_mode;
            first = now;
            have = true;
        }
    }
    return v;
}
static void test_determinism() {
    const std::vector<uint32_t> a = scripted_run();
    uint32_t stats = 0;
    for (int q = 0; q < 4; q++) stats += mp(q).deaths + mp(q).kills_wizards + mp(q).kills_creatures;
    const std::vector<uint32_t> b = scripted_run();
    CHECK(!a.empty() && a == b);
    for (size_t i = 0; i < a.size() && i < b.size(); i++)
        if (a[i] != b[i]) { std::printf("determinism: first difference at tick %zu\n", i); break; }
    std::printf("determinism: two scripted 2 v 1 v 1 runs, %zu ticks, %s; deaths + kills %u; match %s\n", a.size(),
                a == b ? "identical checksums every tick" : "DIVERGED", (unsigned)stats,
                g_mode.match_state == MATCH_OVER ? "over" : "running");
}

int main(int argc, char **argv) {
    mc_install_crash_handler();
    g_game = argc > 1 ? argv[1] : MC_DEFAULT_GAME_DIR;
    if (!sim_init(g_game.c_str())) { std::printf("SKIP: no game data in %s\n", g_game.c_str()); return 0; }
    sim_register_gameplay();
    mode_level_register();

    // the faithful path: no mode, no teams, same_side is a == b
    CHECK(!mode_active() && !g_mode_teams && g_hook_mode_side_of_owner == nullptr);
    CHECK(same_side(5, 5) && !same_side(5, 6));

    test_sides();
    test_respawn();
    test_desecration();
    test_win();
    test_determinism();

    mode_stop_run();
    CHECK(!g_mode_teams && g_hook_mode_side_of_owner == nullptr);
    if (g_fail) { std::printf("mode_rules_test: %d FAILED\n", g_fail); return 1; }
    std::printf("mode_rules_test: all passed\n");
    return 0;
}
