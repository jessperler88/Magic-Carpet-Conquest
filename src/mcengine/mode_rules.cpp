// Match rules v1 of the Conquest mode (mode_rules.h). Port round 12, task B; report docs/analysis/port_r12_rules.md.
// Port-only: nothing here exists in carpet.exe.
//
// g_mode_teams, g_hook_mode_side_of_owner, mode_same_side_records, mode_rules_sync and the hook pointers live in
// mode.cpp (every unit test links it; same_side() is called from the simulation core). This file resolves the sides
// at the level start, runs the rules once per tick (g_hook_mode_match_tick, after thing_update_all) and receives the
// death / castle calls of player.cpp, creature_common.cpp and castle.cpp.
//
// Determinism: only simulated state is read (GameState, g_mode), integers only, records in index order; nothing
// depends on the local player (the one place that reads it, mode_world_mana, takes its term back out).
#include "mode_rules.h"
#include "mode.h"
#include "mc_globals.h"
#include "thing.h"
#include "player.h"
#include "castle.h"
#include "world.h"

namespace {

constexpr int TPS = MODE_TICKS_PER_SECOND;

int record_count() {
    int n = (int)(uint16_t)g_state->player_count;
    return n < 0 ? 0 : n > 8 ? 8 : n;
}
bool in_match(int p) { return p >= 0 && p < 8 && (g_mode.players[p].flags & MODE_PLAYER_IN_MATCH) != 0; }
bool eliminated(int p) { return (g_mode.players[p].flags & MODE_PLAYER_ELIMINATED) != 0; }

// The wizard Thing of record p (null before its first spawn).
Thing *wizard_of(int p) {
    const unsigned idx = g_state->players[p].thing;
    if (idx == 0 || idx >= (unsigned)thing_pool_slots()) return nullptr;
    Thing *w = thing_at(idx);
    return (w->cls == 3 && (w->type == 0 || w->type == 1)) ? w : nullptr;
}
bool wizard_alive(const Thing *w) {
    return w && (w->state == 0 || w->state == 1) && w->health >= 0 && !(w->flags & 0x20);
}
// The castle Thing of record p (null without one).
Thing *castle_of(int p) {
    const unsigned idx = g_state->players[p].blk.castle;
    if (idx == 0 || idx >= (unsigned)thing_pool_slots()) return nullptr;
    Thing *c = thing_at(idx);
    return (c->cls == 3 && c->type == 2) ? c : nullptr;
}
// The record whose wizard is Thing `owner` (-1: no player's wizard - wild creatures, the world).
int record_of_owner(int owner) {
    if (owner <= 0) return -1;
    for (int p = 0; p < 8; p++)
        if (in_match(p) && (int)g_state->players[p].thing == owner) return p;
    return -1;
}
int64_t record_castle_mana(int p) {
    const Thing *c = castle_of(p);
    if (!c) return 0;
    return (int64_t)c->mana + (int64_t)g_state->players[p].blk.mana_in_transit;
}
int sides_at_start() {
    bool seen[8] = {};
    int n = 0;
    for (int p = 0; p < 8; p++) {
        if (!in_match(p)) continue;
        const int s = g_mode.players[p].side & 7;
        if (!seen[s]) { seen[s] = true; n++; }
    }
    return n;
}
bool side_alive(int side) {
    for (int p = 0; p < 8; p++)
        if (in_match(p) && g_mode.players[p].side == side && !eliminated(p)) return true;
    return false;
}
uint32_t side_kills(int side) {
    uint32_t k = 0;
    for (int p = 0; p < 8; p++)
        if (in_match(p) && g_mode.players[p].side == side) k += g_mode.players[p].kills_wizards + g_mode.players[p].kills_creatures;
    return k;
}
uint32_t side_last_elimination(int side) {
    uint32_t t = 0;
    for (int p = 0; p < 8; p++)
        if (in_match(p) && g_mode.players[p].side == side && g_mode.players[p].eliminated_tick > t) t = g_mode.players[p].eliminated_tick;
    return t;
}
int side_lowest_record(int side) {
    for (int p = 0; p < 8; p++)
        if (in_match(p) && g_mode.players[p].side == side) return p;
    return 8;
}

// The ranking of the timer and of the places: a side still in the match before an eliminated one, then the most
// castle mana, the most kills, the later elimination, the lowest record. True when side a ranks before side b.
bool ranks_before(int a, int b) {
    const bool la = side_alive(a), lb = side_alive(b);
    if (la != lb) return la;
    const int64_t ma = mode_side_castle_mana(a), mb = mode_side_castle_mana(b);
    if (ma != mb) return ma > mb;
    const uint32_t ka = side_kills(a), kb = side_kills(b);
    if (ka != kb) return ka > kb;
    const uint32_t ea = side_last_elimination(a), eb = side_last_elimination(b);
    if (ea != eb) return ea > eb;
    return side_lowest_record(a) < side_lowest_record(b);
}
// The sides of the match in ranking order (excluding `skip`, -1 = none); returns their number.
int ranked_sides(int out[8], int skip) {
    bool seen[8] = {};
    int n = 0;
    for (int p = 0; p < 8; p++) {
        if (!in_match(p)) continue;
        const int s = g_mode.players[p].side & 7;
        if (seen[s] || s == skip) continue;
        seen[s] = true;
        out[n++] = s;
    }
    for (int i = 1; i < n; i++)                         // insertion sort: stable, index order for equal keys
        for (int j = i; j > 0 && ranks_before(out[j], out[j - 1]); j--) {
            const int t = out[j]; out[j] = out[j - 1]; out[j - 1] = t;
        }
    return n;
}

void eliminate(int p) {
    ModePlayer &m = g_mode.players[p];
    if (m.flags & MODE_PLAYER_ELIMINATED) return;
    m.flags = (m.flags | MODE_PLAYER_ELIMINATED) & ~MODE_PLAYER_DESECRATING;
    m.eliminated_tick = g_mode.tick;
    m.respawn_ticks = 0;
    m.desecrate_rec1 = 0;
    m.desecrate_ticks = 0;
}

void stop_desecrating(ModePlayer &m) {
    m.flags &= ~MODE_PLAYER_DESECRATING;
    m.desecrate_rec1 = 0;
    m.desecrate_ticks = 0;
}

// A castle level taken by a desecration collapses as when the castle is destroyed (castle_destroyed_update:
// castle_collapse_level spills a tenth per level and runs the collapse handler on the footprint; at level 0 the
// castle is deleted and the owner's P.castle cleared), then its remaining mana is spilled as mana balls and the
// balloons drop their cargo (castle_spill_mana / castle_manage_balloons_and_guards at level 0).
// A completed channel takes ONE level (user, 2026-10-10: one tier per desecration, by a human or a computer wizard
// alike; a whole castle went in a single channel before). Returns true when that was the last level: the castle is
// gone, its remaining mana spilled and the balloons' cargo dropped as above.
bool desecrate_castle_level(Thing *c, int by_record) {
    c->killer = g_state->players[by_record].thing;       // castles_razed for the channeller (g_hook_mode_castle_gone)
    castle_collapse_level(c);                           // aux - 1; deleted (P.castle = 0) when that leaves none
    castle_spill_mana(c);
    castle_manage_balloons_and_guards(c);
    if (c->aux <= 0) return true;
    // The rest of castle_destroyed_update: the 5-update rebuild timer, whose sequencer ends by releasing the owner's
    // Castle spell (castle_collapse_level leaves it "busy"; without the timer it stayed busy until the castle was
    // gone - user, 2026-10-10: the castle spell stuck on almost cast after a level was desecrated).
    c->state = 4;
    c->cast_ticks = 0;
    c->duration = 5;
    return false;
}

// Is wizard w inside castle c's courtyard? The footprint box of the castle's level (Thing extents, as
// castle_crush_wizards tests the villages under a growing castle, without its one-level / one-cell margin), on the
// world's torus; any height.
bool inside_courtyard(const Thing *w, const Thing *c) {
    int dx = world_signed(w->x - c->x), dy = world_signed(w->y - c->y);
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    return dx <= (int)c->ext_x + (int)w->ext_x && dy <= (int)c->ext_y + (int)w->ext_y;
}

// ---- the hooks ------------------------------------------------------------------------------------------------

void on_wizard_died(Thing *w) {
    if (!mode_active() || g_mode.match_state != MATCH_RUNNING) return;
    const int p = player_block(w)->player_no & 7;
    if (!in_match(p) || g_state->players[p].thing != thing_index(w)) return;
    ModePlayer &m = g_mode.players[p];
    m.deaths++;
    stop_desecrating(m);
    const MatchRulesResolved r = mode_rules_resolve(g_mode.params.match);
    const Thing *c = castle_of(p);
    m.respawn_ticks = c ? r.respawn_ticks + r.respawn_level_ticks * (int)(int16_t)c->aux : 0;
    if (m.respawn_ticks < 0) m.respawn_ticks = 0;
    const int k = record_of_owner((int)w->killer);
    if (k >= 0 && k != p && !mode_same_side_records(k, p)) g_mode.players[k].kills_wizards++;
}

void on_creature_died(Thing *c) {
    if (!mode_active() || g_mode.match_state != MATCH_RUNNING) return;
    const int k = record_of_owner((int)c->killer);
    if (k < 0) return;
    const int o = record_of_owner((int)(int16_t)c->owner);     // a guard / converted skeleton of a player
    if (o >= 0 && mode_same_side_records(k, o)) return;      // own and allied creatures do not count
    g_mode.players[k].kills_creatures++;
}

void on_castle_gone(Thing *c) {
    if (!mode_active() || g_mode.match_state != MATCH_RUNNING) return;
    const int k = record_of_owner((int)c->killer);
    const int o = record_of_owner((int)(int16_t)c->owner);
    if (k < 0 || k == o || (o >= 0 && mode_same_side_records(k, o))) return;
    g_mode.players[k].castles_razed++;
}

void install_hooks() {
    g_hook_mode_wizard_died = on_wizard_died;
    g_hook_mode_creature_died = on_creature_died;
    g_hook_mode_castle_gone = on_castle_gone;
}
// Installed whenever mode_rules.cpp is linked (every call site tests mode_active() first); the level start
// installs them again.
struct HookInstaller { HookInstaller() { install_hooks(); } } s_hook_installer;

// ---- the tick parts ----------------------------------------------------------------------------------------------

void tick_players() {
    const int n = record_count();
    for (int p = 0; p < n; p++) {
        if (!in_match(p) || eliminated(p)) continue;
        ModePlayer &m = g_mode.players[p];
        const PlayerRec &rec = g_state->players[p];
        if (rec.active == 0) { eliminate(p); continue; }    // left the game (command 2 / 0x1d)
        Thing *w = wizard_of(p);
        if (!w) continue;                                   // not spawned yet
        const Thing *c = castle_of(p);
        // lying dead (state 3, after the fall of state 2 that drops the spells and counts the death)
        const bool dead = w->state == 3;
        if (dead && !c) { eliminate(p); continue; }         // the original's "no castle = level lost"
        if (dead) {
            if (m.respawn_ticks > 0) m.respawn_ticks--;
        } else if (wizard_alive(w)) {
            m.respawn_ticks = 0;
        }
        if (!wizard_alive(w)) stop_desecrating(m);
        if (c) {
            const int64_t mana = record_castle_mana(p);
            const int32_t v = mana > INT32_MAX ? INT32_MAX : (int32_t)mana;
            if (v > m.mana_peak) m.mana_peak = v;
        }
    }
}

void tick_desecration(const MatchRulesResolved &r) {
    if (r.desecrate_ticks <= 0) return;
    const int n = record_count();
    for (int p = 0; p < n; p++) {
        if (!in_match(p) || eliminated(p)) continue;
        ModePlayer &m = g_mode.players[p];
        const Thing *w = wizard_of(p);
        if (!wizard_alive(w)) { stop_desecrating(m); continue; }
        int target = -1;
        // the castle being channelled stays the target while the wizard is inside it; otherwise the first enemy
        // castle (record order) whose courtyard holds him
        const int cur = m.desecrate_rec1 - 1;
        if (cur >= 0 && cur < n) {
            const Thing *c = castle_of(cur);
            if (c && !mode_same_side_records(p, cur) && inside_courtyard(w, c)) target = cur;
        }
        for (int q = 0; q < n && target < 0; q++) {
            if (q == p || !in_match(q) || mode_same_side_records(p, q)) continue;
            const Thing *c = castle_of(q);
            if (c && inside_courtyard(w, c)) target = q;
        }
        if (target < 0) {
            // Outside: the channel on the castle left drains at twice the rate it built up instead of being lost at
            // once (user, 2026-10-10: a human flying passes through a courtyard was thrown back to 0 every three
            // seconds and never razed a castle, while a computer wizard hovers in it for the 25 s).
            const bool keep = cur >= 0 && cur < n && castle_of(cur) && m.desecrate_ticks > 2;
            if (!keep) { stop_desecrating(m); continue; }
            m.flags &= ~MODE_PLAYER_DESECRATING;
            m.desecrate_ticks -= 2;
            continue;
        }
        if (m.desecrate_rec1 != target + 1) { m.desecrate_rec1 = target + 1; m.desecrate_ticks = 0; }
        m.flags |= MODE_PLAYER_DESECRATING;
        if (m.desecrate_ticks < r.desecrate_ticks) m.desecrate_ticks++;
        if (m.desecrate_ticks < r.desecrate_ticks) continue;
        // complete: razed once the castle is standing (state 4; a castle in its build sequencer has a build effect
        // in flight that would write into the deleted Thing - the channel waits at 100 %)
        Thing *c = castle_of(target);
        if (c->state != 4 || c->duration != 0) continue;    // (duration: the rebuild timer after a collapse)
        if (!desecrate_castle_level(c, p)) {
            m.desecrate_ticks = 0;          // one level down: this wizard's channel starts again for the next
            continue;
        }
        // the last level: every channel on that castle ends with it
        for (int q = 0; q < 8; q++)
            if (g_mode.players[q].desecrate_rec1 == target + 1) stop_desecrating(g_mode.players[q]);
        if (!wizard_alive(wizard_of(target))) eliminate(target);   // dead without a castle; alive: castle-less
    }
}

// The end of the match by the win conditions; true when it ended.
bool tick_win(const MatchRulesResolved &r) {
    if (r.win & MATCH_WIN_NONE) return false;
    const int n = record_count();
    // elimination: the last side standing (only a match that started with two sides or more)
    if ((r.win & MATCH_WIN_ELIMINATION) && sides_at_start() >= 2) {
        int alive = 0, last = -1;
        for (int s = 0; s < 8; s++)
            if (side_alive(s)) { alive++; if (last < 0) last = s; }
        if (alive == 1) return mode_rules_end_match(last, MATCH_END_ELIMINATION);
        if (alive == 0) {                                   // everybody out on the same tick: the ranking decides
            int order[8];
            return mode_rules_end_match(ranked_sides(order, -1) > 0 ? order[0] : -1, MATCH_END_ELIMINATION);
        }
    }
    // mana dominance: a side's castles + transit hold mana_pct of the world's mana for hold_ticks in a row
    if (r.win & MATCH_WIN_MANA) {
        const int64_t world = mode_world_mana();
        bool counted[8] = {};
        for (int p = 0; p < n; p++) {
            if (!in_match(p)) continue;
            const int s = g_mode.players[p].side & 7;
            if (counted[s]) continue;
            counted[s] = true;
            const bool holds = world > 0 && side_alive(s) && mode_side_castle_mana(s) * 100 >= world * r.mana_pct;
            uint32_t hold = 0;
            for (int q = 0; q < 8; q++)
                if (in_match(q) && g_mode.players[q].side == s) {
                    ModePlayer &m = g_mode.players[q];
                    m.mana_hold_ticks = holds ? m.mana_hold_ticks + 1 : 0;
                    if (m.mana_hold_ticks > hold) hold = m.mana_hold_ticks;
                }
            if (holds && (int64_t)hold >= (int64_t)r.hold_ticks) return mode_rules_end_match(s, MATCH_END_MANA);
        }
    }
    // the time limit: the side with the most castle mana (ties: most kills, then the lowest record)
    if (r.time_limit_ticks > 0 && (int64_t)g_mode.tick >= (int64_t)r.time_limit_ticks) {
        int order[8];
        return mode_rules_end_match(ranked_sides(order, -1) > 0 ? order[0] : -1, MATCH_END_TIMER);
    }
    return false;
}

// After the pause the level ends for every record on the same tick: the winners "won and left" (status 10, what
// command 0x1b sets), everybody else "left" (status 8, command 0x1d; not 0xc, whose lost bit would make
// game_level_tick restart the level as for a lost campaign level). game_level_tick ends the loop on status & 8.
void tick_over() {
    if ((int64_t)g_mode.tick < (int64_t)g_mode.match_end_tick + MODE_MATCH_END_DELAY_TICKS) return;
    const int n = record_count();
    const bool has_winner = g_mode.end_reason != MATCH_END_NONE && g_mode.winner_side < 8;
    for (int p = 0; p < n; p++) {
        const bool won = has_winner && in_match(p) && g_mode.players[p].side == g_mode.winner_side;
        g_state->players[p].status = (uint16_t)(won ? 10 : 8);
    }
}

} // namespace

