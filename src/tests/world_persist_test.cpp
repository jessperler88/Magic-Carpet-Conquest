// Round 11 task D (docs/analysis/port_world_persist.md): persistence, checksum, debug commands and the mode-run
// hook in a world wider than 256 cells (world.h, the tiled test bed world_widen_tiled).
//
//  1. level 38 tiled to 1024 cells; the local wizard teleported to cell (900, 700) by a debug packet (the 20-bit
//     teleport layout of debug_cmd.h), a dragon spawned at cell (600, 800) (12-bit spawn cells), a creature
//     moved by hand to x = 900 cells, a home.y in the fourth copy; assert_pos / the dump see the int32 positions;
//  2. the checksum: changes when only a high half changes (a Thing's x, its home, a start position), comes back
//     when restored; the world size itself is in it;
//  3. save state at T (WRLD chunk, no MAPS chunk), play 2000 ticks, reload the level (a 256 world!) and load the
//     state (back to 1024), play again: identical checksums (with the parts) every tick; the 900-cell Things are
//     where they were at the save right after the load;
//  4. a 256-cell save loaded into the wide world goes back to 256 with the original's checksum;
//  5. the sound cull: a source (128, 128) cells away is culled in the 1024 world (int64 squares; in the 256
//     world that difference wraps), one 20 cells away is heard;
//  6. a v3 movie of an rts run with ModeParams.world_size = 1024 (mode_level.cpp widens the template), with a
//     wide teleport in the recording: gax has the WRLD block, max the wide map format, and the playback from a
//     256-cell campaign level gives the recording's checksums every tick.
#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "sim.h"
#include "demo.h"
#include "mode.h"
#include "mode_level.h"
#include "mode_dump.h"
#include "thing.h"
#include "player.h"
#include "net.h"
#include "savegame.h"
#include "debug_cmd.h"
#include "scenario.h"
#include "sound.h"
#include "settings.h"
#include "world.h"
#include "mc_globals.h"
#include "mc_image.h"
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

static Thing *wizard(int p) { return thing_at(thing_wrap(g_state->players[p & 7].thing)); }

// One tick with the debug queue pumped first (as mcport's sim_tick does).
static void tick() {
    debug_cmd_pump();
    game_tick_sim();
}

static bool queue_line(const char *line) {
    const ParsedCommand c = cmd_parse(line);
    if (c.kind != CmdKind::PACKET) { std::printf("  '%s': %s\n", line, c.error.c_str()); return false; }
    std::string why;
    return debug_cmd_queue(c.packet, &why);
}

static bool has_chunk(const std::string &f, const char *tag) {
    if (f.size() < SAVESTATE_HEADER_SIZE) return false;
    for (size_t off = SAVESTATE_HEADER_SIZE; off + 8 <= f.size();) {
        uint32_t len;
        std::memcpy(&len, f.data() + off + 4, 4);
        if (!std::memcmp(f.data() + off, tag, 4)) return true;
        off += 8 + len;
    }
    return false;
}

static int g_creature = 0;             // the creature moved to x = 900 cells

static void setup_wide_level() {
    g_cfg->flags = 0;
    g_cfg->paused = 0;
    CHECK(sim_load_level(38));
    tick();                                                     // the players spawn
    CHECK(world_widen_tiled(1024));
    CHECK(g_world.size == 1024 && g_world.units == 1024 * 256 && g_world.cells == 1024u * 1024u);
}

