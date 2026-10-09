# Controls

How to play the port: the original game's controls, the port's additions, the controller and the debug suite.
Keys that the port added never clash with the original's; with `--faithful` (or `keys.wasd = 0`,
`keys.book_tab = 0`, `keys.menu = none` in `mcport.ini`) the keyboard behaves exactly like the 1994 game.

Running the game: `mcport.exe "<your game folder>"` opens the front end (new game, load, options, multiplayer).
`mcport.exe "<game folder>" play 5` goes straight into level 5 (levels are numbered from 0).

## Flying

| Input | Action |
|---|---|
| **Mouse** | Steer: the further the pointer is from the screen centre, the harder you turn and climb / dive |
| **W** / **↑** | Fly faster |
| **S** / **↓** | Slow down, fly backwards |
| **A** / **←**, **D** / **→** | Slide left / right |
| **Left mouse button** | Cast the spell in your left hand |
| **Right mouse button** | Cast the spell in your right hand |
| **1 … 0** | Quick-select spell 1-10 into the left hand |
| **Ctrl+1 … Ctrl+0** | Quick-select spell 1-10 into the right hand |
| **Space** | Respawn at your castle after dying; leave a level you have won |
| **Esc** | Pause menu (resume, save / load, options, leave level, quit) |
| **P** | Pause (the original's pause: freezes everything, no menu) |

W / A / S / D are a port addition (the arrows always work). Mana is claimed with the *Possession* spell; the
level is won when you hold the required share of the world's mana.

## Spell book and map

| Input | Action |
|---|---|
| **Tab** or **Enter** | Open / close the spell book (it also shows the map) |
| **Mouse wheel** (map showing) | Zoom the map: in to 25 %, out to the whole world of a big Conquest world (the original scale is the most in the campaign) |
| **Left click** a spell | Put it in your left hand |
| **Right click** a spell | Put it in your right hand |
| **1 … 0** over a spell | Assign it to quick-select key 1-10 |

Tab is a port addition (`keys.book_tab`); Enter is the original key.

## Display and sound (original keys)

| Key | Action |
|---|---|
| **F1** / **F2** | Sound effects / music on / off |
| **F3** | Game speed: normal / fast / very fast |
| **F4** | Soften (2x2 smoothing) |
| **F5** | Reflections |
| **F6** | Textured sky |
| **F7** | Shadows |
| **F8** | Icons and map overlay |
| **F9** | Speed blur: off / light / heavy |
| **F10** | 3D glasses mode: off / red-blue / 2 |
| **[** / **]** | Bigger / smaller view window |
| **R** | Switch 320x200 / 640x480 (the original's resolution; the port's native-resolution view is a setting) |
| **I** | Type a chat message (network games); Enter sends, Backspace deletes |

## Port keys

| Key | Action |
|---|---|
| **Alt+Enter** | Fullscreen on / off |
| **Ctrl+F1 … Ctrl+F9** | Save the game state into slot 1-9 (any moment, in a level) |
| **Shift+F1 … Shift+F9** | Load slot 1-9 |
| **Ctrl+F10** / **Shift+F10** | Save / load the quick slot 0 (Alt+S, the original's quick save, uses it too) |
| **Alt+R** | Record a movie (the original's recorder) |
| **F11** | Frame-time display; press again for the per-tick profile, again to hide |
| **Ctrl+F11** | Screenshot (PNG in `<save folder>/screenshots`) |
| **F12** | Quit the program at once |

The save folder is `%APPDATA%\Bullfrog\MagicCarpet` (saves, `mcport.ini`, `mcport.log`, screenshots).
`mcport.exe "<game folder>" load 3` starts straight from save slot 3. The save / load keys, F12 and the pause
menu key (Esc) can be rebound in the `[keys]` section of `mcport.ini`.

## Shift keys (original)

| Key | Action |
|---|---|
| **Shift+Q** | Quit to the menu |
| **Shift+R** | Restart the level (not in a Conquest match) |
| **Shift+K** | Kill your own wizard |
| **Shift+L** | Destroy your own castle |

## Conquest (the new mode)

The red **urn** bottom right of the main menu opens the Conquest setup (`[game] mode_menu = 0` in `mcport.ini` or
`--faithful` hides it). On the setup screen **left click** steps a value forward, **right click** back; **arrows /
Tab** move between rows, **Enter** starts the match, **Esc** goes back; the line at the bottom explains the row under
the pointer, the map on the right page previews the world. In the multiplayer lobby the same urn makes the session a
Conquest match with the host's setup.

In a match: when you die, the HUD counts down until you can rise again at your castle (**Space** then respawns you).
Without a castle a dead wizard is out of the match and watches. Hovering inside an enemy castle's courtyard desecrates
it (a bar shows the progress, yours and theirs). The match ends by elimination, mana dominance (75 % of the world's
mana for 30 s) or the time limit; then the result screen shows every wizard's kills, deaths, castles razed and peak
mana. **Esc → Forfeit the match** leaves early.

From the command line: `mcport.exe "<game folder>" rts --size 1024 --bots 3` plus any of `--seed S`, `--teams
2v2|1,1,2,2|ffa`, `--win elim,mana,timer|none`, `--time MIN`, `--mana-pct P`, `--hold S`, `--respawn S`,
`--respawn-level S`, `--desecrate S|off`, `--bot-skill 1..4` (easy .. brutal), `--result` (show the result screen at
the end instead of quitting) and the world generator options in the README.

## Controller (Xbox / PlayStation / Switch pads)

Plug it in at any time. In flight:

| Input | Action |
|---|---|
| **Right stick** | Steer (like the mouse) |
| **Left stick** | Faster / slower (up / down), slide (left / right) |
| **LT** / **RT** | Cast left / right hand |
| **LB** / **RB** | Previous / next spell in the left hand |
| **X** / **Y** | Previous / next spell in the right hand |
| **A** | Spell book and map |
| **B** | Respawn / leave a won level |
| **Start** | Pause |
| **Back** (View / Select) | Pause menu |
| **D-pad up / down** | Bigger / smaller view window |

In the spell book, the map and menus: the left stick or d-pad moves the pointer, **A** = left click, **X** = right
click, **B** = back / close. Bindings, dead zones and sensitivity are in the `[pad]` section of `mcport.ini`.

## Free-fly viewer

`mcport.exe "<game folder>" 38` flies a camera over level 38 without playing: **W/S/A/D** move, **Q/E** turn,
**R/F** up / down, **↑/↓** look up / down, **Z/X** roll, **+/−** zoom, hold **Shift** to go faster (**Ctrl** x4 on top, for the big Conquest worlds), hold the
**right mouse button** to look around, **L / K** next / previous level, **Home** back to the start.
`mcport.exe "<game folder>" demo` plays the game's recorded movie (**Tab** free camera, **Space** pause,
**[ / ]** slower / faster).

## Debug suite

Tools for testing and exploring. Everything that changes the game goes through the game's own command
packets, so it is recorded in movies and save states like normal input; none of it works in network games.

### Time and cameras

| Key | Action |
|---|---|
| **Pause** | Pause / resume |
| **End** / **Shift+End** | Advance one / ten ticks |
| **Page Up** / **Page Down** | Faster (x2 … x64, then as fast as possible) / slower (down to x1/16) |
| **Insert** | Normal speed |
| **Delete** | Free camera on / off (the free-fly viewer's keys above; your wizard keeps flying straight) |
| **Shift+Delete** | Teleport your wizard to the free camera |

### Inspector and overlays

| Input | Action |
|---|---|
| **Home** or **middle mouse button** | Cursor mode on / off: a pointer appears, steering pauses |
| Cursor mode: **left click** | Inspect the creature / object under the pointer (click on nothing closes the panel) |
| Cursor mode: **right click** | Inspect the ground cell under the pointer |
| **Backspace** | Close the inspector panel |
| **Shift+Backspace** or **Keypad /** | Hide / show the inspector panel; the inspected Thing stays selected and the follow camera keeps orbiting it |
| **Keypad 8** or **Ctrl+Home** | Follow the inspected thing with the camera: move the mouse to orbit around it, wheel to zoom (in cursor mode: right-drag orbits) |
| **Keypad 7** / **Keypad 9** | Inspect the previous / next thing |
| **Keypad 1 … 6** | Overlays: grid, labels, anchors, occupancy, damage, network sync |
| **Keypad 0** | All overlays off |

### Console

**`** (the key left of 1) opens and closes it; **Esc** closes it. Up / Down = history, Tab = complete,
Page Up / Down = scroll, Ctrl+L = clear. `help` lists every command, `help <command>` explains one.
Add `@2` to a game command to apply it to player 2.

| Command | What it does |
|---|---|
| `god` / `god on` / `god off` | Health and mana stay full |
| `give mana 5000` · `give spells` · `spells` | Mana; every spell |
| `heal` · `damage 500` | Heal; take damage |
| `teleport 120 80` | Move your wizard to map cell 120, 80 |
| `spawn creature 3 x5 ahead 4` | Create things (class, type, count, distance ahead or `X Y`) |
| `kill creatures` · `kill wizard 2` · `kill castle 1` · `kill #480` | Remove things (`#480` = thing number 480) |
| `claim #480` | Make a thing yours |
| `win` · `lose` | End the level |
| `players` · `where me` · `find creature` · `count creature 3` | Look things up |
| `time pause` · `time step 10` · `time speed 4` · `time normal` | Time control |
| `cam free` · `cam follow #480` · `cam to 120 80` · `cam off` | Cameras |
| `inspect #480` · `overlay grid on` · `overlay list` | Inspector and overlays |
| `save 3` · `load 3` · `shot name` · `dump` | Save state, screenshot, JSON dump of the game |
| `cheat 1` … `cheat 7` | The original's cheats (spells, mana, destroy wizards / castles / balloons, heal, kill creatures) |

### Scripts and headless runs

- `mcport.exe "<game folder>" --scenario file.scn` runs a script of console commands and checks
  (`assert_health me >= 100`, `assert_count creature 3 == 0`, ...) and exits with 1 if a check fails; examples in
  `src/tests/scenarios/`.
- `--console-stdin` reads console commands from a terminal or a pipe.
- `MC_TIME_SPEED=max` runs as fast as possible; `--faithful` plays exactly like the original.
