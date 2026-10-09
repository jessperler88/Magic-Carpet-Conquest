// Round 12 task C (docs/analysis/port_r12_bots.md): bots v1 of the Conquest mode, headless (MC_SIM_ALL).
//
//  1. skill: MatchRules.bot_skill -> the computer wizards' aggression / accuracy / reaction (easy < normal < hard <
//     brutal on every seat; normal = the template's values, i.e. what a generated world played before round 12);
//  2. an idle human (god mode) + 3 bots on a generated 512 and 1024 world: every bot builds a castle, castles grow, a
//     bot killed (debug packet) after its castle respawns there, bots that die naturally come back, no bot is idle in
//     one place for long (the longest stretch without moving more than 4 cells is printed);
//  3. 2 v 2 (MatchRules.team 1,1,2,2): no bot ever targets an ally's wizard / castle / balloon / creature, and no bot
//     claims an ally's mana ball;
//  4. determinism: the same seed twice in one process gives identical net_state_checksum every tick.
//
// Balance harness (not run by ctest): mode_bots_test <game dir> --harness K [--seed S] [--size N] [--bots B]
//   [--ticks T] [--skill 1..4] [--teams 2v2] [--human]: K bot-vs-bot matches (seeds S..S+K-1; the human seat is
//   killed in the first tick and - by the match rules - eliminated, unless --human keeps it as an idle god),
//   match rules = the defaults (elimination + mana dominance) plus a time limit of T ticks, and a table per match
//   (castle levels, mana, kills, deaths, who won by the rules).
#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "sim.h"
#include "mode.h"
#include "mode_level.h"
#include "mode_rules.h"
#include "mode_bots.h"
#include "debug_cmd.h"
#include "thing.h"
#include "player.h"
#include "net.h"
#include "ai_wizard.h"
#include "settings.h"
#include "mc_globals.h"
#include "world.h"
#include "crash_handler.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static std::string g_game;

// ---- a run -----------------------------------------------------------------------------------------------------
struct RunCfg {
    uint32_t seed = 1;
    uint32_t size = 512;
    uint32_t bots = 3;
    uint8_t skill = 0;
    uint8_t team[8] = {};
    int ticks = 6000;
    bool human_god = true;          // the human seat idles in god mode (else it is killed in the first tick)
    int kill_bot_tick = 0;          // > 0: from this tick on, the first bot with a castle is killed once (debug packet)
    uint8_t win = 0;                // MatchRules.win (0 = the defaults)
    uint8_t time_limit_min = 0;
    bool stop_at_match_over = false;
    bool checksums = false;
    int trace = 0;                  // harness --trace N: events + a status line every N ticks
    int visit_tick = 0;             // > 0: the (god) human is teleported into bot 1's castle at this tick
};

struct BotSeen {
    int castle_tick = -1;           // first tick with a castle
    int castle_level_max = 0;
    int castle_level_end = 0;
    int castle_mana_end = 0;
    int mana_total_end = 0;
    int mana_total_max = 0;
    int deaths = 0;                 // state 1 -> 2 (or 3)
    int respawns = 0;               // state 3 -> 1
    int killed_by_test_tick = -1;
    int respawned_after_kill_tick = -1;
    int ally_targets = 0;           // ticks a bot targeted something of an ally
    int ally_claims = 0;            // mana balls this bot took over from an ally
    int ally_ball_ticks = 0;        // ticks in mode 6 with a ball an ally owns as the target (the ally claimed it
                                    // in the same tick: the bot chooses again in its next one)
    int longest_still = 0;          // longest stretch (ticks) alive without moving more than 4 cells
    int still_start = 0;
    int32_t still_x = 0, still_y = 0;
    int mode_ticks[16] = {};
    int last_state = -1;
    int last_castle = 0;
    uint32_t last_flags = 0;
    bool alive_end = false;
};

struct RunResult {
    int players = 0;
    uint32_t template_level = 0;
    BotSeen bot[8];
    std::vector<uint32_t> sums;
    int ticks_run = 0;
    double secs = 0;
    uint32_t match_state = 0, winner_side = 0, end_reason = 0, match_end_tick = 0;
    ModePlayer mp[8];
    bool rules_teams = false;       // task B's mode_rules_level_start resolved the teams (else the test's fallback)
    int visit_targeted_tick = -1;   // first tick bot 1 targeted the visiting human (mode 8)
    bool visit_done = false;        // the human was sent while bot 1 was alive with a castle
};