MatchRulesResolved mode_rules_resolve(const MatchRules &m) {
    MatchRulesResolved r{};
    r.win = m.win != 0 ? m.win : (uint8_t)(MATCH_WIN_ELIMINATION | MATCH_WIN_MANA | (m.time_limit_min ? MATCH_WIN_TIMER : 0));
    r.mana_pct = m.mana_pct == 0 ? 75 : m.mana_pct > 100 ? 100 : m.mana_pct;
    r.hold_ticks = (m.mana_hold_s == 0 ? 30 : m.mana_hold_s) * TPS;
    r.time_limit_ticks = (r.win & MATCH_WIN_TIMER) ? (int)m.time_limit_min * 60 * TPS : 0;
    r.respawn_ticks = (m.respawn_s == 0 ? 10 : m.respawn_s) * TPS;
    r.respawn_level_ticks = (m.respawn_level_s == 0 ? 5 : m.respawn_level_s) * TPS;
    r.desecrate_ticks = m.desecrate_s == 0xff ? 0 : (m.desecrate_s == 0 ? 25 : m.desecrate_s) * TPS;
    return r;
}

void mode_rules_level_start() {
    if (!mode_active()) return;
    install_hooks();
    g_mode.match_state = MATCH_RUNNING;
    g_mode.match_end_tick = 0;
    g_mode.winner_side = 0;
    g_mode.end_reason = MATCH_END_NONE;
    // sides: records with the same non-zero MatchRules.team share one; team 0 = a side of its own. Side numbers in
    // record order of first appearance (0..7), so no side number is shared by accident.
    const int n = record_count();
    int side_of_team[256];
    for (int &s : side_of_team) s = -1;
    int next = 0;
    for (int p = 0; p < 8; p++) {
        ModePlayer &m = g_mode.players[p];
        m.flags &= ~(MODE_PLAYER_IN_MATCH | MODE_PLAYER_BOT | MODE_PLAYER_ELIMINATED | MODE_PLAYER_DESECRATING);
        m.side = 0;
        m.place = 0;
        m.respawn_ticks = 0;
        m.desecrate_rec1 = 0;
        m.desecrate_ticks = 0;
        m.eliminated_tick = 0;
        m.mana_hold_ticks = 0;
        if (p >= n) continue;
        m.flags |= MODE_PLAYER_IN_MATCH;
        if (g_state->players[p].is_computer == 1) m.flags |= MODE_PLAYER_BOT;
        const uint8_t team = g_mode.params.match.team[p];
        int side;
        if (team == 0) side = next++;
        else if (side_of_team[team] >= 0) side = side_of_team[team];
        else side = side_of_team[team] = next++;
        m.side = (uint8_t)side;
    }
    mode_rules_sync();
}

