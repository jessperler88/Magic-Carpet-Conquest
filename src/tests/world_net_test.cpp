// world_net_test (Phase 4 round 11 session 3): a Conquest mode run in lockstep over TCP on a generated 1024 world.
//
// The test starts itself again with --peer (the pattern of net_test parts 2 / 3). Every process joins the session
// "WORLDNET", calls mode_start_net_run (mode_level.h) with ITS OWN ModeParams - only the host's (player 0: seed 7,
// 1024 cells, 3 bots) are the ones played: the others propose seed 99 / 512 cells / 1 bot and must adopt the host's
// through NetGameRules (net.h) - then loads MODE_LEVEL_INDEX with Config.flags 0x10 and runs the ticks with scripted
// input for its own player. Every tick each process records net_state_checksum(); the desync side channel compares
// them online, the parent compares the files afterwards.
// Checks: identical checksums every tick on every peer; the agreed params (world 1024, 2 humans + 3 bots = 5 records:
// 0..1 human, 2..4 computer); the world is generated (wide, pool >= 16384); the computer wizards fly (their positions
// change); creatures within 20 cells of a computer wizard and far from both humans are awake (B6: near ANY wizard),
// creatures 40+ cells from every wizard sleep (the original's network game skips the wake tick: all awake). Exit 0 = pass, SKIP without game data. argv[1] = game dir, MC_WORLD_NET_TICKS = ticks
// (default 2000).
#define _CRT_SECURE_NO_WARNINGS
#include "net.h"
#include "net_tcp.h"
#include "sim.h"
#include "mode.h"
#include "mode_level.h"
#include "player.h"
#include "input.h"
#include "thing.h"
#include "world.h"
#include "settings.h"
#include "mc_globals.h"
#include "crash_handler.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

