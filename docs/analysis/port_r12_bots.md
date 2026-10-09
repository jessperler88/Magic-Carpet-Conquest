# Round 12 task C: bots v1 (skill, allies, a generated world end to end, determinism, harness)

Files: `src/mcengine/mode_bots.{h,cpp}` (stub replaced), `src/mcengine/ai_wizard.{h,cpp}`, new
`src/tests/mode_bots_test.{cpp,cmake}`. `creatures2.cpp` needed no change (crab / kraken / troll / griffon compare no
owners). Everything new runs only in a mode run (`mode_active()`) or through `same_side()` of two **different**
owners (false without teams), so the campaign / network game / every per-tick reference is unchanged: full
`ctest -C Release -j 2` in `build_C` **74/74** (with A / B / D's work of the moment in the tree), zero warnings.

## 1. Skill (`mode_bots_level_start`, `mode_bots_apply_skill`)

The AI knobs come from the level's player block (+4 aggression, +8 reaction, +0xc accuracy, int16): `player_spawn`
copies them into `PlayerBlock.ai_*` when a computer wizard is created (first spawn only). The mode level's
`LevelData` is a fresh copy of the template on every level start (`mode_build_level` through `g_hook_level_source`;
`sim_load_level_data` is its only caller), and `mode_bots_level_start` runs after `players_init_records`, before the
join command spawns the wizards in the first tick - so the hook rewrites `g_state->level.player_block[p]` of every
`is_computer` seat; idempotent across restarts (checked), hashed / saved / recorded with GameState.

| MatchRules.bot_skill | aggression | accuracy, reaction |
|---|---|---|
| 0 / 2 normal | template's (= generated worlds before round 12) | template's |
| 1 easy | v / 2 | v - 96 (min 0) |
| 3 hard | v + (255 - v) / 2 | same |
| 4 brutal | 255 - (255 - v) / 4 | same |

Relative to the template so "normal" stays exactly today's. Template 54 (seed 11) seats 1..7:
(15 5 5) (65 56 59) (77 75 128) (128 128 128) x4 -> seat 1 easy (7 0 0), hard (135 130 130), brutal (195 193 193).
Reaction is also the original's AI respawn delay (`((255 - r) / 8) * 32 + 32`); task B's respawn timer governs mode
runs.

## 2. Allies (ai_wizard.cpp owner sites)

| site | change |
|---|---|
| `ai_goal_attack_castle` (13000) | `p->owner == t->owner` -> `same_side`: no ally castles |
| `ai_goal_attack_wizard` (13210) | same, before the grudge test: no ally wizards (a grudge cannot override) |
| `ai_goal_attack_type3` (13440) | same: no ally balloons |
| `ai_goal_hunt_creature` (13770) | same: no ally creatures (incl. castle guards) |
| `ai_goal_creature_near_rival` (13600, no caller) | same, for consistency |
| `ai_find_mana_ball_target` (13ce0) | a ball whose owner wizard is an ally (different owner, same side) is skipped |
| `ai_mode6_collect_mana` (12830) | the target ball was claimed by an ally on the way (its signature does not change): mode 0, choose again |
| `ai_record_threat_from_projectiles` (150f0) | a shot at an ally's wizard / castle / balloon / mana raises no threat / grudge |
| kept exact | `player_find_nearest_by_type / _wizard_excl / _castle_excl` (castle site clearance must see ally castles too; the mana-ball "foreign wizard near" test), `ai_find_incoming_projectile` |

## 3. A generated world end to end (mode runs only)

What a 30000-tick (20 min) headless 1024 match showed before the fixes (harness, 4 seeds): the bots roamed 300+
cells from home for mana (all toward the same rich area, stealing each other's balls), one balloon ferried the
mana back, castles stayed at level 1-2 with ~0 castle mana for 7000 ticks, and wild creatures (crab swarms) razed
the undefended castles; a bot that then died was eliminated (seed 2: two of three by tick 7091). Two changes:

- **Home-centred mana** (`ai_find_mana_ball_target`): with a castle, free balls are measured from the castle (the
  original measures them from the wizard; hostile balls it already measures from the castle; the creature hunt too).
- **Defend the castle** (new `ai_goal_defend_castle`, in `ai_choose_goal` between upgrade and attack castle, think
  ticks): a visible live enemy wizard within 24 cells of the own castle -> mode 8 (also answers task B's desecration
  channel), else the nearest live creature of another side within 24 cells -> mode 0xd.

Respawn is task B's (player.cpp): verified from here - a bot killed by a debug packet after its castle respawns at
it (512: tick 2000 -> 2500; 1024: 2000 -> 2402).

## 4. Determinism

The AI's only random source is `ai_rand` (`g_ai_rand_seed`, seeded from the match seed by mode_level.cpp, in the mode
checksum). No AI path reads the local player: `ai_cache_human_wizard` writes `g_ai_human_wizard` from the player
list (simulated, same on every peer; read by nothing), `ai_spawn_spells` iterates the type-0 flyers of the list
(every human, deterministic). New code: integers, list / index order, no settings or camera. The test runs the same
1024 match (hard, teams 1,2,1,2) twice: identical `net_state_checksum` every tick; `world_net_test` (D) covers
lockstep.

## 5. Tests: `mode_bots_test` (MC_SIM_ALL, ~45 s)

1. skill table (knob function, a 7-bot run per skill: easy <= normal <= hard <= brutal per seat, 0 = template,
   restart not applied twice);
2. idle god human + 3 bots on 512 (seed 3) and 1024 (seed 1), 6000 ticks: every bot builds a castle (ticks 4 / 8 /
   12), one grows to >= level 2, >= 2 hold one at the end, one is killed at tick 2000 and respawns, nobody still >
   3000 ticks; at tick 4000 the human is teleported into bot 1's castle -> bot 1 targets him within 200 ticks (18 /
   36 ticks); 1024 run: the god human desecrated bot 1's castle (B's rule) while the bot fought him;