// ---- 1 + 2 ---------------------------------------------------------------------------------------------------
static void test_commands_and_checksum() {
    setup_wide_level();
    // the 20-bit teleport packet: b9 = the high nibbles
    const CmdPacket tp = debug_cmd_teleport(900 * 256 + 0x80, 700 * 256 + 0x80);
    CHECK(reinterpret_cast<const uint8_t *>(&tp)[9] == 0x23);
    CHECK(debug_cmd_describe(tp) == "teleport self -> 230528,179328 (ground)");
    const CmdPacket tp256 = debug_cmd_teleport(0x1234, 0x5678);
    CHECK(reinterpret_cast<const uint8_t *>(&tp256)[9] == 0);                                 // 256 world: unchanged
    const CmdPacket sp = debug_cmd_spawn(5, 0, 600, 800, true);
    CHECK(reinterpret_cast<const uint8_t *>(&sp)[9] == 0x32 && reinterpret_cast<const uint8_t *>(&sp)[3] == (600 & 0xff));

    CHECK(queue_line("teleport 900 700"));
    CHECK(queue_line("spawn dragon 600 800"));
    tick();
    tick();
    Thing *w = wizard(0);
    std::printf("wizard after the teleport: %d,%d (cell %d,%d)\n", (int)w->x, (int)w->y, (int)w->x >> 8, (int)w->y >> 8);
    CHECK(std::abs(((int)w->x >> 8) - 900) <= 1 && std::abs(((int)w->y >> 8) - 700) <= 1);
    {
        std::string detail;
        const ParsedCommand a = cmd_parse("assert_pos p0 900 700 2");
        CHECK(a.kind == CmdKind::ASSERT && cmd_check(a.check, &detail));
        const ParsedCommand b = cmd_parse("assert_pos p0 132 188 2");      // the 256-alias of the position
        CHECK(b.kind == CmdKind::ASSERT && !cmd_check(b.check, &detail));
    }
    int dragons = 0;
    for (int i = 1; i < thing_pool_slots(); i++) {
        const Thing *t = thing_at((unsigned)i);
        if (t->cls == 5 && ((int)t->x >> 8) >= 595 && ((int)t->x >> 8) <= 605 && ((int)t->y >> 8) >= 795 && ((int)t->y >> 8) <= 805)
            dragons++;
    }
    std::printf("Things spawned at cell 600,800: %d\n", dragons);
    CHECK(dragons >= 1);

    // a creature moved by hand to x = 900 cells, a scenery Thing's home in the fourth copy
    g_creature = 0;
    for (int i = 1; i < thing_pool_slots() && !g_creature; i++)
        if (thing_at((unsigned)i)->cls == 5 && (int)thing_at((unsigned)i)->x < 0x10000) g_creature = i;
    CHECK(g_creature != 0);
    Thing *c = thing_at((unsigned)g_creature);
    Pos to = *thing_pos(c);
    to.x = 900 * 256 + 0x40;
    thing_move_to(c, &to);
    CHECK((int)c->x == 900 * 256 + 0x40);

    // the dump: world + int32 positions
    {
        std::string doc;
        mode_dump_json(&doc);
        CHECK(doc.find("\"world\":{\"size\":1024,\"units\":262144,\"cells\":1048576}") != std::string::npos);
        char want[64];
        std::snprintf(want, sizeof want, "\"x\":%d", (int)w->x);
        CHECK(doc.find(want) != std::string::npos);
    }

    // 2. the checksum sees every high half
    NetChecksumParts p0, p1;
    net_state_checksum_parts(&p0);
    const uint32_t c0 = net_state_checksum();
    CHECK(c0 == p0.total);
    c->x = c->x + 0x10000;                                     // only the high half (no relink: restored at once)
    const uint32_t c1 = net_state_checksum();
    c->x = c->x - 0x10000;
    CHECK(c1 != c0 && net_state_checksum() == c0);
    c->home.x = c->home.x + 0x20000;
    net_state_checksum_parts(&p1);
    c->home.x = c->home.x - 0x20000;
    CHECK(p1.total != c0 && net_checksum_parts_first_diff(p0, p1, nullptr) == NCP_THINGS + 5);
    Pos sp0 = world_start_pos(2), sp1 = sp0;
    sp1.y = sp1.y + 0x30000;
    world_set_start_pos(2, sp1);
    net_state_checksum_parts(&p1);
    world_set_start_pos(2, sp0);
    CHECK(p1.total != c0 && net_checksum_parts_first_diff(p0, p1, nullptr) == NCP_PLAYERS + 2);
    CHECK(net_state_checksum() == c0);
    std::printf("checksum %08x; a creature's x high half -> %08x, its home's -> %08x part things.creature, start pos -> "
                "part player2\n", (unsigned)c0, (unsigned)c1, (unsigned)p1.total);
}

// ---- 3 + 4 ---------------------------------------------------------------------------------------------------
static std::vector<NetChecksumParts> run_parts(int n) {
    std::vector<NetChecksumParts> v((size_t)n);
    for (int i = 0; i < n; i++) { tick(); net_state_checksum_parts(&v[(size_t)i]); }
    return v;
}

