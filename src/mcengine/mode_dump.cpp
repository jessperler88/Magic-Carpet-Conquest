// JSON state dumps (mode_dump.h). Round 10 task A. Read-only.
#define _CRT_SECURE_NO_WARNINGS
#include "mode_dump.h"
#include "mode.h"
#include "thing.h"
#include "player.h"
#include "net.h"
#include "settings.h"
#include "mc_globals.h"
#include "level.h"
#include "world.h"
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>

namespace {

// A tiny JSON writer: objects / arrays with automatic commas, strings escaped.
struct Json {
    std::string *s;
    bool first = true;
    explicit Json(std::string *out) : s(out) {}
    void sep() { if (!first) *s += ','; first = false; }
    void key(const char *k) { sep(); str_raw(k); *s += ':'; first = true; }
    void str_raw(const char *v) {
        *s += '"';
        for (const unsigned char *p = reinterpret_cast<const unsigned char *>(v); *p; p++) {
            if (*p == '"' || *p == '\\') { *s += '\\'; *s += (char)*p; }
            else if (*p < 0x20 || *p >= 0x7f) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", *p); *s += b; }
            else *s += (char)*p;
        }
        *s += '"';
    }
    void open(char c) { sep(); *s += c; first = true; }
    void close(char c) { *s += c; first = false; }
    void num(long long v) { sep(); *s += std::to_string(v); }
    void unum(unsigned long long v) { sep(); *s += std::to_string(v); }
    void boolean(bool v) { sep(); *s += v ? "true" : "false"; }
    void null() { sep(); *s += "null"; }
    void str(const char *v) { sep(); str_raw(v); }
    void hex(uint32_t v) { char b[16]; std::snprintf(b, sizeof b, "0x%08x", (unsigned)v); str(b); }
    // key + value shorthands
    void kv(const char *k, long long v) { key(k); num(v); }
    void kvu(const char *k, unsigned long long v) { key(k); unum(v); }
    void kvs(const char *k, const char *v) { key(k); str(v); }
    void kvb(const char *k, bool v) { key(k); boolean(v); }
    void kvh(const char *k, uint32_t v) { key(k); hex(v); }
    void obj(const char *k) { key(k); open('{'); }
    void arr(const char *k) { key(k); open('['); }
};

const char *class_name(int cls) {
    switch (cls) {
    case 2: return "scenery";
    case 3: return "player";
    case 5: return "creature";
    case 7: return "weather";
    case 9: return "projectile";
    case 10: return "effect";
    case 11: return "switch";
    case 12: return "spell";
    default: return nullptr;
    }
}
const char *player_type_name(int type) {        // class 3 (player.h, ai_wizard.h, castle.h)
    switch (type) {
    case 0: return "flyer";
    case 1: return "ai_wizard";
    case 2: return "castle";
    case 3: return "balloon";
    default: return nullptr;
    }
}

int player_count() {
    int n = g_state->player_count;
    if (n < 1) n = 1;
    if (n > 8) n = 8;
    return n;
}

} // namespace

