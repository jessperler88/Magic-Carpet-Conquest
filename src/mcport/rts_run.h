// `mcport <dir> rts [options]`: a game-mode run (Phase 4, round 10 task A; docs/analysis/port_mode.md), and the
// periodic JSON state dumps (mode_dump.h) of any run.
//
// Options after `rts` (all optional):
//   --seed S          match seed (default 1): template map, AI seed, mode RNG
//   --size N          world size in cells: a power of two 256..4096 (round 11 session 1: > 256 = the template
//                     map tiled N/256 times per axis, mode_level.h)
//   --bots B          computer wizards (default 3; humans + bots <= 8)
//   --map M           template level 50..69 (default: chosen from the seed)
//   --reseed          re-seed the template's terrain generator from the seed (default: the map's own terrain)
//   --debug / --no-debug   debug commands (packets 0x40.., debug_cmd.h) accepted (default on)
//   --net N           a network run of N humans (2..8) without the lobby (round 11 session 3): needs MC_NET=1 or
//                     MC_NET_HOST=<name server>; every instance joins the session "MCRTS" and plays the host's
//                     (player 0's) options; records N.. are the bots. MC_NET_SYNC=n compares the state every n ticks
//   --ticks N         quit after N simulation ticks (= MC_QUIT_AFTER_TICKS; main.cpp reads rts_quit_after_ticks)
//   --dump-every N    write dump_<tick>.json every N ticks (= MC_DUMP_EVERY)
//   --dump-dir D      where the dumps go (= MC_DUMP_DIR; default the current directory)
//   --dump-final      a dump when the run ends (= MC_DUMP_FINAL=1)
// Each dump also prints one line on stdout (mode_dump_line): `dump tick=1200 run=1200 checksum=0x... ...`.
#pragma once
#include "mode.h"
#include <string>

// Parses the options after `rts` (argv[first..argc-1]); unknown options are an error. Defaults: seed 1,
// size 256, bots 3, one human, debug commands on. Reads the MC_DUMP_* environment first (options override).
bool rts_parse_args(int argc, char **argv, int first, ModeParams *out, std::string *err);
long rts_quit_after_ticks();                // --ticks N (0 = not given)
int  rts_net_players();                     // --net N (0 = offline)

// Starts the run (mode_start_run + game_level_begin(MODE_LEVEL_INDEX)). False when the level could not load.
bool rts_begin(const char *game_dir, const ModeParams &params);

// After every simulation tick of the run (main.cpp sim_tick): the periodic dumps.
void rts_after_tick(long ticks_run);

// The level ended or the program quits: the final dump (--dump-final), mode_stop_run.
void rts_end();

// ---- dumps of any run (rts or not) ----------------------------------------------------------------------
// MC_DUMP_EVERY=n, MC_DUMP_DIR=dir, MC_DUMP_FINAL=1. rts_parse_args calls it; other runs call it at start-up.
void dump_configure_from_env();
void dump_configure(long every, const char *dir, bool final_dump);
bool dump_enabled();                        // every > 0 or final
// After a simulation tick: writes <dir>/dump_<ticks_run>.json when ticks_run % every == 0, prints the line.
void dump_after_tick(long ticks_run);
// The run ends: the final dump (dump_final.json) when configured.
void dump_final(long ticks_run);
// One dump now (console `dump`): <dir>/dump_<ticks_run>.json + the stdout line. Returns the file written.
std::string dump_now(long ticks_run);
// Extra members for every dump (e.g. task D's tick profile): `fn` returns JSON members without braces,
// e.g. "\"profile\": {...}" ("" = none). Null = none.
void dump_set_extra(std::string (*fn)());
