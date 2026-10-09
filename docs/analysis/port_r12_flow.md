# Port round 12, task D: the Conquest flow, CLI, lobby path, network (2026-10-09)

Branch `round12`, build dir `build_D`. Briefing: `docs/port/BRIEFING_round12.md` task D.

## What was done

### mcport flow (main.cpp, new `mcengine/mode_flow.{h,cpp}`)
- **Setup screen Start** (`FE_START_MODE`): `mode_flow_menu_params(fe_mode_params(), fresh seed)` (seed 0 = random,
  taken from the performance counter and the clock; humans 1; bots <= 7; `MODE_PARAM_DEBUG` on as in the campaign;
  `mode_match_rules_finish`), `mode_flow_enter()` (campaign progress aside + `game_after_frontend(MODE_LEVEL_INDEX)`),
  `rts_begin(..., RTS_START_OFFLINE)`. `fe_mode_set_params(mp)` afterwards, so the setup screen shows the seed played.
- **Level end of a mode run**: `game_level_end()`, the final dump (`rts_dump_final`), `mode_flow_finish(forfeit)`:
  `fe_mode_set_result(mode_result_snapshot(local, forfeit), local)`, `mode_stop_run`, campaign restored,
  `game_before_frontend`, `fe_enter(FE_STATE_MODE_RESULT)`. The command line's `rts` still quits mcport at the level end
  unless `--result` (or `result` in the scenario's `rts` header); Shift+Q (GAME_QUIT) always quits.
- **Pause menu** in a mode run: the row reads "Forfeit the match" (`GameMenuEnv.mode_run`). Offline it leaves the level
  at once (`mode_flow_forfeit`: sound / music stopped, `game_level_end` without the end sequence, then
  `mode_flow_finish(true)`). Nothing is simulated, so nothing has to be recorded. In a network match it is the
  original's Esc (command 0x1d: this peer leaves, the others play on); the result screen follows when the local loop
  ends, marked as forfeited. `rts` without `--result`: forfeit quits.
- **Forfeit in the result**: `mode_result_snapshot` marks the local record `MODE_PLAYER_ELIMINATED` and shows a running
  match as `MATCH_OVER` with `end_reason = MATCH_END_NONE` (no winner). A's result screen already shows this as
  "Match left". No mode.h change was needed.
- **Campaign progress** (`mode_campaign_save / _restore`, mode_level.h): Config.level and the .gam's GameState bytes
  +0x3bd6 (spells, 0x18) and +0x2195 (options, 0xc) are kept for the length of a run. A mode level clears every
  record's `spell_found`, and its `game_level_end` moves Config.level (`world_campaign_next(0x100)` offline, `++`
  in a network game). Without this, a match from the menu would have wiped the campaign. In the lobby, the save holds
  the lobby's level choice, so the lobby shows it again afterwards.
- **Save / load**: a mode state loaded from the menu or a level, or with `load SLOT`, saves the campaign and ends on the
  result screen. A campaign state loaded during a match ends the run and releases the saved campaign; the loaded state
  then replaces Config. Save-slot rows of a mode state read "conquest tick N" (game_menu.cpp).

### Config (config.cpp)
`[game] mode_menu` with a k_desc line; playing default **on**, `--faithful` off (PortSettings default false). config_test
checks both and the round trip.

### CLI (rts_run.{h,cpp}, scenario.{h,cpp}, mode_level.{h,cpp})
One shared parser: `mode_match_option(key, value, MatchRules*)` / `mode_match_option_known` /
`mode_match_rules_finish` / `mode_match_rules_describe` in mode_level.cpp. Both `rts_parse_args` and the scenario
header use it.
- `--teams 1,1,2,2` (team id per record, 0..8) | `2v2`, `1v3`, `2v2v2v2` (team sizes in record order) | `ffa`
- `--win elim,mana,timer` (any of them, `+` also allowed) | `none`
- `--mana-pct 1..100`, `--hold 1..255`, `--time 1..255` (minutes), `--respawn 1..255`, `--respawn-level 1..255`,
  `--desecrate 1..254|off`, `--bot-skill 1..4`, `--result`
- **Decision**: a time limit adds `MATCH_WIN_TIMER` to the win bits (an empty field first becomes the default
  elim + mana), unless the win is `none`. `--win timer` without `--time` is an error. Respawn values of 0 cannot be
  given: 0 means the default in MatchRules.
- Scenario header: `rts seed S ... teams 2v2 time 1 desecrate off result` (the same keys without dashes).
- Usage line (main.cpp) and the rts_run.h comment are updated. rts_begin logs the rules
  (`rts: match rules: win timer, mana 75% for 30 s, time 1 min, ...`).

### Lobby path (net.{h,cpp}, mode_level.{h,cpp}, rts_run.cpp, main.cpp)
- `net_rules_applied(NetGameRules*)` (net.h): the rules `net_apply_rules` forces right now. mp_join_done (frontend.cpp)
  already exchanges the urn's proposal through `net_agree_rules(net_local_rules())`.
- `mode_start_net_run` is split into `mode_net_agree` (start, agree, stop) and `mode_start_agreed_net_run` (humans :=
  the session's player count, bots trimmed to 8 records via `mode_net_session_params`, `net_apply_rules`,
  Config.flags 0x10, `mode_start_run`). `mode_start_net_run` = both, so `rts --net N` is unchanged.
- mcport: `FE_START_LEVEL` while `mode_flow_lobby_agreed()` (network game + applied rules with a mode) ->
  `rts_begin(..., RTS_START_LOBBY)`. That takes the host's mode and ModeParams from `net_rules_applied` without
  exchanging a second time, and loads `MODE_LEVEL_INDEX` instead of the lobby's level. At the end every peer shows the
  result screen. A's result screen returns to state 4 (the lobby) when Config.flags 0x10 is set.
