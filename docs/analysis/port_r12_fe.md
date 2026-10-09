# Port round 12, task A: the Conquest front end - urn, setup screen, lobby urn, result screen, match HUD

Files: `frontend.{h,cpp}`, `fe_mode.{h,cpp}`, `hud.{h,cpp}`, `worldgen.{h,cpp}` (an added preview function only), tests
`fe_mode_test.{cpp,cmake}` and `hud_mode_test.{cpp,cmake}` (new), `frontend_test.cmake` / `render_reference_fe_test.cmake`
(link line: `+ mcengine/fe_mode.cpp`). Nothing else touched. Release build in `build_A`: 0 warnings.

## What was built

### Wiring: frontend.cpp never calls fe_mode.cpp
The screens live in `fe_mode.cpp`, but six test link lines that I do not own (`config_test`, `desync_test`, `net_test`,
`replay_check_test`, `rts_headless_test`, `world_persist_test`) list `frontend.cpp` without `fe_mode.cpp`. So
`frontend.cpp` only knows the screens through a registration (`frontend.h`: `FeModeScreens {frame, params,
open_setup}`, `fe_set_mode_screens`). `fe_mode.cpp` registers itself from a static initialiser and from the explicit
`fe_mode_register()`. A program without `fe_mode.cpp` builds as before and never shows the urn. `fe_mode.cpp` draws
with a small toolkit that `frontend.cpp` exports (`fe_tk_*` in `frontend.h`): the original's back and background
buffers, palette and bright table, fades as blockers, the busy flag, sfont0/1/2, the input edge flags, the pointer
sprite, the state change and "leave with FE_START_MODE". The screens are 320x200 like the rest of the front end and
use the same frame-stepped phase pattern as the translated screens.

