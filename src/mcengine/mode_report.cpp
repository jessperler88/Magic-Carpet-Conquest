// Match report of a mode run (mode_report.h). Read-only: nothing here writes game state.
#define _CRT_SECURE_NO_WARNINGS
#include "mode_report.h"
#include "mode.h"
#include "mode_level.h"
#include "mode_bots.h"
#include "projectiles.h"
#include "thing.h"
#include "player.h"
#include "mc_globals.h"
#include "level.h"
#include "world.h"
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

namespace {

constexpr int kSampleTicks = 30 * MODE_TICKS_PER_SECOND;
constexpr size_t kMaxEvents = 4000;     // (600 until 2026-10-11: a 40-minute match of 8 dropped its last 312 - the endgame)

struct Sample { uint32_t tick; int16_t level[8]; int32_t castle_mana[8], mana_total[8]; };

struct Seen {                       // one wizard, as of the last tick
    bool     spawned = false;
    int      state = -1;
    uint16_t castle = 0;
    int      level = 0;             // castle level (aux + 1), 0 = none
    int      level_max = 0;
    uint32_t spells = 0;            // bit per spell owned
    uint32_t flags = 0;             // ModePlayer.flags
    int      castles_founded = 0, castles_lost = 0, levels_lost = 0;
    int      mana_total_max = 0;
    uint32_t mode_ticks[16] = {};   // computer wizards: ticks alive per AI mode
    uint32_t alive_ticks = 0;
    uint16_t cooldown[24] = {};     // computer wizards: last tick's spell cooldowns (a rise = a cast)
    uint32_t casts[24] = {};
    uint32_t towns_peak = 0;        // most town buildings held at once
};

struct Recording {
    bool       active = false;
    ModeParams params;
    uint32_t   template_level = 0;
    uint32_t   last_tick = 0;
    uint32_t   first_tick = 0;      // > 1: the recording began in the middle of a match (a loaded state)
    Seen       seen[8];
    std::vector<Sample> samples;
    std::vector<std::string> events;
    size_t     events_dropped = 0;
};
Recording s_rec;

void appendf(std::string *out, const char *fmt, ...) {
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    *out += b;
}

std::string clock_of(uint32_t tick) {
    const uint32_t s = tick / MODE_TICKS_PER_SECOND;
    char b[32];
    std::snprintf(b, sizeof b, "%u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
    return b;
}

std::string thing_name(const Thing *t) {
    if (!t || t->cls == 0) return "nothing";
    char b[64];
    if (t->cls == 5) {
        const char *n = mc_model_name(5, t->type);
        if (n && std::strcmp(n, "?")) return n;
        std::snprintf(b, sizeof b, "creature %d", t->type);
        return b;
    }
    if (t->cls == 3) {
        const char *what = t->type == 2 ? "castle" : t->type == 3 ? "balloon" : "wizard";
        for (int p = 0; p < 8; p++)
            if (g_state->players[p].thing != 0 && g_state->players[p].thing == (t->type <= 1 ? thing_index(t) : t->owner)) {
                std::snprintf(b, sizeof b, "player %d's %s", p, what);
                return b;
            }
        return what;
    }
    std::snprintf(b, sizeof b, "class %d type %d", t->cls, t->type);
    return b;
}

// Who destroyed / killed `t`: the Thing its killer index names, or (a projectile / effect) that Thing's owner.
std::string killer_name(const Thing *t) {
    const Thing *k = thing_at(thing_wrap(t->killer));
    if (t->killer == 0 || k->cls == 0) return "unknown";
    if ((k->cls == 9 || k->cls == 10) && k->owner > 0 && k->owner < thing_pool_slots()) {
        const Thing *o = thing_at((unsigned)k->owner);
        if (o->cls != 0) return thing_name(o);
    }
    return thing_name(k);
}

void event(uint32_t tick, int p, const char *fmt, ...) {
    if (s_rec.events.size() >= kMaxEvents) { s_rec.events_dropped++; return; }
    char msg[256], line[320];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    std::snprintf(line, sizeof line, "%7s  p%d  %s", clock_of(tick).c_str(), p, msg);
    s_rec.events.push_back(line);
}

int player_count() {
    int n = g_state->player_count;
    return n < 1 ? 1 : n > 8 ? 8 : n;
}

const Thing *wizard_of(int p) {
    const PlayerRec &r = g_state->players[p];
    const Thing *t = thing_at(thing_wrap(r.thing));
    return (r.thing != 0 && t->cls == 3 && t->type <= 1) ? t : nullptr;
}
// Flying: a computer wizard (type 1) in state 1 (2 / 3: dying / dead); a human's flyer while its health is not negative.
bool wizard_alive(const Thing *t) { return t->health >= 0 && (t->type == 0 || t->state == 1); }
const Thing *castle_of(int p) {
    const uint16_t ci = g_state->players[p].blk.castle;
    const Thing *c = thing_at(thing_wrap(ci));
    return (ci != 0 && c->cls == 3 && c->type == 2) ? c : nullptr;
}

const char *end_reason_name(uint32_t e) {
    switch (e) {
    case MATCH_END_ELIMINATION: return "elimination";
    case MATCH_END_MANA: return "mana dominance";
    case MATCH_END_TIMER: return "time limit";
    case MATCH_END_DEBUG: return "debug command";
    default: return "-";
    }
}

std::string spell_list(uint32_t bits) {
    std::string s;
    for (int id = 0; id < 24; id++) {
        if (!(bits >> id & 1)) continue;
        if (!s.empty()) s += ", ";
        s += mc_model_name(12, id);
    }
    return s.empty() ? "none" : s;
}

void take_sample() {
    Sample s{};
    s.tick = g_mode.tick;
    for (int p = 0; p < player_count(); p++) {
        const Thing *t = wizard_of(p), *c = castle_of(p);
        s.level[p] = (int16_t)(c ? c->aux + 1 : 0);
        s.castle_mana[p] = c ? c->mana : 0;
        s.mana_total[p] = t ? t->mana_total : 0;
    }
    s_rec.samples.push_back(s);
}

} // namespace

void mode_report_tick() {
    if (!mode_active() || !g_state || !g_cfg) return;
    if (!s_rec.active || g_mode.tick < s_rec.last_tick || std::memcmp(&g_mode.params, &s_rec.params, sizeof(ModeParams)) != 0) {
        s_rec = Recording{};
        s_rec.active = true;
        s_rec.params = g_mode.params;
        s_rec.first_tick = g_mode.tick;
        std::memset(g_ai_shot_stats, 0, sizeof g_ai_shot_stats);   // the bots' shot counters (debug, not game state)
    }
    s_rec.template_level = g_mode.template_level;
    s_rec.last_tick = g_mode.tick;
    const uint32_t tick = g_mode.tick;
    for (int p = 0; p < player_count(); p++) {
        if (!(g_mode.players[p].flags & MODE_PLAYER_IN_MATCH)) continue;
        Seen &s = s_rec.seen[p];
        const PlayerRec &rec = g_state->players[p];
        const Thing *t = wizard_of(p);
        if (!t) continue;
        const bool first = !s.spawned;
        s.spawned = true;
        // the wizard
        const int alive = wizard_alive(t) ? 1 : 0;
        if (!first && s.state == 1 && !alive) event(tick, p, "killed by %s", killer_name(t).c_str());
        if (!first && s.state == 0 && alive) event(tick, p, "respawned");
        s.state = alive;
        if (t->mana_total > s.mana_total_max) s.mana_total_max = t->mana_total;
        if (alive) {
            s.alive_ticks++;
            if (rec.is_computer == 1) s.mode_ticks[rec.blk.ai_mode & 15]++;
        }
        if (rec.is_computer == 1)
            for (int id = 0; id < 24; id++) {
                if (rec.blk.spell_cooldown[id] > s.cooldown[id]) s.casts[id]++;
                s.cooldown[id] = rec.blk.spell_cooldown[id];
            }
        if (tick % 250 == 0) {
            uint32_t towns = 0;
            const uint16_t self = rec.thing;
            for (uint32_t i = g_cfg->wizard_list; i != 0 && i < (uint32_t)thing_pool_slots(); i = thing_at(i)->next)
                towns += thing_at(i)->mana_owner == self;
            if (towns > s.towns_peak) s.towns_peak = towns;
        }
        // the castle
        const Thing *c = castle_of(p);
        const uint16_t ci = c ? rec.blk.castle : 0;
        const int level = c ? c->aux + 1 : 0;
        if (ci != s.castle) {
            if (s.castle != 0 && !first) {
                const Thing *old = thing_at(thing_wrap(s.castle));
                event(tick, p, "castle destroyed by %s", killer_name(old).c_str());
                s.castles_lost++;
            }
            if (ci != 0) {
                if (!first) event(tick, p, "castle founded at cell %d,%d", (int)(c->x >> 8), (int)(c->y >> 8));
                s.castles_founded++;
            }
        } else if (ci != 0 && level != s.level && !first) {
            if (level > s.level) event(tick, p, "castle grown to level %d", level);
            else {
                event(tick, p, "castle knocked down to level %d by %s", level, killer_name(c).c_str());
                s.levels_lost += s.level - level;
            }
        }
        s.castle = ci;
        s.level = level;
        if (level > s.level_max) s.level_max = level;
        // the spells
        uint32_t spells = 0;
        for (int32_t idx : rec.blk.spell_slot) {                // (the book: spell_thing[] is only kept for the AI)
            if (idx <= 0 || idx >= thing_pool_slots()) continue;
            const Thing *sp = thing_at((unsigned)idx);
            if (sp->cls == 12 && sp->type < 24) spells |= 1u << sp->type;
        }
        if (alive) {                                            // (the book is rebuilt on a respawn)
            const uint32_t fresh = spells & ~s.spells;
            if (!first)
                for (int id = 0; id < 24; id++) if (fresh >> id & 1) event(tick, p, "learnt %s", mc_model_name(12, id));
            s.spells |= spells;
        }
        // the match flags
        const uint32_t flags = g_mode.players[p].flags;
        if (!first && (flags & MODE_PLAYER_ELIMINATED) && !(s.flags & MODE_PLAYER_ELIMINATED)) event(tick, p, "ELIMINATED");
        if (!first && (flags & MODE_PLAYER_DESECRATING) && !(s.flags & MODE_PLAYER_DESECRATING))
            event(tick, p, "began desecrating player %d's castle", (int)g_mode.players[p].desecrate_rec1 - 1);
        s.flags = flags;
    }
    if (s_rec.samples.empty() || tick - s_rec.samples.back().tick >= (uint32_t)kSampleTicks) take_sample();
}

std::string mode_report_name() {
    if (!s_rec.active) return "";
    char b[96];
    std::snprintf(b, sizeof b, "conquest_s%u_%u_%ubots_t%u", (unsigned)s_rec.params.seed, (unsigned)s_rec.params.world_size,
                  (unsigned)s_rec.params.bots, (unsigned)s_rec.last_tick);
    return b;
}

bool mode_report_text(std::string *out) {
    out->clear();
    if (!s_rec.active || !mode_active() || !g_state || !g_cfg) return false;
    const ModeState &m = g_mode;
    const ModeParams &mp = m.params;
    const int n = player_count();
    const int local = g_state->local_player & 7;

    appendf(out, "MAGIC CARPET CONQUEST - MATCH REPORT\n");
    appendf(out, "setup:   seed %u, %u cells, %u humans + %u bots (skill %s), template level %u, generated world %s\n",
            (unsigned)mp.seed, (unsigned)mp.world_size, (unsigned)mp.humans, (unsigned)mp.bots,
            mode_bot_skill_name(mode_bot_skill(mp.match.bot_skill)), (unsigned)m.template_level,
            mode_params_generated(mp) ? "yes" : "no");
    appendf(out, "world:   symmetry %u, roughness %u, water %u, continent %u, springs %u, wild %u, lairs %u, features 0x%02x "
            "(0 = default)\n", mp.world.symmetry, mp.world.roughness, mp.world.water, mp.world.continent, mp.world.springs,
            mp.world.wild, mp.world.lairs, mp.world.features);
    appendf(out, "rules:   %s\n", mode_match_rules_describe(mp.match).c_str());
    appendf(out, "length:  %s (%u ticks)%s\n", clock_of(m.tick).c_str(), (unsigned)m.tick,
            s_rec.first_tick > 1 ? "  [recorded from a loaded state: events and timeline start there]" : "");
    if (m.match_state == MATCH_OVER)
        appendf(out, "result:  over at %s by %s, winning side %u\n", clock_of(m.match_end_tick).c_str(),
                end_reason_name(m.end_reason), (unsigned)m.winner_side);
    else
        appendf(out, "result:  not decided (the match was still running)\n");

    appendf(out, "\nPLAYERS\n");
    appendf(out, "  p  kind   side place  state       castle  (max)  castle mana  mana total  (max)    peak mana  "
                 "kills w/c  deaths  razed  castles +/-  levels lost\n");
    for (int p = 0; p < n; p++) {
        const ModePlayer &q = m.players[p];
        if (!(q.flags & MODE_PLAYER_IN_MATCH)) continue;
        const Seen &s = s_rec.seen[p];
        const Thing *t = wizard_of(p), *c = castle_of(p);
        const char *state = (q.flags & MODE_PLAYER_ELIMINATED) ? "eliminated" : (t && wizard_alive(t)) ? "alive" : "dead";
        char out_at[24] = "";
        if (q.flags & MODE_PLAYER_ELIMINATED) std::snprintf(out_at, sizeof out_at, "@%s", clock_of(q.eliminated_tick).c_str());
        appendf(out, "  %d  %-5s  %4u %5u  %-10s%-6s  L%d  (L%d)  %11d  %10d  %-8d %9d  %5u/%-4u  %5u  %5u  %5d/%-5d  %6d\n", p,
                (q.flags & MODE_PLAYER_BOT) ? "bot" : (p == local ? "you" : "human"), (unsigned)q.side, (unsigned)q.place, state,
                out_at, c ? c->aux + 1 : 0, s.level_max, c ? (int)c->mana : 0, t ? (int)t->mana_total : 0, s.mana_total_max,
                (int)q.mana_peak, (unsigned)q.kills_wizards, (unsigned)q.kills_creatures, (unsigned)q.deaths,
                (unsigned)q.castles_razed, s.castles_founded, s.castles_lost, s.levels_lost);
    }
    appendf(out, "\nSPELLS OWNED DURING THE MATCH\n");
    for (int p = 0; p < n; p++) {
        if (!(m.players[p].flags & MODE_PLAYER_IN_MATCH)) continue;
        appendf(out, "  p%d  %s\n", p, spell_list(s_rec.seen[p].spells).c_str());
    }
    {
        const PlayerBlock &P = g_state->players[local].blk;
        appendf(out, "\nYOU (p%d): %d shots, %d hits, %d kills\n", local, (int)P.shots, (int)P.hits, (int)P.kills);
    }

    appendf(out, "\nBOT BEHAVIOUR (%% of the ticks alive per AI mode; knobs = aggression / accuracy / reaction)\n");
    appendf(out, "  p  knobs        upgrade  to-site  fetch-urn  mana  raid-castle  fight-wizard  balloon  go-home  idle  creatures\n");
    for (int p = 0; p < n; p++) {
        if (!(m.players[p].flags & MODE_PLAYER_BOT)) continue;
        const Seen &s = s_rec.seen[p];
        const PlayerBlock &P = g_state->players[p].blk;
        const double a = s.alive_ticks ? 100.0 / s.alive_ticks : 0.0;
        appendf(out, "  %d  %3d/%3d/%3d  %7.1f  %7.1f  %9.1f  %4.1f  %11.1f  %12.1f  %7.1f  %7.1f  %4.1f  %9.1f\n", p,
                (int)P.ai_aggression, (int)P.ai_accuracy, (int)P.ai_reaction, s.mode_ticks[1] * a, s.mode_ticks[3] * a,
                s.mode_ticks[4] * a, s.mode_ticks[6] * a, s.mode_ticks[7] * a, s.mode_ticks[8] * a, s.mode_ticks[9] * a,
                s.mode_ticks[0xb] * a, s.mode_ticks[0xc] * a, s.mode_ticks[0xd] * a);
    }

    appendf(out, "\nBOT ACCURACY (shots / %% that hit the Thing the bot was targeting; fireball and lightning, by kind of target)\n");
    appendf(out, "  p  fireball: creature      wizard        castle        lightning: creature      wizard        castle\n");
    for (int p = 0; p < n; p++) {
        if (!(m.players[p].flags & MODE_PLAYER_BOT)) continue;
        appendf(out, "  %d          ", p);
        for (int w = 0; w < 2; w++) {
            for (int k = 0; k < 3; k++) {
                const AiShotStats &s = g_ai_shot_stats[p][w][k];
                char cell[32];
                std::snprintf(cell, sizeof cell, "%u/%.0f%%", (unsigned)s.shots, s.shots ? 100.0 * s.hit_target / s.shots : 0.0);
                appendf(out, " %-13s", cell);
            }
            if (w == 0) appendf(out, "           ");
        }
        *out += '\n';
    }

    appendf(out, "\nBOT CASTS (spells cast, by spell id; towns = most town buildings held at once)\n");
    appendf(out, "  p  fireball  heal  speed  possess  shield  meteor  volcano  invisible  rebound  lightning  castle  skeleton  towns\n");
    for (int p = 0; p < n; p++) {
        if (!(m.players[p].flags & MODE_PLAYER_BOT)) continue;
        const Seen &s = s_rec.seen[p];
        appendf(out, "  %d  %8u  %4u  %5u  %7u  %6u  %6u  %7u  %9u  %7u  %9u  %6u  %8u  %5u\n", p, (unsigned)s.casts[0], (unsigned)s.casts[1],
                (unsigned)s.casts[2], (unsigned)s.casts[3], (unsigned)s.casts[4], (unsigned)s.casts[7], (unsigned)s.casts[8],
                (unsigned)s.casts[0xc], (unsigned)s.casts[0xe], (unsigned)s.casts[0xf], (unsigned)s.casts[0x10], (unsigned)s.casts[0x11],
                (unsigned)s.towns_peak);
    }

    appendf(out, "\nTIMELINE (every 30 s: castle level / castle mana in thousands / mana total in thousands)\n   time ");
    for (int p = 0; p < n; p++) if (m.players[p].flags & MODE_PLAYER_IN_MATCH) appendf(out, "  p%d          ", p);
    *out += '\n';
    for (const Sample &s : s_rec.samples) {
        appendf(out, "%7s ", clock_of(s.tick).c_str());
        for (int p = 0; p < n; p++) {
            if (!(m.players[p].flags & MODE_PLAYER_IN_MATCH)) continue;
            char cell[32];
            std::snprintf(cell, sizeof cell, "L%d/%d/%d", s.level[p], s.castle_mana[p] / 1000, s.mana_total[p] / 1000);
            appendf(out, "  %-12s", cell);
        }
        *out += '\n';
    }

    appendf(out, "\nEVENTS\n");
    for (const std::string &e : s_rec.events) { *out += e; *out += '\n'; }
    if (s_rec.events_dropped) appendf(out, "  (+ %u more events not kept)\n", (unsigned)s_rec.events_dropped);

    // the world at the end
    std::map<std::string, int> creatures;
    int balls_free = 0, balls_owned = 0, urns = 0;
    long long free_mana = 0;
    for (int i = 1; i < thing_pool_slots(); i++) {
        const Thing *t = thing_at((unsigned)i);
        if (t->cls == 5 && t->health > 0) creatures[thing_name(t)]++;
        else if (t->cls == 12 && t->type < 24 && t->state == t->type * 3 + 2) urns++;
    }
    for (uint32_t bi = g_cfg->mana_ball_list; bi != 0 && bi < (uint32_t)thing_pool_slots(); bi = thing_at(bi)->next) {
        const Thing *ball = thing_at(bi);
        if (thing_at(thing_wrap(ball->mana_owner))->cls == 3) balls_owned++;
        else { balls_free++; free_mana += ball->mana; }
    }
    appendf(out, "\nWORLD AT THE END\n  mana: world total %u; free balls %d holding %lld; claimed balls %d; spell urns %d\n",
            (unsigned)g_cfg->total_mana, balls_free, free_mana, balls_owned, urns);
    appendf(out, "  creatures alive (worms and dragons count per body segment):");
    for (const auto &e : creatures) appendf(out, " %s %d,", e.first.c_str(), e.second);
    if (!creatures.empty()) out->pop_back();
    appendf(out, "\n  things %d of %d slots\n", thing_pool_live_count(), thing_pool_slots());
    return true;
}