3. 2 v 2 (512 seed 5, human + bot 1 vs bots 2 + 3, B's sides): 0 ticks with an ally targeted, 0 ally balls taken
   over (1 tick with a just-claimed ally ball as the target, dropped the next think);
4. determinism as above (1500 ticks); `MC_BOTS_TEST_SCALE=N` multiplies the tick counts.

## 6. Balance harness (`mode_bots_test <dir> --harness K [--seed S] [--size N] [--bots B] [--ticks T] [--skill k] [--teams 2v2] [--human] [--trace N]`)

Bot vs bot: the human seat is killed in the first tick (B: eliminated), rules = elimination + mana + timer (T ticks).
Prints per match the end reason / winner side / tick and per seat side, castle level, castle mana, mana total, kills
(wizards / creatures, B's stats), deaths, razed, place. Final numbers (20-minute matches, ~10 s each):

| set | ended by | castle level at the end (bots) | bots without a castle at the end | wins per seat |
|---|---|---|---|---|
| 1024, 3 bots, 10 seeds | timer 9, elimination 1 | mostly 4-6 | 3 / 30 | 1:2 2:4 3:4 |
| 512, 3 bots, 10 seeds | timer 9, elimination 1 | mostly 5-6 | 6 / 30 | 1:7 2:1 3:2 |
| 1024, 7 bots, 5 seeds | timer 5 | mostly 5-6 (one 7) | 11 / 35 | spread |
| 1024, 3 bots easy / brutal, 5 seeds each | timer | 4-6 / 3-6 | 1 / 2 of 15 | seat 3 4 / 3 |
| 1024, 2 v 2 with an idle god human, 5 seeds | timer | 2-5 | 0 / 15 | - |
| 2048, 3 bots, 3 seeds | timer | 3-5 | 0 / 9 | - |

Before the round 12 fixes (same harness, 1024, 4 seeds): castle levels 2-4 at the end, 2 of 12 bots eliminated by
tick 7100. Kills of wizards: **0 in every match** - bots never attack each other across a big world (sight radius
32 cells, `ai_goal_attack_castle`'s mana condition), so bot-only matches end by the timer; a human ends them by
desecration / elimination. Raw output: harness1/2.txt in the scratch dir (summarised above).

## 7. Exact code needed in files I do not own

None. (B's `mode_rules_sync` restores `g_mode_teams` after a load, which the AI's `same_side` needs.)

## 8. Open items (round 17 and the integrator)

- **Crab swarms at the seats** (worldgen, not bots): on some seeds ~12 crabs (5000 hp, regenerating) attack a
  level-1 castle by tick ~700; with only fireball (125 damage) a bot cannot hold it (1024 / 7 bots seed 2: 4 castles
  lost before tick 1600, the owners then eliminated). A human faces the same start. Suggest: no grade-1 crab lair
  within ~40 cells of a seat, or castle guards / a start spell beyond fireball (kept the density defaults wild 100 /
  lairs 3 as asked).
- Bots are passive toward other wizards (no attacks beyond 32 cells, `kills_wizards` 0) and never desecrate:
  round 17's strategic layer (targets beyond sight, desecration, using portals / urns).
- Seat fairness of "normal": the templates' knobs differ per seat (template 54: seat 1 = 15 / 5 / 5, seats 4-7 =
  128); the skill scales relative to them. Round 17 could use one profile for every seat.
- Mana dominance never fires in bot matches (75 % of the world's mana); the timer decides.