void mode_dump_json(std::string *out, long ticks_run, const char *extra_members) {
    out->clear();
    if (!g_state || !g_cfg) { *out = "{}"; return; }
    Json j(out);
    const GameState *s = g_state;
    const int local = s->local_player & 7;
    j.open('{');
    j.kvs("format", "mcport-dump");
    j.kv("version", 1);
    j.kvu("tick", s->players[local].tick);
    if (ticks_run >= 0) j.kv("ticks_run", ticks_run);
    j.kv("level", g_cfg->level);
    j.kvs("mode", mode_name(g_mode.mode));
    j.kvu("mode_id", g_mode.mode);
    j.kvu("seed", mode_active() ? g_mode.params.seed : 0u);
    j.kvh("checksum", net_state_checksum());
    {   // round 11: the world (world.h); positions below are int32 world units 0 .. units-1
        j.obj("world");
        j.kv("size", g_world.size);
        j.kv("units", g_world.units);
        j.kvu("cells", g_world.cells);
        j.close('}');
    }
    // TODO(round 10 task E): the named parts of NetChecksumParts (net.h net_state_checksum_parts) go here as
    // {"<part name>": "0x%08x", ...} once they exist.
    j.key("checksum_parts"); j.null();
    {
        const GameplayRules r = gameplay_rules();
        j.obj("rules");
        j.kv("possession_range_pct", r.possession_range_pct);
        j.kv("mode", r.mode);
        j.close('}');
    }
    {
        const ModeState &m = g_mode;
        j.obj("mode_block");
        j.kvu("version", m.version);
        j.kvu("mode", m.mode);
        j.kvu("tick", m.tick);
        j.kvh("rng", m.rng);
        j.kvu("debug_flags", m.debug_flags);
        j.kvu("template_level", m.template_level);
        j.kvh("ai_seed", m.ai_seed);
        j.obj("params");
        j.kvu("seed", m.params.seed);
        j.kvu("world_size", m.params.world_size);
        j.kvu("bots", m.params.bots);
        j.kvu("humans", m.params.humans);
        j.kvu("flags", m.params.flags);
        j.kvu("map", m.params.map);
        j.close('}');
        j.arr("player_flags");
        for (const ModePlayer &p : m.players) j.unum(p.flags);
        j.close(']');
        j.close('}');
    }
    j.obj("pool");
    j.kv("slots", thing_pool_slots());
    j.kv("live", thing_pool_live_count());
    j.kv("free_top", s->free_top);
    j.kv("active_top", s->active_top);
    j.kvu("alloc_failures", g_thing_alloc_failures);
    j.close('}');
    j.kvu("world_mana", g_cfg->total_mana);

    j.arr("players");
    for (int p = 0; p < player_count(); p++) {
        const PlayerRec &r = s->players[p];
        const PlayerBlock &P = r.blk;
        const unsigned ti = thing_wrap(r.thing);
        const Thing *t = thing_at(ti);
        const bool has_thing = r.thing != 0 && t->cls == 3;
        j.open('{');
        j.kv("index", p);
        j.kv("active", r.active);
        j.kv("is_computer", r.is_computer);
        j.kvb("local", p == local);
        char name[sizeof r.name + 1];
        std::memcpy(name, r.name, sizeof r.name);
        name[sizeof r.name] = 0;
        j.kvs("name", name);
        j.kv("status", r.status);
        j.kv("thing", r.thing);
        j.kvb("alive", has_thing && t->health >= 0);
        j.kv("state", has_thing ? t->state : -1);
        j.kv("health", has_thing ? t->health : 0);
        j.kv("max_health", has_thing ? t->max_health : 0);
        j.kv("mana", has_thing ? t->mana_total : 0);
        j.kv("wizard_mana", P.mana);
        j.kv("x", has_thing ? (int32_t)t->x : 0);
        j.kv("y", has_thing ? (int32_t)t->y : 0);
        j.kv("z", has_thing ? t->z : 0);
        j.kv("ai_mode", P.ai_mode);
        j.kv("kills", P.kills);
        j.arr("kills_of_player");
        for (uint16_t k : P.kills_of_player) j.num(k);
        j.close(']');
        j.kv("shots", P.shots);
        j.kv("hits", P.hits);
        int spells = 0;
        for (int32_t v : P.spell_slot) if (v > 0) spells++;
        j.kv("spells", spells);
        j.kvu("mode_flags", g_mode.players[p].flags);
        const Thing *c = thing_at(thing_wrap(P.castle));
        if (P.castle != 0 && c->cls == 3 && c->type == 2) {
            j.obj("castle");
            j.kv("thing", P.castle);
            j.kv("level", P.castle_level);
            j.kv("health", c->health);
            j.kv("max_health", c->max_health);
            j.kv("mana", c->mana);
            j.close('}');
        } else {
            j.key("castle"); j.null();
        }
        j.close('}');
    }
    j.close(']');

    // census in slot order; names sorted (std::map) so the document is stable
    std::map<std::string, int> by_class, creatures, player_things, effects;
    int total = 0;
    for (int i = 1; i < thing_pool_slots(); i++) {
        const Thing *t = thing_at((unsigned)i);
        if (t->cls == 0) continue;
        total++;
        const char *cn = class_name(t->cls);
        by_class[cn ? cn : "class" + std::to_string(t->cls)]++;
        if (t->cls == 5) {
            const char *n = mc_model_name(5, t->type);
            creatures[n && std::strcmp(n, "?") ? n : "type" + std::to_string(t->type)]++;
        } else if (t->cls == 3) {
            const char *n = player_type_name(t->type);
            player_things[n ? n : "type" + std::to_string(t->type)]++;
        } else if (t->cls == 10) {
            effects[std::to_string(t->type)]++;
        }
    }
    j.obj("census");
    j.kv("total", total);
    auto emit = [&](const char *k, const std::map<std::string, int> &m) {
        j.obj(k);
        for (const auto &e : m) j.kv(e.first.c_str(), e.second);
        j.close('}');
    };
    emit("by_class", by_class);
    emit("creatures", creatures);
    emit("player_things", player_things);
    emit("effects", effects);
    j.close('}');
    if (extra_members && *extra_members) { j.sep(); *out += extra_members; }
    j.close('}');
    *out += '\n';
}

void mode_dump_line(std::string *out, long ticks_run) {
    out->clear();
    if (!g_state || !g_cfg) return;
    char b[256];
    const GameState *s = g_state;
    std::snprintf(b, sizeof b, "dump tick=%u", (unsigned)s->players[s->local_player & 7].tick);
    *out += b;
    if (ticks_run >= 0) { std::snprintf(b, sizeof b, " run=%ld", ticks_run); *out += b; }
    int live = 0;
    for (int i = 1; i < thing_pool_slots(); i++) if (thing_at((unsigned)i)->cls) live++;
    std::snprintf(b, sizeof b, " checksum=0x%08x mode=%s seed=%u things=%d players=%d", (unsigned)net_state_checksum(),
                  mode_name(g_mode.mode), mode_active() ? (unsigned)g_mode.params.seed : 0u, live, player_count());
    *out += b;
    for (int p = 0; p < player_count(); p++) {
        const PlayerRec &r = s->players[p];
        const Thing *t = thing_at(thing_wrap(r.thing));
        const bool has = r.thing != 0 && t->cls == 3;
        const Thing *c = thing_at(thing_wrap(r.blk.castle));
        const bool castle = r.blk.castle != 0 && c->cls == 3 && c->type == 2;
        std::snprintf(b, sizeof b, " p%d=%s:h%d/%d,m%d,c%d,ai%d", p, r.is_computer ? "ai" : "hu", has ? (int)t->health : 0,
                      has ? (int)t->max_health : 0, has ? (int)t->mana_total : 0, castle ? (int)r.blk.castle_level : -1,
                      (int)r.blk.ai_mode);
        *out += b;
    }
}

bool mode_dump_write(const char *path, long ticks_run, const char *extra_members) {
    std::string doc;
    mode_dump_json(&doc, ticks_run, extra_members);
    std::FILE *f = std::fopen(path, "wb");
    if (!f) return false;
    const bool ok = std::fwrite(doc.data(), 1, doc.size(), f) == doc.size();
    return std::fclose(f) == 0 && ok;
}