static std::atomic<int> g_fail{0};
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)
#define CHECK_EQ(a, b) do { long long _a = (long long)(a), _b = (long long)(b); if (_a != _b) { \
    std::printf("FAIL %s:%d: %s == %s (%lld != %lld)\n", __FILE__, __LINE__, #a, #b, _a, _b); g_fail++; } } while (0)

static std::string g_exe, g_game, g_outdir;

static void script_input(int tick, int me) {
    const int ph = tick + me * 37;
    const int dx = (((ph / 50) & 1) ? 1 : -1) * ((ph * 7) % 120);
    const int dy = ((ph / 80) % 3 - 1) * 40;
    input_mouse_move(320 + dx, 200 + dy);
    if (ph % 45 == 0) input_mouse_button(0, true);
    if (ph % 45 == 3) input_mouse_button(0, false);
    if (ph % 70 == 10) input_mouse_button(1, true);
    if (ph % 70 == 12) input_mouse_button(1, false);
    if (ph % 300 == 0) input_key_event(MC_SC_UP | MC_SC_EXT, true);
    if (ph % 300 == 150) input_key_event(MC_SC_UP | MC_SC_EXT, false);
}

// Within `d2` (squared xy units) of a wizard of records first..players-1. The wake test is 0x2400000 (24 cells).
static bool near_any(const Thing *t, int players, int first, int64_t d2 = 0x2400000) {
    for (int p = first; p < players; p++) {
        const PlayerRec &r = g_state->players[p];
        if (r.thing == 0) continue;
        const Thing *w = thing_at(r.thing);
        if (w->cls != 3) continue;
        if (world_dist_sq(world_signed(w->x - t->x), world_signed(w->y - t->y)) < d2) return true;
    }
    return false;
}
// A live creature within 20 cells of a computer wizard and more than 24 cells from every human.
static bool near_bot_only(const Thing *c, int humans, int records) {
    return c->health >= 0 && !near_any(c, humans, 0) && near_any(c, records, humans, 0x1900000);
}

// One peer: join, mode_start_net_run with this peer's params, the level, `ticks` ticks; "tick crc" lines to `out`.
static int run_player(TcpTransport *tr, int players, int ticks, const char *out) {
    net_set_transport(tr);
    if (net_init() != 1) { std::printf("  peer: no network (%s)\n", tr->last_error()); return 2; }
    const int me = net_session_join("WORLDNET", players);
    if (me < 0) { std::printf("  peer: join failed\n"); return 2; }
    g_state->player_count = (int16_t)players;
    g_state->local_player = (int16_t)me;
    std::snprintf(g_cfg->save_str_a, sizeof g_cfg->save_str_a, "Player %d", me);
    std::snprintf(g_cfg->save_str_b, sizeof g_cfg->save_str_b, "CALL%d", me);
    ModeParams mp;
    if (me == 0) { mp.seed = 7; mp.world_size = 1024; mp.bots = 3; }
    else         { mp.seed = 99; mp.world_size = 512; mp.bots = 1; }
    mp.flags = MODE_PARAM_DEBUG;
    bool differs = false;
    if (!mode_start_net_run(g_game.c_str(), GAME_MODE_CONQUEST, mp, &differs)) { std::printf("  peer %d: no mode run\n", me); return 2; }
    const ModeParams &a = g_mode.params;
    if (a.seed != 7 || a.world_size != 1024 || a.bots != 3 || a.humans != (uint32_t)players || !differs) {
        std::printf("  peer %d: params seed %u size %u bots %u humans %u (differs %d), expected the host's 7 / 1024 / 3 / %d\n",
                    me, a.seed, a.world_size, a.bots, a.humans, (int)differs, players);
        return 2;
    }
    input_reset();
    if (!sim_load_level(MODE_LEVEL_INDEX)) { std::printf("  peer %d: the mode level was not loaded\n", me); return 2; }
    input_mouse_center();
    net_sync_arm();
    const int records = (int)(uint16_t)g_state->player_count;
    int bad_records = 0;
    for (int p = 0; p < records; p++) if ((g_state->players[p].is_computer == 1) != (p >= players)) bad_records++;
    FILE *f = std::fopen(out, "w");
    if (!f) return 2;
    int32_t bot_start[8][2] = {};
    long long bot_moved = 0;
    int far_awake = 0, far_creatures = 0, asleep = 0;
    std::vector<uint32_t> near_bot;                         // creatures near a computer wizard only, 3 ticks before the end
    std::vector<bool> near_still, awake_prev;               // ... still so 2 ticks before the end / awake then
    const auto t0 = std::chrono::steady_clock::now();
    int t = 0;
    for (; t < ticks; t++) {
        g_timer_ticks = (uint32_t)(t * 5 + me * 1000);     // real time differs per machine: kept out of the checksum
        script_input(t, me);
        game_tick_sim();
        std::fprintf(f, "%d %08x\n", t, net_state_checksum());
        if (const char *d = std::getenv("MC_WORLD_NET_DUMP")) {        // diagnosis: the player records after tick d
            if (std::atoi(d) == t) {
                FILE *g = std::fopen((std::string(out) + ".rec").c_str(), "wb");
                if (g) { std::fwrite(g_state->players, 1, sizeof g_state->players, g); std::fclose(g); }
            }
        }
        if (t == 20)
            for (int p = players; p < records && p < 8; p++) {
                const Thing *w = thing_at(g_state->players[p].thing);
                bot_start[p][0] = w->x; bot_start[p][1] = w->y;
            }
        if (t == ticks - 1) {
            for (int p = players; p < records && p < 8; p++) {
                const Thing *w = thing_at(g_state->players[p].thing);
                const int64_t d = world_dist_sq(world_signed(w->x - bot_start[p][0]), world_signed(w->y - bot_start[p][1]));
                if (d > 0x10000) bot_moved++;                   // more than one cell from where it was at tick 20
            }
            // near a computer wizard only at both of the last two ticks: the wake test ran in between (timer_a
            // reaches 0 at most one tick before its retest), so they are awake; under the original's local-player
            // rule they would sleep. Creatures 40+ cells from every wizard sleep (the original's network game
            // skips the wake tick: everything stays awake there).
            for (size_t k = 0; k < near_bot.size(); k++) {
                const Thing *c = thing_at(near_bot[k]);
                if (c->cls != 5 || !near_bot_only(c, players, records) || !near_still[k]) continue;
                far_creatures++;
                if (c->timer_a != 0 || awake_prev[k]) far_awake++;
            }
            for (int type = 0; type < 0x14; type++)
                for (uint32_t i = g_cfg->creature_lists[type]; i != 0; i = thing_at(i)->next) {
                    const Thing *c = thing_at(i);
                    if (c->health >= 0 && !near_any(c, records, 0, 0x6400000) && c->timer_a == 0) asleep++;
                }
        }
        if (t == ticks - 3)
            for (int type = 0; type < 0x14; type++)
                for (uint32_t i = g_cfg->creature_lists[type]; i != 0; i = thing_at(i)->next)
                    if (near_bot_only(thing_at(i), players, records)) near_bot.push_back(i);
        if (t == ticks - 2)
            for (uint32_t i : near_bot) {
                const Thing *c = thing_at(i);
                near_still.push_back(c->cls == 5 && near_bot_only(c, players, records));
                awake_prev.push_back(c->timer_a != 0);
            }
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const NetSyncStats s = net_sync_stats();
    std::fprintf(f, "end %d sync %u %u %d\n", t, s.checks, s.mismatches, s.first_mismatch);
    std::fprintf(f, "info %d %d %lld %d %d %d %d %d\n", records, bad_records, bot_moved, far_creatures, far_awake,
                 g_world.size, thing_pool_slots(), asleep);
    std::fclose(f);
    char line[512];
    std::snprintf(line, sizeof line, "  player %d: %d ticks in %.1f s, %u checksums compared online, %u mismatches; world %d cells, "
                  "%d records, pool %d; computer wizards moved %lld / %d; creatures near a computer wizard only: %d, awake %d; "
                  "asleep 40+ cells from every wizard: %d\n",
                  me, t, secs, s.checks, s.mismatches, g_world.size, records, thing_pool_slots(), bot_moved, records - players,
                  far_creatures, far_awake, asleep);
    std::fwrite(line, 1, std::strlen(line), stdout);
    std::fflush(stdout);
    mode_stop_run();
    return 0;
}

#ifdef _WIN32
static HANDLE spawn_peer(int players, int port, int ticks, const std::string &out) {
    char cmd[4096];
    std::snprintf(cmd, sizeof cmd, "\"%s\" \"%s\" --peer %d %d %d \"%s\"", g_exe.c_str(), g_game.c_str(), players, port,
                  ticks, out.c_str());
    STARTUPINFOA si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessA(nullptr, cmd, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) return nullptr;
    CloseHandle(pi.hThread);
    return pi.hProcess;
}
#endif

static int free_port() {
    static int next = 32700 + (int)(std::chrono::steady_clock::now().time_since_epoch().count() % 2000);
    return next++;
}

static bool read_file(const std::string &path, std::vector<std::string> &lines, std::string &end, std::string &info) {
    FILE *f = std::fopen(path.c_str(), "r");
    if (!f) return false;
    char buf[256];
    while (std::fgets(buf, sizeof buf, f)) {
        std::string s(buf);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        if (s.rfind("end", 0) == 0) end = s;
        else if (s.rfind("info", 0) == 0) info = s;
        else lines.push_back(s);
    }
    std::fclose(f);
    return true;
}

static void run_session(int players, int ticks) {
    std::printf("== %d processes over TCP, rts 1024 / 3 bots, %d ticks\n", players, ticks);
#ifndef _WIN32
    std::printf("  SKIP: process spawning is implemented for Windows only\n");
#else
    NetContext *ctx = net_context_create();
    NetContext *prev = net_context_select(ctx);
    int port = 0;
    TcpTransport *tr = nullptr;
    for (int tries = 0; tries < 20; tries++) {
        port = free_port();
        tr = new TcpTransport("", (uint16_t)port);
        if (tr->present() && tr->is_name_server()) break;
        delete tr;
        tr = nullptr;
    }
    if (!tr) { std::printf("FAIL: no free TCP port for the name server\n"); g_fail++; return; }
    std::vector<std::string> files;
    std::vector<HANDLE> kids;
    for (int i = 0; i < players; i++) files.push_back(g_outdir + "/world_net_peer" + std::to_string(i) + ".txt");
    for (int i = 1; i < players; i++) {
        HANDLE h = spawn_peer(players, port, ticks, files[(size_t)i]);
        CHECK(h != nullptr);
        if (h) kids.push_back(h);
    }
    const int r = run_player(tr, players, ticks, files[0].c_str());
    CHECK_EQ(r, 0);
    for (HANDLE h : kids) {
        const DWORD w = WaitForSingleObject(h, 400000);
        DWORD code = 99;
        if (w == WAIT_OBJECT_0) GetExitCodeProcess(h, &code);
        else TerminateProcess(h, 98);
        CHECK_EQ(code, 0);
        CloseHandle(h);
    }
    net_shutdown();
    delete tr;
    net_context_select(prev);
    net_context_destroy(ctx);
    std::vector<std::vector<std::string>> crc((size_t)players);
    std::vector<std::string> ends((size_t)players), infos((size_t)players);
    for (int i = 0; i < players; i++) CHECK(read_file(files[(size_t)i], crc[(size_t)i], ends[(size_t)i], infos[(size_t)i]));
    int compared = 0, differ = 0, first = -1;
    for (int t = 0; t < ticks; t++)
        for (int i = 1; i < players; i++) {
            if ((size_t)t >= crc[0].size() || (size_t)t >= crc[(size_t)i].size()) continue;
            compared++;
            if (crc[0][(size_t)t] != crc[(size_t)i][(size_t)t]) { differ++; if (first < 0) first = t; }
        }
    std::printf("  %d per-tick checksum pairs compared, %d differ (first at tick %d)\n", compared, differ, first);
    CHECK_EQ(differ, 0);
    CHECK_EQ(compared, ticks * (players - 1));
    {
        std::vector<std::string> v = crc[0];
        std::sort(v.begin(), v.end());
        const size_t distinct = (size_t)(std::unique(v.begin(), v.end()) - v.begin());
        CHECK(distinct + 10 >= crc[0].size());
    }
    for (int i = 0; i < players; i++) {
        unsigned c = 0, m = 0; int fm = 0, n = 0;
        if (std::sscanf(ends[(size_t)i].c_str(), "end %d sync %u %u %d", &n, &c, &m, &fm) == 4) { CHECK_EQ(m, 0); CHECK(c > 0); }
        else CHECK(false);
        int records = 0, bad = 0, far_n = 0, awake = 0, size = 0, pool = 0, asleep = 0;
        long long moved = 0;
        if (std::sscanf(infos[(size_t)i].c_str(), "info %d %d %lld %d %d %d %d %d", &records, &bad, &moved, &far_n, &awake, &size,
                        &pool, &asleep) == 8) {
            CHECK_EQ(records, players + 3);
            CHECK_EQ(bad, 0);
            CHECK(moved >= 2);                  // the computer wizards fly about
            CHECK_EQ(size, 1024);
            CHECK(pool >= 16384);
            CHECK_EQ(awake, far_n);             // every creature near a computer wizard only is awake
            CHECK(asleep > 100);                // far from every wizard they sleep (not the original's net "all awake")
        } else CHECK(false);
    }
#endif
}

int main(int argc, char **argv) {
    mc_install_crash_handler();
    g_exe = argv[0];
    g_game = argc > 1 ? argv[1] : MC_DEFAULT_GAME_DIR;
#ifdef _WIN32
    { char p[MAX_PATH]; if (GetModuleFileNameA(nullptr, p, MAX_PATH)) g_exe = p; }
#endif
    g_outdir = g_exe.substr(0, g_exe.find_last_of("/\\"));
    if (g_outdir == g_exe) g_outdir = ".";
    setvbuf(stdout, nullptr, _IONBF, 0);
    const int ticks = std::getenv("MC_WORLD_NET_TICKS") ? std::atoi(std::getenv("MC_WORLD_NET_TICKS")) : 2000;

    if (argc > 2 && std::strcmp(argv[2], "--peer") == 0) {             // a child
        if (argc < 7) return 2;
        std::thread([] { std::this_thread::sleep_for(std::chrono::seconds(390)); std::printf("peer: watchdog\n"); std::_Exit(4); }).detach();
        if (!sim_init(g_game.c_str())) return 2;
        sim_register_gameplay();
        g_video_mode_flags = 1;
        TcpTransport tr("127.0.0.1", (uint16_t)std::atoi(argv[4]));
        const int r = run_player(&tr, std::atoi(argv[3]), std::atoi(argv[5]), argv[6]);
        net_shutdown();
        return r;
    }

    if (!sim_init(g_game.c_str())) { std::printf("SKIP: game data not found in %s\n", g_game.c_str()); return 0; }
    sim_register_gameplay();
    g_video_mode_flags = 1;
    std::thread([] { std::this_thread::sleep_for(std::chrono::seconds(420)); std::printf("world_net_test: watchdog\n"); std::_Exit(4); }).detach();

    run_session(2, ticks);

    if (g_fail) { std::printf("world_net_test: %d failure(s)\n", g_fail.load()); return 1; }
    std::printf("world_net_test: OK\n");
    return 0;
}
