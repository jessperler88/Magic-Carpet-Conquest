// `mcport <dir> rts ...` and the JSON dumps (rts_run.h). Round 10 task A.
#define _CRT_SECURE_NO_WARNINGS
#include "rts_run.h"
#include "mode_level.h"
#include "mode_dump.h"
#include "worldgen.h"
#include <algorithm>
#include "game.h"
#include "net.h"
#include "mc_globals.h"
#include "mclog.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

namespace {
long        s_dump_every = 0;
std::string s_dump_dir;
bool        s_dump_final = false;
long        s_quit_after = 0;
int         s_net_players = 0;      // --net N: a network run of N humans (0 = offline)
bool        s_result = false;       // --result: the result screen when the level ends (round 12) instead of quitting
long        s_last_tick = -1;      // the last ticks_run rts_after_tick saw (the final dump's number)
long        s_last_dump = -1;       // ticks_run of the last dump written (no duplicate final dump)
std::string (*s_extra)() = nullptr;

bool write_dump(const std::string &path, long ticks_run) {
    const std::string extra = s_extra ? s_extra() : std::string();
    return mode_dump_write(path.c_str(), ticks_run, extra.empty() ? nullptr : extra.c_str());
}

std::string dump_path(const char *name) {
    if (s_dump_dir.empty()) return name;
    std::error_code ec;
    std::filesystem::create_directories(s_dump_dir, ec);
    return (std::filesystem::path(s_dump_dir) / name).string();
}
} // namespace

void dump_configure(long every, const char *dir, bool final_dump) {
    s_dump_every = every > 0 ? every : 0;
    s_dump_dir = dir ? dir : "";
    s_dump_final = final_dump;
}

void dump_configure_from_env() {
    if (const char *e = std::getenv("MC_DUMP_EVERY")) s_dump_every = std::atol(e) > 0 ? std::atol(e) : 0;
    if (const char *e = std::getenv("MC_DUMP_DIR")) s_dump_dir = e;
    if (const char *e = std::getenv("MC_DUMP_FINAL")) s_dump_final = std::atoi(e) != 0;
}

bool dump_enabled() { return s_dump_every > 0 || s_dump_final; }
void dump_set_extra(std::string (*fn)()) { s_extra = fn; }

std::string dump_now(long ticks_run) {
    char name[64];
    std::snprintf(name, sizeof name, "dump_%ld.json", ticks_run);
    const std::string path = dump_path(name);
    if (!write_dump(path, ticks_run)) mclog(MCLOG_WARN, "dump: cannot write %s", path.c_str());
    std::string line;
    mode_dump_line(&line, ticks_run);
    mclog_str(MCLOG_INFO, line.c_str());
    std::fflush(stdout);
    s_last_dump = ticks_run;
    return path;
}

void dump_after_tick(long ticks_run) {
    if (s_dump_every > 0 && ticks_run > 0 && ticks_run % s_dump_every == 0) dump_now(ticks_run);
}

void dump_final(long ticks_run) {
    if (!s_dump_final || ticks_run < 0) return;
    if (s_last_dump == ticks_run) return;           // the periodic dump of this tick is already there
    const std::string path = dump_path("dump_final.json");
    if (!write_dump(path, ticks_run)) mclog(MCLOG_WARN, "dump: cannot write %s", path.c_str());
    std::string line;
    mode_dump_line(&line, ticks_run);
    mclog(MCLOG_INFO, "%s final", line.c_str());
    std::fflush(stdout);
    s_last_dump = ticks_run;
}

