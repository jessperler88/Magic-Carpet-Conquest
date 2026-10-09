// The "movie" demo system of carpet.exe: movie/mviNNNNN.dat (command packets), movie/gamNNNNN.dat
// (GameState snapshot) and movie/mapNNNNN.dat (the maps). Owner: demo.cpp.
//
// Recording format (established from demo_record_playback_step_3c540 and the shipped mvi00000.dat):
// a headerless stream of 10-byte CmdPacket records, one per player per tick in player order
// (player_count of the snapshot = 4 in the shipped movie), written from inside
// player_commands_process_3a8b0 right before each packet is executed. No lengths, no checksums. The
// recording ends with the packet that carried the quit command (cmd 2) of the recording player, so
// the file is ticks * players * 10 + 10 bytes: 342,010 = 8550 * 4 * 10 + 10.
//
// The snapshot pair is written when the first packet is recorded, i.e. in the middle of
// player_commands_process: the tick counters are already incremented, player 0's packet is not yet
// executed. Playback therefore loads the state at the same point (first call for player 0).
#pragma once
#include <cstring>
#include "mc_types.h"
#include "mode.h"

// Start playing movie `number` (movie/mvi%05d.dat under game_dir): remembers the directory, sets
// g_cfg->movie and the playback bit (Config.flags & 4). The files are opened by the first
// demo_record_playback_step, i.e. by the first game_tick_sim() / demo_step() after this call, which
// replaces the whole GameState and the maps by the snapshot. Returns false when the recording is
// missing.
bool demo_open(const char *game_dir, int number);
// True while a movie is being played (Config.flags & 4).
bool demo_playing();
// One tick of playback: game_tick_sim() with the packets of the recording. Returns false once the
// recording has ended (the local player's status gets bit 8, as in the original).
bool demo_step();
// Number of packets read from the recording so far / total packets in it (v3: records).
long demo_packets_read();
long demo_packets_total();

// demo_record_playback_step_3c540(packet): called by player_commands_process for every player's
// packet. Playback fills the packet from the file. Recording (Config.flags & 2, set by command 0xc):
// the first packet of a tick opens <record dir>/movie/mvi%05d.dat (Config.movie) and writes the
// snapshot pair gam / map, then every packet is appended (a packet with command 0xc is cleared after
// it was written). Without a record directory the record bit is dropped, as the original does when
// the file cannot be created.
void demo_record_playback_step(CmdPacket *pkt);
bool demo_load_state(int number);      // demo_load_state_3c200   (movie/gam%05d.dat)
bool demo_load_terrain(int number);    // demo_load_terrain_3c360 (movie/map%05d.dat)
void demo_close();                     // demo_close_3c7c0 (also flushes / closes a recording)

// ---- recording (port round 6) ------------------------------------------------------------------
// Where recordings and the quick save (command 10: gam10000.dat) are written: <dir>/movie/. The
// platform sets a writable directory (never the game's own files); "" (default) = recording off.
// Reads (playback, quick load) look in the demo_open directory first, then here.
void demo_set_record_dir(const char *dir);
bool demo_recording();                 // a recording file is open
long demo_packets_written();
bool demo_save_state(int number);      // demo_save_state_3c2c0   (movie/gam%05d.dat, original layout)
bool demo_save_terrain(int number);    // demo_save_terrain_3c430 (movie/map%05d.dat)

// The snapshot holds the original's 32-bit pointers (Thing.next, the free / recyclable stacks,
// Thing.desc, Thing.player). The original relinks only Thing.player (players) and Thing.desc (rebased
// on player 0's) when it loads a snapshot and rebuilds the stacks; Thing.next and the .player of other
// Things are used as they are, so a snapshot must carry the addresses of the run that plays it.
// Defaults: the bundled DOSBox 0.74-3 (GameState at 0x190d0, g_move_desc[0] at 0x1fda10, the dummy
// player block at 0x21de90 - identical in every reference dump of tools/reference).
struct DemoPointerBases {
    uint32_t state;         // run-time address of the GameState (things[0] = state + 0x7463)
    uint32_t move_desc;     // run-time address of g_move_desc[0] (DAT_00096a10)
    uint32_t dummy_block;   // run-time address of the dummy player block (Thing.player of unowned Things)
};
void demo_set_pointer_bases(const DemoPointerBases &b);
DemoPointerBases demo_pointer_bases();
// The GameState `src` in the original's layout (MC_STATE_IMAGE_SIZE bytes at dst).
void demo_state_to_original(const GameState *src, uint8_t *dst);
extern void (*g_hook_demo_textures_mark)();     // texture_mark_needed_4c130 when a recording starts

