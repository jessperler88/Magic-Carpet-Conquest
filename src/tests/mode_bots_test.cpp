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
#include "spells.h"
#include "castle.h"
#include "mode_report.h"
#include "mode_bots.h"
#include "debug_cmd.h"
#include "thing.h"
#include "player.h"
#include "net.h"
#include "ai_wizard.h"
#include "level_features.h"
#include "projectiles.h"
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
    int casts_fireball = 0, casts_lightning = 0, casts_meteor = 0;     // since the last trace line
    uint32_t spells = 0;            // bit per spell the bot owned at some tick
    int first_learnt_tick = -1;     // first tick it owned one beyond the start kit (0, 2, 3, 0x10)
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
    int orphan_tick = -1;           // first tick a Thing's mana_owner named something that owns no mana (see run)
    int orphan_max = 0;             // most such Things in one tick
};

// Things whose mana_owner is neither a wizard (class 3) nor a mana hoard (class 10 type 0x28): the owner was deleted
// and mana_totals_update adds their mana to whatever lives in that slot now, every tick.
static int count_mana_orphans() {
    int n = 0;
    for (int i = 1; i < thing_pool_slots(); i++) {
        const Thing *t = thing_at((unsigned)i);
        if (t->cls == 0 || t->mana_owner == 0) continue;
        const Thing *o = thing_at(thing_wrap(t->mana_owner));
        if (o->cls != 3 && !(o->cls == 10 && o->type == 0x28)) n++;
    }
    return n;
}

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
        mode_report_tick();
        if (c.visit_tick > 0 && i > c.visit_tick && r.visit_targeted_tick < 0) {
            const PlayerRec &b1 = g_state->players[1];
            const Thing *t1 = thing_at(thing_wrap(b1.thing));
            if (b1.blk.ai_mode == 8 && t1->target == g_state->players[0].thing) r.visit_targeted_tick = i;
        }
        r.ticks_run = i;
        if (c.checksums) r.sums.push_back(net_state_checksum());
        if ((i & 7) == 0) {
            const int orphans = count_mana_orphans();
            if (orphans > 0 && r.orphan_tick < 0) r.orphan_tick = i;
            if (orphans > r.orphan_max) r.orphan_max = orphans;
        }
        // a bot taking a ball over from an ally (the owners as wizard Things: a ball's mana_owner is the wizard)
        for (uint32_t bi = g_cfg->mana_ball_list; bi != 0 && bi < ball_owner.size(); bi = thing_at(bi)->next) {
            const Thing *ball = thing_at(bi);
            const uint16_t was = ball_owner[bi], now = ball->mana_owner;
            ball_owner[bi] = now;
            if (was == now || was == 0 || now == 0 || ball->type != 0x27) continue;
            const Thing *ow = thing_at(thing_wrap(was)), *nw = thing_at(thing_wrap(now));
            if (ow->cls != 3 || nw->cls != 3 || nw->type != 1 || !same_side(ow->owner, nw->owner)) continue;
            // a claim is made in mode 6 (ai_mode6_collect_mana); a ball that changes hands under a wizard doing
            // something else merged with one of its own (the mana ball merge keeps one ball's owner)
            if (player_block(nw)->ai_mode != 6) continue;
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
                    {
                        const Thing *sp = ai_get_spell_thing(t, 0x10);
                        int balls_free = 0, balls_mine = 0, balls_near = 0;
                        for (uint32_t bi = g_cfg->mana_ball_list; bi != 0 && bi < (uint32_t)thing_pool_slots(); bi = thing_at(bi)->next) {
                            const Thing *ball = thing_at(bi);
                            if (ball->mana_owner == rec.thing) { balls_mine++; continue; }
                            if (thing_at(thing_wrap(ball->mana_owner))->cls == 3) continue;
                            balls_free++;
                            if (cs && wdist(ball->x, cs->x) < 60 * 256 && wdist(ball->y, cs->y) < 60 * 256) balls_near++;
                        }
                        std::printf("             castle spell price %d cd %d cast %d | castle state %d duration %d footprint %d | balls: mine %d, "
                                    "free %d (within 60 cells of the castle %d)\n", sp ? (int)sp->mana_total : -1,
                                    (int)rec.blk.spell_cooldown[0x10], sp ? (int)sp->cast_ticks : -1, cs ? cs->state : -1,
                                    cs ? (int)cs->duration : -1, cs ? (int)(uint8_t)castle_footprint_clear(thing_at(thing_wrap(rec.blk.castle))) : -1,
                                    balls_mine, balls_free, balls_near);
                    }
                    const Thing *tg = thing_at(thing_wrap(t->target));
                    std::printf("             casts since: fireball %d lightning %d meteor %d\n", s.casts_fireball, s.casts_lightning,
                                s.casts_meteor);
                    s.casts_fireball = s.casts_lightning = s.casts_meteor = 0;
                    std::printf("             hp %d/%d z %d spells 0x%06x burst %d conserve %d cd0 %d | target cls %d type %d state %d hp %d "
                                "z %d dist3d 0x%x yawdiff %d\n", (int)t->health, (int)t->max_health, (int)(int16_t)t->z,
                                (unsigned)s.spells, (int)rec.blk.ai_burst, (int)rec.blk.ai_conserve, (int)rec.blk.spell_cooldown[0],
                                tg->cls, tg->type, tg->state, (int)tg->health, (int)(int16_t)tg->z,
                                (unsigned)pos_dist_xyz(thing_pos(t), thing_pos(tg)),
                                (int)(int16_t)(t->yaw - t->target_yaw));
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
            if (P.spell_cooldown[0] == 2) s.casts_fireball++;       // (the reload values: cast in this tick)
            if (P.spell_cooldown[0xf] == 1) s.casts_lightning++;
            if (P.spell_cooldown[7] == 4) s.casts_meteor++;
            for (int id = 0; id < 24; id++) if (P.spell_thing[id] != 0) s.spells |= 1u << id;
            if (s.first_learnt_tick < 0 && (s.spells & ~(1u << 0 | 1u << 2 | 1u << 3 | 1u << 0x10))) s.first_learnt_tick = i;
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
    if (const char *dir = std::getenv("MC_BOTS_REPORT_DIR")) {         // analysis: the match report of every run (mode_report.h)
        std::string text;
        if (mode_report_text(&text)) {
            const std::string path = std::string(dir) + "/" + mode_report_name() + "_skill" + std::to_string((int)c.skill) + ".txt";
            if (FILE *f = std::fopen(path.c_str(), "w")) { std::fwrite(text.data(), 1, text.size(), f); std::fclose(f); }
        }
    }
    mode_stop_run();
    return r;
}

