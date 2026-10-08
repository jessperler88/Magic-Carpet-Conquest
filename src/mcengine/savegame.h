// Save games (round 5, task B): save\carpet%02X.gam slots 0..5, fe_savegame_read_names_58f60 /
// fe_savegame_load_59030 / fe_savegame_save_591d0 (format in docs/ENGINE.md "Front-end state machine",
// docs/analysis/port_frontend.md). The port reads and writes the original's format byte for byte, so
// DOS saves load and port saves load in DOS.
//
// File layout (142 bytes, little endian, no padding):
//   +0x00 u32  version (4)
//   +0x04 char slot name [0x14] (space padded, no terminator needed)
//   +0x18 char Config+0x1d [0x20]  (player name, menu item 2)
//   +0x38 char Config+0x3d [0x20]  (call-name)
//   +0x58 u8   GameState+0x2195 [0xc] (detail / render options)
//   +0x64 u32  checksum = (Config.level + DAT_0012ed30 + DAT_0012ed31) * 4
//   +0x68 u8   GameState+0x3bd6 [0x18] (players[0].blk.spell_found: the campaign's spells)
//   +0x80 u8   DAT_0012ed30 (lobby session counter)
//   +0x81 u8   DAT_0012ed31 (lobby player count)
//   +0x82 u8   GameState+0x2195 [0xc] again (the loader reads it over the first copy)
// Loading sets Config.level = checksum / version - DAT_0012ed30 - DAT_0012ed31 and clears
// DAT_0009e500 (frontend.h g_fe_game_in_progress).
#pragma once
#include <cstddef>
#include <cstdint>

constexpr int    SAVE_SLOTS = 6;
constexpr int    SAVE_NAME_LEN = 0x14;           // bytes of the slot name in the file
constexpr uint32_t SAVEGAME_VERSION = 4;
constexpr size_t SAVEGAME_FILE_SIZE = 142;

struct SaveGame {
    uint32_t version;
    char     name[SAVE_NAME_LEN];
    char     player_name[0x20];      // Config+0x1d
    char     call_name[0x20];        // Config+0x3d
    uint8_t  options[0xc];           // GameState+0x2195
    uint32_t checksum;
    uint8_t  spells[0x18];           // GameState+0x3bd6
    uint8_t  session;                // DAT_0012ed30
    uint8_t  players;                // DAT_0012ed31
    uint8_t  options2[0xc];          // GameState+0x2195 (second copy)
};

// Pure (de)serialisation of the 142-byte record.
void savegame_encode(const SaveGame &g, uint8_t out[SAVEGAME_FILE_SIZE]);
bool savegame_decode(const uint8_t *data, size_t len, SaveGame *out);   // false when shorter than the record
// The record fe_savegame_save_591d0 would write now (from g_cfg / g_state / the front-end globals).
void savegame_capture(SaveGame *out, const char *slot_name);
// What fe_savegame_load_59030 does with a record whose version is 4: copies everything back, sets
// Config.level and clears g_fe_game_in_progress. Returns false (nothing changed) for another version.
bool savegame_apply(const SaveGame &g);
int  savegame_level(const SaveGame &g);            // checksum / version - session - players

// Directories. `save_dir` is where slots are written and read first ("" / null = saving disabled, as
// when the original's c:\carpet.cd\save is not writable); `dos_game_dir` (optional) is the original
// game directory whose save\carpet%02X.gam files are read (never written) when a slot is missing in
// save_dir.
void        savegame_set_dirs(const char *save_dir, const char *dos_game_dir);
const char *savegame_save_dir();
// Path of slot 0..5 in the save dir ("<dir>/carpet%02X.gam"); false when saving is disabled.
bool savegame_slot_path(int slot, char *buf, size_t cap);

// fe_savegame_read_names_58f60: the six slot names ("--" for a missing slot or a wrong version).
// `names[i]` gets 0x14 bytes + terminator.
void savegame_read_names(char names[SAVE_SLOTS][SAVE_NAME_LEN + 1]);
// fe_savegame_load_59030(slot + 1): 1 when the slot was loaded; `name_out` (optional) receives the
// slot name as the original reads it into the name table.
bool savegame_load(int slot, char *name_out);
// fe_savegame_save_591d0(slot + 1) with the slot name `name` (0x14 bytes written).
bool savegame_save(int slot, const char *name);
// The same on an explicit file (tests, tools).
bool savegame_load_file(const char *path, char *name_out);
bool savegame_save_file(const char *path, const char *name);

