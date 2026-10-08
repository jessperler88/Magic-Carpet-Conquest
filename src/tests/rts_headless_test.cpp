// Round 10 task A (docs/analysis/port_mode.md): a game-mode run headless, without SDL or the renderer.
//
//  1. determinism: an rts run (seed 1, 3 bots, 2000 ticks) twice in the same process, with a campaign level
//     played in between (it leaves the AI's CRT seed, the cached human wizard and the campaign spells
//     elsewhere): identical net_state_checksum every tick (the mode block included);
//  2. the run is a game: the level is a multiplayer template, 4 players (record 0 human, 1..3 computer
//     wizards), the computer wizards act (castles built, AI modes change, they move), the mode block ticks;
//  3. the JSON dump (mode_dump_json) parses (a small JSON parser below) and has the expected structure and
//     values; dumping changes nothing (checksum before == after);
//  4. a savestate round trip inside the mode: save at tick T, run N, load, run N again - identical checksums
//     including the mode block; the same from a fresh process state through mode_start_run_for_state (what
//     mcport's `load` must do for an rts state);
//  5. the level restart path (sim_load_level(MODE_LEVEL_INDEX) again, as level_finish does) rebuilds the same
//     level; a different seed gives a different run; the original's levels are untouched (mode inactive).
// Prints the simulation speed (ticks / second, no rendering).
#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "sim.h"
#include "mode.h"
#include "mode_level.h"
#include "mode_dump.h"
#include "savegame.h"
#include "thing.h"
#include "player.h"
#include "net.h"
#include "ai_wizard.h"
#include "settings.h"
#include "mc_globals.h"
#include "crash_handler.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

// ---- a minimal JSON parser (enough to validate the dump) ------------------------------------------------
struct JVal {
    enum Kind { NUL, BOOL, NUM, STR, ARR, OBJ } kind = NUL;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<JVal> a;
    std::map<std::string, JVal> o;
    const JVal *get(const char *k) const { auto it = o.find(k); return it == o.end() ? nullptr : &it->second; }
};
struct JParser {
    const char *p, *end;
    bool ok = true;
    void ws() { while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++; }
    bool lit(const char *w) { size_t n = std::strlen(w); if ((size_t)(end - p) >= n && !std::strncmp(p, w, n)) { p += n; return true; } return false; }
    JVal fail() { ok = false; return JVal{}; }
    std::string str() {
        std::string r;
        if (p >= end || *p != '"') { ok = false; return r; }
        p++;
        while (p < end && *p != '"') {
            if ((unsigned char)*p < 0x20) { ok = false; return r; }
            if (*p == '\\') {
                p++;
                if (p >= end) { ok = false; return r; }
                switch (*p) {
                case '"': r += '"'; break;
                case '\\': r += '\\'; break;
                case '/': r += '/'; break;
                case 'n': r += '\n'; break;
                case 't': r += '\t'; break;
                case 'r': r += '\r'; break;
                case 'b': r += '\b'; break;
                case 'f': r += '\f'; break;
                case 'u': {
                    if (end - p < 5) { ok = false; return r; }
                    r += (char)std::strtol(std::string(p + 1, 4).c_str(), nullptr, 16);
                    p += 4;
                    break;
                }
                default: ok = false; return r;
                }
                p++;
            } else {
                r += *p++;
            }
        }
        if (p >= end) { ok = false; return r; }
        p++;
        return r;
    }
    JVal value(int depth) {
        if (depth > 32) return fail();
        ws();
        if (p >= end) return fail();
        JVal v;
        if (*p == '{') {
            v.kind = JVal::OBJ;
            p++; ws();
            if (p < end && *p == '}') { p++; return v; }
            for (;;) {
                ws();
                std::string k = str();
                if (!ok) return v;
                ws();
                if (p >= end || *p != ':') return fail();
                p++;
                if (v.o.count(k)) return fail();          // duplicate key
                v.o[k] = value(depth + 1);
                if (!ok) return v;
                ws();
                if (p < end && *p == ',') { p++; continue; }
                if (p < end && *p == '}') { p++; return v; }
                return fail();
            }
        }
        if (*p == '[') {
            v.kind = JVal::ARR;
            p++; ws();
            if (p < end && *p == ']') { p++; return v; }
            for (;;) {
                v.a.push_back(value(depth + 1));
                if (!ok) return v;
                ws();
                if (p < end && *p == ',') { p++; continue; }
                if (p < end && *p == ']') { p++; return v; }
                return fail();
            }
        }
        if (*p == '"') { v.kind = JVal::STR; v.s = str(); return v; }
        if (lit("true")) { v.kind = JVal::BOOL; v.b = true; return v; }
        if (lit("false")) { v.kind = JVal::BOOL; return v; }
        if (lit("null")) return v;
        if (*p == '-' || (*p >= '0' && *p <= '9')) {
            char *e = nullptr;
            v.kind = JVal::NUM;
            v.n = std::strtod(p, &e);
            if (e == p) return fail();
            p = e;
            return v;
        }
        return fail();
    }
};
static bool json_parse(const std::string &text, JVal *out) {
    JParser jp{text.data(), text.data() + text.size()};
    *out = jp.value(0);
    jp.ws();
    return jp.ok && jp.p == jp.end && out->kind == JVal::OBJ;
}