// The test's own side hook while task B's mode_rules is not in the tree: a wizard's Thing index -> its record's team.
static uint8_t s_test_team[8];
static int test_side_of_owner(int owner) {
    if (owner <= 0) return -1;
    for (int p = 0; p < 8 && p < (int)g_state->player_count; p++)
        if ((int)g_state->players[p].thing == owner) return s_test_team[p] ? 100 + s_test_team[p] : p;
    return -1;
}

static int32_t wdist(int32_t a, int32_t b) {
    int32_t d = a - b;
    const int32_t u = g_world.units;
    d %= u;
    if (d < 0) d += u;
    if (d > u / 2) d = u - d;
    return d;
}

static RunResult run(const RunCfg &c) {
    RunResult r;
    ModeParams p;
    p.seed = c.seed;
    p.world_size = c.size;
    p.bots = c.bots;
    p.humans = 1;
    p.flags = MODE_PARAM_DEBUG;
    p.match.bot_skill = c.skill;
    std::memcpy(p.match.team, c.team, 8);
    p.match.win = c.win;
    p.match.time_limit_min = c.time_limit_min;
    g_cfg->flags = 0;
    g_cfg->paused = 0;
    debug_cmd_clear_queue();
    if (!mode_start_run(g_game.c_str(), GAME_MODE_CONQUEST, p) || !sim_load_level(MODE_LEVEL_INDEX)) {
        std::printf("FAIL: the run did not start (seed %u size %u)\n", (unsigned)c.seed, (unsigned)c.size);
        g_fail++;
        return r;
    }
    r.players = (int)g_state->player_count;
    r.template_level = g_mode.template_level;
    bool any_team = false;
    for (int i = 0; i < 8; i++) any_team |= c.team[i] != 0;
    r.rules_teams = !any_team || g_mode_teams;
    if (any_team && !g_mode_teams) {            // task B pending: the test resolves the sides itself
        std::memcpy(s_test_team, c.team, 8);
        g_hook_mode_side_of_owner = test_side_of_owner;
        g_mode_teams = true;
    }
    if (c.human_god) g_mode.players[0].flags |= MODE_PLAYER_GOD;
    else debug_cmd_queue(debug_cmd_kill_wizard(0));
    bool killed = false;
    std::vector<uint16_t> ball_owner((size_t)thing_pool_slots(), 0);     // a mana ball's mana_owner last tick
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 1; i <= c.ticks; i++) {
        if (c.kill_bot_tick > 0 && !killed && i >= c.kill_bot_tick) {
            for (int b = 1; b < r.players; b++) {
                const PlayerRec &rec = g_state->players[b];
                const Thing *t = thing_at(thing_wrap(rec.thing));
                if (rec.blk.castle != 0 && t->state == 1 && rec.blk.invuln_timer == 0) {
                    debug_cmd_queue(debug_cmd_kill_wizard(b));
                    r.bot[b].killed_by_test_tick = i;
                    killed = true;
                    break;
                }
            }
        }
        if (c.visit_tick > 0 && i == c.visit_tick && g_state->players[1].blk.castle != 0) {
            const Thing *cs = thing_at(thing_wrap(g_state->players[1].blk.castle));
            debug_cmd_queue(debug_cmd_teleport((int)cs->x + 0x300, (int)cs->y));
            r.visit_done = thing_at(thing_wrap(g_state->players[1].thing))->state == 1;
        }
        debug_cmd_pump();
        game_tick_sim();
        if (c.visit_tick > 0 && i > c.visit_tick && r.visit_targeted_tick < 0) {
            const PlayerRec &b1 = g_state->players[1];
            const Thing *t1 = thing_at(thing_wrap(b1.thing));
            if (b1.blk.ai_mode == 8 && t1->target == g_state->players[0].thing) r.visit_targeted_tick = i;
        }
        r.ticks_run = i;
        if (c.checksums) r.sums.push_back(net_state_checksum());
        // a bot taking a ball over from an ally (the owners as wizard Things: a ball's mana_owner is the wizard)
        for (uint32_t bi = g_cfg->mana_ball_list; bi != 0 && bi < ball_owner.size(); bi = thing_at(bi)->next) {
            const Thing *ball = thing_at(bi);
            const uint16_t was = ball_owner[bi], now = ball->mana_owner;
            ball_owner[bi] = now;
            if (was == now || was == 0 || now == 0 || ball->type != 0x27) continue;
            const Thing *ow = thing_at(thing_wrap(was)), *nw = thing_at(thing_wrap(now));
            if (ow->cls != 3 || nw->cls != 3 || nw->type != 1 || !same_side(ow->owner, nw->owner)) continue;
            for (int b = 1; b < r.players; b++)
                if (g_state->players[b].thing == now) r.bot[b].ally_claims++;
        }
        for (int b = 1; b < r.players; b++) {
            BotSeen &s = r.bot[b];
            const PlayerRec &rec = g_state->players[b];
            if (rec.thing == 0) continue;
            Thing *t = thing_at(thing_wrap(rec.thing));
            if (t->cls != 3 || t->type != 1) continue;
            const int st = t->state;
            if (c.trace) {
                const Thing *k = thing_at(thing_wrap(t->killer));
                if (st != s.last_state)
                    std::printf("    t%-6d bot %d state %d -> %d (killer cls %d type %d owner %d)\n", i, b, s.last_state, st,
                                k->cls, k->type, (int)k->owner);
                if (rec.blk.castle != s.last_castle) {
                    const Thing *old = thing_at(thing_wrap((unsigned)s.last_castle));
                    const Thing *ck = thing_at(thing_wrap(old->killer));
                    std::printf("    t%-6d bot %d castle %d -> %d (old castle cell %d,%d killer %d cls %d type %d owner %d)\n", i,
                                b, s.last_castle, (int)rec.blk.castle, (int)old->x >> 8, (int)old->y >> 8, (int)old->killer,
                                ck->cls, ck->type, (int)ck->owner);
                }
                if (g_mode.players[b].flags != s.last_flags)
                    std::printf("    t%-6d bot %d mode flags 0x%x -> 0x%x\n", i, b, (unsigned)s.last_flags,
                                (unsigned)g_mode.players[b].flags);
                if (i % c.trace == 0) {
                    const Thing *cs = rec.blk.castle ? thing_at(thing_wrap(rec.blk.castle)) : nullptr;
                    if (cs && cs->health < cs->max_health) {        // the creatures of another side near the castle
                        for (int list = 0; list < 20; list++)
                            for (uint32_t ci = g_cfg->creature_lists[list]; ci != 0 && ci < (uint32_t)thing_pool_slots();
                                 ci = thing_at(ci)->next) {
                                const Thing *cr = thing_at(ci);
                                const int dx = wdist(cr->x, cs->x) >> 8, dy = wdist(cr->y, cs->y) >> 8;
                                if (dx > 40 || dy > 40 || same_side(cr->owner, t->owner)) continue;
                                std::printf("      near castle: creature %u type %d state %d hp %d at %d,%d cells, target %d\n",
                                            ci, cr->type, cr->state, (int)cr->health, dx, dy, (int)cr->target);
                            }
                    }
                    std::printf("    t%-6d bot %d st %d mode %2d cell %4d,%4d mana %6d/%-6d castle L%d %6d hp %d at %d,%d "
                                "target %d\n", i, b, st, rec.blk.ai_mode, (int)t->x >> 8, (int)t->y >> 8, (int)t->mana,
                                (int)t->mana_total, cs ? cs->aux + 1 : 0, cs ? (int)cs->mana : 0, cs ? (int)cs->health : 0,
                                cs ? (int)cs->x >> 8 : 0, cs ? (int)cs->y >> 8 : 0, (int)t->target);
                }
                s.last_castle = rec.blk.castle;
                s.last_flags = g_mode.players[b].flags;
            }
            if (s.last_state == 1 && st != 1) s.deaths++;
            if (s.last_state == 3 && st == 1) {
                s.respawns++;
                if (s.killed_by_test_tick > 0 && s.respawned_after_kill_tick < 0) s.respawned_after_kill_tick = i;
            }
            s.last_state = st;
            const PlayerBlock &P = rec.blk;
            if (P.castle != 0) {
                const Thing *cs = thing_at(thing_wrap(P.castle));
                if (s.castle_tick < 0) s.castle_tick = i;
                const int lvl = cs->aux + 1;
                if (lvl > s.castle_level_max) s.castle_level_max = lvl;
                s.castle_level_end = lvl;
                s.castle_mana_end = cs->mana;
            } else {
                s.castle_level_end = 0;
                s.castle_mana_end = 0;
            }
            s.mana_total_end = t->mana_total;
            if (t->mana_total > s.mana_total_max) s.mana_total_max = t->mana_total;
            if (st != 1) { s.still_start = i; continue; }
            s.mode_ticks[P.ai_mode & 15]++;
            // an ally's anything as the target of an attack mode
            const int m = P.ai_mode;
            if (m == 7 || m == 8 || m == 9 || m == 0xd || m == 6) {
                const Thing *tg = thing_at(thing_wrap(t->target));
                if (t->target != 0 && ai_target_valid(t, tg)) {
                    int owner = tg->owner;
                    if (m == 6) {                   // a mana ball: its owner's wizard
                        const Thing *o = thing_at(thing_wrap(tg->mana_owner));
                        owner = (tg->mana_owner != 0 && o->cls == 3) ? o->owner : 0;
                    }
                    if (owner != 0 && owner != t->owner && same_side(owner, t->owner)) {
                        if (m == 6) s.ally_ball_ticks++; else s.ally_targets++;
                    }
                }
            }
            if (wdist(t->x, s.still_x) > 4 * 256 || wdist(t->y, s.still_y) > 4 * 256) {
                s.still_x = t->x;
                s.still_y = t->y;
                s.still_start = i;
            } else if (i - s.still_start > s.longest_still) {
                s.longest_still = i - s.still_start;
            }
        }
        if (c.stop_at_match_over && g_mode.match_state == MATCH_OVER) break;
    }
    r.secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    for (int b = 1; b < r.players; b++) r.bot[b].alive_end = r.bot[b].last_state == 1;
    r.match_state = g_mode.match_state;
    r.winner_side = g_mode.winner_side;
    r.end_reason = g_mode.end_reason;
    r.match_end_tick = g_mode.match_end_tick;
    std::memcpy(r.mp, g_mode.players, sizeof r.mp);
    if (g_hook_mode_side_of_owner == test_side_of_owner) g_hook_mode_side_of_owner = nullptr;
    mode_stop_run();
    return r;
}