// ==== save anywhere (port round 7, task E; docs/analysis/port_settings.md) =========================
// A complete snapshot of a running level, port-only (the original has nothing like it; its quick save
// movie/gam10000.dat is the GameState alone and its load leaves stale cell / class lists). Taken and
// applied between two ticks - or from inside a tick by the full quick save (demo.h
// g_hook_demo_quick_save, PortSettings::quicksave_full) - so that running on from a loaded state gives
// exactly the ticks the saved run gave.
//
// File <save dir>/state%02d.mcs (slot 0 = the quick slot, 1..9 the numbered slots), little endian:
//   header (SAVESTATE_HEADER_SIZE bytes, SaveStateHeader below)
//   chunks: char tag[4], u32 length, data
//     "GAME" the GameState in the port's form (indices, not the original's pointers), sizeof(GameState)
//     "CONF" the Config (sizeof(Config)); platform fields are not restored (see savestate_apply)
//     "MAPS" type, height, light, flags (0x10000 each), cell heads (0x20000), corner table (0x12c2) - a
//            256-cell world only
//     "WRLD" (round 11) a world wider than 256 cells (world.h) instead of MAPS: u32 version 1, u32 size N,
//            u32 slots (= header thing_slots), type / height / light / flags (N*N each), cell heads (u16 N*N),
//            corner table (0x12c2), ThingHi[slots] (mc_image.h: the high halves of x / y / home.x / home.y of
//            every slot, GameState's 1000 then the pool extension), WorldHi (world.h g_world_hi: start
//            position and camera log high halves). The GAME / POOL chunks keep the original's 16-bit image;
//            a load sets the world size first (world_set_size), then restores the coordinates with their
//            high halves. A file with neither MAPS nor WRLD (or both) is refused; a reader older than round
//            11 refuses a WRLD file (MAPS missing). A MAPS file loads as a 256-cell world (g_world_hi cleared).
//     "GLOB" the simulation globals outside GameState / Config (g_rng16, AI seed, ...; savegame.cpp)
//     "CAMP" the campaign record (the 142-byte front-end save record of savegame_capture) + 1 byte
//            g_fe_game_in_progress: player names, spells found, lobby counters
//     "RULE" (round 9, optional) u32 version 1, u32 possession_range_pct (version 2, round 10: + u32 mode): the gameplay rules
//            (settings.h GameplayRules) in force when saved; a load forces them for the rest of the level
//            (a state without the chunk continues with the current settings)
//     "MODE" (round 10, only while a game mode runs) the mode block: mode.h mode_serialise (u32 version +
//            ModeState); a state without it loads as the original game (mode_end)
//     "POOL" pool extension (only when thing_pool_slots() > 1000): u32 slots, Things 1000..slots-1,
//            the extension's free stack and recyclable stack (thing.h thing_pool_ext_*), i32 each
// Unknown chunks are skipped (forward compatibility); a missing required chunk refuses the file.
constexpr int      SAVESTATE_SLOTS = 10;
constexpr uint32_t SAVESTATE_FORMAT = 1;
constexpr uint32_t SAVESTATE_HEADER_SIZE = 96;
constexpr uint32_t MC_PORT_VERSION = 0x00040001;   // Phase 4, round 10 (major << 16 | minor)

struct SaveStateHeader {
    char     magic[8];          // "MCPSTATE"
    uint32_t format;            // SAVESTATE_FORMAT
    uint32_t header_size;       // SAVESTATE_HEADER_SIZE
    uint32_t port_version;      // MC_PORT_VERSION of the writer
    int32_t  level;             // 0-based campaign level being played (Config.level)
    uint32_t tick;              // PlayerRec.tick of the local player
    uint32_t thing_slots;       // thing_pool_slots() of the writer (1000 = original pool)
    uint64_t time;              // seconds since 1970 (UTC) when it was written
    uint32_t flags;             // bit 0: 640x480 game logic (g_video_mode_flags != 1)
    uint32_t chunk_bytes;       // bytes after the header
    uint32_t checksum;          // FNV-1a 32 of the bytes after the header
    char     name[52];          // free text, NUL terminated ("level 5  tick 1234")
};

// Directory: the save dir of savegame_set_dirs. Path of slot 0..9; false when saving is disabled.
bool savestate_slot_path(int slot, char *buf, size_t cap);
// Can a state be saved / loaded now? Not in a network game (Config.flags & 0x10), not while a movie
// plays or records (Config.flags & 6).
bool savestate_allowed();
// Write the current state. `name` null = "level N  tick T". Returns false (and leaves an existing file
// alone) on any error; savestate_error() says why.
bool savestate_save_file(const char *path, const char *name);
bool savestate_save(int slot, const char *name = nullptr);
// Read and check a file's header (magic, format, size, checksum). False when it is not a valid state.
bool savestate_read_header(const char *path, SaveStateHeader *out);
// Load a state into the running engine. The level must already be loaded (game_level_begin /
// sim_load_level of header.level): the textures / palette / sprite banks come from it. Refused (false,
// nothing changed) when the file is invalid, when it was written with more Thing slots than
// MC_THING_SLOTS_MAX allows, or when !savestate_allowed(). An extended-pool save is loaded with its own
// pool size (thing_pool_force_slots for the rest of the level), a 1000-slot save into any pool.
bool savestate_load_file(const char *path);
bool savestate_load(int slot);
const char *savestate_error();          // the reason of the last failure ("" after a success)
// The full quick save: installs g_hook_demo_quick_save / g_hook_demo_quick_load (demo.h) with slot 0,
// used by commands 10 / 0xb while PortSettings::quicksave_full is set.
void savestate_install_quick_hooks();
// After loading, the cell lists are checked (demo_repair_cell_lists, demo.h) and rebuilt if a cycle or a
// stale link is found (a state saved from a broken original-style quick load); savestate_error() then
// reports how many lists were repaired.
