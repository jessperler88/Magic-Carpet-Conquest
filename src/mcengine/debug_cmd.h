// Port-only debug commands (Phase 4, round 10 task B; docs/analysis/port_console.md). Nothing here exists in
// carpet.exe.
//
// Every state-changing debug action (teleport, spawn, give mana, god, kill, claim, heal, all spells, win / lose,
// damage) travels as a command packet (CmdPacket.cmd 0x40..0x5f) in the local player's command slot ->
// player_commands_process -> g_hook_port_command (debug_cmd_apply), so it is recorded in movies, replayed by
// tests and valid in lockstep. The console (mcport/console.*), scenario files (scenario.h) and the other tools
// only *build* packets with the builders below and queue them (debug_cmd_queue); this file applies them inside
// the tick.
//
// ---- packet layout ------------------------------------------------------------------------------------------
// The packet is the original's 10 bytes; b[0] = CmdPacket.cmd, b[1] = arg, b[2] = pad2, b[3] = steer_x,
// b[4] = steer_y, b[5] = bits, b[6..9] = pad6. Multi-byte values are little endian. "target" = player record
// 0..7, 0xff = the player who sent the packet. After a debug packet was applied, its b[3..5] are cleared, so
// player_commands_process applies "no steering, no keys" for that tick (the same in the record run, the
// playback and on every peer). Unused bytes are 0.
//
//   id    name       b1        b2                 b3..b9
//   0x40  teleport   target    flags: 1 = z given  b3-4 x, b5-6 y (world units, low 16 bits), b7-8 z (world
//                                                  units, signed), b9 = bits 16..19 of x (low nibble) and of
//                                                  y (high nibble)
//                                                  - without z: ground height + 0x100 (the spawn height)
//   0x41  spawn      class     type (model)       b3 x cell, b4 y cell (low 8 bits), b5 count (0 = 1, max 64),
//                                                  b6 flags (1 = at the cell b3/b4, else b8 cells in front of
//                                                  the target's wizard), b7 target, b8 distance ahead (0 = 4),
//                                                  b9 = bits 8..11 of the x cell (low nibble) / y cell (high)
//                                                  - level_spawn_thing_record at the cell centre (as a level
//                                                  file record with dis_id 0); several Things in a square
// Round 11 (wide worlds, world.h): a world has at most MC_WORLD_MAX = 4096 cells = 2^20 units per side, so a
// position needs 20 bits per axis: x and y (40 bits) + z (16) are exactly the 7 data bytes b3..b9. The extra
// nibbles live in b9, which was 0 in every packet before (a 256-cell world: still 0, the packet is unchanged);
// the coordinates are wrapped onto the world when applied (WCoord / world_wrap_cell).
//   0x42  give mana  target    0 wizard, 1 castle, b3-6 amount (int32). wizard: Thing.mana += n (at most
//                              2 mana ball       mana_total); castle: castle Thing.mana += n (at most its
//                                                  mana_total); ball: cheat 2 - a mana ball of n owned by the
//                                                  wizard at its position (the wizard's mana is also refilled)
//   0x43  god        target    0 off, 1 on,       -
//                              2 toggle
//   0x44  kill       target    mode: 0 slot,      b3-4 slot (mode 0), b5 class / b6 type the Thing must have
//                              1 class/type,      (0xff = any; mode 0 checks them, mode 1 selects with them)
//                              2 wizard of target, - creatures and wizards / castles / balloons: health = -1
//                              3 castle of target    (the game's own death), anything else: marked for
//                                                    deletion. Mode 1 never kills the sender's own wizard.
//   0x45  claim      target    -                  b3-4 slot, b5 class / b6 type it must have (0xff = any)
//                                                  - a mana ball: mana_owner = the target's wizard, anything
//                                                  else: owner = the target's wizard
//   0x46  heal       target    1 wizard, 2 castle  - health = max_health (0 = wizard)
//                              (bits)
//   0x47  spells     target    -                  - cheat 1's spell part: one spell Thing for every spell id
//                                                  the wizard lacks (mana cost 0; switches are not touched)
//   0x48  level end  target    0 win, 1 lose      b3 1 = only mark (status 2 / 4, the level goes on);
//                                                  else win = status 10 (won and left, command 0x1b),
//                                                  lose = status 0xc (lost: the campaign restarts the level)
//   0x49  damage     target    0 wizard, 1 castle b3-6 amount (int32): pending damage in slot 0 (the game's
//                                                  damage path: shields, invulnerability and god apply)
//   0x4a..0x5f       reserved
//
// Things are named by slot + the class / type they must still have (b5 / b6): a stale slot (the Thing died
// and the slot was reused) is refused. thing_slot_generation (thing.h) cannot be used inside the tick: it is a
// process-local render counter that no savestate / movie / peer shares (a playback would refuse what the
// record run applied), see the report.
//
// ---- god mode -------------------------------------------------------------------------------------------------
// Where the flag lives: in a mode run g_mode.players[p].flags & MODE_PLAYER_GOD (mode.h); in the campaign bit 0
// of the player block byte P+0x3ae (PlayerBlock.pad3ae[0], between slot_left and slot_right; zero in every one
// of the original's 51924 per-tick reference dumps). It is GameState, so savestates, movie snapshots and
// net_state_checksum (the P block) carry it; players_init_records clears it with the record at the next level
// start). debug_cmd_tick() (once per tick, after the mode tick) keeps a god wizard's
// health and mana full and its invulnerability timer >= 2 (pending damage is discarded, the HUD blinks).
#pragma once
#include "mc_types.h"
#include <string>
#include <vector>