// The dead rise (user, 2026-10-10: the undead should be a lasting threat - the wild skeletons a world starts with are
// shot down by the towns' archers and the wizards within a quarter of an hour). Every five minutes of a generated
// world's match, two rings of the Skeleton Army effect (class 10 type 0x24: up to 8 skeletons each, 64 wild ones at
// most) rise on land 40 cells or more from every castle; they march on the nearest castle and turn the villagers they
// reach, as the original's skeletons do. The place comes from the mode's own RNG (simulated: the same on every peer).
// Skipped while 40 wild skeletons or more are about.
static void tick_undead() {
    constexpr uint32_t kEvery = 5 * 60 * MODE_TICKS_PER_SECOND;
    if (!mode_params_generated(g_mode.params) || g_mode.tick == 0 || g_mode.tick % kEvery != 0) return;
    int wild = 0;
    for (uint32_t i = g_cfg->creature_lists[9]; i != 0 && i < (uint32_t)thing_pool_slots(); i = thing_at(i)->next)
        wild += thing_at(i)->mana_owner == 0;
    if (wild >= 40) return;
    const int n = record_count();
    for (int attempt = 0; attempt < 64; attempt++) {
        const uint32_t r = mode_rng_next();
        const int cx = (int)(r & g_world.cell_mask), cy = (int)((r >> 12) & g_world.cell_mask);
        if (g_map_height[world_cell(cx, cy)] == 0) continue;                // water
        bool clear = true;
        for (int p = 0; p < n && clear; p++) {
            const Thing *c = castle_of(p);
            if (!c) continue;
            int dx = world_signed(c->x - (cx << 8)), dy = world_signed(c->y - (cy << 8));
            if (dx < 0) dx = -dx;
            if (dy < 0) dy = -dy;
            clear = dx >= 40 * 256 || dy >= 40 * 256;
        }
        if (!clear) continue;
        for (int k = 0; k < 2; k++) {
            Pos pos{};
            pos.x = (cx << 8) + (k ? 0x500 : -0x500);
            pos.y = cy << 8;
            pos.z = (int16_t)terrain_height_at(&pos);
            thing_create(&pos, 10, 0x24);
        }
        return;
    }
}