// ---- the runs -------------------------------------------------------------------------------------------
static std::string g_game;

static ModeParams params(uint32_t seed, uint32_t bots) {
    ModeParams p;
    p.seed = seed;
    p.bots = bots;
    p.humans = 1;
    p.flags = MODE_PARAM_DEBUG;
    return p;
}
static bool start(const ModeParams &p) {
    g_cfg->flags = 0;
    g_cfg->paused = 0;
    if (!mode_start_run(g_game.c_str(), GAME_MODE_CONQUEST, p)) return false;
    return sim_load_level(MODE_LEVEL_INDEX);
}
static std::vector<uint32_t> run_ticks(int n) {
    std::vector<uint32_t> v;
    v.reserve((size_t)n);
    for (int i = 0; i < n; i++) { game_tick_sim(); v.push_back(net_state_checksum()); }
    return v;
}
static int first_diff(const std::vector<uint32_t> &a, const std::vector<uint32_t> &b) {
    for (size_t i = 0; i < a.size() && i < b.size(); i++) if (a[i] != b[i]) return (int)i;
    return a.size() == b.size() ? -1 : (int)(std::min)(a.size(), b.size());
}

struct AiSeen {
    bool castle[8] = {};
    bool mode_changed[8] = {};
    bool moved[8] = {};
    uint8_t first_mode[8] = {};
    uint16_t x0[8] = {}, y0[8] = {};
};