enum : uint8_t {
    DEBUG_CMD_FIRST = 0x40,
    DEBUG_CMD_TELEPORT = 0x40,
    DEBUG_CMD_SPAWN = 0x41,
    DEBUG_CMD_GIVE_MANA = 0x42,
    DEBUG_CMD_GOD = 0x43,
    DEBUG_CMD_KILL = 0x44,
    DEBUG_CMD_CLAIM = 0x45,
    DEBUG_CMD_HEAL = 0x46,
    DEBUG_CMD_SPELLS = 0x47,
    DEBUG_CMD_LEVEL_END = 0x48,
    DEBUG_CMD_DAMAGE = 0x49,
    DEBUG_CMD_LAST = 0x5f,
};

constexpr uint8_t DEBUG_TARGET_SELF = 0xff;          // b1 "target": the sender
constexpr uint8_t DEBUG_ANY = 0xff;                  // class / type "any"
constexpr int     DEBUG_Z_GROUND = -0x8000;          // teleport without z: ground + 0x100

enum : uint8_t { DEBUG_MANA_WIZARD = 0, DEBUG_MANA_CASTLE = 1, DEBUG_MANA_BALL = 2 };
enum : uint8_t { DEBUG_GOD_OFF = 0, DEBUG_GOD_ON = 1, DEBUG_GOD_TOGGLE = 2 };
enum : uint8_t { DEBUG_KILL_SLOT = 0, DEBUG_KILL_CLASS = 1, DEBUG_KILL_WIZARD = 2, DEBUG_KILL_CASTLE = 3 };
enum : uint8_t { DEBUG_HEAL_WIZARD = 1, DEBUG_HEAL_CASTLE = 2 };
enum : uint8_t { DEBUG_END_WIN = 0, DEBUG_END_LOSE = 1 };
constexpr uint8_t DEBUG_PLAYER_GOD = 1;              // campaign: bit of PlayerBlock.pad3ae[0] (debug_player_flags)

// ---- builders (pure: no state read or written) ------------------------------------------------------------------
CmdPacket debug_cmd_teleport(int x, int y, int z = DEBUG_Z_GROUND, int target = DEBUG_TARGET_SELF);   // world units
// At cell (cx, cy) when at_cell, else `ahead` cells in front of the target's wizard (0 = 4).
CmdPacket debug_cmd_spawn(int cls, int type, int cx, int cy, bool at_cell, int count = 1, int target = DEBUG_TARGET_SELF,
                          int ahead = 0);
