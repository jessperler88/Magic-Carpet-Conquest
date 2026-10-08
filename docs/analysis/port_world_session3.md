# Round 11 session 3: wake near any wizard, network mode runs, scaling decisions, the gate

Integrator's part of session 3 (agents: F `world_test` -> `port_world_test.md`, G `MC_REF_WORLD` + wide renderer ->
`port_world_refwide.md`). Everything below is behind `mode_active()` or `world_wide()`: the campaign, Hidden Worlds
and every reference set are unchanged (69/69 ctest).

## 1. B6: creatures wake near any wizard (`creature_common.cpp`, `thing.cpp`)

- `creature_proximity_wake_timer` in a mode run measures to the **nearest wizard of every player record in play**
  (`p < player_count`, a live class-3 Thing: humans and computer wizards alike); the nearest one's dy feeds
  `cast_ticks` as the original's local player did. Same 0x2400000 (24 cells) radius, same timers.
- The wake tick also runs in a **network** mode run (`thing_update_all`): the original skips it in a network game,
  which leaves every creature awake forever (`timer_a` stays at its 0xfa spawn value). In a 1024 world that would
  mean ~5000 live creatures thinking every tick and lairs active with nobody near; now both offline and online
  mode runs have one rule.
- Consequence seen in `rts_headless_test` (template 62, seed 1): one of three computer wizards is killed by the
  monsters near its start before it casts its castle and - the original's rule - never respawns without a castle.
  The test now asks for >= 2 castles. Bot survival = round 13 (respawn) / round 17 (bot competence).

## 2. Network mode runs (`net.h`, `mode_level.*`, `player.cpp`, `sim.cpp`, `rts_run.*`, `main.cpp`)

- `NetGameRules` grew to 48 bytes: `+ ModeParams mode_params` (g_mode.params while a mode is active). The lobby and
  `net_test` are unchanged (their rules carry the default params).
- `mode_start_net_run(dir, mode, params, &differs)`: humans := the session's player count, bots trimmed to 8
  records; this peer's run starts first (so its pool proposal is right), the peers agree on the **host's**
  NetGameRules, the run restarts with the host's params under the forced rules, `Config.flags |= 0x10`.
- `players_init_records`: in a network mode run the records `>= humans` are computer wizards (the original has none
  in a network game) and take their own player block, as offline. `level_start_from_state` sets `player_count` from
  the level (humans + bots) in a mode run also in a network game.
- `mcport <dir> rts --net N [options]` (MC_NET=1 / MC_NET_HOST): every instance joins session "MCRTS" of N
  humans (blocking, the window keeps pumping events) and plays the host's options. Round 12's lobby replaces it.
- **Two checksum fixes** (found by the 1024 runs; both are per-instance state of the original's own network game,
  not simulation): `PlayerBlock.combat_music` (+0x2e: every hit sets it, only the local player's flyer counts it
  down, only the music reads it) and Thing.flags bit 0 of **free** slots (a freed spell / flyer keeps its "local
  instance" bit) are left out of `net_state_checksum`, like the HUD flash counters. `desync_test`'s legacy function
  and `tools/reference/diff_state.py` follow.
- `world_net_test` (new, ~40 s): two processes over TCP (127.0.0.1), the host proposes seed 7 / 1024 / 3 bots, the
  other seed 99 / 512 / 1 bot and adopts the host's; 2000 ticks, 2000 checksums compared online and offline, 0
  differ; 5 records (0..1 human, 2..4 computer), 16384-slot pool, the bots fly; creatures within 20 cells of a bot
  and far from both humans are awake two ticks running (6 / 6), 651 creatures 40+ cells from every wizard sleep.
- **Gate run**: two headless `mcport rts --net 2 --seed 1 --size 1024 --bots 3` (the second asked for seed 5 / 512 /
  1 bot), `MC_NET_SYNC=1`: 4000 ticks, no DESYNC line, identical final dump checksum `0x9f5737dc`, the same
  players / castles / mana on both. (Before the free-slot fix: one desync at tick 1179, `things.free`.)

## 3. Scaling decisions

| range / search | decision |
|---|---|
| AI castle site (`ai_find_castle_site`) | **changed** for a mode run in a wide world: the wizard's own seat first (worldgen flattened a plateau there), then the original's 64-cell quadrants, 4 x 4 from the wizard's own on (a quarter of a 1024 world is 256 cells: first sites were 150-350 cells away). Bot runs: every first castle 1 cell from its seat at 1024 and 2048. |
| start spells of a generated world (`worldgen_build`) | **changed**: every seat spells 0, 3 and Castle (0x10), all 24 allowed (the multiplayer standard). Templates 59 / 54 / 60 / 61 / 68 start some or all seats without the Castle because their maps hold castle urns, which a generated world replaces: on template 59 (seed 2) no computer wizard ever built a castle. Also makes the symmetric world's start symmetric. |
| the original's separation quirk line (sign-extended subtraction: no test across x = 0x8000) | **changed** for N > 256 (torus difference): `creature_common.cpp` / `creatures3.cpp` separation, `pos_dist_manhattan`. At 256 the quirk stays (agent G found the line moves to the half-world in a wide world). `area_cells_slot0`'s one-cell centre offset in the upper half stays. |
| sound cull 48 cells, hearing 0x3000, tether 0x1400, fire / ridge 0x600 | kept: they are about what a player perceives, not the world's size |
| spiral search 15 rings (search.dat), castle footprints | kept: local searches |
| Teleport spell without a castle (64 cells), projectile target ranges, magnet / blast / balloon | kept: spell ranges are game balance (round 19's spell work may tune them) |
| teleport gate default destination (0x8000 = 128 cells) | kept: generated portals carry their destination; only template levels use the default |
| skeleton -> nearest enemy castle anywhere; townies / traders / builders -> nearest castle; AI mana-ball / creature hunts unlimited | kept (nearest = correct on the torus). Bot runs at 1024 / 2048: bots roam 300-600 cells for mana (AI mode 6 dominates). A hunt radius / territory belongs to the bot work of round 17. |
| rivers / source tries by area | done in session 2 |
| draw distance 255 | done in session 2; G measured 1.5-2x the frame time of 127 |

Free camera: Ctrl x4 on top of Shift (~120 cells/s at 165 fps): a 1024 world in ~9 s.

## 4. Bot-vs-bot runs (`rts --bots 7`, idle human, 6000 ticks)

- 1024 seed 1: 7 castles on the seats by tick 1500, castle levels 1-2, one bot killed by tick 3000, one castle lost.
- 1024 seed 3: 7 castles on the seats, 3 at level 2 by tick 6000.
- 2048 seed 2 (template 59, after the spell fix): 5 castles by tick 3000, 2 lost again by 6000 (attacks across
  hundreds of cells happen); slower than 1024 (mana is sparser per wizard).
- The idle human dies in every run (monsters near its seat are awake now).

## 5. For round 12 and later

- `mode_start_net_run` is the call the lobby makes after its setup exchange; `rts --net` can go then.
- A human that leaves a network mode run: untested (net_test part 3 covers the original's rule).
- Bot behaviour at scale (hunt radius, defending the castle, using portals) = round 17; respawn = round 13.
