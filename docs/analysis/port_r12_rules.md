# Port round 12, task B: match rules v1 and teams

Agent B, 2026-10-09, branch `round12`, build dir `build_B`. Everything new runs only in a mode run (`mode_active()`);
the original's code paths are unchanged except for `same_side()`, whose faithful path is exactly `a == b`.
**ctest: 73/73 passed** (69 + `mode_rules_test` + the other agents' new tests at the time of the run), every
per-tick / pixel reference identical, zero warnings.

## What was done

### Files

| file | change |
|---|---|
| `mode_rules.cpp` | replaced the stub: sides, rules tick (respawn, elimination, desecration, win, level end), stats hooks, queries |
| `mode_rules.h` | added: `mode_rules_sync`, hooks `g_hook_mode_wizard_died / _creature_died / _castle_gone`, `MatchRulesResolved` + `mode_rules_resolve`, `MODE_MATCH_END_DELAY_TICKS` (125), `mode_rules_end_match`, `mode_rules_debug_end`, `mode_side_castle_mana`, `mode_world_mana`; includes `mc_types.h` (Thing is an alias, no forward declaration possible) |
| `mode.cpp` | the hook pointers, the side function (`g_hook_mode_side_of_owner`) and `mode_rules_sync()` (recomputes `g_mode_teams` from g_mode); called by `mode_begin`, `mode_end`, `mode_deserialise` - a MODE chunk loaded into a fresh process gets its teams back (`g_mode_teams` is derived, not stored) |
| `player.cpp` | respawn gate in state 3 (bots) and command 0xf (humans), death hook, `game_check_level_won` off in a mode run, one `same_side` |
| `castle.cpp` | castle-gone hook in `castle_collapse_level` (stats) |
| `spatial.cpp`, `creature_common.cpp`, `creatures.cpp`, `creatures3.cpp`, `effects.cpp`, `projectiles.cpp`, `thing.cpp` | `same_side` at the sites of the table below; creature death hook |
| `debug_cmd.{h,cpp}` | `win` / `lose` in a mode run end the match (`MATCH_END_DEBUG`) |
| `tests/mode_rules_test.{cpp,cmake}` | new (MC_SIM_ALL, ~30 s) |

### Sides

`mode_rules_level_start`: records `p < player_count` get `MODE_PLAYER_IN_MATCH` (+ `_BOT` for `is_computer`); side
numbers in record order of first appearance: team 0 = a new side, the same non-zero team id = the same side.
`g_mode_teams` is true only when two records really share a side (so a "teams" setup with distinct ids costs nothing).
The side of an owner: the record whose `PlayerRec.thing == owner` (checked: `thing_create` starts every Thing with
its own index as owner; spells, projectiles, effects, castle, balloons, guards copy the wizard's; the wizard Thing is
reused on respawn). Exceptions noted below (skeleton army, mana balls).

### Respawn

The death hook (state 2 -> 3 in `player_dying_update`) sets `respawn_ticks = (respawn_s + respawn_level_s * castle
level) * 25`, `deaths++`, the killer's `kills_wizards++` (not for allies / self). `mode_rules_tick` counts it down.
A bot respawns (original `player_spawn` at the castle) in the tick it reaches 0; a human needs command 0xf (Space)
afterwards; 0xf before the timer / without a castle / eliminated does nothing (no "level lost", no "leaves the game").
Measured: human level-2 castle, 3 s + 2 s/level: dead 175 ticks; bot level 1: respawned exactly 125 ticks after death.

### Elimination

Lying dead (state 3) without a castle, or `PlayerRec.active == 0` (quit / left) -> `MODE_PLAYER_ELIMINATED`,
`eliminated_tick`. The Thing stays dead (spectator, `active` stays 1), mana stays where it is. Alive without a
castle = not eliminated.

### Desecration

Each tick, a living wizard inside the footprint box of an enemy castle (castle Thing extents + wizard extents on the
torus, any height - `castle_crush_wizards`' test without its +1 level / +1 cell margin) channels; the current target is
kept while he stays in it, else the first enemy castle in record order. Leaving / dying resets. At `desecrate_s` the
castle is razed once it stands (state 4: a castle in its build sequencer has a build effect in flight that writes into
it; the channel waits at 100 %): `castle_collapse_level` per level (a tenth spilled + footprint collapse; deleted at
0), then `castle_spill_mana` + `castle_manage_balloons_and_guards` as `castle_destroyed_update` does. `killer` = the
channeller -> `castles_razed++` through the castle-gone hook. Owner dead -> eliminated, alive -> castle-less.
Measured: 2 s in, out -> reset to 0; 4 s -> razed after exactly 100 ticks, 10000 castle mana -> 10000 in balls owned
by the owner. 0xff = off (30 s inside: nothing).

### Win conditions and the level end

`mode_rules_resolve`: win 0 = ELIMINATION | MANA (+ TIMER when `time_limit_min != 0`); TIMER needs minutes > 0.
- elimination: one side with a record not eliminated left (only if the match started with >= 2 sides);
- mana: a side's castle + transit mana (not eliminated records) * 100 >= world * mana_pct for hold ticks in a row
  (`mana_hold_ticks` on every record of the side). World = `Config.total_mana` minus the local player's `P.mana` term
  `mana_totals_update` starts it with (so the same on every peer);
- timer: at `time_limit_min * 1500` ticks the ranking decides: side alive, castle mana, kills, later elimination,
  lowest record.
Then `MATCH_OVER`, `match_end_tick`, `winner_side` (0xff = nobody), `end_reason`, places (winner 1, others by the
ranking). 125 ticks later every record `< player_count` gets its status in the same tick: winners **10** (won and
left, cmd 0x1b's value), everybody else **8** (left, cmd 0x1d's) - not 0xc, whose lost bit makes `game_level_tick`
restart the level. `game_level_tick` ends on `status & 8` on every peer in the same tick. `MATCH_WIN_NONE`: never.
The original's `game_check_level_won` is off in a mode run.

### Stats

`kills_wizards` (death hook), `kills_creatures` (`creature_die` hook; killer = a match wizard, not the victim's own
side; all creature types), `deaths`, `castles_razed` (castle destroyed to level 0 by damage or desecration, killer
an enemy wizard), `mana_peak` (castle + transit). Frozen once the match is over. Queries: `mode_respawn_seconds`,
`mode_desecration_pct` (highest channel on the record's castle), `mode_time_left_seconds` (-1 without a limit; frozen
at the end).

## Owner-comparison sites

| site | what | decision |
|---|---|---|
| spatial.cpp `area_cells_slot0` (:80) | area damage slot 0 victims | **same_side** (allies unhurt) |
| spatial.cpp `thing_find_collision` (:98) | what a projectile hits | **same_side** (shots pass through allies, their creatures and castles) |
| spatial.cpp `thing_find_collision_other_owner` (:105) | only caller: mana ball merge (balls own themselves) | kept |
| spatial.cpp `thing_find_mana_near` (:114, :115) | Possession homing on balls / hoards / villages | **same_side** both (no claim of an ally's mana; mana stays per wizard) |
| spatial.cpp `thing_area_damage` castles (:151), other slots (:166) | castle damage, slots 1-5 | **same_side** |
| spatial.cpp fire (:183), quake (:197) castles | castle damage | **same_side** (quake busy timer still set: terrain shake) |
| spatial.cpp `thing_try_damage` (:218) | direct hit | **same_side** |
| spatial.cpp `cell_kill_things` (:227) | castle build kills creatures under the footprint | **same_side** (allied units survive) |
| creature_common.cpp follow separation (:359) | flocking | **same_side** (allied units flock like own) |
| creature_common.cpp `creature_die` (:397) | kill credit "creature owns itself" | kept (not a comparison of two owners); + stats hook |
| creatures.cpp skeleton attack (:353) | target of the own side -> back off | **same_side** |
| creatures.cpp skeleton castle search (:437), player-list search (:458) | targeting | **same_side** |
| creatures3.cpp guard retaliation (:255), search (:265), separation (:506) | targeting / flocking | **same_side** |
| effects.cpp mana ball claim (:720) | slot-1 claim changes the ball's owner | **same_side** (an ally's ball stays his) |
| effects.cpp skeleton army cap (:677) | 64 per caster | kept (per wizard) |
| effects.cpp hoard claim (:796, :800), `mana_ball_merge` (:834..) | mana ownership | kept (per wizard) |
| effects.cpp blast (:1184) | smart bomb victims | **same_side** |
| projectiles.cpp `projectile_pick_target` (:663, :675, :694, :706, :764) | homing targets | **same_side** |
| projectiles.cpp possession targets (:725, :734) | balls / villages | **same_side** |
| projectiles.cpp hit stats (:175) | accuracy stat | kept |
| castle.cpp :49, :341, :414 | balloons fetch / castle swallows own balls | kept (mana per wizard) |
| castle.cpp :80 | castle-spell upgrade request by the owner | kept |
| player.cpp spawn threat (:418) | other wizards fear the newcomer | **same_side** (allies no threat) |
| player.cpp cheat 3/4/5 (:979), dying ball re-home (:835) | cheat / ownership | kept |
| thing.cpp `thing_find_in_sight_of_class` (:848) | no caller in the port | **same_side** |
| constructors.cpp, spells.cpp | only assignments (`p->owner = c->owner`) | nothing to change |

Not owner tests: `creature_ai_step`'s player-list search (wild creatures attack everyone; no owner test in the
original) - unchanged.

AI sites seen (task C's, `ai_wizard.cpp`): 111, 118 (`player_find_nearest_by_type`), 134, 148 (nearest wizard excl.),
496-507 (threat from projectiles; C already added a same_side skip), 589-607 (mana ball hunt), 725/734 (castle
attack), 754/763 (wizard), 786/795 (balloon), 810/819, 837/846 (creatures) - C has same_side at most of them.

## Tests (`mode_rules_test`, 256-cell generated world, ~30 s)

1. sides: FFA records 0..3 sides 0..3, `same_side == (a == b)`; 2 v 2 sides 0 0 1 1; wizards / castles / wild
   creature; teams restored by `mode_deserialise`; no mode: no teams, no hook.
2. allies: effect owned by the ally: `thing_area_damage` / `thing_try_damage` leave the wizard untouched, the enemy's
   hit; a shot's `thing_find_collision` never returns the ally; parked 4 s in an ally's castle: no channel.
3. respawn: human 175 ticks + 0xf (refused before), at the castle (0,0 off); bot exactly 125 ticks.
4. elimination: bot without castle and human after its castle was destroyed: eliminated, stay dead, 0xf ignored,
   status 0, `MATCH_WIN_NONE` keeps running.
5. desecration: reset on leaving, razed after 100 ticks, castles_razed 1, owner castle-less then eliminated on death;
   0xff = off.
6. wins (each checks status 10 / 8 for every record on tick end + 125 and the places): elimination (tick 43), mana
   2 v 1 (5 %, 2 s: both team records hold 50 ticks; castle 40000 of world 261944), timer 1 min (ends tick 1500,
   winner = most castle mana 24336), debug win @2, debug lose (best other side), NONE.
7. determinism: the same scripted 2 v 1 v 1 run (teams, desecration, respawns) twice: 1500 identical checksums.

## Code needed in files I do not own

1. `level_features.cpp:589` (village claim by Possession, same rule as the mana ball): add `#include "mode_rules.h"`
   and replace
   `if (claimer != t->mana_owner) {` by `if (!same_side(claimer, t->mana_owner)) {   // round 12: an ally's village stays his`.
2. `input.cpp` Shift+R (`case 0x13`): in a mode run it would set status 0xc and restart the mode level; replace
   `if (!net_or_custom()) {` by `if (!net_or_custom() && !mode_active()) {` (Shift+K / Shift+L may stay: they kill
   the own wizard / castle through the game's paths and the rules handle them).
3. `scenario.cpp:865` (`assert_status`): in a mode run "lost" is status 8:
   `ok = a.status == 0 ? bits == 0 : a.status == 2 ? (st & 2) != 0 : ((st & 6) == 4 || (mode_active() && st == 8));`
   (a scenario `win` / `lose` in an rts run now ends the match; the status follows 125 ticks later).
4. `game.cpp game_level_end` (nobody's file; for D's flow): a won mode level sets status 2, runs
   `player_compute_level_stats` and moves `g_cfg->level` to `world_campaign_next(0x100)` / `level + 1`. If the flow
   reads `g_cfg->level` afterwards, guard with `if (mode_active()) {} else if (g_cfg->flags & 0x10) ...`.
5. HUD (A): ally colouring in the map (`hud.cpp:205, 210` compare owners) could use `same_side`; render-only.

## Open items

- **Skeleton army** (`effect_skeleton_army_update`) sets only `mana_owner`; the skeletons own themselves, so they
  walk to the nearest castle of "another owner" - including their summoner's and allies' (the original's behaviour,
  kept). A fix would set `owner` too (gameplay change, round 14's units).
- Bots that die before they cast their first castle are eliminated by the rule (round 11 saw one in three die to
  monsters near the seat). C / round 17 may want a grace period or a safer seat.
- Performance with teams: `same_side` of two different owners walks the 8 records twice; in projectile target picks
  over thousands of creatures this is ~16 compares per creature. Not measured at 1024 with teams; a per-tick cache of
  the 8 wizard indices would make it O(1) if needed.
- A human that leaves a network mode run (`active = 0`) is eliminated; the network leave paths themselves (status 8 on
  that peer) are D's.
