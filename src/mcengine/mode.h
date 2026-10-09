// Port-only game modes (Phase 4, round 10 scaffolding; docs/ROADMAP_PHASE4.md, docs/port/BRIEFING_round10.md).
// Nothing here exists in carpet.exe.
//
// The original has exactly two "modes": the campaign and a network game. Phase 4 adds new ones (first:
// Conquest, the RTS / RPG hybrid). A mode is selected by the run (`mcport <dir> rts ...`, later the setup
// screen / lobby), never by the config file, and is carried by GameplayRules.mode (settings.h), which movies,
// network sessions and save states force exactly like the other gameplay rules.
//
// Simulated state of a mode lives OUTSIDE GameState (a fixed 0x38d03-byte image): in g_mode below. It is
// - hashed by net_state_checksum (only while a mode is active, so the original's checksum is unchanged),
// - saved in the "MODE" savestate chunk (savegame.h),
// - carried by movie format v3 and the gax snapshot (demo.h, round 10 task A).
// With mode 0 (GAME_MODE_ORIGINAL) the block is all zero and nothing reads it: every reference stays identical.
//
// Determinism: everything in g_mode is written only by the simulation (tick, packets, level start), integers
// only, never from the local player, the camera, settings or real time.
#pragma once
#include <cstddef>
#include <cstdint>

enum : uint32_t {
    GAME_MODE_ORIGINAL = 0,     // the campaign / network game of carpet.exe
    GAME_MODE_CONQUEST = 1,     // Phase 4: PvP RTS / RPG hybrid on a generated world
    GAME_MODE_COUNT
};

// Level index a mode level is loaded under (Config.level is u16; the campaign uses 0..69). sim_load_level
// asks g_hook_level_source (sim.h) for it instead of levels.dat.
constexpr int MODE_LEVEL_INDEX = 0x100;

#pragma pack(push, 1)
// The generator controls of a generated world (round 11 session 2, worldgen.h). 8 bytes in ModeParams (where
// round 10 had reserved zeros): 0 in a field = its default, so older movies / save states read as the defaults.
struct WorldParams {
    uint8_t symmetry = 0;       // WORLD_SYM_*; 0 = from the player count (worldgen.h)
    uint8_t roughness = 0;      // % of the template's gnarl (second noise amplitude), 1..255; 0 = 100
    uint8_t water = 0;          // % of the cells below sea level, 1..90; 0 = 30 (the original's "field < 0" drowns
                                // whole worlds once the continent levels shift the field)
    uint8_t continent = 0;      // amplitude of the levels above 128 cells, % of the 128-cell level; 0 = 50
    uint8_t springs = 0;        // mana springs per player; 0 = 3, 0xff = none
    uint8_t wild = 0;           // wild creature density in %, 1..250; 0 = 100, 0xff = none
    uint8_t lairs = 0;          // creature lairs per player (graded 1-5 by distance); 0 = 3, 0xff = none
    uint8_t features = 0;       // low nibble: trigger areas per player (0 = 3, 15 = none);
                                // high nibble: portal pairs per player (0 = 1, 15 = none)
};
static_assert(sizeof(WorldParams) == 8);

// The match rules v1 of a Conquest run (round 12, docs/port/BRIEFING_round12.md; implemented by mode_rules.cpp).
// 32 bytes in ModeParams: 0 in a field = its default, so a round-11 movie / save state reads as the defaults.
struct MatchRules {
    uint8_t team[8] = {};       // side of player record i: 0 = its own side (free for all), 1..8 = team id; records
                                // with the same non-zero id are allies (mode_rules.h same_side)
    uint8_t win = 0;            // MATCH_WIN_* bits; 0 = MATCH_WIN_ELIMINATION | MATCH_WIN_MANA
    uint8_t mana_pct = 0;       // mana dominance: a side's castles + transit hold this % of the world's mana; 0 = 75
    uint8_t mana_hold_s = 0;    // ... for this many seconds in a row; 0 = 30
    uint8_t time_limit_min = 0; // 0 = none; at the limit the side with the most castle mana wins (MATCH_WIN_TIMER)
    uint8_t respawn_s = 0;      // respawn delay at the castle, seconds; 0 = 10
    uint8_t respawn_level_s = 0;// + this many seconds per castle level; 0 = 5
    uint8_t desecrate_s = 0;    // desecration channel inside an enemy courtyard, seconds; 0 = 25, 0xff = off
    uint8_t bot_skill = 0;      // computer wizards: 0 = the template's (normal), 1 easy .. 4 hard (mode_bots.h)
    uint8_t reserved[16] = {};
};
static_assert(sizeof(MatchRules) == 32);
enum : uint8_t {
    MATCH_WIN_ELIMINATION = 1u << 0,    // the last side with a wizard not eliminated wins
    MATCH_WIN_MANA = 1u << 1,           // mana dominance (mana_pct for mana_hold_s)
    MATCH_WIN_TIMER = 1u << 2,          // time_limit_min: most castle mana at the limit
    MATCH_WIN_NONE = 1u << 7,           // no win condition (sandbox / tests): the match never ends by itself
};
// Simulation ticks per second of match time (the port's fixed 25 Hz tick; seconds in MatchRules are sim seconds).
constexpr int MODE_TICKS_PER_SECOND = 25;

