// Unit test for player.cpp / demo.cpp against the engine's own snapshot of level 38
// (movie/gam00000.dat + map00000.dat) and the input recording that goes with it (movie/mvi00000.dat):
//  1. the recording format (ticks x players x 10 bytes + the final quit packet),
//  2. the snapshot's player records versus what players_init_records / player_spawn /
//     player_log_position / mana_totals_update / the flyer update produce,
//  3. a full replay of the recording through game_tick_sim() with only the player handlers bound:
//     packet alignment, flight invariants, path dump (player_replay.csv / .ppm next to the exe).
// argv[1] = game dir.
#define _CRT_SECURE_NO_WARNINGS
#include "player.h"
#include "demo.h"
#include "mc_math.h"
#include "terrain.h"
#include "mcfile.h"
#include "gen/dispatch_tables.h"
#include "gen/player_tables.h"
#include "crash_handler.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)
#define CHECK_EQ(a, b) do { long long va_ = (long long)(a), vb_ = (long long)(b); if (va_ != vb_) { \
    std::printf("FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, va_, vb_); g_fail++; } } while (0)

static std::string g_out_dir = ".";

// ---- snapshot ----------------------------------------------------------------------------------

static bool load_snapshot(const char *game_dir) {
    char path[1024];
    mc_blob gam, map;
    mc_path_join(path, sizeof path, game_dir, "movie/gam00000.dat");
    if (!mc_read_file(path, &gam)) return false;
    mc_path_join(path, sizeof path, game_dir, "movie/map00000.dat");
    if (!mc_read_file(path, &map)) { mc_blob_free(&gam); return false; }
    bool ok = gam.len == MC_STATE_IMAGE_SIZE && map.len >= 0x60000;
    if (ok) {
        std::memcpy(g_map_type, map.data, 0x10000);
        std::memcpy(g_map_height, map.data + 0x10000, 0x10000);
        std::memcpy(g_map_light, map.data + 0x20000, 0x10000);
        std::memcpy(g_map_flags, map.data + 0x30000, 0x10000);
        std::memcpy(g_cell_things, map.data + 0x40000, 0x20000);
        ok = thing_load_snapshot_image(gam.data, g_state);
    }
    mc_blob_free(&gam);
    mc_blob_free(&map);
    return ok;
}

// ---- test-only constructors for the spawn test (the real ones belong to another subsystem) ------

static Thing *fake_create(const Pos *pos, int cls, int type, int state, int desc) {
    Thing *t = thing_alloc();
    if (!t) return nullptr;
    t->cls = (uint8_t)cls; t->type = (uint8_t)type; t->state = (uint8_t)state; t->desc = (uint32_t)desc;
    thing_link_cell(t, pos);
    return t;
}
static Thing *fake_wizard0(const Pos *p) { return fake_create(p, 3, 0, 0, 7); }
static Thing *fake_wizard1(const Pos *p) { return fake_create(p, 3, 1, 1, 8); }
static Thing *fake_castle(const Pos *p) {
    Thing *t = fake_create(p, 3, 2, 5, 0);
    if (t) t->home = *p;
    return t;
}
template <int N> static Thing *fake_spell(const Pos *p) {
    Thing *t = fake_create(p, 12, N, N * 3, 0);
    if (t) t->duration = 10;
    return t;
}
static const ThingCreateFn k_fake_spells[24] = {
    fake_spell<0>, fake_spell<1>, fake_spell<2>, fake_spell<3>, fake_spell<4>, fake_spell<5>,
    fake_spell<6>, fake_spell<7>, fake_spell<8>, fake_spell<9>, fake_spell<10>, fake_spell<11>,
    fake_spell<12>, fake_spell<13>, fake_spell<14>, fake_spell<15>, fake_spell<16>, fake_spell<17>,
    fake_spell<18>, fake_spell<19>, fake_spell<20>, fake_spell<21>, fake_spell<22>, fake_spell<23>,
};
static int s_stamps = 0, s_extents = 0;
static void fake_stamp(Thing *) { s_stamps++; }
static void fake_extents(Thing *, int) { s_extents++; }

static void bind_fakes(bool on) {
    thing_register_create(g_dispatch_b_cls3[0].handler, on ? fake_wizard0 : nullptr);
    thing_register_create(g_dispatch_b_cls3[1].handler, on ? fake_wizard1 : nullptr);
    thing_register_create(g_dispatch_b_cls3[2].handler, on ? fake_castle : nullptr);
    for (int i = 0; i < 24; i++)
        thing_register_create(g_dispatch_b_cls12[i].handler, on ? k_fake_spells[i] : nullptr);
    g_hook_castle_stamp_footprint = on ? fake_stamp : nullptr;
    g_hook_thing_set_castle_extents = on ? fake_extents : nullptr;
}

// ---- 1. recording format -----------------------------------------------------------------------

struct Movie {
    std::vector<CmdPacket> pk;
    int players = 0, ticks = 0;
    const CmdPacket &at(int tick, int player) const { return pk[(size_t)tick * players + player]; }
};

static bool test_movie_format(const char *game_dir, Movie &mv) {
    char path[1024];
    mc_blob f;
    mc_path_join(path, sizeof path, game_dir, "movie/mvi00000.dat");
    if (!mc_read_file(path, &f)) { std::printf("movie: %s missing\n", path); return false; }
    CHECK_EQ(f.len % sizeof(CmdPacket), 0);
    mv.pk.resize(f.len / sizeof(CmdPacket));
    std::memcpy(mv.pk.data(), f.data, mv.pk.size() * sizeof(CmdPacket));
    size_t len = f.len;
    mc_blob_free(&f);
    mv.players = g_state->player_count;                 // of the snapshot
    CHECK_EQ(mv.players, 4);
    // ticks * players full packets plus the packet that carried the quit command
    CHECK_EQ(mv.pk.size() % mv.players, 1);
    mv.ticks = (int)(mv.pk.size() / mv.players);
    CHECK_EQ(mv.pk.back().cmd, 2);
    int by_cmd[256] = {}, other_nonzero = 0, cmd2 = 0, pad_nonzero = 0;
    for (size_t i = 0; i < mv.pk.size(); i++) {
        const CmdPacket &p = mv.pk[i];
        by_cmd[p.cmd]++;
        if (p.cmd == 2) cmd2++;
        if (i % mv.players != 0) {
            static const CmdPacket zero{};
            if (std::memcmp(&p, &zero, sizeof zero) != 0) other_nonzero++;
        }
        for (uint8_t b : p.pad6) if (b) pad_nonzero++;
    }
    CHECK_EQ(cmd2, 1);
    CHECK_EQ(other_nonzero, 0);                         // the AI players never produce packets
    CHECK_EQ(pad_nonzero, 0);
    std::printf("movie: %zu bytes = %d ticks x %d players x %zu bytes + 1 final packet (cmd 2); commands:",
                len, mv.ticks, mv.players, sizeof(CmdPacket));
    for (int c = 0; c < 256; c++) if (by_cmd[c]) std::printf(" 0x%02x x%d", c, by_cmd[c]);
    std::printf("\n");
    return true;
}

// ---- 2. snapshot versus the translated functions -----------------------------------------------

static void test_tables() {
    CHECK_EQ(g_player_flight_consts[1], 0x10);
    CHECK_EQ(g_player_flight_consts[2], -0x50);
    CHECK_EQ(g_player_flight_consts[3], 0x50);
    CHECK_EQ(g_player_flight_consts[4], -4);
    CHECK_EQ(g_player_flight_consts[9], 0x10);
    CHECK_EQ(g_player_flight_consts[10], -0x50);
    CHECK_EQ(g_player_flight_consts[11], 0x50);
    CHECK_EQ(g_player_flight_consts[12], -4);
    int seen = 0;
    for (int i = 0; i < 24; i++) seen |= 1 << g_spell_slot_order[i];
    CHECK_EQ(seen, 0xffffff);                           // a permutation of the 24 spell ids
}

static void test_snapshot_static(const GameState &snap) {
    const PlayerRec &r0 = g_state->players[0];
    Thing *t0 = thing_at(r0.thing);
    PlayerBlock *P0 = player_block(t0);
    CHECK(t0->cls == 3 && t0->type == 0 && t0->state == 0);
    CHECK_EQ(r0.tick, 413);
    CHECK_EQ(r0.index, 0);
    CHECK_EQ(r0.is_computer, 0);
    CHECK_EQ(r0.log_count, 0x20);
    CHECK_EQ(r0.view_entry, 0x1f);
    for (int p = 1; p < 4; p++) {
        CHECK_EQ(g_state->players[p].index, p);
        CHECK_EQ(g_state->players[p].is_computer, 1);
        CHECK_EQ(player_block(thing_at(g_state->players[p].thing))->player_no, p);
    }
    // player_spawn puts the wizard 0x100 above the ground of the start position; player 0 has not
    // moved in the 412 ticks before the snapshot (speed 0), so it must still be exactly there.
    Pos start = world_start_pos(0);
    CHECK_EQ(t0->x, start.x);
    CHECK_EQ(t0->y, start.y);
    CHECK_EQ(t0->z, terrain_height_at(&start) + 0x100);
    CHECK_EQ(t0->speed_cur, 0);
    CHECK_EQ(t0->speed_base, g_player_flight_consts[3]);
    // regeneration values of the last player_type0_s0_update
    CHECK_EQ(t0->mana_cost, t0->mana_total / 2000);
    CHECK_EQ(P0->health_regen, t0->max_health / 2000);
    CHECK_EQ(P0->countdown15f, 2000 - ((int)r0.tick - 1));     // spawned on tick 1, -1 per update
    CHECK_EQ(P0->aim_charge, 200);
    CHECK_EQ(P0->mana, 1000);
    CHECK_EQ(P0->fire_dist, 0x800);
    CHECK_EQ(P0->ridge_dist, 0x800);
    for (uint8_t b : P0->unk14d) CHECK_EQ(b, 0x10);

    // player_log_position: the live log entry is what the function writes for the current Thing.
    {
        PlayerRec copy = r0;
        std::memset(&g_state->players[0].log[31], 0xee, sizeof(PosLogEntry));
        cfg_tick_bit(1) = 1;
        player_log_position(&g_state->players[0], t0);
        CHECK(std::memcmp(&g_state->players[0].log[31], &copy.log[31], sizeof(PosLogEntry)) == 0);
        // entries 0..30 are still the template of players_init_records: only the last one is written
        for (int i = 0; i < 31; i++) CHECK(std::memcmp(&r0.log[i], &r0.log_template, sizeof(PosLogEntry)) == 0);
        Camera c = player_camera(0);
        CHECK(c.cam_x == t0->x && c.cam_y == t0->y && c.cam_z == t0->z + 0x80 && c.yaw == t0->yaw);
        CHECK(c.pitch == 0 && c.roll == 0 && c.zoom == 0x80);
        std::printf("snapshot: player 0 thing %d at (%04x, %04x, %d) yaw %03x; camera x %d y %d z %d yaw %d pitch %d roll %d zoom %d\n",
                    r0.thing, (unsigned)t0->x, (unsigned)t0->y, t0->z, t0->yaw, c.cam_x, c.cam_y, c.cam_z, c.yaw, c.pitch, c.roll, c.zoom);
    }

    // player_rebuild_spell_index reproduces P+0x2a4 of every player.
    for (int p = 0; p < 4; p++) {
        Thing *t = thing_at(g_state->players[p].thing);
        PlayerBlock *P = player_block(t);
        uint16_t before[24];
        std::memcpy(before, P->spell_thing, sizeof before);
        player_rebuild_spell_index(t);
        CHECK(std::memcmp(before, P->spell_thing, sizeof before) == 0);
        int n = 0;
        for (int i = 0; i < 24; i++) if (P->spell_slot[i]) { n++; CHECK(thing_at(P->spell_slot[i])->cls == 12); }
        std::printf("snapshot: player %d owns %d spells, hands = slots %d / %d\n", p, n, P->slot_left, P->slot_right);
    }

    // mana_totals_update reproduces the stored Thing.mana_total of the player wizards (it ran at
    // the start of the last thing_update_all before the snapshot).
    {
        int32_t before[4], after[4];
        for (int p = 0; p < 4; p++) before[p] = thing_at(g_state->players[p].thing)->mana_total;
        mana_totals_update(t0);
        for (int p = 0; p < 4; p++) after[p] = thing_at(g_state->players[p].thing)->mana_total;
        std::printf("snapshot: mana totals stored %d %d %d %d, recomputed %d %d %d %d, world total %u\n",
                    before[0], before[1], before[2], before[3], after[0], after[1], after[2], after[3], g_cfg->total_mana);
        for (int p = 0; p < 4; p++) CHECK_EQ(before[p], after[p]);
        CHECK(g_cfg->total_mana >= (uint32_t)after[0]);
    }

    // castle level table versus the castles in the snapshot
    {
        static const int32_t health[8] = {0, 20000, 40000, 40000, 60000, 60000, 80000, 80000};
        int castles = 0;
        for (int i = 1; i < MC_THING_SLOTS; i++) {
            Thing *c = thing_at(i);
            if (c->cls != 3 || c->type != 2) continue;
            castles++;
            Thing probe = *c;
            int32_t max_before = c->max_health, total_before = c->mana_total;
            castle_set_level_stats(&probe);
            CHECK_EQ(probe.max_health, max_before);
            CHECK_EQ(probe.mana_total, total_before);
            CHECK_EQ(max_before, health[c->aux & 7]);
        }
        CHECK(castles >= 2);
        std::printf("snapshot: %d castles match castle_set_level_stats\n", castles);
    }

    // One flyer update with the (all zero) inputs of the snapshot is a fixed point: the wizard has
    // been hovering at its start position for 412 ticks, so nothing but the countdown may change.
    {
        Thing tb = *t0;
        PlayerBlock pb = *P0;
        player_type0_s0_update(t0);
        int thing_diff = 0, block_diff = 0;
        for (size_t i = 0; i < sizeof(Thing); i++)
            if (reinterpret_cast<uint8_t *>(t0)[i] != reinterpret_cast<uint8_t *>(&tb)[i]) thing_diff++;
        for (size_t i = 0; i < sizeof(PlayerBlock); i++) {
            if (i >= offsetof(PlayerBlock, countdown15f) && i < offsetof(PlayerBlock, countdown15f) + 4) continue;
            if (reinterpret_cast<uint8_t *>(P0)[i] != reinterpret_cast<uint8_t *>(&pb)[i]) {
                if (!block_diff) std::printf("  P+0x%zx differs\n", i);
                block_diff++;
            }
        }
        CHECK_EQ(thing_diff, 0);
        CHECK_EQ(block_diff, 0);
        CHECK_EQ(P0->countdown15f, pb.countdown15f - 1);
        *t0 = tb;
        *P0 = pb;
    }
    (void)snap;
}

// players_init_records + player_spawn on the snapshot's level data versus the snapshot's records.
static void test_init_and_spawn(const GameState &snap) {
    g_cfg->flags = 0;
    players_init_records();
    for (int p = 0; p < 8; p++) {
        const PlayerRec &a = g_state->players[p], &s = snap.players[p];
        CHECK_EQ(g_state->commands[p].cmd, 1);
        if (p >= 4) CHECK_EQ(snap.commands[p].cmd, 1);         // never consumed: player_count is 4
        CHECK_EQ(a.index, s.index);
        CHECK_EQ(a.is_computer, s.is_computer);
        CHECK_EQ(a.view_entry, s.view_entry);
        CHECK_EQ(a.log_count, s.log_count);
        CHECK(std::memcmp(&a.log_template, &s.log_template, sizeof a.log_template) == 0);
        CHECK(std::memcmp(a.log, s.log, 31 * sizeof(PosLogEntry)) == 0);
        CHECK(std::strcmp(a.name, s.name) == 0);
        CHECK(std::memcmp(a.blk.ai_allowed, s.blk.ai_allowed, 24) == 0);
        CHECK(std::memcmp(a.blk.spell_found, s.blk.spell_found, 24) == 0);
        if (p == 0) {
            // no spell found so far in this campaign state: an empty book (the 24 spells of the
            // snapshot come from the "access all spells" cheat, see the report)
            for (int i = 0; i < 24; i++) CHECK_EQ(a.blk.spell_slot[i], -1);
            CHECK(a.blk.slot_left == 0xff && a.blk.slot_right == 0xff);
            CHECK(s.blk.slot_left == 0xff && s.blk.slot_right == 0xff);
        } else {
            CHECK(std::memcmp(a.blk.hotkey_slot, s.blk.hotkey_slot, 24) == 0);
            CHECK_EQ(a.blk.slot_left, s.blk.slot_left);
            CHECK_EQ(a.blk.slot_right, s.blk.slot_right);
            if (p < 4) {
                // the spell id the template gives slot i == the type of the spell Thing in that slot
                for (int i = 0; i < 24; i++) {
                    int32_t idx = s.blk.spell_slot[i];
                    int want = idx ? (int)snap.things[idx].type : -1;
                    CHECK_EQ(a.blk.spell_slot[i], want);
                }
            }
        }
    }

    // Spawn the four players of the level on an empty pool with test constructors.
    std::memset(g_state->things, 0, sizeof g_state->things);
    std::memset(g_cell_things, 0, g_world.cells * sizeof(uint16_t));
    thing_pool_reset();
    std::memset(g_cfg->creature_lists, 0, sizeof g_cfg->creature_lists);
    g_cfg->player_list = g_cfg->mana_ball_list = g_cfg->wizard_list = g_cfg->projectile_list = 0;
    g_cfg->substeps = 2;
    bind_fakes(true);
    s_stamps = s_extents = 0;
    for (int p = 0; p < 4; p++) {
        PlayerRec *rec = &g_state->players[p];
        player_spawn(rec, thing_at(0));
        const PlayerRec &s = snap.players[p];
        const Thing &st = snap.things[s.thing];
        CHECK(rec->thing != 0);
        Thing *t = thing_at(rec->thing);
        PlayerBlock *P = &rec->blk;
        CHECK_EQ(t->player, player_block_offset(p));
        CHECK(t->cls == 3 && t->type == st.type && t->state == st.state);
        CHECK_EQ(t->flags, st.flags);
        CHECK_EQ(t->sprite, st.sprite);
        if (!(t->ext_x == st.ext_x && t->ext_h == st.ext_h && t->draw_type == st.draw_type))
            std::printf("  note: player %d extents after thing_set_sprite (%d, %d, frames %d) vs snapshot (%d, %d, %d)\n", p,
                        t->ext_x, t->ext_h, t->draw_type, st.ext_x, st.ext_h, st.draw_type);
        CHECK_EQ(t->max_health, st.max_health);
        CHECK_EQ(t->health, 10000);
        CHECK_EQ(t->mana, 1000);
        CHECK_EQ(P->mana, s.blk.mana);
        CHECK_EQ(P->player_no, s.blk.player_no);
        CHECK(std::memcmp(P->unk14d, s.blk.unk14d, 8) == 0);
        CHECK_EQ(P->fire_dist, s.blk.fire_dist);
        CHECK_EQ(P->ridge_dist, s.blk.ridge_dist);
        CHECK_EQ(P->countdown15f, 2000);
        CHECK_EQ(P->invuln_timer, 100);
        CHECK_EQ(P->ai_aggression, s.blk.ai_aggression);
        CHECK_EQ(P->ai_accuracy, s.blk.ai_accuracy);
        CHECK_EQ(P->ai_reaction, s.blk.ai_reaction);
        for (int k = 0; k < 8; k++) {
            uint16_t now = *player_threat(const_cast<PlayerBlock *>(&s.blk), k);
            CHECK_EQ(*player_threat(P, k), p == 0 ? 0 : 0x601f);
            if (*player_threat(P, k) != now)
                std::printf("  note: player %d threat[%d] is %04x at spawn, %04x in the snapshot (413 ticks later)\n", p, k,
                            *player_threat(P, k), now);
        }
        if (p == 0) {
            CHECK(t->x == st.x && t->y == st.y && t->z == st.z);      // player 0 never moved
            CHECK_EQ(g_cfg->substeps, 0);                              // reset by the local player's spawn
        } else {
            int slots = 0;
            for (int i = 0; i < 24; i++) {
                int32_t idx = s.blk.spell_slot[i];
                if (!idx) { CHECK_EQ(P->spell_slot[i], 0); continue; }
                slots++;
                Thing *sp = thing_at(P->spell_slot[i]);
                CHECK(P->spell_slot[i] != 0 && sp->cls == 12 && sp->type == snap.things[idx].type);
                CHECK(sp->caster == rec->thing && (sp->flags & 1));
                CHECK_EQ(P->spell_thing[sp->type], P->spell_slot[i]);
            }
            CHECK(slots > 0);
            // level footer: castle level 1 for the three AI wizards, who own the Castle spell
            CHECK_EQ(g_state->level.castle_level[p], 1);
            CHECK(P->castle != 0);
            Thing *c = thing_at(P->castle);
            CHECK(c->cls == 3 && c->type == 2 && c->owner == t->owner && c->aux == 0);
            CHECK_EQ(c->mana_total, 5000);
            CHECK_EQ(c->mana, 5000);
        }
    }
    CHECK_EQ(s_stamps, 3);
    CHECK_EQ(s_extents, 3);
    bind_fakes(false);
    std::printf("init/spawn: records, spell books, AI parameters and spawn fields match the snapshot\n");
}

// player_terrain_collide on a synthetic wall (cell type 8 = terrain bit 0x100, impassable): an
// oblique step into the wall slides along the free axis, a step into a corner is refused. The
// recording never flies into such a cell, so the replay does not cover this path.
static void test_collide(const char *game_dir) {
    CHECK(load_snapshot(game_dir));
    Thing *t = thing_at(g_state->players[0].thing);
    const int cx = 0x40, cy = 0x40;
    for (int dy = -1; dy <= 2; dy++)
        for (int dx = -1; dx <= 2; dx++) g_map_type[mc_cell(cx + dx, cy + dy)] = 2;
    g_map_type[mc_cell(cx + 1, cy)] = 8;                // wall to the +x side
    g_map_type[mc_cell(cx + 1, cy + 1)] = 8;
    Pos here{(uint16_t)(cx * 256 + 0xf0), (uint16_t)(cy * 256 + 0x80), 0x2000};
    thing_move_to(t, &here);
    CHECK_EQ(terrain_type_mask_at(thing_pos(t)), 4);
    // free target: untouched apart from the minimum clearance
    g_pos_scratch = here;
    g_pos_scratch.y = (uint16_t)(here.y + 0x40);
    Pos want = g_pos_scratch;
    CHECK_EQ(player_terrain_collide(t), 1);
    CHECK(g_pos_scratch.x == want.x && g_pos_scratch.y == want.y && g_pos_scratch.z == want.z);
    g_pos_scratch.z = -1000;
    CHECK_EQ(player_terrain_collide(t), 1);
    CHECK_EQ(g_pos_scratch.z, terrain_height_at(&g_pos_scratch) + mc_move_desc(t->desc)->clear_hi);
    // oblique into the wall: (+0x40, +0x10) -> only the +y share of the step survives
    g_pos_scratch = here;
    g_pos_scratch.x = (uint16_t)(here.x + 0x40);
    g_pos_scratch.y = (uint16_t)(here.y + 0x10);
    CHECK_EQ(terrain_type_mask_at(&g_pos_scratch), 0x100);
    CHECK_EQ(player_terrain_collide(t), 1);
    CHECK_EQ(g_pos_scratch.x, here.x);
    CHECK(g_pos_scratch.y > here.y && g_pos_scratch.y <= here.y + 0x10);
    std::printf("collide: step (+64, +16) into a wall becomes (%+d, %+d)\n", (int16_t)(g_pos_scratch.x - here.x),
                (int16_t)(g_pos_scratch.y - here.y));
    // corner: +x and +y neighbours and the diagonal are walls -> refused
    g_map_type[mc_cell(cx, cy + 1)] = 8;
    Pos corner{(uint16_t)(cx * 256 + 0xf0), (uint16_t)(cy * 256 + 0xf0), 0x2000};
    thing_move_to(t, &corner);
    g_pos_scratch = corner;
    g_pos_scratch.x = (uint16_t)(corner.x + 0x30);
    g_pos_scratch.y = (uint16_t)(corner.y + 0x30);
    CHECK_EQ(player_terrain_collide(t), 0);
}

// Hit, knock-back, shield, death, spectating and respawn of the flyer on the snapshot state. No
// ground truth exists for these paths (player 0 is never hit before the snapshot), so this checks
// the documented behaviour and that the whole cycle runs.
static Thing *fake_mana_ball(const Pos *p) { return fake_create(p, 10, 0x28, 0x2a, 0); }

static void test_hits_and_death(const char *game_dir) {
    CHECK(load_snapshot(game_dir));
    g_cfg->flags = 0;
    g_cfg->paused = 0;
    PlayerRec *rec = &g_state->players[0];
    Thing *t = thing_at(rec->thing);
    PlayerBlock *P = player_block(t);
    Thing *att = thing_at(g_state->players[1].thing);
    const uint16_t ai = thing_index(att);

    // 500 damage from the AI wizard: health, knock-back away from the attacker, flashes, music timer
    Pos before = *thing_pos(t);
    t->damage_slots[0].amount = 500;
    t->damage_slots[0].attacker = ai;
    player_type0_s0_update(t);
    CHECK_EQ(t->health, 10000 - 500);
    CHECK_EQ(t->damage_slots[0].attacker, 0);
    CHECK_EQ(P->knock_yaw, pos_angle_to(thing_pos(att), &before));
    CHECK_EQ(P->knock_speed, 50 - 4);
    CHECK_EQ(P->hit_flash, 4);
    CHECK_EQ(P->regen_pause, 0x10 - 1);
    CHECK_EQ(P->combat_music, 100 - 1);
    CHECK_EQ(g_cfg->palette_effect, 2);
    Pos want = before;
    math_rotate_offset(&want, P->knock_yaw, 0, 50);
    CHECK(t->x == want.x && t->y == want.y);
    int steps = 0;
    while (P->knock_speed != 0 && steps < 100) { player_type0_s0_update(t); steps++; }
    CHECK_EQ(steps, 11);                                // 46, 42, ... 6, then 2 -> 0
    CHECK_EQ(t->health, 10000 - 500);                   // no regeneration while regen_pause runs
    for (int i = 0; i < 20; i++) player_type0_s0_update(t);
    CHECK(t->health > 10000 - 500 && P->regen_pause == 0);

    // shield (flag 0x4000): a quarter of the damage, paid again in mana; the flag is consumed
    t->health = t->max_health;
    t->flags |= 0x4000;
    t->mana = 5000;
    t->damage_slots[0].amount = 400;
    t->damage_slots[0].attacker = ai;
    player_type0_s0_update(t);
    CHECK_EQ(t->health, 10000 - 100);
    CHECK_EQ(t->mana, 5000 - 100 + t->mana_cost);
    CHECK(!(t->flags & 0x4000));

    // inside the invulnerability window (spawn: 100 ticks) pending damage is dropped
    P->invuln_timer = 3;
    t->health = t->max_health;
    t->damage_slots[0].amount = 400;
    t->damage_slots[0].attacker = ai;
    player_type0_s0_update(t);
    CHECK(t->health == t->max_health && t->damage_slots[0].attacker == 0 && P->invuln_timer == 2);
    P->invuln_timer = 0;

    // lethal hit -> state 2 (falling), then the spells are dropped on the ground -> state 3
    for (int i = 0; i < 24; i++) thing_register_create(g_dispatch_b_cls12[i].handler, k_fake_spells[i]);
    thing_register_create(g_dispatch_b_cls10[0x28].handler, fake_mana_ball);
    int32_t slots_before[24];
    std::memcpy(slots_before, P->spell_slot, sizeof slots_before);
    t->damage_slots[0].amount = 1000000;
    t->damage_slots[0].attacker = ai;
    player_type0_s0_update(t);
    CHECK(t->state == 2 && t->health < 0 && t->killer == ai && t->z_vel == 0);
    int fall = 0;
    while (t->state == 2 && fall < 1000) { player_dying_update(t); fall++; }
    CHECK_EQ(t->state, 3);
    CHECK(t->flags & 0x20);
    CHECK_EQ(t->z, terrain_height_at(thing_pos(t)) + mc_move_desc(t->desc)->clear_hi);
    CHECK_EQ(player_block(att)->kills_of_player[0], 1);
    for (int i = 0; i < 24; i++) {
        Thing *s = thing_at(slots_before[i]);
        CHECK_EQ(P->spell_slot[i], s->type);            // back to spell ids for the respawn
        CHECK(!(s->flags & 1) && s->state == s->type * 3 + 1 && s->health >= 200 && s->health < 290);
        CHECK(std::abs((int16_t)(s->x - t->x)) <= 0x100 && std::abs((int16_t)(s->y - t->y)) <= 0x100);
    }
    std::printf("death: on the ground after %d ticks, 24 spells dropped, respawn delay value %d\n", fall, t->aux);

    // dead: the camera turns toward the killer
    uint16_t yaw0 = t->yaw;
    int want_yaw = pos_angle_to(thing_pos(t), thing_pos(att));
    for (int i = 0; i < 200; i++) player_type3_s3_update(t);
    CHECK_EQ(t->yaw, want_yaw & 0x7ff);
    CHECK(P->yaw_rate == 0 && P->pitch_acc == 0 && t->pitch == 0);
    CHECK_EQ(g_cfg->palette_effect, 7);
    (void)yaw0;

    // command 0xf: respawn; without a castle the level is lost (status bits 4 | 8) but the wizard is back
    g_state->commands[0].cmd = 0xf;
    player_commands_process();
    CHECK_EQ(rec->status & 0xc, 0xc);
    CHECK(rec->thing == thing_index(t) && t->state == 0 && !(t->flags & 0x20));
    CHECK(t->health == t->max_health && t->max_health == 10000 && t->mana == 1000);
    Pos start = world_start_pos(0);
    CHECK(t->x == start.x && t->y == start.y && t->z == terrain_height_at(&start) + 0x100);
    for (int i = 0; i < 24; i++) {
        Thing *s = thing_at(P->spell_slot[i]);
        CHECK(P->spell_slot[i] != 0 && P->spell_slot[i] != slots_before[i] && s->cls == 12 && s->type == i);
    }
    CHECK_EQ(P->invuln_timer, 100);
    for (int i = 0; i < 24; i++) thing_register_create(g_dispatch_b_cls12[i].handler, nullptr);
    thing_register_create(g_dispatch_b_cls10[0x28].handler, nullptr);
}

// ---- 3. replay ---------------------------------------------------------------------------------

static void write_ppm(const std::vector<Pos> &path) {
    const int S = 3, W = 256 * S;
    std::vector<uint8_t> img((size_t)W * W * 3);
    for (int y = 0; y < W; y++)
        for (int x = 0; x < W; x++) {
            int h = g_map_height[mc_cell(x / S, y / S)];
            uint8_t *px = &img[((size_t)y * W + x) * 3];
            if (h == 0) { px[0] = 20; px[1] = 40; px[2] = 110; }
            else {
                int v = 50 + h;
                if (v > 255) v = 255;
                px[0] = (uint8_t)(v * 3 / 4); px[1] = (uint8_t)v; px[2] = (uint8_t)(v * 2 / 3);
            }
        }
    auto dot = [&](int wx, int wy, int r, int cr, int cg, int cb) {
        int cx = wx * S / 256, cy = wy * S / 256;
        for (int dy = -r; dy <= r; dy++)
            for (int dx = -r; dx <= r; dx++) {
                int x = ((cx + dx) % W + W) % W, y = ((cy + dy) % W + W) % W;
                uint8_t *px = &img[((size_t)y * W + x) * 3];
                px[0] = (uint8_t)cr; px[1] = (uint8_t)cg; px[2] = (uint8_t)cb;
            }
    };
    for (int i = 1; i < MC_THING_SLOTS; i++) {          // castles and the other wizards for orientation
        const Thing *t = thing_at(i);
        if (t->cls == 3 && t->type == 2) dot(t->x, t->y, 3, 0, 0, 0);
        if (t->cls == 3 && t->type == 1) dot(t->x, t->y, 2, 255, 0, 255);
    }
    for (size_t i = 0; i < path.size(); i++) {
        int f = (int)(i * 255 / (path.size() ? path.size() : 1));
        dot(path[i].x, path[i].y, 0, 255, 255 - f, 0);  // yellow -> red over time
    }
    if (!path.empty()) {
        dot(path.front().x, path.front().y, 3, 0, 255, 0);
        dot(path.back().x, path.back().y, 3, 255, 255, 255);
    }
    std::string name = g_out_dir + "/player_replay.ppm";
    if (std::FILE *f = std::fopen(name.c_str(), "wb")) {
        std::fprintf(f, "P6\n%d %d\n255\n", W, W);
        std::fwrite(img.data(), 1, img.size(), f);
        std::fclose(f);
        std::printf("replay: wrote %s\n", name.c_str());
    }
}

static void test_replay(const char *game_dir, const Movie &mv, const GameState &snap) {
    player_register_handlers();
    // demo_load_state / demo_load_terrain give the same state as the direct load
    CHECK(demo_open(game_dir, 0));
    CHECK(demo_playing());
    CHECK(demo_load_state(0) && demo_load_terrain(0));
    {
        GameState *a = g_state;
        int diff = 0;
        for (size_t i = 4; i < sizeof(GameState); i++) {
            if (i >= 0x2195 && i < 0x21b9) continue;            // option bytes are kept
            // free / recyclable stacks: rebuilt by models_initialise (other order, active_top = -1)
            if (i >= offsetof(GameState, free_list) && i < offsetof(GameState, opt_second_surface)) continue;
            if (reinterpret_cast<const uint8_t *>(a)[i] != reinterpret_cast<const uint8_t *>(&snap)[i]) diff++;
        }
        CHECK_EQ(diff, 0);
        CHECK_EQ(a->free_top, snap.free_top);
        CHECK_EQ(a->active_top, -1);
        std::vector<uint8_t> is_free(MC_THING_SLOTS);
        for (int i = 0; i <= snap.free_top; i++) is_free[snap.free_list[i]] = 1;
        for (int i = 0; i <= a->free_top; i++) CHECK(is_free[a->free_list[i]] == 1);
    }
    g_cfg->paused = 0;
    g_cfg->substeps = 0;
    thing_dispatch_reset_stats();

    std::string csv_name = g_out_dir + "/player_replay.csv";
    std::FILE *csv = std::fopen(csv_name.c_str(), "w");
    if (csv) std::fprintf(csv, "tick,x,y,z,yaw,pitch,speed,strafe,ground,cmd,arg,steer_x,steer_y,bits,slot_left,slot_right,health,mana\n");

    std::vector<Pos> path;
    int ticks = 0, max_step = 0, max_dz = 0, max_alt = 0, min_alt = 1 << 30, max_dyaw = 0, bad_feed = 0, low = 0;
    int moved_ticks = 0, water_ticks = 0, log_bad = 0, fired = 0, blocked = 0;
    long long dist = 0;
    int want_left = 0xff, want_right = 0xff;
    Pos prev{};
    uint16_t prev_yaw = 0;
    const int lim = mv.ticks + 10;
    while (ticks < lim && !(g_state->players[g_state->local_player].status & 8)) {
        const CmdPacket &pk = mv.at(ticks, 0);
        game_tick_sim();
        const PlayerRec &rec = g_state->players[0];
        Thing *t = thing_at(rec.thing);
        PlayerBlock *P = player_block(t);
        // the packet of this tick reached the player
        if (P->input_bits != pk.bits) bad_feed++;
        if (pk.cmd == 0x15 && (int8_t)pk.arg != -1) want_left = (int8_t)pk.arg;
        if (pk.cmd == 0x16 && (int8_t)pk.arg != -1) want_right = (int8_t)pk.arg;
        if (P->slot_left != want_left || P->slot_right != want_right) bad_feed++;
        if (pk.bits & 0x30) fired++;
        // the camera log holds the position the tick started from
        if (ticks > 0 && (rec.log[31].x != prev.x || rec.log[31].y != prev.y || rec.log[31].z != prev.z ||
                          rec.log[31].yaw != prev_yaw)) log_bad++;
        int ground = terrain_height_at(thing_pos(t));
        int alt = t->z - ground;
        if (ticks > 0) {
            int dx = (int16_t)(t->x - prev.x), dy = (int16_t)(t->y - prev.y), dz = t->z - prev.z;
            int step = (int)mc_isqrt((uint32_t)(dx * dx + dy * dy));
            if (step > max_step) max_step = step;
            if (std::abs(dz) > max_dz) max_dz = std::abs(dz);
            if (step) moved_ticks++;
            else if (t->speed_cur != 0) blocked++;
            dist += step;
            int dyaw = (int16_t)((t->yaw - prev_yaw) << 5) >> 5;       // wrapped 11-bit difference
            if (std::abs(dyaw) > max_dyaw) max_dyaw = std::abs(dyaw);
        }
        if (alt > max_alt) max_alt = alt;
        if (alt < min_alt) min_alt = alt;
        if (alt < mc_move_desc(t->desc)->clear_hi) low++;
        if (g_map_height[mc_cell_of(t->x, t->y)] == 0) water_ticks++;
        CHECK(t->cls == 3 && t->state == 0);
        CHECK(t->speed_cur >= -0x50 && t->speed_cur <= 0x50);
        CHECK(P->target_speed >= -0x50 && P->target_speed <= 0x50);
        CHECK(P->strafe_speed >= -0x50 && P->strafe_speed <= 0x50);
        CHECK(t->yaw < 0x800 && t->pitch < 0x800 && P->move_pitch < 0x800);
        CHECK(P->yaw_rate >= -254 && P->yaw_rate <= 254 && P->pitch_acc >= -254 && P->pitch_acc <= 254);
        CHECK(t->health == t->max_health && t->mana == t->mana_total);
        if (g_fail > 20) break;
        if (csv)
            std::fprintf(csv, "%d,%u,%u,%d,%u,%u,%d,%d,%d,%u,%u,%d,%d,%u,%d,%d,%d,%d\n", ticks, (unsigned)t->x, (unsigned)t->y, t->z, t->yaw,
                         P->move_pitch, t->speed_cur, P->strafe_speed, ground, pk.cmd, pk.arg, pk.steer_x, pk.steer_y,
                         pk.bits, P->slot_left, P->slot_right, t->health, t->mana);
        path.push_back(*thing_pos(t));
        prev = *thing_pos(t);
        prev_yaw = t->yaw;
        ticks++;
    }
    if (csv) { std::fclose(csv); std::printf("replay: wrote %s\n", csv_name.c_str()); }

    const PlayerRec &rec = g_state->players[0];
    Thing *t = thing_at(rec.thing);
    std::printf("replay: %d ticks (%ld of %ld packets read), player tick %u -> %u, demo %s, status %04x\n", ticks,
                demo_packets_read(), demo_packets_total(), snap.players[0].tick, rec.tick,
                demo_playing() ? "still playing" : "closed", rec.status);
    std::printf("replay: path length %lld units (%.1f cells), moving in %d ticks, %d ticks with speed but no move, "
                "%d ticks over water, fire keys in %d ticks\n", dist, dist / 256.0, moved_ticks, blocked, water_ticks, fired);
    std::printf("replay: max step %d units/tick, max |dz| %d, altitude above ground %d..%d, max |dyaw| %d, end (%04x, %04x, %d) yaw %03x\n",
                max_step, max_dz, min_alt, max_alt, max_dyaw, (unsigned)t->x, (unsigned)t->y, t->z, t->yaw);
    CHECK_EQ(ticks, mv.ticks + 1);                      // 8550 full ticks + the tick of the quit packet
    CHECK_EQ(demo_packets_read(), demo_packets_total());
    CHECK(!demo_playing());
    CHECK_EQ(rec.status & 8, 8);
    CHECK_EQ(rec.tick, snap.players[0].tick + (uint32_t)ticks - 1);
    CHECK_EQ(bad_feed, 0);
    CHECK_EQ(log_bad, 0);
    CHECK_EQ(low, 0);                                   // never below ground + minimum clearance
    // forward 0x50 and strafe 0x50 at right angles, each rounded separately: sqrt(2) * 80 + 2
    CHECK(max_step <= 116);
    CHECK(max_dyaw <= 254 / 8);
    CHECK(dist > 256 * 100);                            // the player really flies around
    write_ppm(path);

    std::printf("replay: handlers dispatched but not ported:\n");
    int missing = thing_dispatch_report(stdout);
    std::printf("replay: %d distinct unported handlers\n", missing);
}

int main(int argc, char **argv) {
    mc_install_crash_handler();
    const char *game_dir = argc > 1 ? argv[1] : MC_DEFAULT_GAME_DIR;
    {
        std::string exe = argv[0];
        size_t cut = exe.find_last_of("/\\");
        if (cut != std::string::npos) g_out_dir = exe.substr(0, cut);
    }
    mc_globals_init();
    CHECK(sprite_table_init_sizes(game_dir));
    test_tables();
    if (!load_snapshot(game_dir)) {
        std::printf("player_test: snapshot movie/gam00000.dat + map00000.dat missing in %s\n", game_dir);
        return 1;
    }
    static GameState snap;
    snap = *g_state;
    Movie mv;
    if (!test_movie_format(game_dir, mv)) return 1;
    test_snapshot_static(snap);
    test_init_and_spawn(snap);
    test_collide(game_dir);
    test_hits_and_death(game_dir);
    test_replay(game_dir, mv, snap);
    if (g_fail) { std::printf("player_test: %d FAILED\n", g_fail); return 1; }
    std::printf("player_test: OK\n");
    return 0;
}