int main(int argc, char **argv) {
    mc_install_crash_handler();
    g_game = argc > 1 ? argv[1] : MC_DEFAULT_GAME_DIR;
    if (!sim_init(g_game.c_str())) { std::printf("SKIP: no game data in %s\n", g_game.c_str()); return 0; }
    sim_register_gameplay();
    mode_level_register();
    const fs::path tmp = fs::temp_directory_path() / "mc_rts_headless_test";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp);
    savegame_set_dirs(tmp.string().c_str(), nullptr);

    constexpr int kTicks = 2000;
    // ---- 1 + 2: the first run ----
    CHECK(start(params(1, 3)));
    CHECK(mode_active() && mode_run_active());
    CHECK(gameplay_rules().mode == (int)GAME_MODE_CONQUEST);
    CHECK(g_cfg->level == MODE_LEVEL_INDEX);
    CHECK(g_mode.template_level >= 50 && g_mode.template_level <= 69);
    CHECK(g_state->player_count == 4);
    for (int p = 0; p < 4; p++) CHECK(g_state->players[p].is_computer == (p == 0 ? 0 : 1));
    CHECK(g_ai_rand_seed == g_mode.ai_seed);
    const uint32_t start_checksum = net_state_checksum();
    AiSeen seen;
    std::vector<uint32_t> a;
    std::string dump_text;
    uint32_t dump_checksum_before = 0, dump_checksum_after = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 1; i <= kTicks; i++) {
        game_tick_sim();
        a.push_back(net_state_checksum());
        for (int p = 1; p < 4; p++) {
            const PlayerRec &r = g_state->players[p];
            const Thing *t = thing_at(thing_wrap(r.thing));
            if (i == 2) { seen.first_mode[p] = r.blk.ai_mode; seen.x0[p] = t->x; seen.y0[p] = t->y; }
            if (i > 2) {
                if (r.blk.ai_mode != seen.first_mode[p]) seen.mode_changed[p] = true;
                if (t->x != seen.x0[p] || t->y != seen.y0[p]) seen.moved[p] = true;
            }
            if (r.blk.castle != 0) seen.castle[p] = true;
        }
        if (i == 1000) {
            dump_checksum_before = net_state_checksum();
            mode_dump_json(&dump_text, i);
            dump_checksum_after = net_state_checksum();
        }
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    CHECK(g_mode.tick == (uint32_t)kTicks);
    int castles = 0, changed = 0, moved = 0;
    for (int p = 1; p < 4; p++) { castles += seen.castle[p]; changed += seen.mode_changed[p]; moved += seen.moved[p]; }
    std::printf("run 1: template level %u, %d ticks in %.2f s (%.0f ticks/s, simulation only); computer wizards: "
                "%d built a castle, %d changed AI mode, %d moved\n", (unsigned)g_mode.template_level, kTicks, secs,
                kTicks / (secs > 0 ? secs : 1), castles, changed, moved);
    // round 11 session 3 (B6): creatures wake near any wizard in a mode run, so a computer wizard can now be killed by
    // the monsters near its start before it casts its castle (seed 1: one of three; without a castle it does not
    // respawn - the original's rule until round 13's respawn)
    CHECK(castles >= 2 && changed >= 2 && moved == 3);
    const uint32_t template_1 = g_mode.template_level;
    mode_stop_run();
    CHECK(!mode_active());
    CHECK(gameplay_rules().mode == 0);

    // ---- 3: the dump ----
    {
        JVal d;
        const bool parsed = json_parse(dump_text, &d);
        CHECK(parsed);
        CHECK(dump_checksum_before == dump_checksum_after);
        if (parsed) {
            auto num = [&](const JVal *v) { return v && v->kind == JVal::NUM ? v->n : -1e9; };
            auto strv = [&](const JVal *v) { return v && v->kind == JVal::STR ? v->s : std::string("?"); };
            CHECK(strv(d.get("format")) == "mcport-dump");
            CHECK(num(d.get("version")) == 1);
            CHECK(num(d.get("tick")) == 1000 && num(d.get("ticks_run")) == 1000);
            CHECK(strv(d.get("mode")) == "conquest" && num(d.get("mode_id")) == 1 && num(d.get("seed")) == 1);
            char hx[16];
            std::snprintf(hx, sizeof hx, "0x%08x", (unsigned)a[999]);
            CHECK(strv(d.get("checksum")) == hx);
            CHECK(d.get("checksum_parts") != nullptr);
            const JVal *mb = d.get("mode_block");
            CHECK(mb && num(mb->get("tick")) == 1000 && num(mb->get("template_level")) == template_1);
            CHECK(mb && mb->get("params") && num(mb->get("params")->get("bots")) == 3);
            CHECK(mb && mb->get("player_flags") && mb->get("player_flags")->a.size() == 8);
            const JVal *pl = d.get("players");
            CHECK(pl && pl->kind == JVal::ARR && pl->a.size() == 4);
            if (pl && pl->a.size() == 4) {
                for (int p = 0; p < 4; p++) {
                    const JVal &q = pl->a[(size_t)p];
                    CHECK(num(q.get("index")) == p);
                    CHECK(num(q.get("is_computer")) == (p == 0 ? 0 : 1));
                    CHECK(q.get("local") && q.get("local")->b == (p == 0));
                    CHECK(q.get("name") && !q.get("name")->s.empty());
                    CHECK(num(q.get("max_health")) > 0);
                    for (const char *k : {"health", "mana", "x", "y", "z", "ai_mode", "kills", "thing", "castle", "alive"})
                        CHECK(q.get(k) != nullptr);
                }
                int with_castle = 0;
                for (int p = 1; p < 4; p++) {
                    const JVal *c = pl->a[(size_t)p].get("castle");
                    if (c && c->kind == JVal::OBJ && num(c->get("level")) >= 1 && num(c->get("max_health")) > 0) with_castle++;
                }
                CHECK(with_castle >= 1);
            }
            const JVal *cs = d.get("census");
            CHECK(cs && num(cs->get("total")) > 100);
            const JVal *pt = cs ? cs->get("player_things") : nullptr;
            CHECK(pt && num(pt->get("ai_wizard")) == 3);
            const JVal *pool = d.get("pool");
            CHECK(pool && num(pool->get("live")) == num(cs ? cs->get("total") : nullptr));
            std::printf("dump at tick 1000: %zu bytes, parses, 4 players, %d Things\n", dump_text.size(),
                        (int)num(cs ? cs->get("total") : nullptr));
        }
        std::FILE *f = std::fopen((tmp / "dump_1000.json").string().c_str(), "wb");
        if (f) { std::fwrite(dump_text.data(), 1, dump_text.size(), f); std::fclose(f); }
    }

    // ---- between the runs: a campaign level (moves the AI seed, the cached human, campaign spells) ----
    g_cfg->flags = 0;
    CHECK(sim_load_level(2));
    CHECK(!mode_active() && g_cfg->level == 2);
    for (int i = 0; i < 300; i++) game_tick_sim();
    g_state->players[0].blk.spell_found[0] = 1;
    g_ai_rand_seed ^= 0x12345678u;

    // ---- 1: the second run, identical ----
    CHECK(start(params(1, 3)));
    CHECK(net_state_checksum() == start_checksum);
    const std::vector<uint32_t> b = run_ticks(kTicks);
    const int d = first_diff(a, b);
    std::printf("run 2 (same process, after a campaign level): %s", d < 0 ? "identical checksums every tick\n" : "DIVERGED");
    if (d >= 0) std::printf(" at tick %d\n", d + 1);
    CHECK(d < 0);

    // ---- 4: savestate round trip inside the mode ----
    {
        CHECK(start(params(1, 3)));
        run_ticks(600);
        const std::string path = (tmp / "state_rts.mcs").string();
        CHECK(savestate_save_file(path.c_str(), nullptr));
        const uint32_t mode_sum = mode_checksum(0);
        const std::vector<uint32_t> x = run_ticks(400);
        CHECK(savestate_load_file(path.c_str()));
        CHECK(mode_active() && g_mode.tick == 600 && mode_checksum(0) == mode_sum);
        const std::vector<uint32_t> y = run_ticks(400);
        const int e = first_diff(x, y);
        CHECK(e < 0);
        // the run of tick 601.. equals the first run's ticks 601..1000 as well
        CHECK(std::equal(x.begin(), x.end(), a.begin() + 600));
        std::printf("savestate at tick 600, 400 ticks, load, 400 ticks: %s\n", e < 0 ? "identical (mode block included)" : "DIVERGED");
        mode_stop_run();
        // a fresh start from the state file alone (mcport `load` of an rts state): header level, MODE chunk
        SaveStateHeader h{};
        CHECK(savestate_read_header(path.c_str(), &h) && h.level == MODE_LEVEL_INDEX);
        g_cfg->flags = 0;
        CHECK(sim_load_level(3));                                   // something else was running
        CHECK(mode_start_run_for_state(g_game.c_str(), path.c_str()));
        CHECK(mode_active() && g_mode.params.seed == 1 && g_mode.params.bots == 3);
        CHECK(sim_load_level(h.level));                             // game_level_begin(header.level)
        CHECK(savestate_load_file(path.c_str()));
        const std::vector<uint32_t> z = run_ticks(400);
        CHECK(first_diff(x, z) < 0);
        std::printf("state loaded through mode_start_run_for_state: %s\n", first_diff(x, z) < 0 ? "identical" : "DIVERGED");
        mode_stop_run();
        // a campaign state loaded while a mode runs ends the mode
        CHECK(sim_load_level(3));
        run_ticks(10);
        const std::string camp = (tmp / "state_campaign.mcs").string();
        CHECK(savestate_save_file(camp.c_str(), nullptr));
        CHECK(start(params(1, 3)));
        CHECK(!mode_start_run_for_state(g_game.c_str(), camp.c_str()));   // no MODE chunk: nothing to start
        CHECK(sim_load_level(3));
        CHECK(savestate_load_file(camp.c_str()));
        CHECK(!mode_active() && gameplay_rules().mode == 0);
        mode_stop_run();
    }

    // ---- 5: restart, another seed, the original untouched ----
    {
        CHECK(start(params(1, 3)));
        run_ticks(50);
        CHECK(sim_load_level(MODE_LEVEL_INDEX));                    // level_finish: the same level again
        CHECK(net_state_checksum() == start_checksum && g_mode.tick == 0);
        // the AI's CRT seed (outside GameState) is part of a mode run's checksum (mode_checksum)
        g_ai_rand_seed ^= 1;
        CHECK(net_state_checksum() != start_checksum);
        g_ai_rand_seed ^= 1;
        mode_stop_run();
        CHECK(start(params(7, 3)));
        const std::vector<uint32_t> c = run_ticks(200);
        CHECK(first_diff(a, c) >= 0);                              // another run
        std::printf("seed 7: template level %u\n", (unsigned)g_mode.template_level);
        mode_stop_run();
        CHECK(start(params(7, 2)));
        CHECK(g_state->player_count == 3);
        mode_stop_run();
        // explicit map and re-seeded terrain
        ModeParams pm = params(5, 1);
        pm.map = 57;
        pm.flags |= MODE_PARAM_RESEED;
        CHECK(start(pm));
        CHECK(g_mode.template_level == 57 && g_state->level.gen.seed == 5 && g_state->player_count == 2);
        mode_stop_run();
        // without a mode, MODE_LEVEL_INDEX is no level and the campaign levels load as always
        g_cfg->flags = 0;
        CHECK(!sim_load_level(MODE_LEVEL_INDEX));
        CHECK(sim_load_level(2) && !mode_active() && g_mode.version == 0);
    }

    fs::remove_all(tmp, ec);
    if (g_fail) { std::printf("rts_headless_test: %d FAILED\n", g_fail); return 1; }
    std::printf("rts_headless_test: all passed\n");
    return 0;
}
