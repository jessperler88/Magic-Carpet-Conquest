#define _CRT_SECURE_NO_WARNINGS
// Integration test: load level 38 (the level of the shipped demo recording), render the demo's first
// camera view with the full engine (terrain generator + tables + rasteriser + landscape renderer)
// and write engine_test_level38.ppm next to the executable. Also renders one frame of every other
// level to catch crashes. Exit code 0 when every frame rendered without touching the guard bands.
#include "engine.h"
#include "sim.h"
#include "mc_globals.h"
#include "render.h"
#include "thing.h"
#include "player.h"
#include "demo.h"
#include "mcfile.h"
#include "crash_handler.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static const int W = 320, H = 200, GUARD = 4096;

static bool write_ppm(const char *path, const uint8_t *px, const uint8_t *pal6) {
    FILE *f = std::fopen(path, "wb");
    if (!f) return false;
    uint8_t rgb[768];
    mc_palette_to_rgb(pal6, rgb);
    std::fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int i = 0; i < W * H; i++) std::fwrite(rgb + px[i] * 3, 1, 3, f);
    std::fclose(f);
    return true;
}

static bool render_one(const Camera &cam, std::vector<uint8_t> &buf, const char *ppm) {
    std::memset(buf.data(), 0xAB, buf.size());
    FrameBuffer fb{buf.data() + GUARD, W, H};
    render_set_view_window(fb, g_state->view_size);   // render_frame_1fab0 does this before render_view
    render_view(fb, cam);
    for (int i = 0; i < GUARD; i++)
        if (buf[i] != 0xAB || buf[GUARD + W * H + i] != 0xAB) { std::printf("FAIL: guard band written\n"); return false; }
    int hist[256] = {};
    for (int i = 0; i < W * H; i++) hist[fb.pixels[i]]++;
    int distinct = 0;
    for (int c : hist) distinct += c != 0;
    if (ppm) {
        write_ppm(ppm, fb.pixels, g_palette6);
        std::printf("wrote %s (%d distinct colours)\n", ppm, distinct);
    }
    return distinct > 8;
}

// Level start of the whole engine (features + constructors + players) against the engine's own
// snapshot of level 38, taken 413 ticks later: things that never move must sit in the same pool slot
// with the same position and sprite, and the four player things must be the snapshot's.
static bool compare_level38_with_snapshot(const char *game) {
    char path[1024];
    mc_blob gam;
    mc_path_join(path, sizeof path, game, "movie/gam00000.dat");
    if (!mc_read_file(path, &gam)) { std::printf("snapshot missing, comparison skipped\n"); return true; }
    if (gam.len != MC_STATE_IMAGE_SIZE) { mc_blob_free(&gam); std::printf("FAIL: snapshot size\n"); return false; }
    static GameState snap_state;
    GameState *snap = &snap_state;
    bool ok = thing_load_snapshot_image(gam.data, snap);
    int stat[16][2] = {};     // per class: static things in the snapshot / matched in the same slot
    for (int i = 1; i < MC_THING_SLOTS && ok; i++) {
        const Thing &s = snap->things[i], &o = g_state->things[i];
        bool is_static = s.cls == 2 || s.cls == 11 || (s.cls == 10 && s.type == 0x2d && i < 400);
        if (!is_static) continue;
        stat[s.cls][0]++;
        if (o.cls == s.cls && o.type == s.type && o.x == s.x && o.y == s.y && o.sprite == s.sprite && o.rng == s.rng)
            stat[s.cls][1]++;
    }
    // players_init_records queues command 1 (join) for every player; the first tick spawns them.
    game_tick_sim();
    int players_ok = 0;
    for (int p = 0; p < 4; p++) {
        const Thing &s = snap->things[snap->players[p].thing], &o = g_state->things[g_state->players[p].thing];
        if (snap->players[p].thing == g_state->players[p].thing && o.cls == 3 && o.type == s.type && o.max_health == s.max_health)
            players_ok++;
    }
    std::printf("level 38 vs snapshot (same slot, type, position, sprite, rng): scenery %d/%d, switches %d/%d, "
                "wizard castles %d/%d, player things %d/4\n",
                stat[2][1], stat[2][0], stat[11][1], stat[11][0], stat[10][1], stat[10][0], players_ok);
    ok = ok && stat[2][0] > 0 && stat[2][1] == stat[2][0] && stat[11][1] == stat[11][0] &&
         stat[10][1] == stat[10][0] && players_ok == 4;
    if (!ok) std::printf("FAIL: level 38 does not reproduce the snapshot's static things\n");
    int missing = thing_dispatch_report(nullptr);
    std::printf("level start dispatched %d unported handler(s)\n", missing);
    if (missing) thing_dispatch_report(stdout);
    mc_blob_free(&gam);
    return ok;
}