static void print_run(const char *title, const RunCfg &c, const RunResult &r) {
    std::printf("%s: seed %u, %u cells, template %u, %d players, skill %s, %d ticks in %.1f s (%.0f ticks/s)\n", title,
                (unsigned)c.seed, (unsigned)c.size, (unsigned)r.template_level, r.players,
                mode_bot_skill_name(mode_bot_skill(c.skill)), r.ticks_run, r.secs, r.ticks_run / (r.secs > 0 ? r.secs : 1));
    std::printf("  bot  castle@  lvl max/end  castle mana  mana_total max/end  deaths respawns  still  alive  modes(4/6/7/8/c/d)  "
                "spells (first learnt at)\n");
    for (int b = 1; b < r.players; b++) {
        const BotSeen &s = r.bot[b];
        std::printf("  %3d  %7d  %4d / %-4d  %11d  %9d / %-9d  %6d %8d  %5d  %5s  %d/%d/%d/%d/%d/%d  0x%06x (%d)\n", b,
                    s.castle_tick, s.castle_level_max, s.castle_level_end, s.castle_mana_end, s.mana_total_max,
                    s.mana_total_end, s.deaths, s.respawns, s.longest_still, s.alive_end ? "yes" : "no", s.mode_ticks[4],
                    s.mode_ticks[6], s.mode_ticks[7], s.mode_ticks[8], s.mode_ticks[12], s.mode_ticks[13], (unsigned)s.spells,
                    s.first_learnt_tick);
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
    // the bots fetch the urns in reach of their castles themselves (ai_goal_fetch_spell; the idle human takes none)
    int learnt = 0;
    for (int b = 1; b < r.players; b++) learnt += r.bot[b].first_learnt_tick > 0;
    if (size == 512) CHECK(learnt >= 1);
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

// ---- 5: mana owners ----------------------------------------------------------------------------------------------
// A bot claims a dead wizard's hoard by writing its mana_owner (ai_mode6_collect_mana); the hoard was then deleted
// without handing its balls on, and they added their mana to whatever Thing took the slot (a spell's price). Seed 3
// had 5 such balls from tick 4592 on.
static void test_mana_owners(uint32_t seed, int ticks) {
    RunCfg c;
    c.seed = seed;
    c.size = 512;
    c.bots = 7;
    c.ticks = ticks;
    c.human_god = false;
    c.win = MATCH_WIN_NONE;
    const RunResult r = run(c);
    std::printf("mana owners (512 cells, seed %u, 7 bots, %d ticks): most Things with a deleted mana owner %d (first at "
                "tick %d)\n", (unsigned)seed, r.ticks_run, r.orphan_max, r.orphan_tick);
    CHECK(r.orphan_max == 0);
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
    long long sum_level = 0, sum_mana = 0;
    int n_bots = 0, n_out = 0, n_deaths = 0, n_wkills = 0, n_ckills = 0, n_razed = 0, n_learnt = 0;
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
            n_bots++;
            sum_level += s.castle_level_end;
            sum_mana += s.castle_mana_end;
            n_out += (mp.flags & MODE_PLAYER_ELIMINATED) != 0;
            n_deaths += (int)mp.deaths;
            n_wkills += (int)mp.kills_wizards;
            n_ckills += (int)mp.kills_creatures;
            n_razed += (int)mp.castles_razed;
            for (int id = 0; id < 24; id++) n_learnt += (s.spells >> id & 1) && id != 0 && id != 2 && id != 3 && id != 0x10;
        }
        std::printf("  orphans %d@%d  (%.0f s)\n", r.orphan_max, r.orphan_tick, r.secs);
        if (r.match_state == MATCH_OVER) ended++;
        std::fflush(stdout);
    }
    std::printf("SUMMARY: %d bots, eliminated %d, castle level %.2f, castle mana %lld, deaths %d, kills w %d / c %d, "
                "razed %d, spells learnt %.2f\n", n_bots, n_out, n_bots ? (double)sum_level / n_bots : 0.0,
                n_bots ? sum_mana / n_bots : 0, n_deaths, n_wkills, n_ckills, n_razed, n_bots ? (double)n_learnt / n_bots : 0.0);
    std::printf("ended by the rules: %d / %d; wins per seat:", ended, k);
    for (int b = 1; b <= (int)base.bots && b < 8; b++) std::printf(" %d:%d", b, wins[b]);
    std::printf("\n");
    return 0;
}

// ---- the accuracy comparison (--accuracy [ticks]) -------------------------------------------------------------------
// The computer wizards' shots (projectiles.h g_ai_shot_stats) in campaign / multiplayer levels of the base game
// (single player: an idle human, every other seat a computer wizard, the original AI untouched) against Conquest
// matches. One line per run and a total per group.
struct AccTotal { AiShotStats w[3][4] = {}; long kills = 0; int wizards = 0; long knob_acc = 0, knob_react = 0; int ticks = 0; };

