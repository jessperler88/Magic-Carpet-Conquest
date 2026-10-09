// Round 10 task A (docs/analysis/port_mode.md): movie format v3 - recordings of a game-mode run.
//
//  1. record an rts run (seed 1, 3 bots) for 500 ticks with a scripted local player (steering, speed, casting
//     the left / right hand spells, the book) and an order blob on some packets (g_hook_demo_order_out); the
//     file is mvx version 3 with the mode header and variable-length records;
//  2. replay it from a different state (another level loaded, the AI seed moved, no mode running): demo_open
//     starts the mode run with the recorded ModeParams and rules, the gax snapshot restores the mode block and
//     the AI globals; the per-tick checksums (net_state_checksum, mode block included) equal the recording's
//     and every blob arrives with its packet; demo_close stops the mode and releases the rules;
//  3. v1 / v2 still play: an extended-pool (v1) and a non-faithful-rules (v2) recording of a campaign level
//     replay identically and are not v3; the original's movie 0 opens as the original's format.
#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "sim.h"
#include "demo.h"
#include "mode.h"
#include "mode_level.h"
#include "thing.h"
#include "player.h"
#include "net.h"
#include "ai_wizard.h"
#include "settings.h"
#include "mc_globals.h"
#include "mcfile.h"
#include "crash_handler.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static std::string g_game;
static fs::path g_tmp;

static std::string slurp(const fs::path &p) {
    mc_blob b;
    if (!mc_read_file(p.string().c_str(), &b)) return {};
    std::string s(reinterpret_cast<const char *>(b.data), b.len);
    mc_blob_free(&b);
    return s;
}

// ---- the scripted local player (player_local_input replaced) --------------------------------------------
static int g_script_tick = 0;
static void script_input() {
    const int lp = g_state->local_player & 7;
    CmdPacket *pk = &g_state->commands[lp];
    if (pk->cmd != 0) return;                       // the join packet of the level start
    const int t = g_script_tick;
    std::memset(pk, 0, sizeof *pk);
    // weave around, speed up, slow down, cast both hands now and then, open / close the book once
    pk->steer_x = (int8_t)((t / 40) % 2 ? 30 : -25);
    pk->steer_y = (int8_t)((t / 60) % 3 - 1) * 10;
    if (t == 120) { pk->cmd = 0x14; pk->arg = 2; return; }      // book
    if (t == 140) { pk->cmd = 0x14; pk->arg = 0; return; }      // close
    int bits = 0;
    if (t % 100 < 40) bits |= 1;
    else if (t % 100 > 80) bits |= 2;
    if (t % 50 == 10) bits |= 0x10;
    if (t % 70 == 30) bits |= 0x20;
    if (bits) { pk->cmd = 6; pk->bits = (uint8_t)bits; }
}

// ---- order blobs ----------------------------------------------------------------------------------------
struct Blob { uint32_t tick; int player; std::vector<uint8_t> data; };
static std::vector<Blob> g_blobs_out, g_blobs_in;
static size_t order_out(int player, uint8_t *buf, size_t cap) {
    const uint32_t t = g_state->players[g_state->local_player & 7].tick;
    if (player != 0 || t % 7 != 3 || cap < 64) return 0;
    const size_t n = 1 + t % 37;                    // 1..37 bytes
    for (size_t i = 0; i < n; i++) buf[i] = (uint8_t)(t * 31 + i * 7);
    g_blobs_out.push_back({t, player, std::vector<uint8_t>(buf, buf + n)});
    return n;
}
static void order_in(int player, const uint8_t *data, size_t len) {
    if (len == 0) return;
    g_blobs_in.push_back({g_state->players[g_state->local_player & 7].tick, player, std::vector<uint8_t>(data, data + len)});
}

// v1 / v2 snapshots do not carry g_rng16 (the terrain generator's RNG, part of net_state_checksum): those
// recordings are compared on the GameState image, as config_test does.
static uint32_t state_hash() {
    const uint8_t *b = reinterpret_cast<const uint8_t *>(g_state);
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < sizeof(GameState); i++) { h ^= b[i]; h *= 16777619u; }
    return h;
}
static std::vector<uint32_t> play(int limit, uint32_t (*sum)() = net_state_checksum) {
    std::vector<uint32_t> v;
    while (demo_step() && (int)v.size() < limit) v.push_back(sum());
    return v;
}
// The playback starts at the recording's first packet (the tick after the record bit was set).
static int match_offset(const std::vector<uint32_t> &rec, const std::vector<uint32_t> &played, size_t min_n) {
    for (int off = 0; off < 3; off++) {
        if ((size_t)off >= rec.size()) break;
        std::vector<uint32_t> tail(rec.begin() + off, rec.end());
        const size_t n = (std::min)(tail.size(), played.size());
        if (n >= min_n && std::equal(tail.begin(), tail.begin() + (long)n, played.begin())) return off;
    }
    return -1;
}