// Hooks (null = skipped).
extern int  (*g_hook_demo_input_changed)();     // input_changed_34090: non-zero aborts the playback
extern void (*g_hook_demo_input_snapshot)();    // input_snapshot_34060 when a movie starts
extern void (*g_hook_demo_textures_reload)();   // texture_load_needed_4c1a0 after the state was replaced

// ==== port round 7 (task E, docs/analysis/port_settings.md) ========================================
// Full quick save / load: with PortSettings::quicksave_full, demo_save_state(10000) (command 10) and
// demo_load_state(10000) (command 0xb) call these instead of writing / reading gam10000.dat (the
// original's GameState-only snapshot). savegame.h savestate_install_quick_hooks() installs them (the
// complete state in save slot 0). Null hooks: the original's files, plus a cell-list repair after the
// load (demo_repair_cell_lists).
extern bool (*g_hook_demo_quick_save)();
extern bool (*g_hook_demo_quick_load)();

// Check every cell list (g_cell_things heads, Thing.cell_next / cell_prev, Thing.flags & 4) and rebuild
// them when one is broken (cycle, stale link, a linked Thing no list reaches, a Thing in the wrong
// cell's list): first from the Things' own links (heads = linked Things whose cell_prev is 0 - exactly the
// lists of the moment the Things were saved, which repairs the original's quick load), and if that is
// not consistent either, every linked Thing in index order. Returns the number of problems found
// (0 = consistent, nothing changed).
int demo_repair_cell_lists();

// Recordings made with a Thing pool other than the original's 1000 slots (PortSettings::thing_slots) or
// with non-faithful gameplay rules (settings.h GameplayRules) are not the original's format - the
// original would overrun its 1000 slots / play them differently. They get their own file names, which the
// original never opens (a missing movie: it refuses cleanly), and a header:
//   movie/mvx%05d.dat  DemoExtHeader (16 bytes; version 2: + DemoExtRules, 16 bytes) + the CmdPacket
//                      stream as in mvi%05d.dat
//   movie/gax%05d.dat  the GameState in the port's form (indices) + u32 slots + Things 1000..slots-1
//                      (the stacks are rebuilt by models_initialise on both sides, as for gam%05d.dat)
//   movie/max%05d.dat  as map%05d.dat
// demo_open plays mvi%05d.dat when it exists (with the original's pool, thing_pool_force_slots(1000), and
// faithful rules), otherwise mvx%05d.dat with the pool size and rules of its header (version 1: faithful
// rules); an mvx file with a bad header is refused. Recording writes version 1 when the rules are faithful.
struct DemoExtHeader {
    char     magic[4];          // "MCPX"
    uint32_t version;           // 1, 2 or 3
    uint32_t thing_slots;       // pool size of the recording (version 1: > 1000; versions 2 / 3: >= 1000)
    uint32_t flags;             // 0 (reserved)
};
static_assert(sizeof(DemoExtHeader) == 16);
struct DemoExtRules {           // versions 2 and 3: follows DemoExtHeader
    uint32_t possession_range_pct;  // GameplayRules
    uint32_t mode;                  // GameplayRules.mode (version 3; written 0 by version 2, ignored there)
    uint32_t reserved[2];
};
static_assert(sizeof(DemoExtRules) == 16);
bool demo_extended();           // the movie being played / recorded uses the mvx / gax / max files