- `rts_begin` gained an `RtsStart` argument (CLI / OFFLINE / LOBBY), so a menu match after a `rts --net` start never
  joins a session.

## Tests (build_D, Release)
- **`mode_flow_test`** (new, 2.4 s). It runs mcport's own sequence (mode_flow.cpp + rts_run.cpp linked in):
  1. The main menu with mode_menu on vs. off, drawn twice off to prove the menu is reproducible. The urn is the
     difference: 462 pixels in 285,165..312,189.
  2. Clicking the urn opens state 11. Enter returns FE_START_MODE with the params set beforehand.
  3. The match: 1024 cells, 3 bots, idle human in god mode, `MATCH_WIN_TIMER` with 1 minute. The match ended at tick
     1500 and the level at tick 1625 (the end pause), by the timer. The result is in state 12, all 4 records placed,
     and the campaign (level, spells, options) is intact. Enter twice (the first shows every line) returns to state 2.
  4. The forfeit after 200 ticks: state 12, local eliminated, `MATCH_OVER` / `MATCH_END_NONE`, out of the level,
     campaign intact, back to state 2.
  5. Option parsing and errors for the CLI and the scenario header.
  The test has PENDING fallbacks (no urn -> fe_mode_set_params + FE_START_MODE directly; no time limit -> forfeit
  instead), but with A and B in the tree neither fired: **the gate passes in full**.
- **world_net_test**, new lobby part: two processes. The host proposes Conquest 1024 / 3 bots / 1-minute timer
  through `mode_set_proposal`; the client proposes nothing. Each peer runs mp_join_done's calls (level exchange,
  `net_agree_rules(net_local_rules())`, `net_apply_rules`), then `net_rules_applied` -> `mode_start_agreed_net_run` ->
  level -> ticks until status & 8. Result: 1625 per-tick checksum pairs, 0 differ, 0 online mismatches. **Both peers
  ended on tick 1624**, match state over, end reason 3 (timer), winning side 0, match end tick 1500. humans 2,
  bots 3. The existing 2000-tick part is unchanged (0 mismatches). Total 108 s in ctest; the watchdog was raised to
  840 s.
- **net_test** part 4: the host's rules carry a Conquest proposal. After the lobby's FE_START_LEVEL,
  `net_rules_applied` has it (seed / size / time), `gameplay_rules().mode` is Conquest, no mode runs yet, and the
  session params come out as 2 humans + 3 bots. `net_shutdown` releases it.
- **config_test**: mode_menu default on, `--faithful` off, the round trip.
- **mcport smoke** (dummy SDL, MC_TIME_SPEED=max): `rts --size 1024 --bots 3 --teams 2v2 --time 1 --win timer
  --result` logs the rules, then "match over after 1625 ticks (end reason 3, winning side 0)", then waits on the
  result screen / menu (killed by the timeout).
- **ctest -C Release -j 2**: 73 tests (69 + mode_flow_test + A's / B's / C's new tests), 72 passed on the first run.
  `replay_check_test` failed once and passed when rerun alone (a concurrent run by another agent); everything else
  passed. Zero warnings in build_D (scenario.cpp's shadowed `e` was renamed `merr`).
- Screens looked at (converted to PNG next to the PPMs): `build_D/Release/mode_flow_menu_urn.ppm` (the urn bottom
  right), `mode_flow_result.ppm` ("Victory"), `mode_flow_result_forfeit.ppm` ("Match left").

## Exact code needed in files I do not own
None is required: everything links and passes. Notes for the other tasks and the integrator:
- **A**: `mode_flow_result.ppm` from `fe_screen_pixels()` shows only the title line ("Victory" / "Match left") over
  the dimmed background. The per-seat table is not in the 320x200 image. If it is drawn on the 640x400 present path
  that is fine; otherwise the table is missing. mode_flow_test relies on Enter = Start in the setup screen, and on
  "any key twice" leaving the result screen (it is like that now).
- **B**: in the 1-minute timer test the idle human (record 0, god mode) "won" (winning side 0). With no castle mana
  anywhere, the tie-break (most kills, then lowest record) presumably picks record 0. That is correct by the rules, but
  worth a look in the balance runs.
- **Integrator**: `docs/CONTROLS.md` / README should mention `[game] mode_menu` and the new `rts` options (shared docs,
  not edited). Rebuild `build/Release/mcport.exe` (the user plays it).

## Files changed
`src/mcport/main.cpp`, `rts_run.{h,cpp}`, `config.cpp`, `game_menu.{h,cpp}`; `src/mcengine/mode_level.{h,cpp}`,
`net.{h,cpp}`, `scenario.{h,cpp}`, new `mode_flow.{h,cpp}` (picked up by the mcengine glob); tests
`mode_flow_test.{cpp,cmake}` (new), `world_net_test.cpp`, `net_test.{cpp,cmake}`, `config_test.{cpp,cmake}`,
`rts_headless_test.cmake` (fe_mode.cpp added to the link lines of the tests that link frontend.cpp; harmless, since
fe_mode registers its screens through a static registrar).

## Open
- A network forfeit uses the original's leave (0x1d). The leaver's result is local only; the remaining peers play on
  and see the leaver as the rules treat an inactive record (B).
- After a network match the lobby keeps the agreed rules applied until the next join (as before for the pool rules).
  `mode_flow_lobby_agreed` only looks at them on a lobby FE_START_LEVEL (Config.flags 0x10), which always follows a
  fresh `net_apply_rules`.
- By hand in `build/Release/mcport.exe`: urn -> setup -> Start -> match -> result -> menu has not been played
  interactively in this session (headless only).