static void test_v3() {
    constexpr int kTicks = 500;
    const fs::path rec = g_tmp / "rec";
    demo_set_record_dir(rec.string().c_str());
    g_settings.possession_range_pct = 120;                          // a non-faithful rule rides along
    ModeParams mp;
    mp.seed = 1;
    mp.bots = 3;
    mp.humans = 1;
    mp.flags = MODE_PARAM_DEBUG;
    g_cfg->flags = 0;
    g_cfg->paused = 0;
    CHECK(mode_start_run(g_game.c_str(), GAME_MODE_CONQUEST, mp));
    CHECK(sim_load_level(MODE_LEVEL_INDEX));
    void (*saved_input)() = g_hook_player_local_input;
    g_hook_player_local_input = script_input;
    g_hook_demo_order_out = order_out;
    g_script_tick = 1;
    game_tick_sim();                                                // tick 1: the players spawn
    g_cfg->movie = 11;
    g_cfg->flags |= 2;                                              // command 0xc: record from the next packet
    std::vector<uint32_t> recorded;
    for (int i = 0; i < kTicks; i++) {
        g_script_tick = 2 + i;
        game_tick_sim();
        recorded.push_back(net_state_checksum());
    }
    CHECK(demo_recording());
    CHECK(demo_extended() && demo_version() == DEMO_VERSION_MODE);
    const long written = demo_packets_written();
    demo_close();
    g_hook_demo_order_out = nullptr;
    g_hook_player_local_input = saved_input;
    const uint32_t ai_seed_end = g_ai_rand_seed;
    mode_stop_run();
    CHECK(!mode_active());
    g_settings.possession_range_pct = 100;

    // the file
    const std::string mvx = slurp(rec / "movie" / "mvx00011.dat");
    size_t blob_bytes = 0;
    for (const Blob &b : g_blobs_out) blob_bytes += b.data.size();
    const size_t hdr = sizeof(DemoExtHeader) + sizeof(DemoExtRules) + sizeof(DemoModeHeader);
    CHECK(mvx.size() == hdr + (size_t)written * (sizeof(CmdPacket) + 2) + blob_bytes);
    DemoExtHeader h{};
    DemoExtRules r{};
    DemoModeHeader m{};
    if (mvx.size() >= hdr) {
        std::memcpy(&h, mvx.data(), sizeof h);
        std::memcpy(&r, mvx.data() + sizeof h, sizeof r);
        std::memcpy(&m, mvx.data() + sizeof h + sizeof r, sizeof m);
    }
    CHECK(std::memcmp(h.magic, "MCPX", 4) == 0 && h.version == DEMO_VERSION_MODE && h.thing_slots == 1000);
    CHECK(r.possession_range_pct == 120 && r.mode == GAME_MODE_CONQUEST);
    CHECK(m.mode == GAME_MODE_CONQUEST && m.params.seed == 1 && m.params.bots == 3 && m.record_format == DEMO_RECORD_FORMAT);
    CHECK(fs::exists(rec / "movie" / "gax00011.dat") && fs::exists(rec / "movie" / "max00011.dat"));
    CHECK(written == 4L * kTicks && !g_blobs_out.empty());

    // replay from somewhere else
    g_cfg->flags = 0;
    CHECK(sim_load_level(5));
    for (int i = 0; i < 50; i++) game_tick_sim();
    g_ai_rand_seed = 0xdeadbeefu;
    void (*input)() = g_hook_player_local_input;
    g_hook_player_local_input = nullptr;                            // mcport plays movies without local input
    g_hook_demo_order_in = order_in;
    CHECK(demo_open(g_tmp.string().c_str(), 11));
    CHECK(mode_active() && g_mode.params.seed == 1 && demo_version() == DEMO_VERSION_MODE);
    CHECK(gameplay_rules().possession_range_pct == 120 && gameplay_rules().mode == (int)GAME_MODE_CONQUEST);
    const std::vector<uint32_t> played = play(kTicks + 10);
    CHECK(demo_packets_total() == written);
    CHECK(demo_packets_read() >= written - 4);
    const int off = match_offset(recorded, played, kTicks - 10);
    // every blob came back with its packet
    bool blobs_ok = g_blobs_in.size() == g_blobs_out.size();
    for (size_t i = 0; blobs_ok && i < g_blobs_in.size(); i++)
        blobs_ok = g_blobs_in[i].player == g_blobs_out[i].player && g_blobs_in[i].data == g_blobs_out[i].data &&
                   g_blobs_in[i].tick == g_blobs_out[i].tick;
    CHECK(blobs_ok);
    std::printf("v3 movie: %ld records (%zu order blobs, %zu bytes) of an rts run, %zu ticks played back from another "
                "level: %s, blobs %s\n", written, g_blobs_out.size(), blob_bytes, played.size(),
                off >= 0 ? "identical checksums every tick" : "DIVERGED", blobs_ok ? "identical" : "DIFFERENT");
    CHECK(off >= 0);
    CHECK(g_ai_rand_seed == ai_seed_end);                           // the AI globals came with the snapshot
    CHECK(!mode_active());                                          // the movie ended: demo_close stopped the mode
    CHECK(gameplay_rules().possession_range_pct == 100 && gameplay_rules().mode == 0);
    demo_close();
    g_hook_demo_order_in = nullptr;
    g_hook_player_local_input = input;

    // without the mode hooks a v3 movie is refused cleanly
    auto start_hook = g_hook_demo_mode_start;
    g_hook_demo_mode_start = nullptr;
    CHECK(!demo_open(g_tmp.string().c_str(), 11));
    CHECK(!mode_active() && !demo_playing());
    g_hook_demo_mode_start = start_hook;
    demo_set_record_dir("");
}