static void print_run(const char *title, const RunCfg &c, const RunResult &r) {
    std::printf("%s: seed %u, %u cells, template %u, %d players, skill %s, %d ticks in %.1f s (%.0f ticks/s)\n", title,
                (unsigned)c.seed, (unsigned)c.size, (unsigned)r.template_level, r.players,
                mode_bot_skill_name(mode_bot_skill(c.skill)), r.ticks_run, r.secs, r.ticks_run / (r.secs > 0 ? r.secs : 1));
    std::printf("  bot  castle@  lvl max/end  castle mana  mana_total max/end  deaths respawns  still  alive  modes(6/7/8/c/d)\n");
    for (int b = 1; b < r.players; b++) {
        const BotSeen &s = r.bot[b];
        std::printf("  %3d  %7d  %4d / %-4d  %11d  %9d / %-9d  %6d %8d  %5d  %5s  %d/%d/%d/%d/%d\n", b, s.castle_tick,
                    s.castle_level_max, s.castle_level_end, s.castle_mana_end, s.mana_total_max, s.mana_total_end,
                    s.deaths, s.respawns, s.longest_still, s.alive_end ? "yes" : "no", s.mode_ticks[6], s.mode_ticks[7],
                    s.mode_ticks[8], s.mode_ticks[12], s.mode_ticks[13]);
    }
}