// What a mode run was started with. Exchanged in the lobby / written into movies and save states as is.
struct ModeParams {
    uint32_t seed = 0;          // match seed: the level generator and ModeRng derive from it
    uint32_t world_size = 256;  // cells per side, a power of two 256..4096 (world.h)
    uint32_t bots = 0;          // computer wizards besides the human players
    uint32_t humans = 1;        // human players (1 offline)
    uint32_t flags = 0;         // MODE_PARAM_* bits
    uint32_t map = 0;           // template level (50..69, the multiplayer maps); 0 = chosen from the seed
    WorldParams world;          // generator controls (round 11 session 2)
    MatchRules match;           // match rules v1 (round 12)
};
static_assert(sizeof(ModeParams) == 64);
constexpr size_t MODE_PARAMS_V1_SIZE = 32;      // rounds 10-11: without MatchRules (movie v3, MODE chunk v1)

enum : uint32_t {
    MODE_PARAM_DEBUG = 1u << 0,         // debug commands (packets 0x40.., debug_cmd.h) accepted in this run
    MODE_PARAM_RESEED = 1u << 1,        // the template's terrain generator is re-seeded from the seed (else its own)
    MODE_PARAM_TILED = 1u << 2,         // world_size > 256: the template level tiled (world_widen_tiled, the round 11
                                        // session 1 test bed) instead of a generated world
    MODE_PARAM_GENERATE = 1u << 3,      // world_size 256: a generated world instead of the template level
};
// The Thing pool of a generated world is at least this big (thing.cpp thing_pool_wanted_slots): the pool's ceiling
// 32768 (i16 Thing indices) at every size (user, 2026-10-09; 16384 up to 1024 cells before).
inline int mode_generated_world_slots(uint32_t /*world_size*/) { return 32768; }   // = MC_THING_SLOTS_MAX
// A generated world (worldgen.h) instead of the template level.
inline bool mode_params_generated(const ModeParams &p) {
    if (p.world_size > 256) return !(p.flags & MODE_PARAM_TILED);
    return (p.flags & MODE_PARAM_GENERATE) != 0;
}

// Per player state of the mode (index = player record 0..7). Round 12: the match rules' state and the result
// screen's stats (written by mode_rules.cpp only; the front end / HUD read them).
struct ModePlayer {
    uint32_t flags = 0;         // MODE_PLAYER_* bits
    uint8_t  side = 0;          // resolved side 0..7 at the level start (allies share it; mode_rules.cpp)
    uint8_t  place = 0;         // final placing once the match is over (1 = winning side), 0 = not decided
    uint16_t pad = 0;
    int32_t  respawn_ticks = 0; // > 0: dead, ticks until the respawn is allowed / done; 0 otherwise
    int32_t  desecrate_rec1 = 0;    // 1 + the record whose castle this wizard is desecrating, 0 = none (the block
                                    // stays all zero by default)
    int32_t  desecrate_ticks = 0;   // channel progress of that castle, ticks
    uint32_t eliminated_tick = 0;   // g_mode.tick of the elimination (MODE_PLAYER_ELIMINATED)
    uint32_t kills_wizards = 0;     // stats: enemy wizards killed
    uint32_t kills_creatures = 0;   // stats: creatures killed
    uint32_t deaths = 0;            // stats: own deaths
    uint32_t castles_razed = 0;     // stats: enemy castles desecrated / destroyed
    int32_t  mana_peak = 0;         // stats: highest castle + transit mana
    uint32_t mana_hold_ticks = 0;   // mana dominance progress of this player's side (MATCH_WIN_MANA)
    int32_t  reserved[4] = {};
};
static_assert(sizeof(ModePlayer) == 64);

enum : uint32_t {
    MODE_PLAYER_GOD = 1u << 0,          // debug: health / mana kept full every tick (debug command "god")
    MODE_PLAYER_IN_MATCH = 1u << 1,     // the record plays this match (a human or a bot seat), set at the level start
    MODE_PLAYER_BOT = 1u << 2,          // a computer wizard
    MODE_PLAYER_ELIMINATED = 1u << 3,   // out of the match (dead without a castle / castle desecrated): spectates
    MODE_PLAYER_DESECRATING = 1u << 4,  // channelling inside an enemy courtyard (desecrate_rec1)
};