bool rts_parse_args(int argc, char **argv, int first, ModeParams *out, std::string *err) {
    ModeParams p;
    p.seed = 1;
    p.world_size = 256;
    p.bots = 3;
    p.humans = 1;
    p.flags = MODE_PARAM_DEBUG;
    dump_configure_from_env();
    s_quit_after = 0;
    s_net_players = 0;
    s_result = false;
    for (int i = first; i < argc; i++) {
        const char *a = argv[i];
        auto value = [&](uint32_t *dst) {
            if (i + 1 >= argc) { *err = std::string(a) + " needs a value"; return false; }
            char *end = nullptr;
            const unsigned long v = std::strtoul(argv[++i], &end, 0);
            if (!end || *end) { *err = std::string(a) + ": not a number: " + argv[i]; return false; }
            *dst = (uint32_t)v;
            return true;
        };
        uint32_t v = 0;
        if (!std::strcmp(a, "--seed")) { if (!value(&p.seed)) return false; }
        else if (!std::strcmp(a, "--size")) { if (!value(&p.world_size)) return false; }
        else if (!std::strcmp(a, "--bots")) { if (!value(&p.bots)) return false; }
        else if (!std::strcmp(a, "--map")) { if (!value(&p.map)) return false; }
        else if (!std::strcmp(a, "--reseed")) p.flags |= MODE_PARAM_RESEED;
        else if (!std::strcmp(a, "--tiled")) p.flags |= MODE_PARAM_TILED;
        else if (!std::strcmp(a, "--generate")) p.flags |= MODE_PARAM_GENERATE;
        else if (!std::strcmp(a, "--symmetry")) {
            if (i + 1 >= argc) { *err = "--symmetry needs auto|none|point|mirror|rot4|dihedral"; return false; }
            const int s = worldgen_symmetry_parse(argv[++i]);
            if (s < 0) { *err = std::string("--symmetry: unknown ") + argv[i]; return false; }
            p.world.symmetry = (uint8_t)s;
        }
        // generator controls (WorldParams, mode.h): 0 = the default; for the counts "none" = 0xff / 15
        else if (!std::strcmp(a, "--rough") || !std::strcmp(a, "--water") || !std::strcmp(a, "--continent") ||
                 !std::strcmp(a, "--springs") || !std::strcmp(a, "--wild") || !std::strcmp(a, "--lairs") ||
                 !std::strcmp(a, "--triggers") || !std::strcmp(a, "--portals")) {
            if (i + 1 < argc && !std::strcmp(argv[i + 1], "none")) {
                i++;
                if (!std::strcmp(a, "--springs")) p.world.springs = 0xff;
                else if (!std::strcmp(a, "--wild")) p.world.wild = 0xff;
                else if (!std::strcmp(a, "--lairs")) p.world.lairs = 0xff;
                else if (!std::strcmp(a, "--triggers")) p.world.features = (uint8_t)((p.world.features & 0xf0) | 15);
                else if (!std::strcmp(a, "--portals")) p.world.features = (uint8_t)((p.world.features & 0x0f) | 0xf0);
                else { *err = std::string(a) + " takes a number"; return false; }
                continue;
            }
            if (!value(&v)) return false;
            const bool nibble = !std::strcmp(a, "--triggers") || !std::strcmp(a, "--portals");
            if (v < 1 || v > (nibble ? 14u : 250u)) { *err = std::string(a) + (nibble ? " takes 1..14 or none" : " takes 1..250"); return false; }
            if (!std::strcmp(a, "--rough")) p.world.roughness = (uint8_t)v;
            else if (!std::strcmp(a, "--water")) p.world.water = (uint8_t)std::min(v, 90u);
            else if (!std::strcmp(a, "--continent")) p.world.continent = (uint8_t)v;
            else if (!std::strcmp(a, "--springs")) p.world.springs = (uint8_t)v;
            else if (!std::strcmp(a, "--wild")) p.world.wild = (uint8_t)v;
            else if (!std::strcmp(a, "--lairs")) p.world.lairs = (uint8_t)v;
            else if (!std::strcmp(a, "--triggers")) p.world.features = (uint8_t)((p.world.features & 0xf0) | v);
            else p.world.features = (uint8_t)((p.world.features & 0x0f) | (v << 4));
        }
        // round 12: the match rules (MatchRules, mode.h; parsed by mode_level.h mode_match_option)
        else if (a[0] == '-' && a[1] == '-' && mode_match_option_known(a + 2)) {
            if (i + 1 >= argc) { *err = std::string(a) + " needs a value"; return false; }
            if (!mode_match_option(a + 2, argv[++i], &p.match, err)) return false;
        }
        else if (!std::strcmp(a, "--result")) s_result = true;
        else if (!std::strcmp(a, "--debug")) p.flags |= MODE_PARAM_DEBUG;
        else if (!std::strcmp(a, "--no-debug")) p.flags &= ~MODE_PARAM_DEBUG;
        else if (!std::strcmp(a, "--ticks")) { if (!value(&v)) return false; s_quit_after = (long)v; }
        else if (!std::strcmp(a, "--net")) {
            if (!value(&v)) return false;
            if (v < 2 || v > 8) { *err = "--net takes the number of human players 2..8"; return false; }
            s_net_players = (int)v;
        }
        else if (!std::strcmp(a, "--dump-every")) { if (!value(&v)) return false; s_dump_every = (long)v; }
        else if (!std::strcmp(a, "--dump-final")) s_dump_final = true;
        else if (!std::strcmp(a, "--dump-dir")) {
            if (i + 1 >= argc) { *err = "--dump-dir needs a directory"; return false; }
            s_dump_dir = argv[++i];
        }
        else { *err = std::string("unknown rts option ") + a; return false; }
    }
    // round 11: 256 .. 4096, a power of two; > 256 = a generated world (worldgen.h; --tiled: the template repeated)
    if (p.world_size < 256 || p.world_size > 4096 || (p.world_size & (p.world_size - 1))) {
        *err = "--size takes a power of two 256..4096";
        return false;
    }
    if (p.map != 0 && (p.map < 50 || p.map > 69)) { *err = "--map takes a multiplayer level 50..69"; return false; }
    if (p.bots > 7) p.bots = 7;
    if (!mode_match_rules_finish(&p.match, err)) return false;
    *out = p;
    return true;
}