static const char *end_reason_name(uint32_t e) {
    switch (e) {
    case MATCH_END_NONE: return "-";
    case MATCH_END_ELIMINATION: return "elimination";
    case MATCH_END_MANA: return "mana";
    case MATCH_END_TIMER: return "timer";
    case MATCH_END_DEBUG: return "debug";
    default: return "?";
    }
}

// ---- 1: skill ----------------------------------------------------------------------------------------------------
static void test_skill() {
    // the knob function itself
    uint8_t blk[0xd8] = {};
    auto set = [&](int a, int r, int acc) { std::memcpy(blk + 4, &a, 2); std::memcpy(blk + 8, &r, 2); std::memcpy(blk + 0xc, &acc, 2); };
    auto get = [&](int off) { int16_t v; std::memcpy(&v, blk + off, 2); return (int)v; };
    set(100, 100, 100);
    mode_bots_apply_skill(blk, 2);
    CHECK(get(4) == 100 && get(8) == 100 && get(0xc) == 100);
    mode_bots_apply_skill(blk, 1);
    CHECK(get(4) == 50 && get(8) == 4 && get(0xc) == 4);
    set(100, 100, 100);
    mode_bots_apply_skill(blk, 3);
    CHECK(get(4) == 177 && get(8) == 177 && get(0xc) == 177);
    set(100, 100, 100);
    mode_bots_apply_skill(blk, 4);
    CHECK(get(4) == 217 && get(8) == 217 && get(0xc) == 217);
    CHECK(mode_bot_skill(0) == 2 && mode_bot_skill(1) == 1 && mode_bot_skill(9) == 4);

    // in a run: the spawned wizards carry the knobs (the first tick spawns them); a restart gives the same
    int16_t knob[5][8][3] = {};
    for (int skill = 0; skill <= 4; skill++) {
        RunCfg c;
        c.seed = 11;
        c.size = 512;
        c.bots = 7;
        c.skill = (uint8_t)skill;
        ModeParams p;
        p.seed = c.seed; p.world_size = c.size; p.bots = c.bots; p.flags = MODE_PARAM_DEBUG; p.match.bot_skill = c.skill;
        g_cfg->flags = 0;
        CHECK(mode_start_run(g_game.c_str(), GAME_MODE_CONQUEST, p) && sim_load_level(MODE_LEVEL_INDEX));
        for (int rep = 0; rep < 2; rep++) {
            if (rep == 1) CHECK(sim_load_level(MODE_LEVEL_INDEX));           // a restart: not applied twice
            game_tick_sim();
            for (int b = 1; b < 8; b++) {
                const PlayerBlock &P = g_state->players[b].blk;
                if (rep == 0) { knob[skill][b][0] = P.ai_aggression; knob[skill][b][1] = P.ai_accuracy; knob[skill][b][2] = P.ai_reaction; }
                else CHECK(knob[skill][b][0] == P.ai_aggression && knob[skill][b][1] == P.ai_accuracy && knob[skill][b][2] == P.ai_reaction);
            }
        }
        if (skill == 0) {
            std::printf("skill: template %u, the seats' (aggression, accuracy, reaction):", (unsigned)g_mode.template_level);
            for (int b = 1; b < 8; b++) std::printf(" (%d %d %d)", knob[0][b][0], knob[0][b][1], knob[0][b][2]);
            std::printf("\n");
        }
        mode_stop_run();
    }
    int ordered = 0;
    for (int b = 1; b < 8; b++) {
        for (int k = 0; k < 3; k++) {
            CHECK(knob[0][b][k] == knob[2][b][k]);                        // 0 = normal = the template's
            CHECK(knob[1][b][k] <= knob[2][b][k] && knob[2][b][k] <= knob[3][b][k] && knob[3][b][k] <= knob[4][b][k]);
            if (knob[1][b][k] < knob[2][b][k] || knob[2][b][k] < knob[3][b][k]) ordered++;
        }
    }
    CHECK(ordered > 0);
    std::printf("skill: easy (%d %d %d) normal (%d %d %d) hard (%d %d %d) brutal (%d %d %d) on seat 1\n",
                knob[1][1][0], knob[1][1][1], knob[1][1][2], knob[2][1][0], knob[2][1][1], knob[2][1][2],
                knob[3][1][0], knob[3][1][1], knob[3][1][2], knob[4][1][0], knob[4][1][1], knob[4][1][2]);
}