void mode_rules_tick() {
    if (!mode_active()) return;
    if (g_mode.match_state == MATCH_OVER) { tick_over(); return; }
    const MatchRulesResolved r = mode_rules_resolve(g_mode.params.match);
    tick_undead();
    tick_players();
    tick_desecration(r);
    tick_win(r);
}

bool mode_rules_end_match(int winner_side, uint32_t reason) {
    if (!mode_active() || g_mode.match_state != MATCH_RUNNING) return false;
    g_mode.match_state = MATCH_OVER;
    g_mode.match_end_tick = g_mode.tick;
    g_mode.winner_side = (winner_side >= 0 && winner_side < 8) ? (uint32_t)winner_side : 0xffu;
    g_mode.end_reason = reason;
    // places: the winning side 1, the others in ranking order
    int order[8];
    const int ns = ranked_sides(order, winner_side);
    int place_of_side[8] = {};
    int place = 1;
    if (winner_side >= 0 && winner_side < 8) place_of_side[winner_side] = place++;
    for (int i = 0; i < ns; i++) place_of_side[order[i]] = place++;
    for (int p = 0; p < 8; p++) {
        ModePlayer &m = g_mode.players[p];
        stop_desecrating(m);
        m.respawn_ticks = 0;
        m.place = in_match(p) ? (uint8_t)place_of_side[m.side & 7] : 0;
    }
    return true;
}