// The match (ModeState.match_state).
enum : uint32_t { MATCH_RUNNING = 0, MATCH_OVER = 1 };
enum : uint32_t {                       // ModeState.end_reason
    MATCH_END_NONE = 0, MATCH_END_ELIMINATION, MATCH_END_MANA, MATCH_END_TIMER, MATCH_END_DEBUG,
};

struct ModeState {
    uint32_t   version = 0;     // MODE_STATE_VERSION while a mode is active, 0 otherwise
    uint32_t   mode = 0;        // GAME_MODE_*
    ModeParams params;
    uint32_t   tick = 0;        // mode ticks run since the level start (mode_tick)
    uint32_t   rng = 0;         // ModeRng state (mode_rng_next); seeded from params.seed at the level start
    uint32_t   debug_flags = 0; // reserved for simulated debug switches (MODE_DEBUG_*)
    uint32_t   template_level = 0;  // the levels.dat entry the mode level was built from (mode_level.cpp)
    uint32_t   ai_seed = 0;     // what g_ai_rand_seed (ai_wizard.h) was seeded with at the level start
    uint32_t   match_state = 0; // MATCH_RUNNING / MATCH_OVER (round 12, mode_rules.cpp)
    uint32_t   match_end_tick = 0;  // g_mode.tick when the match ended
    uint32_t   winner_side = 0; // ModePlayer.side of the winners (valid when MATCH_OVER and end_reason != NONE)
    uint32_t   end_reason = 0;  // MATCH_END_*
    uint32_t   reserved[7] = {};
    ModePlayer players[8];
};
#pragma pack(pop)
static_assert(sizeof(ModeState) == 8 + sizeof(ModeParams) + 16 * 4 + 8 * sizeof(ModePlayer));
// 2 = round 12 (ModeParams + MatchRules, ModePlayer match fields). mode_deserialise also reads version 1 (32-byte
// ModeParams, match rules = defaults).
constexpr uint32_t MODE_STATE_VERSION = 2;

extern ModeState g_mode;

inline bool mode_active() { return g_mode.mode != GAME_MODE_ORIGINAL; }
const char *mode_name(uint32_t mode);           // "original", "conquest", "?"

// Start / stop a mode run. mode_begin clears g_mode, stores mode + params and forces GameplayRules.mode
// (the other rules from g_settings, clamped); mode_end clears g_mode and releases the forced rules.
void mode_begin(uint32_t mode, const ModeParams &params);
void mode_end();
// The level start of a mode level (sim_load_level_data calls it when a mode is active): per-level parts
// reset, ModeRng seeded from params.seed. Not called for the campaign.
void mode_level_start();
// One simulation tick of the mode: installed as g_hook_mode_tick (player.h) by mode_begin, removed by
// mode_end. Runs after thing_update_all, before the sound hook.
void mode_tick();
// Called at the end of mode_level_start (null = nothing): mode_level.cpp's level-start work that needs the
// gameplay subsystems (the AI's g_ai_rand_seed from the match seed, the human players' start spells).
// Installed by mode_level_register() (mode_level.h).
extern void (*g_hook_mode_level_start)();
// Called by mode_tick after g_mode.tick++ (null = nothing): the match rules (mode_rules.h mode_rules_tick), installed by
// mode_level_register().
extern void (*g_hook_mode_match_tick)();

// The mode's own LCG (never std::rand): deterministic from the match seed.
uint32_t mode_rng_next();

// FNV-1a 32 over the whole block (the "mode" checksum part, net.cpp), then g_hook_mode_checksum_globals:
// the simulation globals outside GameState that a mode run depends on and the original's checksum leaves
// out (mode_level.cpp: the AI's g_ai_rand_seed and g_ai_human_wizard, g_terrain_nearly_flat).
uint32_t mode_checksum(uint32_t h);
extern uint32_t (*g_hook_mode_checksum_globals)(uint32_t h);

// The block as the MODE savestate chunk / movie v3 snapshot carry it: u32 version, then the raw ModeState.
size_t mode_serialised_size();
void   mode_serialise(uint8_t *out);                    // mode_serialised_size() bytes
bool   mode_deserialise(const uint8_t *in, size_t n);   // false (g_mode unchanged) on a wrong size / version
bool   mode_serialised_size_ok(size_t n);               // n is the size of a version 2 or version 1 block
// ModeParams from `n` bytes (MODE_PARAMS_V1_SIZE: rounds 10-11 without MatchRules, or sizeof(ModeParams)).
bool   mode_params_read(const uint8_t *in, size_t n, ModeParams *out);

// The mode a front end screen chose for the next network session (round 12): the multiplayer lobby's urn sets it
// before the lobby's rules exchange; net_local_rules (net.cpp) proposes it while no mode is active. mode 0 = none.
void mode_set_proposal(uint32_t mode, const ModeParams &params);
uint32_t mode_proposal(ModeParams *params);     // the proposed mode (0 = none), params filled when non-null