// ---- 2: a match with an idle human --------------------------------------------------------------------------------
static void test_match(uint32_t size, uint32_t seed, int ticks) {
    RunCfg c;
    c.seed = seed;
    c.size = size;
    c.ticks = ticks;
    c.kill_bot_tick = ticks / 3;
    c.visit_tick = ticks * 2 / 3;
    c.win = MATCH_WIN_NONE;
    const RunResult r = run(c);
    char title[64];
    std::snprintf(title, sizeof title, "idle human + 3 bots (%u)", (unsigned)size);
    print_run(title, c, r);
    CHECK(r.players == 4);
    int grown = 0, killed = 0, came_back = 0, held = 0;
    for (int b = 1; b < r.players; b++) {
        const BotSeen &s = r.bot[b];
        CHECK(s.castle_tick > 0);
        if (s.castle_level_end > 0) held++;
        if (s.castle_level_max >= 2) grown++;
        if (s.killed_by_test_tick > 0) {
            killed++;
            if (s.respawned_after_kill_tick > 0) came_back++;
            std::printf("  bot %d killed at tick %d, respawned at tick %d\n", b, s.killed_by_test_tick, s.respawned_after_kill_tick);
        }
        CHECK(s.respawns <= s.deaths);
        CHECK(s.longest_still < 3000);
    }
    CHECK(grown >= 1);
    // a castle can fall to the wild creatures near a seat (a crab lair at grade 1: report section 4)
    CHECK(held >= 2);
    CHECK(killed == 1 && came_back == 1);
    // the human flies into bot 1's castle: the bot comes to fight him (ai_goal_defend_castle)
    std::printf("  human sent into bot 1's castle at tick %d (bot alive: %s): targeted by bot 1 at tick %d\n", c.visit_tick,
                r.visit_done ? "yes" : "no", r.visit_targeted_tick);
    if (r.visit_done) CHECK(r.visit_targeted_tick > c.visit_tick && r.visit_targeted_tick <= c.visit_tick + 200);
}