int main(int argc, char **argv) {
    const char *game = argc > 1 ? argv[1] : MC_DEFAULT_GAME_DIR;
    setvbuf(stdout, nullptr, _IONBF, 0);
    mc_install_crash_handler();
    std::printf("init\n");
    if (!engine_init(game)) return 1;
    std::printf("init ok\n");
    std::vector<uint8_t> buf(W * H + 2 * GUARD);
    int fails = 0;

    if (!engine_load_level(38)) { std::printf("FAIL: level 38\n"); return 1; }
    if (!compare_level38_with_snapshot(game)) fails++;
    // Camera of the demo's first frame: player thing at (640, 30080, 0x100), yaw 0, zoom 0x80,
    // render_frame adds 0x80 to z. (movie/gam00000.dat, PlayerRec position history entry 31.)
    Camera demo{640, 30080, 0, 0x100 + 0x80, 0, 0, 0x80};
    if (!render_one(demo, buf, "engine_test_level38.ppm")) fails++;
    Camera high = demo; high.cam_z += 0x800; high.pitch = 0x40;   // positive pitch looks down
    if (!render_one(high, buf, "engine_test_level38_high.ppm")) fails++;

    for (int lv = 0; lv < 70; lv++) {
        if (lv == 38) continue;
        if (!engine_load_level(lv)) { std::printf("FAIL: level %d did not load\n", lv); fails++; continue; }
        Camera c = engine_default_camera();
        if (!render_one(c, buf, lv == 0 ? "engine_test_level0.ppm" : nullptr)) { std::printf("FAIL: level %d frame\n", lv); fails++; }
    }
    // The shipped movie: the first tick loads its state snapshot; fly 400 ticks of the recording and
    // render what the player sees at the start and at the end.
    sim_prepare_movie();        // game logic as the original plays the movie (sim.h)
    if (demo_open(game, 0)) {
        if (!engine_tick()) { std::printf("FAIL: movie 0 did not start\n"); fails++; }
        if (!render_one(player_camera(g_state->local_player), buf, "engine_test_demo_start.ppm")) fails++;
        int ticks = 1;
        while (ticks < 400 && engine_tick()) ticks++;
        if (ticks != 400) { std::printf("FAIL: movie 0 stopped after %d ticks\n", ticks); fails++; }
        if (!render_one(player_camera(g_state->local_player), buf, "engine_test_demo_tick400.ppm")) fails++;
        const Thing *pt = thing_at(g_state->players[g_state->local_player].thing);
        std::printf("movie 0 after %d ticks: player at %d,%d z=%d yaw=%d, %ld/%ld packets\n", ticks, pt->x >> 8, pt->y >> 8, pt->z, pt->yaw,
                    demo_packets_read(), demo_packets_total());
        demo_close();
    } else {
        std::printf("movie 0 missing, playback skipped\n");
    }
    std::printf("%s: %d failure(s)\n", fails ? "FAILED" : "OK", fails);
    engine_shutdown();
    return fails ? 1 : 0;
}