### 1. Main-menu urn
- The sprite is tmaps chunk 116 (SpriteDesc 0x4d's base sprite, 32x26), loaded once with `mc_tmap_set_load` /
  `mc_tmap_get`. It is remapped from `data/palette.dat` to the screen palette with `palette_find_nearest` (normal,
  30 % brighter = the menu's highlight rule, 55 % = dim). The tables are rebuilt whenever the palette changes.
- It is drawn at (282, 164), on the grey book bottom right, inside `fe_main_menu_draw_overlay`, so it is part of the
  background that `fe_highlight` copies. It is drawn only in the menu-button mode (`s_slot_mode == 0`).
- Item id 12 (`fe_menu_items[12]` is the empty record and no `mmmask.dat` pixel has value 12). `fe_menu_item_at`
  tests the urn rectangle before the mask lookup. `fe_menu_item_enabled(12)` = shown and slot mode 0. Tab and
  Shift+Tab wrap at 12 instead of 0xb only while the urn is shown. On hover or selection the urn is redrawn bright
  after `fe_highlight`. A click or Enter calls `open_setup(2)`, sets state 11 and runs the menu's normal exit (fade,
  free).
- "Shown" = `g_settings.mode_menu && screens registered && urn loaded`. With `mode_menu` off, `s_item` can never be
  12, the Tab bound is 0xb and no pixel changes. `render_reference_fe_test`: **33/33 dumps pixel-identical** with
  `fe_mode.cpp` linked.

### 2. Setup screen (state 11)
- **Background:** the lobby's book (`pmulti.dat`). Its right page (the level list's arrows) is painted over with the
  blank right page of `gconfig.dat` (the same book render), remapped to `pmulti.pal`. The book does not use palette
  entries 246..254 (nor does sptrs entry 4, the lobby's hand pointer, which this screen shows), so they become the
  minimap's water and the 8 wizard colours (from `g_hud_player_colours` through the game palette).
- **Title:** "Conquest" in sfont1 gold over the table, with the urn beside it.
- **Left page (match):** 12 rows of sfont0, label in the lobby's ink, value right-aligned in dark red, 8-pixel pitch:
  - SEED: click = a new random seed; keys 0-9 type, Backspace deletes; 0 shows "RANDOM", the minimap shows "?",
    and mcport's flow draws a seed at Start.
  - WORLD: 256 (`MODE_PARAM_GENERATE`), 512, 1024, 2048.
  - BOTS: 0..8-humans.
  - SKILL: EASY / NORMAL / HARD / BRUTAL = `bot_skill` 1/2/3/4.
  - TEAMS: FREE FOR ALL / teams of 2 / teams of 4. The label is the team sizes, e.g. "2 V 2" or "2 V 1 V 1".
  - A seat row: 8 cells in the wizard colours with the team digit. A click cycles that seat's team 0..4, which
    makes the setup custom. Human seats are framed white.
  - WIN BY: ELIM + MANA / ELIMINATION / MANA / NONE. NONE with a time limit shows "TIME ONLY".
  - MANA % | HOLD s, TIME LIMIT (sets `MATCH_WIN_TIMER` with the minutes), RESPAWN s | + LEVEL s, DESECRATE s / OFF,
    SYMMETRY (auto, none, point, mirror, rot4, dihedral).
- **Right page (world):** a 64x64 minimap in a frame, a 2-column block WATER / ROUGH / RELIEF (continents) / WILD |
  SPRINGS / LAIRS / TRIGGERS / PORTALS, and the caption "4 SEATS, ROT4".
- **Values:** stored as explicit `WorldParams` / `MatchRules` bytes once changed. 0xff / nibble 15 = NONE where the
  struct defines it. Defaults are shown as their effective values.
- **Controls:** left click = next value, right click = previous. Keys: Up / Down / Tab move the selection, Left /
  Right step, R = new seed, Enter = Start, Esc = Back. The lobby's ✓ = Start, its X and the hand = Back. A hovered or
  selected row gets a darker band. A hint line on a darkened strip at the bottom explains the hovered control.
  Disabled rows (SKILL with 0 bots, MANA/HOLD without mana dominance) are greyed.
- **Minimap:** `worldgen_preview` (below) gives the heightmap, which is downsampled to 64x64:
  - water when half or more of a pixel's cells are water;
  - a dark coast when a third of the pixel is water, so 1-cell rivers do not crack the land;
  - land in a 16-step ramp from the book's own colours (olive, tan, brown, grey, white peaks);
  - seats as 3x3 dots in the wizard colour with a dark rim, empty seats of the symmetry group as a dark dot.
  It updates when seed, size, flags, players, water, roughness, continent or symmetry change, 6 quiet frames after
  the last change (18 at 2048). The old image is shown darkened meanwhile.
- **Start:** from the menu, fade out, then `fe_frame` returns `FE_START_MODE` with `fe_mode_params()`. The state
  stays 11, so the setup reappears if mcport comes back without a result. From the lobby, OK selects Conquest for
  the session and returns to state 4. Back fades to the main menu (or the lobby). The setup is remembered.
- **First visit:** if `fe_mode_set_params` was never called, a random seed is rolled (front-end clock, not
  simulated). Defaults: 1024, 3 bots, 1 human.

### 3. Lobby urn (state 4, only with mode_menu)
- The urn stands on the map bottom left: dim = the original game, lit = Conquest, bright = hover.
- Clicking it when off opens the setup (`open_setup(4)`: humans := `g_fe_lobby_players`). The lobby is left
  through its own exit with `s_mp_to_setup` (the player records are not restored). The setup's OK calls
  `fe_lobby_set_conquest(true)` and returns to 4.
- Clicking it when on deselects it (`mode_set_proposal(0, {})`).
- When selected, the right page shows "Conquest / WORLD 512, BOTS 2 / SEED 777 / CLICK TO SET UP" instead of the
  level list. A click there reopens the setup.
- `mode_set_proposal(GAME_MODE_CONQUEST, setup params)` is called on every selection change and again right before
  `net_session_join_begin`, so the proposal holds the latest setup when `mp_join_done` runs `net_agree_rules`.
- With `mode_menu` off, nothing in the lobby is drawn, tested or proposed.

### 4. Result screen (state 12)
- **Picture and headline:** the level result screen's picture (`pperf.dat`). The headline is in its gold sfont2:
  "Victory" (the local record's side = `winner_side`), "Defeat", or "Match left" when the match did not end by a
  rule (forfeit / `MATCH_END_NONE`).
- **Summary lines:** the end reason, "MATCH TIME h:mm:ss - SEED n - WORLD n", and "WINNERS: names".
- **Table:** on a darkened panel, one row per seat (`MODE_PLAYER_IN_MATCH` records, else humans + bots):
  - name (the player name copied when `fe_mode_set_result` is called, or "BOT n");
  - a side swatch in the wizard's colour (pperf entries 74..81, unused by the picture) and the side number;
  - "OUT" for eliminated seats;
  - wizard kills, creature kills, deaths, castles razed, peak mana, placing (1 in green).
  The local row is gold and marked ">".
- **Reveal and exit:** lines appear one by one with the result screen's tick (sample 3). The first click shows
  everything, the next continues (fade). Then the main menu, or the lobby when `Config.flags & 0x10` was set at
  `fe_mode_set_result` (`fe_mode_result_return_to` overrides). No pointer, as the original's result screen.

### 5. Match HUD (hud.cpp `hud_draw_mode`)
- **Where:** called from `frame_pass` after the input-mode switch for modes 0 and 3 (flight, also while the wizard
  is dead, where the original skips its HUD), only when `s_drawing && mode_active()`. It is draw only and reads
  `g_mode` and the local wizard's Thing.
- **Contents:**
  - the match timer (top right under the hand panels, blinking red in the last minute);
  - "RISING AGAIN AT YOUR CASTLE IN n" from `respawn_ticks`, or "PRESS SPACE TO RISE AGAIN" when dead with 0;
  - "ELIMINATED - SPECTATING";
  - "YOUR CASTLE IS BEING DESECRATED n%" (the highest `desecrate_ticks` of the records with `desecrate_rec1` = you)
    and "DESECRATING n%", each with a bar;
  - the VICTORY / DEFEAT banner and the end reason while the match is over.
- **Look:** font 1 on `ui_shade_rect(0x34)` panels. The banner, and in 640 modes the death lines, are drawn at 2x
  by blitting a font-1 scratch rendering. Font 0 is not usable for text: its glyphs are not in ASCII order, so
  "Victory" rendered as "Vjdupsz".
- **Data source:** the numbers come straight from the `ModeState` / `ModePlayer` fields as `mode.h` defines them
  (the same values `mode_rules.h`'s queries report), so the HUD was testable before task B landed.

### worldgen_preview (worldgen.h)
- **Signature:** `bool worldgen_preview(const ModeParams &, const LevelData &tmpl, WorldPreview *out)`. It reads only
  `tmpl.gen.off`. The setup screen gets the template through `mode_build_level`.
- **Steps:** it runs `worldgen_build`'s steps up to the rivers with the same constants (`gnarl` 16, `river` 20,
  `sourc` 40, `raise` 0, the same seeds): fractal, symmetrise, normalise, seats, plateaus, rivers.
- **Isolation:** it runs on a private N x N scratch world. `g_world`, `g_world_unit_mask` and the five map
  pointers are switched to heap vectors and restored. No RNG, Thing or GameState byte is touched.
- **Output:** heights (0 = sea and rivers) and the seats (= `WFEAT_SPAWN`).
- **Cost:** a 2048 world with 8 players takes **519 ms** (Release, measured in `fe_mode_test`). 1024 has a quarter
  of the cells and was not timed separately.

## Tests (build_A, Release)
- **`fe_mode_test` (new, OK).**
  - Preview: level A loaded twice gives `fa5eb58d` / `a0b2317d`. The first load in the process differs from the
    second (carried-over globals, not the preview). A preview of world B (1024, 4 players, water 45) with A loaded
    leaves the hash of maps, `g_cell_things`, GameState, `g_world`, `g_rng16`, `g_mode` and the unit mask equal.
    So does a 2048 / 8-player preview. A loaded after the previews hashes equal to the second load (byte-identical).
    The preview's 4 seats equal the real build's `WFEAT_SPAWN` cells, and its water / land agrees with the built
    world on 99.99 % of the cells.
  - Main menu: `mode_menu` on changes 462 pixels, all inside the urn rectangle (0 outside in the non-animated
    half). Hover lights 487 urn pixels. Shift+Tab reaches the urn (401 lit), Tab leaves it (0), and
    Shift+Tab + Enter opens the setup.
  - Setup: a click rolls a new seed; typing gives 12345; the world size steps 1024 → 2048 → 1024 → 512; bots step
    3 → 4 → 2. The preview regenerates (4 seats), and seed 12345 vs 777 differ in 3496 of 4096 minimap pixels.
    Start returns `FE_START_MODE` with seed 777, 512, 2 bots, 1 human, a generated world. Back in the front end
    the setup reappears and Esc goes to the main menu with the setup remembered.
  - Lobby: urn → setup → OK gives `fe_lobby_conquest()`, and `mode_proposal` = CONQUEST with seed 777, 512 and
    humans = the lobby's player count. The urn again gives no proposal, and the hand goes back to the menu.
  - Result: a synthetic victory (2 v 2, local wins) and a defeat (local = record 2, revealed by the first click)
    are drawn, and continuing returns to the main menu. With `mode_menu` off the menu is the original's again.
- **`hud_mode_test` (new, OK).**
  - Real 512 mode level, 640x480 and 320x200.
  - `hud_draw_mode` with no mode draws nothing. With a mode and no events the frame equals the frame without a mode.
  - Timer (264 / 180 px), respawn, desecration both ways, eliminated, victory and defeat each change the frame.
  - Every draw leaves GameState and `g_mode` byte-identical.
- **`frontend_test` OK, `render_reference_fe_test` 33/33 pixel-identical** (both now link `fe_mode.cpp`).
- **`mode_flow_test` (task D's) passes against these screens:** urn → setup → Enter → a 1024 match ended by the
  timer → result screen → main menu, plus the forfeit.
- **Whole `ctest -C Release -j 2`: 72/74.**
  - `mode_bots_test`: task C's, mid-edit (`s.respawns <= s.deaths`).
  - `world_persist_test` (and in an earlier run `replay_check_test`): fail only in the parallel run and pass alone
    (shared temp / timing). Neither involves these files.

## Screenshots (looked at)
`%TEMP%\mc_fe_mode_test\` (`C:\Users\jessp\AppData\Local\Temp\mc_fe_mode_test\`), 640x400 pixel-doubled:
- `fe_mode_menu_faithful.png`, `fe_mode_menu_urn.png`, `fe_mode_menu_urn_hover.png`
- `fe_mode_setup_first.png`, `fe_mode_setup_random.png`, `fe_mode_setup_seed12345.png`, `fe_mode_setup_seed777.png`
  (two seeds, different islands), `fe_mode_setup_hover_water.png` (hint line)
- `fe_mode_lobby_urn.png`, `fe_mode_setup_from_lobby.png`, `fe_mode_lobby_conquest.png`
- `fe_mode_result_victory.png`, `fe_mode_result_defeat.png`

`%TEMP%\mc_hud_mode_test\hud_mode_{timer,respawn,desecrate,eliminated,victory,defeat}_{640,320}.png`.

Also `build_A\Release\mode_flow_result_forfeit.ppm` (task D's test with these screens: "Match left").

Verdict as a player: everything is readable. The setup screen is dense but ordered (match on the left, world on the
right) and in the lobby's book style. The minimap reads as a world map. The result table is clear on the darkened
picture.

## Code needed in files I do not own
None is required. Recommended for task D / the integrator (`src/mcport/main.cpp`):
1. After `fe_init(...)` succeeds (main.cpp, around line 926), make the registration explicit. It is idempotent, and
   the static initialiser already runs because main.cpp references `fe_mode_params`:
   ```cpp
   fe_mode_register();     // round 12: the Conquest screens (fe_mode.h)
   ```
2. A network match's result screen returns to the lobby when `Config.flags & 0x10` is still set when
   `fe_mode_set_result` is called. If the flow clears the network bit first, call
   `fe_mode_result_return_to(4)` after `fe_mode_set_result(...)`.

Facts for task D:
- The setup screen's seed is 0 only when the player typed 0 (shown as "RANDOM"). `mode_flow_menu_params`'s fresh
  seed for 0 matches that.
- The lobby's proposal carries `humans` = the lobby's player count at the moment the setup was opened. D's
  `humans := session count` overrides it, as specified.

## Decisions
- **320x200, the original's resources, no 640 path:** mcport's front-end frame buffer is 320x200 and the
  setup fits with sfont0 (4 px advance, 8-pixel rows).
- **Palette entries borrowed** (246..254 of `pmulti.pal`, 74..81 of `pperf.pal`) for the minimap's blue and the
  wizard colours. Neither picture uses them, so nothing else changes.
- **The minimap's land uses the book's colours** (an old-map look) and only the sea gets a new blue.
- **The HUD computes its numbers from the contract fields** rather than calling `mode_respawn_seconds` and the
  other queries: same definitions, testable without task B, and no dependency on its stub values.

## Open items
- The minimap preview blocks the front end for ~0.5 s at 2048 (debounced). A worker thread would hide it.
- The setup screen has no "copy seed to clipboard" (optional in the briefing). It would be one line in mcport
  (`SDL_SetClipboardText(std::to_string(fe_mode_params().seed).c_str())` on a key) and was not added to `main.cpp`
  (not mine).
- In the lobby every peer sees only its own urn state until the join; the host's choice wins (`net_agree_rules`).
  Showing the host's choice before Start would need a pre-join exchange (round 19, online).
- Not checked by hand in `build/Release/mcport.exe`: the `game` path starts with the language / config / logo
  screens, so the integrator's hand check of the urn → match → result gate is still to do. The flow itself is covered
  headless by `mode_flow_test` and `fe_mode_test`.