// ---- 3: 2 v 2 ----------------------------------------------------------------------------------------------------
static void test_teams(uint32_t size, uint32_t seed, int ticks) {
    RunCfg c;
    c.seed = seed;
    c.size = size;
    c.ticks = ticks;
    c.win = MATCH_WIN_NONE;
    const uint8_t team[8] = {1, 1, 2, 2, 0, 0, 0, 0};
    std::memcpy(c.team, team, 8);
    const RunResult r = run(c);
    print_run("2 v 2 (human + bot 1 vs bots 2 + 3)", c, r);
    if (!r.rules_teams) std::printf("  (task B's mode_rules_level_start did not resolve the teams: the test's own side hook)\n");
    int ally = 0, claims = 0, ball_ticks = 0;
    for (int b = 1; b < r.players; b++) {
        ally += r.bot[b].ally_targets;
        claims += r.bot[b].ally_claims;
        ball_ticks += r.bot[b].ally_ball_ticks;
    }
    std::printf("  ticks with an ally's wizard / castle / balloon / creature targeted: %d; ally mana balls taken over: %d "
                "(ticks with one as the target: %d)\n", ally, claims, ball_ticks);
    CHECK(ally == 0);
    CHECK(claims == 0);
}

// ---- 4: determinism ----------------------------------------------------------------------------------------------
static void test_determinism(uint32_t size, uint32_t seed, int ticks) {
    RunCfg c;
    c.seed = seed;
    c.size = size;
    c.ticks = ticks;
    c.checksums = true;
    c.skill = 3;
    const uint8_t team[8] = {1, 2, 1, 2, 0, 0, 0, 0};
    std::memcpy(c.team, team, 8);
    const RunResult a = run(c);
    const RunResult b = run(c);
    int diff = -1;
    for (size_t i = 0; i < a.sums.size() && i < b.sums.size(); i++) if (a.sums[i] != b.sums[i]) { diff = (int)i; break; }
    std::printf("determinism (%u cells, seed %u, hard, teams 1,2,1,2): %zu ticks, %s\n", (unsigned)size, (unsigned)seed,
                a.sums.size(), diff < 0 ? "identical checksums every tick" : "DIVERGED");
    if (diff >= 0) std::printf("  first difference at tick %d\n", diff + 1);
    CHECK(diff < 0 && a.sums.size() == (size_t)ticks && b.sums.size() == (size_t)ticks);
}