static void acc_collect(AccTotal *a, int ticks) {
    for (int p = 0; p < 8 && p < (int)g_state->player_count; p++) {
        const PlayerRec &rec = g_state->players[p];
        if (rec.is_computer != 1 || rec.thing == 0) continue;
        a->wizards++;
        a->kills += rec.blk.kills;
        a->knob_acc += rec.blk.ai_accuracy;
        a->knob_react += rec.blk.ai_reaction;
        for (int w = 0; w < 3; w++)
            for (int k = 0; k < 4; k++) {
                a->w[w][k].shots += g_ai_shot_stats[p][w][k].shots;
                a->w[w][k].hit_any += g_ai_shot_stats[p][w][k].hit_any;
                a->w[w][k].hit_target += g_ai_shot_stats[p][w][k].hit_target;
            }
    }
    a->ticks += ticks;
}

static void acc_print(const char *label, const AccTotal &a) {
    static const char *const weapon[3] = {"fireball", "lightning", "other"}, *const at[4] = {"creature", "wizard", "castle", "other"};
    std::printf("%s: %d computer wizards, %d ticks, knobs accuracy %ld reaction %ld (mean)\n", label, a.wizards, a.ticks,
                a.wizards ? a.knob_acc / a.wizards : 0, a.wizards ? a.knob_react / a.wizards : 0);
    for (int w = 0; w < 3; w++)
        for (int k = 0; k < 4; k++) {
            const AiShotStats &s = a.w[w][k];
            if (s.shots == 0) continue;
            std::printf("    %-9s at a %-8s  %6u shots  %5.1f %% hit something  %5.1f %% hit the target\n", weapon[w], at[k],
                        (unsigned)s.shots, 100.0 * s.hit_any / s.shots, 100.0 * s.hit_target / s.shots);
        }
}

static void acc_add(AccTotal *sum, const AccTotal &a) {
    for (int w = 0; w < 3; w++)
        for (int k = 0; k < 4; k++) {
            sum->w[w][k].shots += a.w[w][k].shots;
            sum->w[w][k].hit_any += a.w[w][k].hit_any;
            sum->w[w][k].hit_target += a.w[w][k].hit_target;
        }
    sum->kills += a.kills; sum->wizards += a.wizards; sum->knob_acc += a.knob_acc; sum->knob_react += a.knob_react; sum->ticks += a.ticks;
}