static void test_savestate() {
    constexpr int kTicks = 2000;
    // the state of test 1 (the teleported wizard, the moved creature) after 50 more ticks
    for (int i = 0; i < 50; i++) tick();
    const fs::path path = g_tmp / "wide.mcs";
    CHECK(savestate_save_file(path.string().c_str(), "wide"));
    const std::string file = slurp(path);
    CHECK(has_chunk(file, "WRLD") && !has_chunk(file, "MAPS") && has_chunk(file, "GAME"));
    std::printf("wide save state: %zu bytes (WRLD chunk, no MAPS)\n", file.size());
    const uint32_t at_save = net_state_checksum();
    const int32_t wx = wizard(0)->x, cx = thing_at((unsigned)g_creature)->x;
    const std::vector<NetChecksumParts> a = run_parts(kTicks);

    // reload the level: a 256-cell world again
    g_cfg->flags = 0;
    CHECK(sim_load_level(38));
    CHECK(g_world.size == 256);
    CHECK(savestate_load_file(path.string().c_str()));
    CHECK(g_world.size == 1024);
    CHECK(net_state_checksum() == at_save);
    CHECK((int32_t)wizard(0)->x == wx && (int32_t)thing_at((unsigned)g_creature)->x == cx && cx >= 0x10000);
    CHECK(demo_repair_cell_lists() == 0);
    const std::vector<NetChecksumParts> b = run_parts(kTicks);
    int first = -1;
    for (int i = 0; i < kTicks && first < 0; i++)
        if (a[(size_t)i].total != b[(size_t)i].total || net_checksum_parts_first_diff(a[(size_t)i], b[(size_t)i], nullptr) >= 0)
            first = i;
    std::printf("save at tick T, %d ticks, load (from a 256 world), %d ticks: %s (wizard at %d,%d -> cell %d)\n", kTicks, kTicks,
                first < 0 ? "identical checksums + parts every tick" : "DIVERGED", (int)wx, (int)wizard(0)->y,
                (int)wx >> 8);
    if (first >= 0) std::printf("  first difference at tick %d: part %s\n", first + 1,
                                net_checksum_part_name(net_checksum_parts_first_diff(a[(size_t)first], b[(size_t)first], nullptr)));
    CHECK(first < 0);

    // 4. a 256-cell save loaded into the wide world
    g_cfg->flags = 0;
    CHECK(sim_load_level(38));
    for (int i = 0; i < 20; i++) tick();
    const fs::path p256 = g_tmp / "w256.mcs";
    CHECK(savestate_save_file(p256.string().c_str(), "256"));
    const std::string f256 = slurp(p256);
    CHECK(has_chunk(f256, "MAPS") && !has_chunk(f256, "WRLD"));
    const uint32_t c256 = net_state_checksum();
    CHECK(world_widen_tiled(512));
    CHECK(net_state_checksum() != c256);
    CHECK(savestate_load_file(p256.string().c_str()));
    CHECK(g_world.size == 256 && net_state_checksum() == c256);
    // the original's GameState-only quick save refuses a wide world
    CHECK(world_widen_tiled(1024));
    CHECK(!demo_save_state(10000) && !demo_load_state(10000));
}

// ---- 5 -------------------------------------------------------------------------------------------------------
static void test_sound_cull() {
    g_cfg->flags = 0;
    CHECK(sim_load_level(38));
    tick();
    CHECK(world_widen_tiled(1024));
    Thing *lst = wizard(g_state->local_player);
    Thing *src = nullptr;
    for (int i = 1; i < thing_pool_slots() && !src; i++) if (thing_at((unsigned)i)->cls == 2) src = thing_at((unsigned)i);
    CHECK(src != nullptr);
    if (!src) return;
    src->flags &= ~0x80u;
    Pos p = *thing_pos(lst);
    int vol = 0, pan = 0;
    p.x = p.x + 20 * 256;
    thing_move_to(src, &p);
    const bool heard20 = sound_locate((int)thing_index(src), &vol, &pan);
    p = *thing_pos(lst);
    p.x = p.x + 128 * 256;
    p.y = p.y + 128 * 256;
    thing_move_to(src, &p);
    const bool heard128 = sound_locate((int)thing_index(src), &vol, &pan);
    p.x = p.x + 300 * 256;
    thing_move_to(src, &p);
    const bool farther = sound_locate((int)thing_index(src), &vol, &pan);
    std::printf("sound: 20 cells %s, (128,128) cells %s, (428,128) cells %s\n", heard20 ? "heard" : "culled", heard128 ? "heard" : "culled",
                farther ? "heard" : "culled");
    CHECK(heard20 && !heard128 && !farther);
}

// ---- 6 -------------------------------------------------------------------------------------------------------
static int g_script_tick = 0;
static void script_input() {
    const int lp = g_state->local_player & 7;
    CmdPacket *pk = &g_state->commands[lp];
    if (pk->cmd != 0) return;
    const int t = g_script_tick;
    std::memset(pk, 0, sizeof *pk);
    pk->steer_x = (int8_t)((t / 40) % 2 ? 30 : -25);
    int bits = 0;
    if (t % 100 < 40) bits |= 1;
    else if (t % 100 > 80) bits |= 2;
    if (t % 50 == 10) bits |= 0x10;
    if (bits) { pk->cmd = 6; pk->bits = (uint8_t)bits; }
}