// v1 (extended pool) and v2 (rules) recordings of a campaign level: unchanged formats, identical replays.
static void test_v1_v2() {
    const fs::path rec = g_tmp / "rec";
    demo_set_record_dir(rec.string().c_str());
    struct Case { int movie; int slots; int pct; uint32_t version; };
    const Case cases[] = {{12, 3000, 100, 1}, {13, 1000, 150, 2}};
    for (const Case &c : cases) {
        g_settings.thing_slots = c.slots;
        g_settings.possession_range_pct = c.pct;
        g_cfg->flags = 0;
        g_cfg->paused = 0;
        CHECK(sim_load_level(2));
        game_tick_sim();
        g_cfg->movie = (uint16_t)c.movie;
        g_cfg->flags |= 2;
        std::vector<uint32_t> recorded;
        for (int i = 0; i < 300; i++) { game_tick_sim(); recorded.push_back(state_hash()); }
        CHECK(demo_recording() && demo_version() == (int)c.version);
        const long written = demo_packets_written();
        demo_close();
        char name[32];
        std::snprintf(name, sizeof name, "mvx%05d.dat", c.movie);
        const std::string mvx = slurp(rec / "movie" / name);
        CHECK(mvx.size() == sizeof(DemoExtHeader) + (c.version == 2 ? sizeof(DemoExtRules) : 0) + (size_t)written * sizeof(CmdPacket));
        g_settings.thing_slots = 1000;
        g_settings.possession_range_pct = 100;
        CHECK(sim_load_level(4));
        CHECK(demo_open(g_tmp.string().c_str(), c.movie));
        CHECK(demo_version() == (int)c.version && !mode_active());
        const std::vector<uint32_t> played = play(400, state_hash);
        const int off = match_offset(recorded, played, 280);
        std::printf("v%u movie (pool %d, possession %d %%): %zu ticks played back: %s\n", (unsigned)c.version, c.slots,
                    c.pct, played.size(), off >= 0 ? "identical" : "DIVERGED");
        CHECK(off >= 0);
        demo_close();
    }
    // the original's movie 0: the original's format, faithful rules, no mode
    g_settings.possession_range_pct = 130;
    CHECK(sim_load_level(38));
    if (demo_open(g_game.c_str(), 0)) {
        CHECK(demo_version() == 0 && !demo_extended() && !mode_active());
        CHECK(gameplay_rules().possession_range_pct == 100);
        const std::vector<uint32_t> played = play(100);
        CHECK(played.size() == 100);
        demo_close();
        std::printf("movie 0: original format, %zu ticks played\n", played.size());
    }
    g_settings = PortSettings{};
    demo_set_record_dir("");
}