static int accuracy(int ticks) {
    AccTotal base_sum, mode_sum[5];
    static const int kLevels[] = {9, 14, 19, 24, 29, 34, 39, 44, 50, 52, 55, 60};
    for (int level : kLevels) {
        g_cfg->flags = 0;
        g_cfg->paused = 0;
        std::memset(g_ai_shot_stats, 0, sizeof g_ai_shot_stats);
        if (!sim_load_level(level)) { std::printf("base level %d: not loaded\n", level); continue; }
        for (int i = 0; i < ticks; i++) game_tick_sim();
        AccTotal a;
        acc_collect(&a, ticks);
        char label[48];
        std::snprintf(label, sizeof label, "base level %d", level);
        acc_print(label, a);
        acc_add(&base_sum, a);
    }
    struct Set { uint32_t size, bots; uint8_t skill; } const sets[] = {{512, 3, 0}, {512, 7, 0}, {1024, 3, 0}, {512, 7, 3}, {512, 7, 1}};
    for (int k = 0; k < 5; k++) {
        for (uint32_t seed = 1; seed <= 3; seed++) {
            RunCfg c;
            c.seed = seed;
            c.size = sets[k].size;
            c.bots = sets[k].bots;
            c.skill = sets[k].skill;
            c.ticks = ticks;
            c.win = MATCH_WIN_NONE;
            std::memset(g_ai_shot_stats, 0, sizeof g_ai_shot_stats);
            ModeParams p;
            p.seed = c.seed; p.world_size = c.size; p.bots = c.bots; p.flags = MODE_PARAM_DEBUG; p.match.bot_skill = c.skill;
            p.match.win = c.win;
            g_cfg->flags = 0;
            if (!mode_start_run(g_game.c_str(), GAME_MODE_CONQUEST, p) || !sim_load_level(MODE_LEVEL_INDEX)) continue;
            g_mode.players[0].flags |= MODE_PLAYER_GOD;
            for (int i = 0; i < ticks; i++) game_tick_sim();
            AccTotal a;
            acc_collect(&a, ticks);
            acc_add(&mode_sum[k], a);
            mode_stop_run();
        }
    }
    std::printf("\n");
    acc_print("BASE GAME, all levels", base_sum);
    for (int k = 0; k < 5; k++) {
        char label[80];
        std::snprintf(label, sizeof label, "CONQUEST %u cells, %u bots, skill %s (3 seeds)", (unsigned)sets[k].size, (unsigned)sets[k].bots,
                      mode_bot_skill_name(mode_bot_skill(sets[k].skill)));
        acc_print(label, mode_sum[k]);
    }
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
    for (int i = 1; i < argc; i++)
        if (!std::strcmp(argv[i], "--footprints")) {
            // the instruction bytes of the town building footprints 25..32, one row per line ("--" = cell left alone)
            const unsigned from = i + 1 < argc ? (unsigned)std::atoi(argv[i + 1]) : 25, to = i + 2 < argc ? (unsigned)std::atoi(argv[i + 2]) : 32;
            for (unsigned s = 0; s < 64; s++) {     // summary: capacity, cells per paint kind nibble, tallest roof
                const CastleFootprint *fp = castle_footprint(s);
                if (!fp->w || !fp->h) continue;
                int nib[16] = {}, top = 0;
                uint32_t k = 0;
                for (unsigned rows = fp->h; rows != 0 && k < fp->map_len;) {
                    const int8_t c = (int8_t)fp->map[k++];
                    if (c == 0) rows--;
                    else if (c > 0) for (int n = c; n != 0 && k < fp->map_len; n--) {
                        const uint8_t b = fp->map[k++];
                        nib[b >> 4]++;
                        if (b >= 0x40 && (b & 15) && ((b & 15) - 1) * 4 > top) top = ((b & 15) - 1) * 4;
                    }
                }
                std::printf("%2u: %2ux%-2u capacity %2d, wall (1x) %3d, 2x %3d, 3x %3d, 5x %3d, 6x %3d, 7x %2d, 8x %2d, top %d\n", s, fp->w, fp->h,
                            fp->w * fp->h >> 4, nib[1], nib[2], nib[3], nib[5], nib[6], nib[7], nib[8], top);
            }
            for (unsigned s = from; s <= to; s++) {
                const CastleFootprint *fp = castle_footprint(s);
                std::printf("footprint %u: %ux%u\n  ", s, fp->w, fp->h);
                uint32_t k = 0;
                for (unsigned rows = fp->h; rows != 0 && k < fp->map_len;) {
                    const int8_t c = (int8_t)fp->map[k++];
                    if (c == 0) { rows--; std::printf("\n  "); }
                    else if (c < 0) for (int n = -c; n != 0; n--) std::printf("-- ");
                    else for (int n = c; n != 0 && k < fp->map_len; n--) std::printf("%02x ", fp->map[k++]);
                }
                std::printf("\n");
            }
            return 0;
        }
    for (int i = 1; i < argc; i++)
        if (!std::strcmp(argv[i], "--castle11")) {
            // every computer wizard's castle asked to grow whenever it is idle (the request the Castle spell's seed leaves
            // in damage slot 5): the levels reached, when, and what stopped the others
            ModeParams p;
            p.seed = i + 1 < argc ? (uint32_t)std::atoi(argv[i + 1]) : 7;
            p.world_size = 512; p.bots = 3; p.flags = MODE_PARAM_DEBUG; p.match.win = MATCH_WIN_NONE;
            p.world.wild = 0xff; p.world.lairs = 0xff;
            g_cfg->flags = 0;
            if (!mode_start_run(g_game.c_str(), GAME_MODE_CONQUEST, p) || !sim_load_level(MODE_LEVEL_INDEX)) return 1;
            std::printf("castle_max_level %d\n", castle_max_level());
            int seen[8] = {};
            for (int tick = 0; tick < 12000; tick++) {
                for (int rec = 1; rec < 4; rec++) {
                    Thing *w = thing_at(g_state->players[rec].thing);
                    Thing *c = w->cls == 3 && player_block(w)->castle ? thing_at(player_block(w)->castle) : nullptr;
                    if (!c) continue;
                    if (c->state == 4 && c->duration == 0) c->damage_slots[5].attacker = (uint16_t)c->owner;
                    if (c->aux != seen[rec]) {
                        seen[rec] = c->aux;
                        const CastleFootprint *fp = castle_footprint((unsigned)c->aux);
                        std::printf("  t%-5d player %d: level %d, footprint %ux%u, extents %d x %d, max health %d, holds %d, balloons %d guards %d\n",
                                    tick, rec, c->aux, fp->w, fp->h, c->ext_x, c->ext_y, (int)c->max_health, (int)c->mana_total,
                                    castle_level_balloons((unsigned)c->aux), castle_level_guards((unsigned)c->aux));
                    }
                }
                game_tick_sim();
            }
            for (int rec = 1; rec < 4; rec++) {
                Thing *w = thing_at(g_state->players[rec].thing);
                Thing *c = w->cls == 3 && player_block(w)->castle ? thing_at(player_block(w)->castle) : nullptr;
                if (!c) { std::printf("player %d: no castle\n", rec); continue; }
                {
                    int flying = 0;
                    for (int s = 1; s < thing_pool_slots(); s++) {
                        const Thing *o = thing_at((unsigned)s);
                        flying += o->cls == 3 && o->type == 3 && o->owner == c->owner && o->health >= 0;
                    }
                    std::printf("player %d: %d balloons in the air (level %d allows %d)\n", rec, flying, c->aux, castle_level_balloons((unsigned)c->aux));
                }
                int built = 0, hmin = 255, hmax = 0;
                const int cx = world_cx(c->x), cy = world_cx(c->y);
                for (int dy = -40; dy < 40; dy++)
                    for (int dx = -40; dx < 40; dx++) {
                        const unsigned cell = world_cell(cx + dx, cy + dy);
                        built += (g_map_flags[cell] & 0x80) != 0;
                        if (g_map_height[cell] < hmin) hmin = g_map_height[cell];
                        if (g_map_height[cell] > hmax) hmax = g_map_height[cell];
                    }
                std::printf("player %d: level %d state %d, room for the next %d; in the 80 x 80 cells around it: %d built-on, heights %d..%d\n",
                            rec, c->aux, c->state, c->aux < castle_max_level() ? castle_footprint_clear(c) : -1, built, hmin, hmax);
                if (rec == 1 && i + 2 < argc) {     // the finished castle from above: height over its base in steps of 4 ('.' = base)
                    const int base = (int16_t)c->home.z >> 5;
                    for (int dy = -42; dy < 42; dy++) {
                        char line[90];
                        for (int dx = -42; dx < 42; dx++) {
                            const int h = (g_map_height[world_cell(cx + dx, cy + dy)] - base) / 4;
                            line[dx + 42] = h == 0 ? '.' : h < 0 ? '-' : h < 10 ? (char)('0' + h) : (char)('a' + h - 10);
                        }
                        line[84] = 0;
                        std::printf("%s\n", line);
                    }
                }
            }
            mode_stop_run();
            return 0;
        }
    for (int i = 1; i < argc; i++)
        if (!std::strcmp(argv[i], "--store")) {
            // spell_castle_store of the first computer wizard with a castle, before and after it is given three town buildings
            ModeParams p;
            p.seed = 7; p.world_size = 512; p.bots = 7; p.flags = MODE_PARAM_DEBUG; p.match.win = MATCH_WIN_NONE;
            g_cfg->flags = 0;
            if (!mode_start_run(g_game.c_str(), GAME_MODE_CONQUEST, p) || !sim_load_level(MODE_LEVEL_INDEX)) return 1;
            for (int k = 0; k < 1500; k++) game_tick_sim();
            for (int rec = 1; rec < 8; rec++) {
                Thing *w = thing_at(g_state->players[rec].thing);
                if (w->cls != 3 || player_block(w)->castle == 0) continue;
                const int32_t castle = thing_at(player_block(w)->castle)->mana, before = spell_castle_store(w);
                int given = 0, sum = 0;
                for (uint32_t b = g_cfg->wizard_list; b != 0 && given < 3; b = thing_at(b)->next) {
                    thing_at(b)->mana_owner = thing_index(w);
                    sum += thing_at(b)->mana;
                    given++;
                }
                std::printf("wizard %d: castle mana %d, store before %d, after claiming %d buildings holding %d: %d (expected %d)\n",
                            (int)thing_index(w), (int)castle, (int)before, given, sum, (int)spell_castle_store(w), (int)(before + sum));
                break;
            }
            mode_stop_run();
            return 0;
        }
    for (int i = 1; i < argc; i++)
        if (!std::strcmp(argv[i], "--wyvern")) {
            // a wyvern 30 cells from a town building of the given footprint (default 46, the castle with the courtyard;
            // 0 = the first wizard's castle): where it goes and what happens to the building
            const int want = i + 1 < argc ? std::atoi(argv[i + 1]) : 46;
            ModeParams p;
            p.seed = 7; p.world_size = 512; p.bots = 7; p.flags = MODE_PARAM_DEBUG; p.match.win = MATCH_WIN_NONE;
            p.world.wild = 0xff; p.world.lairs = 0xff;
            g_cfg->flags = 0;
            if (!mode_start_run(g_game.c_str(), GAME_MODE_CONQUEST, p) || !sim_load_level(MODE_LEVEL_INDEX)) return 1;
            for (int k = 0; k < (want ? 50 : 1500); k++) game_tick_sim();
            Thing *b = nullptr;
            for (int s = 1; s < thing_pool_slots() && !b; s++) {
                Thing *t = thing_at((unsigned)s);
                if (want ? (t->cls == 10 && t->type == 0x2d && t->castle_size == want) : (t->cls == 3 && t->type == 2)) b = t;
            }
            if (!b) { std::printf("no such building\n"); return 1; }
            Pos at = *thing_pos(b);
            at.x = at.x + (want ? 30 : 16) * 256;
            at.z = (int16_t)terrain_height_at(&at);
            Thing *w = thing_create(&at, 5, 16);
            if (!w) return 1;
            const int slot = (int)thing_index(w);
            std::printf("building %d (cls %d, footprint %d, ext %d x %d) health %d, wyvern %d sight 0x%x\n", (int)thing_index(b), b->cls,
                        b->castle_size, b->ext_x, b->ext_y, (int)b->health, slot, (unsigned)mc_move_desc(w->desc)->sight_radius);
            for (int tick = 0, traced = 0; tick <= 6000; tick++) {
                game_tick_sim();
                if (w->cls == 5 && w->speed_cur == 0 && w->aux != 0 && traced < 40) {        // holding and firing: its shots, tick by tick
                    traced++;
                    const Thing *tg = thing_at(w->target);
                    for (int s = 1; s < thing_pool_slots(); s++) {
                        const Thing *o = thing_at((unsigned)s);
                        if ((o->cls != 9 && o->cls != 10) || o->type != 0 || o->owner != slot) continue;
                        std::printf("   t%d %s %d at %+d,%+d z %d (ground %d) yaw %d pitch %d speed %d health %d damage %d ext %d/%d/%d/%d flags 0x%x\n",
                                    tick, o->cls == 9 ? "shot" : "blast", s, (int)world_signed(o->x - tg->x), (int)world_signed(o->y - tg->y),
                                    (int)o->z, (int)(int16_t)terrain_height_at(thing_pos(o)), o->yaw, o->pitch, o->speed_cur, (int)o->health,
                                    o->damage, o->ext_z0, o->ext_x, o->ext_y, o->ext_h, (unsigned)o->flags);
                    }
                    if (traced == 1)
                        std::printf("   target %d: pos z %d ext %d/%d/%d/%d flags 0x%x prop 0x%x owner %d filter %d/%d; wyvern at %+d,%+d z %d\n",
                                    (int)w->target, (int)tg->z, tg->ext_z0, tg->ext_x, tg->ext_y, tg->ext_h, (unsigned)tg->flags, tg->prop_flags,
                                    (int)tg->owner, tg->filter_cls, tg->filter_type, (int)world_signed(w->x - tg->x), (int)world_signed(w->y - tg->y),
                                    (int)w->z);
                }
                if (tick % 300 != 0) continue;
                if (w->cls != 5 || w->type != 16) { std::printf("t%d: the wyvern is gone\n", tick); break; }
                std::printf("t%-4d wyvern st %d aux %d target %d at %+d,%+d cells (dist %d.%d) yaw %d | building cls %d st 0x%x health %d level/people %d\n",
                            tick, w->state, w->aux, (int)w->target, world_signed(w->x - b->x) / 256, world_signed(w->y - b->y) / 256,
                            pos_dist_xy(thing_pos(w), thing_pos(b)) >> 8, (pos_dist_xy(thing_pos(w), thing_pos(b)) & 0xff) * 10 / 256, w->yaw,
                            b->cls, b->state, (int)b->health, b->aux);
                {
                    const Thing *tg = thing_at(w->target);
                    int shots = 0, blasts = 0;
                    for (int s = 1; s < thing_pool_slots(); s++) {
                        const Thing *o = thing_at((unsigned)s);
                        shots += o->cls == 9 && o->type == 0;
                        blasts += o->cls == 10 && o->type == 0;
                        if ((o->cls == 9 || o->cls == 10) && o->type == 0 && shots + blasts <= 6)
                            std::printf("      %s %d at %+d,%+d (1/256 cells) of the target, z %d (ground %d) pitch %d ext %d owner %d filter %d/%d\n",
                                        o->cls == 9 ? "shot" : "blast", s, (int)world_signed(o->x - tg->x), (int)world_signed(o->y - tg->y),
                                        (int)o->z, (int)(int16_t)terrain_height_at(thing_pos(o)), o->pitch, o->ext_x, (int)o->owner,
                                        o->filter_cls, o->filter_type);
                    }
                    std::printf("      its target: cls %d type %d size %d health %d z %d, %d.%d cells away, wyvern z %d ground %d; shots %d blasts %d\n",
                                tg->cls, tg->type, tg->castle_size, (int)tg->health, (int)tg->z, pos_dist_xy(thing_pos(w), thing_pos(tg)) >> 8,
                                (pos_dist_xy(thing_pos(w), thing_pos(tg)) & 0xff) * 10 / 256, (int)w->z,
                                (int)(int16_t)terrain_height_at(thing_pos(w)), shots, blasts);
                }
                if (b->cls == 0) break;
            }
            mode_stop_run();
            return 0;
        }
    for (int i = 1; i < argc; i++)
        if (!std::strcmp(argv[i], "--walls")) {
            // which cells of the town buildings carry the impassable wall texture (map type 8) after 20000 ticks
            ModeParams p;
            p.seed = i + 1 < argc ? (uint32_t)std::atoi(argv[i + 1]) : 7;
            p.world_size = i + 2 < argc ? (uint32_t)std::atoi(argv[i + 2]) : 512;
            if (i + 3 < argc) p.world.symmetry = (uint8_t)std::atoi(argv[i + 3]);
            p.bots = 7; p.flags = MODE_PARAM_DEBUG; p.match.win = MATCH_WIN_NONE;
            g_cfg->flags = 0;
            if (!mode_start_run(g_game.c_str(), GAME_MODE_CONQUEST, p) || !sim_load_level(MODE_LEVEL_INDEX)) return 1;
            {
                int buildings = 0;
                for (int s = 1; s < thing_pool_slots(); s++) buildings += thing_at((unsigned)s)->cls == 10 && thing_at((unsigned)s)->type == 0x2d;
                std::printf("seed %u size %u symmetry %d: %d town buildings at the start\n", p.seed, p.world_size, (int)p.world.symmetry, buildings);
                if (i + 4 < argc) { mode_stop_run(); return 0; }
            }
            auto walls = [] { int n = 0; for (int c = 0; c < g_world.size * g_world.size; c++) n += g_map_type[c] == 8; return n; };
            std::printf("wall cells at the start: %d\n", walls());
            for (int tick = 0, last = 0, shown = 0; tick < 20000; tick++) {
                game_tick_sim();
                const int n = tick % 5 == 0 ? walls() : last;
                if (n != last && shown++ < 40) std::printf("  tick %d: %d wall cells\n", tick, n);
                last = n;
            }
            std::printf("wall cells after 20000 ticks: %d\n", walls());
            for (int s = 1; s < thing_pool_slots(); s++) {
                const Thing *t = thing_at((unsigned)s);
                if (t->cls != 10 || t->type != 0x2d) continue;
                const CastleFootprint *fp = castle_footprint(t->castle_size);
                const int x0 = world_cx(t->x) - fp->w / 2, y0 = world_cx(t->y) - fp->h / 2;
                int n = 0, x = x0, y = y0;
                std::string where;
                uint32_t k = 0;
                for (unsigned rows = fp->h; rows != 0 && k < fp->map_len;) {
                    const int8_t c = (int8_t)fp->map[k++];
                    if (c == 0) { rows--; y++; x = x0; }
                    else if (c < 0) x -= c;
                    else for (int m = c; m != 0 && k < fp->map_len; m--, x++) {
                        const uint8_t b = fp->map[k++];
                        const unsigned cell = world_cell(x, y);
                        if (g_map_type[cell] != 8) continue;
                        char buf[64];
                        std::snprintf(buf, sizeof buf, " (%d,%d byte %02x h %d)", x - x0, y - y0, b, g_map_height[cell]);
                        if (n++ < 6) where += buf;
                    }
                }
                if (n) std::printf("building %d size %d state 0x%x at %d,%d base %d: %d wall cells:%s\n", s, t->castle_size, t->state,
                                   world_cx(t->x), world_cx(t->y), (int)(t->z >> 5), n, where.c_str());
            }
            mode_stop_run();
            return 0;
        }
    for (int i = 1; i < argc; i++)
        if (!std::strcmp(argv[i], "--accuracy")) return accuracy(i + 1 < argc ? std::atoi(argv[i + 1]) : 10000);
    for (int i = 1; i < argc; i++)
        if (!std::strcmp(argv[i], "--archers")) {
            // an archer and a skeleton 6 cells apart, far from every wizard: what happens tick by tick
            ModeParams p;
            p.seed = 7; p.world_size = 512; p.bots = 7; p.flags = MODE_PARAM_DEBUG; p.match.win = MATCH_WIN_NONE;
            g_cfg->flags = 0;
            if (!mode_start_run(g_game.c_str(), GAME_MODE_CONQUEST, p) || !sim_load_level(MODE_LEVEL_INDEX)) return 1;
            for (int k = 0; k < 50; k++) game_tick_sim();
            Thing *archer = nullptr, *skel = nullptr;
            for (int s = 1; s < thing_pool_slots(); s++) {
                Thing *t = thing_at((unsigned)s);
                if (t->cls != 5) continue;
                if (t->type == 4 && !archer) archer = t;
                if (t->type == 9 && !skel) skel = t;
            }
            if (!archer || !skel) { std::printf("no archer / skeleton\n"); return 1; }
            Pos at = *thing_pos(archer);
            {   // 10 cells away in the direction with the least difference in height
                int best = 1 << 30;
                for (int dir = 0; dir < 8; dir++) {
                    Pos q = *thing_pos(archer);
                    math_rotate_offset(&q, (uint16_t)(dir * 0x100), 0, 0xa00);
                    const int dh = std::abs((int)(int16_t)terrain_height_at(&q) - (int)(int16_t)terrain_height_at(thing_pos(archer)));
                    if (dh < best) { best = dh; at = q; }
                }
            }
            at.z = (int16_t)terrain_height_at(&at);
            thing_move_to(skel, &at);
            std::printf("archer %d at %d,%d, skeleton %d moved beside it (health %d); sight radius archer 0x%x skeleton 0x%x, think period "
                        "archer %d\n", (int)thing_index(archer), (int)(archer->x >> 8), (int)(archer->y >> 8), (int)thing_index(skel),
                        (int)skel->health, (unsigned)mc_move_desc(archer->desc)->sight_radius, (unsigned)mc_move_desc(skel->desc)->sight_radius,
                        (int)mc_move_desc(archer->desc)->think_period);
            const Pos archer_at = *thing_pos(archer);
            int printed = 0;
            for (int tick = 0; tick < 1500 && printed < 60; tick++) {
                {   // both pinned 12 cells apart (the skeleton converts an archer it reaches)
                    Pos a = archer_at, s = at;
                    a.z = thing_pos(archer)->z; s.z = thing_pos(skel)->z;
                    thing_move_to(archer, &a);
                    thing_move_to(skel, &s);
                }
                game_tick_sim();
                int arrows = 0;
                for (int s = 1; s < thing_pool_slots(); s++) {
                    const Thing *t = thing_at((unsigned)s);
                    arrows += t->cls == 9 && t->type == 0xd;
                }
                if (arrows && printed < 30)
                    for (int s = 1; s < thing_pool_slots(); s++) {
                        const Thing *a = thing_at((unsigned)s);
                        if (a->cls != 9 || a->type != 0xd) continue;
                        std::printf("    arrow %d at %d,%d z %d (ground %d) yaw %d pitch %d speed %d life %d ext %d/%d/%d owner %d filter %d/%d | "
                                    "skeleton at %d,%d z %d ext %d/%d/%d | archer at %d,%d z %d\n", s, (int)a->x, (int)a->y, (int)(int16_t)a->z,
                                    (int)(int16_t)terrain_height_at(thing_pos(a)), (int)a->yaw, (int)a->pitch, (int)a->speed_cur, (int)a->health,
                                    (int)a->ext_x, (int)a->ext_y, (int)a->ext_h, (int)a->owner, a->filter_cls, a->filter_type, (int)skel->x,
                                    (int)skel->y, (int)(int16_t)skel->z, (int)skel->ext_x, (int)skel->ext_y, (int)skel->ext_h, (int)archer->x,
                                    (int)archer->y, (int)(int16_t)archer->z);
                    }
                if (tick % 50 == 0 || arrows) printed++;
                if (tick % 50 == 0 || arrows)
                    std::printf("t%-4d archer st %d tgt %d timer_a %d flags 0x%x filter %d/%d hp %d | skel st %d tgt %d timer_a %d flags 0x%x prop 0x%x hp %d "
                                "slot0 %d/%d dist 0x%x | arrows in flight %d\n", tick, archer->state, (int)archer->target, archer->timer_a,
                                (unsigned)archer->flags, archer->filter_cls, archer->filter_type, (int)archer->health, skel->state,
                                (int)skel->target, skel->timer_a, (unsigned)skel->flags, (unsigned)skel->prop_flags, (int)skel->health,
                                (int)skel->damage_slots[0].attacker, (int)skel->damage_slots[0].amount,
                                (unsigned)pos_dist_xyz(thing_pos(archer), thing_pos(skel)), arrows);
                if (skel->cls != 5 || archer->cls != 5 || archer->type != 4 || skel->type != 9) {
                    std::printf("t%d: gone - archer slot now cls %d type %d state %d, skeleton slot cls %d type %d state %d hp %d\n", tick,
                                archer->cls, archer->type, archer->state, skel->cls, skel->type, skel->state, (int)skel->health);
                    break;
                }
            }
            mode_stop_run();
            return 0;
        }
    for (int i = 1; i < argc; i++)
        if (!std::strcmp(argv[i], "--owners")) {
            // the owner fields of a generated world's creatures (archers / skeletons / towns), and what the archers and
            // skeletons do to each other over the next ticks
            ModeParams p;
            p.seed = 7; p.world_size = 512; p.bots = 7; p.flags = MODE_PARAM_DEBUG; p.match.win = MATCH_WIN_NONE;
            g_cfg->flags = 0;
            if (!mode_start_run(g_game.c_str(), GAME_MODE_CONQUEST, p) || !sim_load_level(MODE_LEVEL_INDEX)) return 1;
            // who kills the archers, skeletons and villagers: [victim type][killer: 0 wizard, 1 archer, 2 skeleton, 3 other]
            static int s_deaths[20][4];
            static void (*s_prev_hook)(Thing *);
            s_prev_hook = g_hook_mode_creature_died;
            g_hook_mode_creature_died = [](Thing *c) {
                const Thing *k = thing_at(thing_wrap(c->killer));
                const Thing *ko = (k->cls == 9 || k->cls == 10) ? thing_at(thing_wrap((unsigned)(int)k->owner)) : k;
                const int who = ko->cls == 3 ? 0 : (ko->cls == 5 && ko->type == 4) ? 1 : (ko->cls == 5 && ko->type == 9) ? 2 : 3;
                if (c->type < 20) s_deaths[c->type][who]++;
                if (s_prev_hook) s_prev_hook(c);
            };
            for (int tick = 0; tick <= 30000; tick++) {
                if (tick % 5000 == 0) {
                    int people = 0, cap = 0, full = 0, big = 0, nb = 0;
                    for (int s = 1; s < thing_pool_slots(); s++) {
                        const Thing *t = thing_at((unsigned)s);
                        if (t->cls != 10 || t->type != 0x2d) continue;
                        nb++; people += t->aux; cap += t->speed_base; full += t->aux >= t->speed_base; big += t->speed_base > 5;
                    }
                    std::printf("  buildings %d: people %d of capacity %d, %d full, %d with capacity above 5\n", nb, people, cap, full, big);
                }
                if (tick % 5000 == 0)
                    for (int ty : {4, 9, 12, 13, 14})
                        std::printf("  deaths so far of type %2d: by wizards %d, archers %d, skeletons %d, other %d\n", ty, s_deaths[ty][0],
                                    s_deaths[ty][1], s_deaths[ty][2], s_deaths[ty][3]);
                if (tick % 5000 == 0) {
                    int n[20][3] = {}, towns = 0, state_n[20][8] = {};
                    long hp[20] = {};
                    for (int s = 1; s < thing_pool_slots(); s++) {
                        const Thing *t = thing_at((unsigned)s);
                        if (t->cls == 10 && t->type == 0x2d) towns++;
                        if (t->cls != 5 || t->type >= 20) continue;
                        n[t->type][t->owner == s ? 0 : t->owner == 0 ? 1 : 2]++;
                        hp[t->type] += t->health;
                        const int rel = t->state - t->type * 6;
                        if (rel >= 0 && rel < 8) state_n[t->type][rel]++;
                    }
                    {
                        int claimed = 0;
                        for (uint32_t b = g_cfg->wizard_list; b != 0; b = thing_at(b)->next) claimed += thing_at(b)->mana_owner != 0;
                        std::printf("  town buildings claimed by a wizard: %d\n", claimed);
                    }
                    std::printf("tick %d: town buildings %d\n", tick, towns);
                    {   // where the townies are: nearest building a castle (footprint 46 / 34) or a house, and how far
                        int at_castle = 0, at_house = 0, away = 0;
                        for (int s = 1; s < thing_pool_slots(); s++) {
                            const Thing *v = thing_at((unsigned)s);
                            if (v->cls != 5 || v->type != 0xd) continue;
                            int best = 1 << 30, size = 0;
                            for (uint32_t b = g_cfg->wizard_list; b != 0; b = thing_at(b)->next) {
                                const int d = pos_dist_xy(thing_pos(v), thing_pos(thing_at(b)));
                                if (d < best) { best = d; size = thing_at(b)->castle_size; }
                            }
                            if (best > 0x1400) away++;
                            else if (size == 46 || size == 34) at_castle++;
                            else at_house++;
                        }
                        std::printf("  townies: %d nearest a castle, %d nearest a house, %d more than 20 cells from any building\n", at_castle,
                                    at_house, away);
                    }
                    for (int ty : {4, 9, 12, 13, 14})
                        std::printf("  type %2d: owner self %d / zero %d / other %d, health sum %ld, states +0..+5: %d %d %d %d %d %d\n", ty,
                                    n[ty][0], n[ty][1], n[ty][2], hp[ty], state_n[ty][0], state_n[ty][1], state_n[ty][2],
                                    state_n[ty][3], state_n[ty][4], state_n[ty][5]);
                }
                game_tick_sim();
            }
            mode_stop_run();
            return 0;
        }
    int scale = 1;
    if (const char *e = std::getenv("MC_BOTS_TEST_SCALE")) scale = std::atoi(e) > 0 ? std::atoi(e) : 1;

    test_skill();
    // (the seed of the 512 match is one where a computer wizard fetches a spell urn within its 6000 ticks; whenever
    // the worlds or the bots change it may need another - MC_BOTS_TEST_SEED tries one)
    test_match(512, std::getenv("MC_BOTS_TEST_SEED") ? (uint32_t)std::atoi(std::getenv("MC_BOTS_TEST_SEED")) : 7, 6000 * scale);
    test_match(1024, 1, 6000 * scale);
    test_teams(512, 5, 6000 * scale);
    test_determinism(1024, 2, 1500 * scale);
    test_mana_owners(3, 6000 * scale);

    if (g_fail) { std::printf("mode_bots_test: %d FAILED\n", g_fail); return 1; }
    std::printf("mode_bots_test: all passed\n");
    return 0;
}