static int match_offset(const std::vector<uint32_t> &rec, const std::vector<uint32_t> &played, size_t min_n) {
    for (int off = 0; off < 3; off++) {
        if ((size_t)off >= rec.size()) break;
        std::vector<uint32_t> tail(rec.begin() + off, rec.end());
        const size_t n = (std::min)(tail.size(), played.size());
        if (n >= min_n && std::equal(tail.begin(), tail.begin() + (long)n, played.begin())) return off;
    }
    return -1;
}

static void test_movie_v3() {
    constexpr int kTicks = 600;
    const fs::path rec = g_tmp / "rec";
    demo_set_record_dir(rec.string().c_str());
    ModeParams mp;
    mp.seed = 7;
    mp.world_size = 1024;
    mp.bots = 3;
    mp.humans = 1;
    mp.flags = MODE_PARAM_DEBUG;
    g_cfg->flags = 0;
    g_cfg->paused = 0;
    CHECK(mode_start_run(g_game.c_str(), GAME_MODE_CONQUEST, mp));
    CHECK(sim_load_level(MODE_LEVEL_INDEX));
    CHECK(g_world.size == 1024);                                    // mode_level.cpp widened the template
    void (*saved_input)() = g_hook_player_local_input;
    g_hook_player_local_input = script_input;
    g_script_tick = 1;
    tick();
    g_cfg->movie = 21;
    g_cfg->flags |= 2;
    std::vector<uint32_t> recorded;
    for (int i = 0; i < kTicks; i++) {
        g_script_tick = 2 + i;
        if (i == 100) CHECK(queue_line("teleport 1000 300"));       // across the x = 1023 -> 0 seam
        if (i == 300) CHECK(queue_line("teleport 640 520"));
        tick();
        recorded.push_back(net_state_checksum());
    }
    const int32_t wx_end = wizard(0)->x;
    CHECK(demo_recording() && demo_version() == 3);
    demo_close();
    g_hook_player_local_input = saved_input;
    mode_stop_run();
    std::printf("v3 recording of a 1024 rts run: wizard ends at %d,%d (cell %d,%d)\n", (int)wx_end, (int)wizard(0)->y,
                (int)wx_end >> 8, (int)wizard(0)->y >> 8);
    CHECK(wx_end >= 0x10000);

    const std::string gax = slurp(rec / "movie" / "gax00021.dat");
    const std::string max = slurp(rec / "movie" / "max00021.dat");
    const size_t wrld = 16 + 1000 * sizeof(ThingHi) + sizeof(WorldHi);
    CHECK(gax.size() > wrld && gax.compare(gax.size() - wrld, 4, "WRLD") == 0);
    CHECK(max.size() == 8 + 1024u * 1024u * 6 + 0x12c2 && max.compare(0, 4, "MCWM") == 0);

    // play it back from a 256-cell campaign level
    g_cfg->flags = 0;
    CHECK(sim_load_level(5));
    for (int i = 0; i < 20; i++) tick();
    CHECK(g_world.size == 256);
    void (*input)() = g_hook_player_local_input;
    g_hook_player_local_input = nullptr;
    CHECK(demo_open(g_tmp.string().c_str(), 21));
    std::vector<uint32_t> played;
    while (demo_step() && (int)played.size() < kTicks + 10) played.push_back(net_state_checksum());
    const int off = match_offset(recorded, played, kTicks - 10);
    std::printf("v3 movie playback: %zu ticks, %s\n", played.size(), off >= 0 ? "identical checksums every tick" : "DIVERGED");
    CHECK(off >= 0);
    CHECK(!mode_active());
    demo_close();
    g_hook_player_local_input = input;
    demo_set_record_dir("");
}

int main(int argc, char **argv) {
    mc_install_crash_handler();
    g_game = argc > 1 ? argv[1] : MC_DEFAULT_GAME_DIR;
    if (!sim_init(g_game.c_str())) { std::printf("SKIP: no game data in %s\n", g_game.c_str()); return 0; }
    sim_register_gameplay();
    mode_level_register();
    g_hook_player_local_input = nullptr;
    g_tmp = fs::temp_directory_path() / "mc_world_persist_test";
    std::error_code ec;
    fs::remove_all(g_tmp, ec);
    fs::create_directories(g_tmp / "rec" / "movie");
    savegame_set_dirs(g_tmp.string().c_str(), nullptr);
    test_commands_and_checksum();
    test_savestate();
    test_sound_cull();
    test_movie_v3();
    fs::remove_all(g_tmp, ec);
    if (g_fail) { std::printf("world_persist_test: %d FAILED\n", g_fail); return 1; }
    std::printf("world_persist_test: all passed\n");
    return 0;
}