// Round 12: ModeParams grew MatchRules (64 bytes). A round-11 MODE block (version 1) and a version-3 movie header (32-byte
// ModeParams) still read, with the match rules at their defaults.
static void test_round11_formats() {
    ModeParams p;
    p.seed = 77; p.world_size = 1024; p.bots = 5; p.world.water = 40;
    p.match.team[2] = 9;                                        // not in a v1 layout: must read back as 0
    mode_begin(GAME_MODE_CONQUEST, p);
    g_mode.tick = 1234; g_mode.ai_seed = 99; g_mode.players[3].flags = MODE_PLAYER_GOD;
    std::vector<uint8_t> v2(mode_serialised_size());
    mode_serialise(v2.data());
    // the v1 layout: version 1, the block with the first 32 bytes of ModeParams only
    const size_t head = 4 + offsetof(ModeState, params), cut = sizeof(ModeParams) - MODE_PARAMS_V1_SIZE;
    std::vector<uint8_t> v1(v2.begin(), v2.begin() + (long)(head + MODE_PARAMS_V1_SIZE));
    v1.insert(v1.end(), v2.begin() + (long)(head + sizeof(ModeParams)), v2.end());
    const uint32_t one = 1;
    std::memcpy(v1.data(), &one, 4);
    CHECK(v1.size() == v2.size() - cut && mode_serialised_size_ok(v1.size()) && mode_serialised_size_ok(v2.size()));
    mode_end();
    CHECK(mode_deserialise(v1.data(), v1.size()));
    CHECK(g_mode.version == MODE_STATE_VERSION && g_mode.mode == GAME_MODE_CONQUEST && g_mode.params.seed == 77 &&
          g_mode.params.world_size == 1024 && g_mode.params.bots == 5 && g_mode.params.world.water == 40 &&
          g_mode.params.match.team[2] == 0 && g_mode.tick == 1234 && g_mode.ai_seed == 99 &&
          g_mode.players[3].flags == MODE_PLAYER_GOD);
    mode_end();
    CHECK(mode_deserialise(v2.data(), v2.size()) && g_mode.params.match.team[2] == 9);
    mode_end();
    // a version 3 movie header: mode, 32-byte params, record format, reserved
    uint8_t raw[sizeof(DemoModeHeader)] = {};
    const uint32_t mode = GAME_MODE_CONQUEST, fmt = DEMO_RECORD_FORMAT;
    std::memcpy(raw, &mode, 4);
    std::memcpy(raw + 4, &p, MODE_PARAMS_V1_SIZE);
    std::memcpy(raw + 4 + MODE_PARAMS_V1_SIZE, &fmt, 4);
    DemoModeHeader mh;
    CHECK(demo_mode_header_size(3) == 48 && demo_mode_header_size(4) == sizeof(DemoModeHeader));
    CHECK(demo_mode_header_read(raw, 3, &mh) && mh.mode == GAME_MODE_CONQUEST && mh.params.seed == 77 &&
          mh.params.match.team[2] == 0 && mh.record_format == DEMO_RECORD_FORMAT);
    std::printf("round 11 formats: MODE v1 block and movie v3 header read (match rules = defaults)\n");
}

int main(int argc, char **argv) {
    mc_install_crash_handler();
    g_game = argc > 1 ? argv[1] : MC_DEFAULT_GAME_DIR;
    if (!sim_init(g_game.c_str())) { std::printf("SKIP: no game data in %s\n", g_game.c_str()); return 0; }
    sim_register_gameplay();
    mode_level_register();
    g_tmp = fs::temp_directory_path() / "mc_movie_v3_test";
    std::error_code ec;
    fs::remove_all(g_tmp, ec);
    fs::create_directories(g_tmp / "rec" / "movie");
    test_round11_formats();
    test_v3();
    test_v1_v2();
    fs::remove_all(g_tmp, ec);
    if (g_fail) { std::printf("movie_v3_test: %d FAILED\n", g_fail); return 1; }
    std::printf("movie_v3_test: all passed\n");
    return 0;
}
