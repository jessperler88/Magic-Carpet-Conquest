// Data sets (world_set.h, Hidden Worlds), ${MC_SIM_ALL} + game.cpp, no renderer:
//  1. campaign index mapping and chaining without the Hidden Worlds data (base outro after level 50);
//  2. with the data (MC_HIDDEN_DIR, <game>/hidden, or the CD folder extracted to
//     <game>/../../extracted/gog_cd/CARPET; SKIP otherwise): level 50 continues with index 100, the
//     campaign ends after index 124, the 25 level names come from HIDDEN.EXE, level 100 loads with the
//     set-1 files (castle footprints / sprite extents reloaded), and loading a base level afterwards gives
//     the same state as before the switch.
// argv[1] = game dir.
#define _CRT_SECURE_NO_WARNINGS
#include "world_set.h"
#include "game.h"
#include "sim.h"
#include "thing.h"
#include "level_features.h"
#include "mc_globals.h"
#include "crash_handler.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)
#define CHECK_EQ(a, b) do { long long _a = (long long)(a), _b = (long long)(b); if (_a != _b) { \
    std::printf("FAIL %s:%d: %s == %s (%lld != %lld)\n", __FILE__, __LINE__, #a, #b, _a, _b); g_fail++; } } while (0)

static std::vector<uint8_t> snapshot() {
    const uint8_t *p = reinterpret_cast<const uint8_t *>(g_state);
    return std::vector<uint8_t>(p, p + sizeof(GameState));
}

int main(int argc, char **argv) {
    mc_install_crash_handler();
    const char *game = argc > 1 ? argv[1] : MC_DEFAULT_GAME_DIR;
    if (!sim_init(game)) { std::printf("SKIP: no game data in %s\n", game); return 0; }

    // 1. without Hidden Worlds data
    world_set_init(game, "Z:/no/such/dir");
    CHECK(!world_set_hidden_available());
    CHECK(world_is_hidden_level(100) && world_is_hidden_level(169) && !world_is_hidden_level(99) && !world_is_hidden_level(170));
    CHECK_EQ(world_level_file_index(112), 12);
    CHECK_EQ(world_level_file_index(38), 38);
    CHECK_EQ(world_campaign_next(48), 49);
    CHECK_EQ(world_campaign_next(49), 50);
    CHECK(world_campaign_complete(50));
    CHECK(!world_campaign_complete(49));
    CHECK(!world_set_select(1));
    CHECK_EQ(world_set(), 0);
    CHECK(!sim_load_level(100));

    // 2. with the data
    std::string hidden;
    if (const char *e = std::getenv("MC_HIDDEN_DIR")) hidden = e;
    else {
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::path a = fs::path(game) / "hidden", b = fs::path(game) / ".." / ".." / "extracted" / "gog_cd" / "CARPET";
        hidden = fs::is_directory(a, ec) ? a.string() : b.lexically_normal().string();
    }
    world_set_init(game, hidden.c_str());
    if (!world_set_hidden_available()) {
        std::printf("part 2 SKIP: no Hidden Worlds data (%s)\n", hidden.c_str());
        std::printf(g_fail ? "world_set_test: %d failures\n" : "world_set_test: OK\n", g_fail);
        return g_fail ? 1 : 0;
    }
    CHECK_EQ(world_campaign_next(49), HW_LEVEL_BASE);
    CHECK(!world_campaign_complete(HW_LEVEL_BASE));
    CHECK(!world_campaign_complete(HW_LEVEL_BASE + 24));
    CHECK(world_campaign_complete(HW_LEVEL_BASE + 25));
    CHECK_EQ(world_campaign_next(HW_LEVEL_BASE + 3), HW_LEVEL_BASE + 4);
    const char *n0 = world_hidden_level_name(HW_LEVEL_BASE), *n24 = world_hidden_level_name(HW_LEVEL_BASE + 24);
    CHECK(n0 && std::strncmp(n0, "1. ", 3) == 0);
    CHECK(n24 && std::strncmp(n24, "25. ", 4) == 0);
    std::printf("names: %s .. %s\n", n0 ? n0 : "?", n24 ? n24 : "?");
    CHECK(world_hidden_level_name(HW_LEVEL_BASE + 25) == nullptr);

    // twice: a level load keeps a backup of the previous level's player records (GameState+0x2409)
    CHECK(sim_load_level(12));
    CHECK(sim_load_level(12));
    const std::vector<uint8_t> base12 = snapshot();
    const CastleFootprint base_fp = *castle_footprint(1);

    for (int k = 0; k < HW_CAMPAIGN_LEVELS; k++) {
        if (!sim_load_level(HW_LEVEL_BASE + k)) { std::printf("FAIL: hidden level %d did not load\n", k); g_fail++; continue; }
        CHECK_EQ(world_set(), 1);
        CHECK_EQ(g_cfg->level, HW_LEVEL_BASE + k);
        int things = 0;
        for (int i = 1; i < MC_THING_SLOTS; i++) things += thing_at(i)->cls != 0;
        CHECK(things > 0);
    }
    const CastleFootprint hw_fp = *castle_footprint(1);
    std::printf("castle size 1 footprint: base %dx%d, hidden %dx%d\n", base_fp.w, base_fp.h, hw_fp.w, hw_fp.h);

    CHECK(sim_load_level(12));
    CHECK(sim_load_level(12));
    CHECK_EQ(world_set(), 0);
    {
        const std::vector<uint8_t> again = snapshot();
        int shown = 0;
        for (size_t i = 0; i < again.size() && shown < 12; i++)
            if (again[i] != base12[i]) { std::printf("  diff at GameState+%#zx: %02x -> %02x\n", i, base12[i], again[i]); shown++; }
        CHECK(again == base12);
    }
    const CastleFootprint back_fp = *castle_footprint(1);
    CHECK(back_fp.w == base_fp.w && back_fp.h == base_fp.h && back_fp.map_len == base_fp.map_len);

    std::printf(g_fail ? "world_set_test: %d failures\n" : "world_set_test: OK\n", g_fail);
    return g_fail ? 1 : 0;
}