long rts_quit_after_ticks() { return s_quit_after; }
int  rts_net_players() { return s_net_players; }
bool rts_show_result() { return s_result; }

bool rts_begin(const char *game_dir, const ModeParams &params, RtsStart how) {
    if (how == RTS_START_LOBBY) {
        // round 12: the lobby joined the session and agreed on the host's rules (frontend.cpp mp_join_done ->
        // net_agree_rules / net_apply_rules with the urn's proposal): start the session's run from them
        NetGameRules agreed{};
        if (!net_rules_applied(&agreed) || agreed.mode == GAME_MODE_ORIGINAL) {
            mclog(MCLOG_ERROR, "rts: the lobby agreed on no mode");
            return false;
        }
        if (!mode_start_agreed_net_run(game_dir, agreed)) { mclog(MCLOG_ERROR, "rts: the session's mode run cannot start"); return false; }
        mclog(MCLOG_INFO, "rts: lobby session, player %d of %d (host %d)", net_local_player(), net_player_count(), net_host());
    } else if (how == RTS_START_CLI && s_net_players > 0) {
        // round 11 session 3: a network run without the lobby (round 12 brings the setup screen / lobby): join the
        // session "MCRTS" of s_net_players humans, every peer adopts the host's params (mode_start_net_run)
        if (!net_present()) { mclog(MCLOG_ERROR, "rts --net: no network (MC_NET=1 / MC_NET_HOST=...)"); return false; }
        mclog(MCLOG_INFO, "rts --net: waiting for %d players in session MCRTS", s_net_players);
        const int me = net_session_join("MCRTS", s_net_players);
        if (me < 0) { mclog(MCLOG_ERROR, "rts --net: the join failed"); return false; }
        g_state->player_count = (int16_t)s_net_players;
        g_state->local_player = (int16_t)me;
        bool differs = false;
        if (!mode_start_net_run(game_dir, GAME_MODE_CONQUEST, params, &differs)) {
            mclog(MCLOG_ERROR, "rts --net: no agreement on the mode run");
            return false;
        }
        mclog(MCLOG_INFO, "rts --net: player %d of %d (host %d)%s", me, s_net_players, net_host(),
              differs ? ", playing with the host's settings" : "");
    } else if (!mode_start_run(game_dir, GAME_MODE_CONQUEST, params)) return false;
    const ModeParams mp = g_mode.params;            // the host's in a network run
    if (!game_level_begin(MODE_LEVEL_INDEX)) { mode_stop_run(); return false; }
    s_last_tick = -1;
    s_last_dump = -1;
    mclog(MCLOG_INFO, "rts: mode %s seed %u size %u humans %u bots %u map %u%s%s", mode_name(g_mode.mode), (unsigned)mp.seed,
          (unsigned)mp.world_size, (unsigned)mp.humans, (unsigned)mp.bots, (unsigned)g_mode.template_level,
          (mp.flags & MODE_PARAM_RESEED) ? " (reseeded)" : "", (mp.flags & MODE_PARAM_DEBUG) ? " debug" : "");
    mclog(MCLOG_INFO, "rts: match rules: %s", mode_match_rules_describe(mp.match).c_str());
    if (mode_params_generated(mp)) {
        const WorldLayout &L = worldgen_last_layout();
        int count[16] = {};
        for (const WorldFeature &f : L.features) count[f.kind & 15]++;
        mclog(MCLOG_INFO, "rts: generated world %d cells, symmetry %s, %d seats, %d records%s: %d springs, %d lairs, "
              "%d trigger areas, %d portal pairs, %d towns, %d groves, %d krakens", L.size, worldgen_symmetry_name(L.symmetry), L.seats,
              L.records, L.records_dropped ? " (some features dropped: record budget)" : "", count[WFEAT_SPRING],
              count[WFEAT_LAIR], count[WFEAT_TRIGGER], count[WFEAT_PORTAL] / 2, count[WFEAT_TOWN], count[WFEAT_TREES],
              count[WFEAT_KRAKEN]);
        for (const WorldFeature &f : L.features)
            if (f.kind == WFEAT_SPAWN) mclog(MCLOG_INFO, "rts: seat %d at cell %d,%d", f.grade, f.x, f.y);
    }
    if (dump_enabled())
        mclog(MCLOG_INFO, "rts: dumps every %ld ticks%s into %s", s_dump_every, s_dump_final ? " + final" : "",
              s_dump_dir.empty() ? "the current directory" : s_dump_dir.c_str());
    return true;
}

void rts_after_tick(long ticks_run) {
    s_last_tick = ticks_run;
    dump_after_tick(ticks_run);
}

void rts_dump_final() {
    if (mode_run_active()) dump_final(s_last_tick);
}

void rts_end() {
    rts_dump_final();
    mode_stop_run();
}