CmdPacket debug_cmd_give_mana(int amount, int where = DEBUG_MANA_WIZARD, int target = DEBUG_TARGET_SELF);
CmdPacket debug_cmd_god(int mode, int target = DEBUG_TARGET_SELF);
CmdPacket debug_cmd_kill_slot(int slot, int cls = DEBUG_ANY, int type = DEBUG_ANY);
CmdPacket debug_cmd_kill_class(int cls, int type = DEBUG_ANY);
CmdPacket debug_cmd_kill_wizard(int target);
CmdPacket debug_cmd_kill_castle(int target);
CmdPacket debug_cmd_claim(int slot, int cls = DEBUG_ANY, int type = DEBUG_ANY, int target = DEBUG_TARGET_SELF);
CmdPacket debug_cmd_heal(int what = DEBUG_HEAL_WIZARD, int target = DEBUG_TARGET_SELF);
CmdPacket debug_cmd_spells(int target = DEBUG_TARGET_SELF);
CmdPacket debug_cmd_level_end(int how, bool mark_only = false, int target = DEBUG_TARGET_SELF);
CmdPacket debug_cmd_damage(int amount, int what = 0, int target = DEBUG_TARGET_SELF);

const char *debug_cmd_name(uint8_t id);              // "teleport", ... (nullptr outside 0x40..0x49)
std::string debug_cmd_describe(const CmdPacket &p);  // one line for logs: "teleport p0 -> 12928,8320 (ground)"

// ---- applying (inside the tick) -------------------------------------------------------------------------------
// Are debug packets applied now? A mode run: only with MODE_PARAM_DEBUG (the run's ModeParams, agreed by every
// peer). Otherwise: any game that is not a network game (Config.flags & 0x10) - single player levels, the
// viewer, movies (a recording's debug packets replay). Never in a network game outside a debug mode run.
bool debug_cmd_allowed();

// g_hook_port_command (player.h): applies a debug packet of player p; other ids >= 0x20 are left alone.
void debug_cmd_apply(int player, Thing *t, CmdPacket *cmd);

// Once per tick (requested hook g_hook_debug_tick in game_tick_sim, after the mode tick): god mode. Reads only
// simulated state; idempotent (a second call in the same tick changes nothing).
void debug_cmd_tick();

// Installs debug_cmd_apply as g_hook_port_command (and debug_cmd_tick as g_hook_debug_tick once that exists;
// sim_register_gameplay calls this).
void debug_cmd_register();

// God flag of player p (mode run: g_mode, campaign: PlayerBlock.pad3ae[0] bit 0).
bool debug_god(int player);
inline uint8_t &debug_player_flags(PlayerRec &rec) { return rec.blk.pad3ae[0]; }

// ---- the host side: queue of packets waiting for the local player's command slot ------------------------------
// Not simulated state (the packets are not in the command stream yet). One packet per tick: the slot takes a
// packet only when it is empty (between ticks it is, except for the join packet of a level start), so packets
// queued together run in consecutive ticks, in order.
// Queue a packet (debug or any other command). False (nothing queued, *why set) when debug commands are not
// allowed now (network game, mode run without MODE_PARAM_DEBUG) or a movie plays.
bool debug_cmd_queue(const CmdPacket &p, std::string *why = nullptr);
// Right before each simulation tick (mcport sim_tick, scenario hosts): moves the next queued packet into the
// local player's empty command slot. Nothing while a movie plays (its packets replace the slot).
void debug_cmd_pump();
int  debug_cmd_pending();                    // packets still queued
void debug_cmd_clear_queue();
// Messages of applied packets ("spawned 3 Creature Dragon at 40,52"), oldest first; the call empties the list.
// Host-side output only (written by debug_cmd_apply, never read by the simulation).
std::vector<std::string> debug_cmd_take_messages();
