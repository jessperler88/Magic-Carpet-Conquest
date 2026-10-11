# Playing Conquest together over a network

Two (or more) PCs, each with its own copy of Magic Carpet and the same build of this port.

## 1. Set up a PC

1. Clone the repository.
2. Put your own Magic Carpet game folder (the one that holds `carpet.exe` and `data/`) at `MagicCarpet/magic/`
   inside the clone. That path is ignored by git; the game's files are never part of the repository. (Another
   place works too: pass `-DMC_GAME_DIR=<dir>` to CMake, and edit the `GAME=` line of the `play_*.bat` scripts.)
3. Build as the README describes (Visual Studio C++ tools, CMake, Python 3, vcpkg with `sdl2:x64-windows`,
   `VCPKG_ROOT` set):

   ```
   cd src
   cmake --preset msvc-x64
   cmake --build --preset release
   ```

   The result is `build/Release/mcport.exe`.

**Every player must run the same build.** Both sides simulate the match and compare a checksum every tick; a
different build drifts apart. After pulling new commits, rebuild on every PC.

## 2. Start

- **Host:** run `play_host.bat`. It prints this PC's IPv4 addresses and starts the game listening on TCP port 30600.
- **Everyone else:** run `play_join.bat` and type one of the host's addresses (or `play_join.bat 192.168.1.20`).
- Allow `mcport` on private networks when Windows Firewall asks.
- The window title ends with `[NETWORK HOST]` on the host and `[network: joined the host]` on the others.
- A PC's very first start shows the game's language screen once.

`play_test_two_windows.bat` opens a host and a second player on one PC (loopback only, no firewall prompt) to try
the steps below alone.

## 3. In the game

1. Every player chooses the multiplayer option on the main menu and arrives in the lobby: an open book with the
   wizards' silhouettes on the left page.
2. The host clicks the small urn on the map at the bottom left of the lobby, sets up the Conquest match (world
   size, bots, rules) and confirms. The right page then reads "Conquest" with the world size, bots and seed.
3. The host presses the tick, then the others do. Everyone loads the same world.

The urn on the **main menu** is always an offline match for that PC alone; a shared match starts from the urn
**inside the lobby**.

## Same house or different houses

- **Same network** (one router): works as described.
- **Different houses** is the internet. Besides port 30600 every instance listens on a second, randomly chosen
  port and the players' games connect to each other directly, so plain port forwarding is awkward. A virtual LAN
  (Tailscale, ZeroTier, ...) is the practical way: join the same virtual network and use the host's virtual
  address in `play_join.bat`.

## Limits

- No pause, no time control and no save states in a network game.
- The host's rules and world options apply to everyone.
- A desync is reported in `mcport.log` (in `%APPDATA%\Bullfrog\MagicCarpet\`). At the end of a match each PC
  writes a match report to the `reports` folder there; two reports of the same match should agree line for line.
