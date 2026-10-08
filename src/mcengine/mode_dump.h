// JSON state dumps for headless runs and tests (Phase 4, round 10 task A; docs/analysis/port_mode.md).
// Port-only, read-only: nothing here writes game state, so a dump may be taken between any two ticks of any
// game (campaign, movie, mode run) without changing the simulation. No SDL, no dependencies.
//
// Document (one object; integers unless noted; "checksum" values are "0x%08x" strings):
//   format "mcport-dump", version 1, tick (PlayerRec.tick of the local player), ticks_run (the host's count),
//   level (Config.level), mode ("original" / "conquest"), mode_id, seed (ModeParams.seed, 0 without a mode),
//   checksum (net_state_checksum), world {size (cells per side), units (= size * 256), cells} (round 11),
//   checksum_parts (null until task E's NetChecksumParts exist),
//   rules {possession_range_pct, mode},
//   mode_block {version, mode, tick, rng, debug_flags, template_level, ai_seed,
//               params {seed, world_size, bots, humans, flags, map}, player_flags [8]},
//   pool {slots, live, free_top, active_top, alloc_failures}, world_mana (Config.total_mana),
//   players [player_count records] {index, active, is_computer, local, name, status, thing, alive, state,
//            health, max_health, mana (Thing.mana_total), wizard_mana (P.mana), x, y (int32 world units), z,
//            ai_mode, kills,
//            kills_of_player [8], shots, hits, spells (book slots in use), mode_flags,
//            castle null | {thing, level, health, max_health, mana}},
//   census {total, by_class {"<name>": n}, creatures {"<name>": n}, player_things {"<name>": n},
//           effects {"<type>": n}},
//   then the caller's extra members (e.g. task D's tick profile), verbatim.
#pragma once
#include <string>

// Replaces *out with the document. `ticks_run` is the host's tick counter (-1 = omit);
// `extra_members` (optional) is inserted verbatim before the closing brace, e.g. "\"profile\": {...}".
void mode_dump_json(std::string *out, long ticks_run = -1, const char *extra_members = nullptr);

// The one-line summary mcport prints per dump:
//   dump tick=1200 checksum=0x1234abcd mode=conquest seed=1 things=812 players=4 p0=h:400/400,m:1200,c:2 ...
void mode_dump_line(std::string *out, long ticks_run = -1);

// Writes the document to `path` (false on an I/O error).
bool mode_dump_write(const char *path, long ticks_run = -1, const char *extra_members = nullptr);