// ==== movie format v3 (Phase 4, round 10 task A; docs/analysis/port_mode.md) ===========================
// A recording made while a game mode runs (mode.h mode_active()) is an mvx file of version 3:
//   DemoExtHeader (version 3) + DemoExtRules (possession_range_pct, mode) + DemoModeHeader (48 bytes)
//   then one record per player per tick, in the order of v1 / v2:
//     CmdPacket (10 bytes) + u16 blob_len + blob_len bytes (the player's order blob of that tick; 0 = none)
//   gax%05d.dat: as version 1 / 2 (GameState + u32 slots + Things 1000..slots-1), then
//     u32 mode_len + mode_serialise() bytes (mode.h), u32 n (= DEMO_SIM_GLOBALS + 1) + n x u32: g_rng16,
//     then the DEMO_SIM_GLOBALS values of g_hook_demo_sim_globals_get (mode_level.cpp: g_terrain_nearly_flat,
//     the AI's g_ai_rand_seed and g_ai_human_wizard, 2 reserved)
//   max%05d.dat: as map%05d.dat.
// Round 11, a world wider than 256 cells (world.h; any mvx version - a wide recording without a mode is
// version 2, the original's mvi / gam / map files are never written or read in a wide world):
//   gax%05d.dat: after the snapshot (v1 / v2: after the extension's Things, v3: after the tail) the world block
//     "WRLD", u32 version 1, u32 size N, u32 slots (= the u32 slots before), ThingHi[slots] (mc_image.h: the
//     high halves of x / y / home of every slot, GameState's 1000 then the extension), WorldHi (g_world_hi);
//     demo_load_state sets the world size from it (world_set_size, 256 without a block) before any coordinate
//     is stored. A 256-cell snapshot has no block (byte-identical to round 10).
//   max%05d.dat: "MCWM", u32 N, type / height / light / flags (N*N each), cell heads (u16 N*N), the corner
//     table (0x12c2); demo_load_terrain expects it exactly when the world is wide.
// demo_open of a version 3 movie starts the mode run through g_hook_demo_mode_start (mode_level.h
// mode_start_run with the recorded mode and ModeParams; refused when the hook is missing) and forces the
// recorded rules; demo_close stops it (g_hook_demo_mode_stop). v1 / v2 movies and the original's mvi movies
// play exactly as before.
struct DemoModeHeader {         // version 3 / 4: follows DemoExtRules
    uint32_t   mode;            // GAME_MODE_* (mode.h), != 0
    ModeParams params;          // as mode_begin got them (version 3: the 32-byte ModeParams of rounds 10-11)
    uint32_t   record_format;   // 1: CmdPacket + u16 blob_len + blob
    uint32_t   reserved[2];
};
static_assert(sizeof(DemoModeHeader) == 80);
// Version 4 (round 12) = version 3 with ModeParams' MatchRules; written for every new mode recording. Version 3
// movies still play (their header is 48 bytes, the match rules read as the defaults).
constexpr int DEMO_VERSION_MODE = 4;
inline size_t demo_mode_header_size(int version) {
    return version >= 4 ? sizeof(DemoModeHeader) : sizeof(DemoModeHeader) - (sizeof(ModeParams) - MODE_PARAMS_V1_SIZE);
}
inline bool demo_mode_header_read(const uint8_t *raw, int version, DemoModeHeader *out) {
    const size_t np = version >= 4 ? sizeof(ModeParams) : MODE_PARAMS_V1_SIZE;
    *out = DemoModeHeader{};
    std::memcpy(&out->mode, raw, 4);
    if (!mode_params_read(raw + 4, np, &out->params)) return false;
    std::memcpy(&out->record_format, raw + 4 + np, 12);
    return true;
}
constexpr uint32_t DEMO_RECORD_FORMAT = 1;
constexpr size_t   DEMO_ORDER_BLOB_MAX = 0xffff;
constexpr int      DEMO_SIM_GLOBALS = 5;
int demo_version();             // 0 = the original's mvi format, else the mvx version of the open movie / recording
// Hooks of a version 3 movie (installed by mode_level_register, mode_level.h; null = not linked).
extern bool (*g_hook_demo_mode_start)(const char *game_dir, uint32_t mode, const ModeParams &params);
extern void (*g_hook_demo_mode_stop)();
extern void (*g_hook_demo_sim_globals_get)(uint32_t v[DEMO_SIM_GLOBALS]);
extern void (*g_hook_demo_sim_globals_set)(const uint32_t v[DEMO_SIM_GLOBALS]);
// Order blobs (round 15 on: RTS orders). Recording: called for every packet written to a v3 movie; returns
// the blob length (0..cap) written to buf for player `player`'s packet of this tick. Playback: called for
// every record read with its blob (also with len 0). Null = no blobs (len 0 written, blobs read are skipped).
extern size_t (*g_hook_demo_order_out)(int player, uint8_t *buf, size_t cap);
extern void   (*g_hook_demo_order_in)(int player, const uint8_t *data, size_t len);