// ---- the balance harness -----------------------------------------------------------------------------------------
static int harness(int argc, char **argv, int k) {
    RunCfg base;
    base.size = 1024;
    base.bots = 3;
    base.ticks = 30000;
    base.human_god = false;
    base.stop_at_match_over = true;
    for (int i = 2; i < argc; i++) {
        const std::string a = argv[i];
        auto next = [&]() { return i + 1 < argc ? std::atoi(argv[++i]) : 0; };
        if (a == "--seed") base.seed = (uint32_t)next();
        else if (a == "--size") base.size = (uint32_t)next();
        else if (a == "--bots") base.bots = (uint32_t)next();
        else if (a == "--ticks") base.ticks = next();
        else if (a == "--skill") base.skill = (uint8_t)next();
        else if (a == "--human") base.human_god = true;
        else if (a == "--trace") base.trace = next();
        else if (a == "--teams" && i + 1 < argc && std::string(argv[i + 1]) == "2v2") {
            i++;
            const uint8_t t[8] = {1, 1, 2, 2, 1, 2, 1, 2};
            std::memcpy(base.team, t, 8);
        }
    }
    const int minutes = (base.ticks + MODE_TICKS_PER_SECOND * 60 - 1) / (MODE_TICKS_PER_SECOND * 60);
    base.time_limit_min = (uint8_t)(minutes > 255 ? 255 : minutes);
    base.win = MATCH_WIN_ELIMINATION | MATCH_WIN_MANA | MATCH_WIN_TIMER;
    base.ticks += 25 * 10;                       // the timer fires on its own tick
    std::printf("harness: %d matches, %u cells, %u bots, skill %s, time limit %d min, human %s\n", k, (unsigned)base.size,
                (unsigned)base.bots, mode_bot_skill_name(mode_bot_skill(base.skill)), minutes,
                base.human_god ? "idle god" : "killed at the start");
    std::printf("seed  tmpl  end          winner  tick    | per seat: side lvl castle_mana mana_total kills(w/c) deaths razed place\n");
    int wins[8] = {}, ended = 0;
    for (int m = 0; m < k; m++) {
        RunCfg c = base;
        c.seed = base.seed + (uint32_t)m;
        const RunResult r = run(c);
        std::printf("%4u  %4u  %-11s  %6s  %6d  |", (unsigned)c.seed, (unsigned)r.template_level,
                    r.match_state == MATCH_OVER ? end_reason_name(r.end_reason) : "running",
                    r.match_state == MATCH_OVER ? std::to_string(r.winner_side).c_str() : "-",
                    r.match_state == MATCH_OVER ? (int)r.match_end_tick : r.ticks_run);
        for (int b = 1; b < r.players; b++) {
            const BotSeen &s = r.bot[b];
            const ModePlayer &mp = r.mp[b];
            std::printf(" [%d s%d L%d %d %d %u/%u d%u r%u p%u]", b, mp.side, s.castle_level_end, s.castle_mana_end,
                        s.mana_total_end, (unsigned)mp.kills_wizards, (unsigned)mp.kills_creatures, (unsigned)mp.deaths,
                        (unsigned)mp.castles_razed, (unsigned)mp.place);
            if (r.match_state == MATCH_OVER && r.end_reason != MATCH_END_NONE && mp.side == r.winner_side) wins[b]++;
        }
        std::printf("  (%.0f s)\n", r.secs);
        if (r.match_state == MATCH_OVER) ended++;
        std::fflush(stdout);
    }
    std::printf("ended by the rules: %d / %d; wins per seat:", ended, k);
    for (int b = 1; b <= (int)base.bots && b < 8; b++) std::printf(" %d:%d", b, wins[b]);
    std::printf("\n");
    return 0;
}

int main(int argc, char **argv) {
    mc_install_crash_handler();
    g_game = argc > 1 && argv[1][0] != '-' ? argv[1] : MC_DEFAULT_GAME_DIR;
    if (!sim_init(g_game.c_str())) { std::printf("SKIP: no game data in %s\n", g_game.c_str()); return 0; }
    sim_register_gameplay();
    mode_level_register();
    for (int i = 1; i + 1 < argc; i++)
        if (!std::strcmp(argv[i], "--harness")) return harness(argc, argv, std::atoi(argv[i + 1]));
    int scale = 1;
    if (const char *e = std::getenv("MC_BOTS_TEST_SCALE")) scale = std::atoi(e) > 0 ? std::atoi(e) : 1;

    test_skill();
    test_match(512, 3, 6000 * scale);
    test_match(1024, 1, 6000 * scale);
    test_teams(512, 5, 6000 * scale);
    test_determinism(1024, 2, 1500 * scale);

    if (g_fail) { std::printf("mode_bots_test: %d FAILED\n", g_fail); return 1; }
    std::printf("mode_bots_test: all passed\n");
    return 0;
}