bool mode_rules_debug_end(int rec, bool win) {
    if (!mode_active() || rec < 0 || rec > 7 || g_mode.match_state != MATCH_RUNNING) return false;
    const int side = g_mode.players[rec].side;
    if (win) return mode_rules_end_match(side, MATCH_END_DEBUG);
    int order[8];
    return mode_rules_end_match(ranked_sides(order, side) > 0 ? order[0] : -1, MATCH_END_DEBUG);
}

bool mode_match_over() { return g_mode.match_state == MATCH_OVER; }

int mode_respawn_seconds(int rec) {
    if (!mode_active() || rec < 0 || rec > 7) return 0;
    const int t = g_mode.players[rec].respawn_ticks;
    return t > 0 ? (t + TPS - 1) / TPS : 0;
}

int mode_desecration_pct(int rec) {
    if (!mode_active() || rec < 0 || rec > 7) return 0;
    const MatchRulesResolved r = mode_rules_resolve(g_mode.params.match);
    if (r.desecrate_ticks <= 0) return 0;
    int best = 0;
    for (int p = 0; p < 8; p++) {
        const ModePlayer &m = g_mode.players[p];
        if (m.desecrate_rec1 == rec + 1 && m.desecrate_ticks > best) best = m.desecrate_ticks;
    }
    const int pct = (int)((int64_t)best * 100 / r.desecrate_ticks);
    return pct > 100 ? 100 : pct;
}

int mode_time_left_seconds() {
    if (!mode_active()) return -1;
    const MatchRulesResolved r = mode_rules_resolve(g_mode.params.match);
    if (r.time_limit_ticks <= 0) return -1;
    const int64_t end = g_mode.match_state == MATCH_OVER ? (int64_t)g_mode.match_end_tick : (int64_t)g_mode.tick;
    const int64_t left = (int64_t)r.time_limit_ticks - end;
    return left > 0 ? (int)((left + TPS - 1) / TPS) : 0;
}

int64_t mode_side_castle_mana(int side) {
    int64_t sum = 0;
    for (int p = 0; p < 8; p++)
        if (in_match(p) && g_mode.players[p].side == side && !eliminated(p)) sum += record_castle_mana(p);
    return sum;
}

int64_t mode_world_mana() {
    // mana_totals_update: total_mana = P(local).mana + the mana of every creature, castle, balloon, ball and village
    const Thing *lw = thing_at(thing_wrap(g_state->players[g_state->local_player & 7].thing));
    const int64_t w = (int64_t)g_cfg->total_mana - (int64_t)player_block(lw)->mana;
    return w > 0 ? w : 0;
}
