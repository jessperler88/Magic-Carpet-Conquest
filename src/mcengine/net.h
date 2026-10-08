#pragma once
// Network layer: net_*_4e4b0..4f620 (ENGINE.md "Network (NetBIOS, int 5Ch through DPMI 0300h)" and
// "region D findings"). The original speaks NetBIOS through int 5Ch; the port keeps the session and
// per-tick exchange logic and replaces NetBIOS by a NetTransport that the platform installs.
//
// Round-6 CONTRACT (task B implements, nobody changes the declarations below; additions are fine).
// Placeholder behaviour until then: no network - net_init fails, joins fail, exchanges do nothing.
#include <cstdint>
#include <cstddef>
#include "mode.h"

// net_init_4ee70: detect the transport and allocate the buffers. 1 = network present (the original
// sets DAT_0009e3c8 = 1, which the front end reads as g_fe_network), -1 = no network.
int  net_init();
// net_shutdown_4efc0
void net_shutdown();
// net_session_join_4f030(session_name, player_count): join / host the named session and wait until
// `player_count` players are connected. Returns the local player index (DAT_0009e3ca), -1 when it
// failed or was aborted (DAT_0009e3f8).
int  net_session_join(const char *session, int player_count);
// net_exchange_frame_4f530(base, size): exchange the per-player blocks base + p * size between all
// players (host receives from every other player, then sends to all; a client sends, then receives).
// Called by player_commands_process_3a8b0 with (commands, 10) every tick and (players, 0x801) after a
// join.
void net_exchange_frame(void *base, int size);
// net_player_disconnect_4f360(player)
void net_player_disconnect(int player);

// ==== additions by task B (round 6) ====================================================================
//
// ---- NetBIOS model ----------------------------------------------------------------------------------
// The original builds NetBIOS control blocks (NCBs, 0x42 bytes allocated in DOS memory, DAT_0009e3fc +
// DAT_0009e400[8]) and submits them through int 5Ch (net_netbios_submit_4eda0, DPMI 0300h). Every command
// it uses is a "no-wait" command: the call returns at once with cmd_cplt (+0x31) = 0xff and the game
// busy-waits until the driver completes it. The port keeps exactly that: net.cpp fills a NetNcb, calls
// NetTransport::submit and spins on `cplt == NET_PENDING` calling NetTransport::poll.
enum : uint8_t {
    NCB_CANCEL      = 0x35,   // cancel the NCB `cancel_target` (net_cancel_4e870)
    NCB_INVALID     = 0x7f,   // presence test: a NetBIOS answers "illegal command" (3) (net_netbios_detect_4e8f0)
    NCB_CALL        = 0x90,   // open a session to `callname` from `name` (net_call_4e600)
    NCB_LISTEN      = 0x91,   // wait for a call from `callname` to `name` (net_listen_4ea10)
    NCB_HANGUP      = 0x92,   // close session `lsn` (net_hangup_4e9e0)
    NCB_SEND        = 0x94,   // send `length` bytes of `buffer` on `lsn` as one message (net_send_4ec80)
    NCB_RECEIVE     = 0x95,   // receive one message of at most `length` bytes on `lsn` (net_receive_4eb30)
    NCB_ADD_NAME    = 0xb0,   // add `name` to the network's name table (net_add_name_4e530)
    NCB_DELETE_NAME = 0xb1,   // remove `name` (net_delete_name_4e940)
};
// Return codes (cmd_cplt) the game tests or the port's transports produce.
enum : uint8_t {
    NRC_GOODRET  = 0x00,
    NRC_ILLCMD   = 0x03,   // the presence test's expected answer
    NRC_INCOMP   = 0x06,   // message larger than the receive buffer (the rest is lost)
    NRC_SCLOSED  = 0x0a,   // session closed by the other side
    NRC_CMDCAN   = 0x0b,   // command cancelled
    NRC_DUPNAME  = 0x0d,   // name already in the local name table (net_session_join deletes and retries)
    NRC_SABORT   = 0x18,   // session ended abnormally (the other side vanished)
    NRC_NOCALL   = 0x14,   // name called not found
    NRC_REJECTED = 0x12,   // session open rejected (no LISTEN pending at the called name)
    NRC_INUSE    = 0x16,   // name in use on another adapter
    NRC_PENDING  = 0xff,   // command still running (cmd_cplt while no-wait commands are pending)
};
constexpr int NET_NAME_LEN = 16;   // NetBIOS names: 15 characters padded with ' ' + 1 byte
struct NetNcb {
    uint8_t  cmd = 0;                       // +0x00
    uint8_t  retcode = 0;                   // +0x01 immediate return code
    uint8_t  lsn = 0;                       // +0x02 local session number (0 = none)
    uint8_t  num = 0;                       // +0x03 name number (net_session_join copies the local one)
    uint8_t *buffer = nullptr;              // +0x04 seg:off of the DOS buffer (DAT_0009e3d0 / 3d4)
    uint16_t length = 0;                    // +0x08
    char     callname[NET_NAME_LEN] = {};   // +0x0a
    char     name[NET_NAME_LEN] = {};       // +0x1a
    uint8_t  rto = 0, sto = 0;              // +0x2a / +0x2b receive / send timeouts (0 = none)
    uint8_t  cplt = 0;                      // +0x31 NRC_PENDING while running
    NetNcb  *cancel_target = nullptr;       // NCB_CANCEL: the NCB to cancel (the original passes its address in +4)
};

// The platform's NetBIOS: name service + reliable message sessions. Implementations: src/mcport/net_tcp.*
// (Winsock, mcport) and the in-memory LAN of tests/net_test.cpp.
struct NetTransport {
    virtual ~NetTransport() = default;
    // net_netbios_detect_4e8f0 (int 5Ch vector + command 0x7f answered with 3). Also the place to start the
    // driver (the original runs system("netbios") in net_init first).
    virtual bool present() = 0;
    // int 5Ch: start `ncb`. Returns -1 when the request could not even be issued, else 0 with
    // ncb->cplt = NRC_PENDING (or already the result). An NCB that is still pending when it is submitted
    // again is replaced (the original reuses NCB memory; e.g. a RECEIVE on a slot that has a LISTEN
    // pending).
    virtual int submit(NetNcb *ncb) = 0;
    // The driver's background work: completes pending NCBs. Waits at most `timeout_ms` for something
    // to happen (0 = just look).
    virtual void poll(int timeout_ms) = 0;
    // Close every session and name (program end / net_shutdown). Optional.
    virtual void close_all() {}
};
// Installs the transport for the calling thread's network context (see net_context_*). nullptr = none
// (net_init fails).
void net_set_transport(NetTransport *t);
NetTransport *net_transport();

// ---- the lobby (fe_screen_multiplayer_54bd0) -------------------------------------------------------
// Progress callbacks of the join; null = nothing. The original calls the front end directly:
struct NetLobbyHooks {
    void (*slot_connecting)(int player) = nullptr;   // fe_lobby_slot_connecting_55920 (slot anim 1, blinking)
    void (*slot_connected)(int player) = nullptr;    // fe_multiplayer_slot_joined_559c0 (slot anim 2)
    void (*slot_clear)(int player) = nullptr;        // fe_lobby_slot_clear_55a60 (slot anim 0)
    // The input half of net_check_cancel_4e4b0: Esc (g_key_last == 1) or a left click on the lobby's
    // abort button (640-space x 0x238..0x25e, y 0x60..0x86) - consume the key / click, start the fade to
    // black (vga_palette_fade_61510(NULL, 0x10, 0)) and return true. Otherwise clear the click event
    // (g_mouse_click_left = 0) and return false.
    bool (*cancel_requested)() = nullptr;
    // Called in every blocking wait of the network layer (the exchanges, a blocking join): the platform
    // can keep its window alive. Not part of the original.
    void (*idle)() = nullptr;
};
void net_set_lobby_hooks(const NetLobbyHooks &hooks);
NetLobbyHooks net_lobby_hooks();
// net_check_cancel_4e4b0: sets the abort flag DAT_0009e3f8 when cancel_requested() says so.
void net_check_cancel();

// The join, frame-stepped for the front end: begin, then step once per frame until it is not
// NET_JOIN_PENDING. net_session_join() = begin + step until done (blocking, waits poll() 10 ms each).
constexpr int NET_JOIN_PENDING = -2;
int  net_session_join_begin(const char *session, int player_count);   // -1 at once when no network
int  net_session_join_step();                                          // local index, -1, NET_JOIN_PENDING
// net_exchange_block_4f620(base, size): every player in turn broadcasts its block to all others (a
// full-mesh all-to-all). Not called anywhere in carpet.exe; translated for completeness.
void net_exchange_block(void *base, int size);
// Port only (round 8): the settings that change the simulation (PortSettings::thing_slots, settings.h
// GameplayRules) must be the same on every peer. Right after the level choice the lobby (and every scripted
// peer, net_test) calls net_agree_rules once: each peer sends its block through net_exchange_block, all
// adopt the host's (*differs: some peer had other values). Without a session it returns `mine`.
// net_apply_rules forces the result over the local settings (thing_pool_force_slots,
// gameplay_force_rules) until net_release_rules (game_after_frontend, when a level starts outside a
// network game, and net_shutdown).
// Round 11 session 3: a mode run's ModeParams (seed, world size, bots, WorldParams...) travel with the rules
// until round 12's lobby has its own setup exchange: a peer starts the run with the host's (mcport rts --net,
// world_net_test). humans = the session's player count, set by the host.
struct NetGameRules {
    uint32_t thing_slots;           // thing_pool_wanted_slots()
    uint32_t possession_range_pct;  // GameplayRules
    uint32_t mode;                  // GameplayRules.mode (round 10; mode.h GAME_MODE_*, 0 = the original)
    uint32_t reserved;
    ModeParams mode_params;         // g_mode.params while a mode is active, else all default
};
static_assert(sizeof(NetGameRules) == 16 + sizeof(ModeParams));
NetGameRules net_local_rules();                                 // this peer's settings
NetGameRules net_agree_rules(const NetGameRules &mine, bool *differs);
void net_apply_rules(const NetGameRules &rules);
void net_release_rules();

// ---- state (the DAT_0009e3c8.. globals) ------------------------------------------------------------
bool net_present();          // DAT_0009e3c8: net_init succeeded
bool net_joined();           // DAT_0009e3c9: a session is joined
int  net_local_player();     // DAT_0009e3ca
int  net_player_count();     // DAT_0009e3cc
int  net_host();             // DAT_0009e3fa: the player that collects and redistributes the packets
bool net_aborted();          // DAT_0009e3f8
int  net_player_status(int p);   // DAT_0009e420[p]: 0 none, 1 connected, 2 local (net_build_player_status_4ed50)

// ---- desync check (port only) ------------------------------------------------------------------------
// While armed, every net_exchange_frame of the 10-byte command packets is followed by one extra message
// per direction on the same sessions: a client sends {exchange number, checksum} to the host after its
// packet, the host answers with its own pair after the packet array. Both sides compare the checksum of
// their simulation state (net_state_checksum) and count mismatches - reported, not acted upon. The
// checksum is taken at the exchange, i.e. of the state every instance reached after the previous tick.
// Every instance of a session must use the same interval (MC_NET_SYNC in mcport; 0 = off, the original's
// messages only).
void     net_sync_set_interval(int every_n_exchanges);   // default 1
void     net_sync_arm();     // game_level_begin / a level restart: exchange counter 0, checks on
void     net_sync_disarm();  // a join leaves it disarmed until the level starts
struct NetSyncStats {
    uint32_t exchanges;        // command exchanges since net_sync_arm
    uint32_t checks;           // checksums compared (per remote checksum received)
    uint32_t mismatches;
    int32_t  first_mismatch;   // exchange number of the first mismatch, -1 none
    uint32_t last_local;       // last local checksum sent / compared
    // round 10 task E: the parts exchange of the first mismatch (net_sync_set_parts below)
    int32_t  first_part;       // NCP_* of the first differing part, -1 none / not exchanged yet
    int32_t  part_peer;        // the player the parts were exchanged with, -1 none
    uint32_t parts_differ;     // how many parts differ
    uint32_t parts_exchanges;  // parts exchanges done (one per peer pair at most)
    uint32_t dumped;           // 1 when the dump hook wrote a dump
};
NetSyncStats net_sync_stats();
// FNV-1a over the simulation state every instance must agree on: GameState.rng, g_rng16, the free /
// recyclable stacks, the whole Thing pool, per player the record head (win timer, status, active,
// index, is_computer, thing) and the P block, and the five maps. Left out because the original keeps
// per-instance values there: Config, GameState.local_player, PlayerRec tick / messages / camera log /
// name / input mode, the level track (music availability), the command packets, PlayerBlock.start_tick
// (real time), the HUD flash counters PlayerBlock +0x187..+0x189 and spell_flash[] (hud_tick_state
// counts them down for the local player only), PlayerBlock.combat_music (+0x2e: only the local player's
// flyer counts it down; round 11), and Thing.flags bit 0 of the human flyers (class 3 type
// 0: "this is the local player", player_spawn_3f360) and of the spells (class 12: "the local player
// owns it", spell pickup) - also of free slots (class 0), which keep it after the free (round 11).
uint32_t net_state_checksum();
// The checksum the side channel uses (null = net_state_checksum); tests give each simulated peer its own.
void net_sync_set_checksum(uint32_t (*fn)());

// ==== round 10 task E: checksum parts, parts exchange on a mismatch, desync dumps =====================
// (docs/analysis/port_desync.md)
//
// net_state_checksum split into named parts. Each part is its own FNV-1a 32 (basis 0x811c9dc5) over the
// same bytes the total hashes, computed alongside it in one pass; the total is still the one sequential
// FNV-1a of today (it is NOT a fold of the parts - FNV does not compose), so its value is unchanged.
//   rng          GameState.rng                        rng16      g_rng16 (terrain generator)
//   pool         free_top, free_list, active_top, active_list   (the original's two 1000-entry stacks)
//   pool_ext     extended pool: slots + its two stacks (basis while the pool is the original's 1000)
//   things.<cls> the Things of that class (Thing.cls 0..14, 15 = 15 and above), each as u32 slot index +
//                the Thing with the masked flags bit (the slot index is in the part only, not in the total)
//   player0..7   record head + the P block minus the local-only fields (exactly as in the total)
//   map.type / map.height / map.light / map.flags / map.cells (the u16 cell-list heads)
//   mode         mode_checksum over the mode block (basis while no mode runs)
//   ai_seed      g_ai_rand_seed (ai_wizard.h) - NOT in the total (the original's checksum never had it);
//                read through g_hook_net_ai_seed, 0 while that is not installed
enum : int {
    NCP_RNG = 0,
    NCP_RNG16,
    NCP_POOL,
    NCP_POOL_EXT,
    NCP_THINGS,                         // + class 0..15
    NCP_PLAYERS = NCP_THINGS + 16,      // + player 0..7
    NCP_MAP_TYPE = NCP_PLAYERS + 8,
    NCP_MAP_HEIGHT,
    NCP_MAP_LIGHT,
    NCP_MAP_FLAGS,
    NCP_CELL_HEADS,
    NCP_MODE,
    NCP_AI_SEED,                        // not part of the total
    NCP_COUNT
};
struct NetChecksumParts {
    uint32_t total;                     // == net_state_checksum()
    uint32_t part[NCP_COUNT];
};
// Fills *out (total + every part). The total equals net_state_checksum() of the same state.
void        net_state_checksum_parts(NetChecksumParts *out);
const char *net_checksum_part_name(int part);           // "rng", "things.creature", "player3", ... ; "?" out of range
// First part that differs (in NCP_ order), -1 when all agree. `count` (optional) = number of differing parts.
int         net_checksum_parts_first_diff(const NetChecksumParts &a, const NetChecksumParts &b, int *count);
// " name=xxxxxxxx" for every part, appended to buf (MC_TICK_LOG_PARTS lines); returns the length written.
int         net_checksum_parts_format(const NetChecksumParts &p, char *buf, size_t cap);
// The AI's C-runtime rand() seed lives in ai_wizard.cpp, which the sim core (net.cpp) does not link; the
// gameplay registration (or replay_check.h desync_tools_install) installs the reader. Null = part 0.
extern uint32_t (*g_hook_net_ai_seed)();

// The parts exchange. With the side channel armed, on the FIRST checksum mismatch between two peers (the
// host and one client - the only pairs that compare) both send each other their parts once, in the same
// exchange right after the checksum messages: the client sends first and then receives, the host receives
// and then sends (no crossing waits). Message: "PRTS", u32 exchange, u32 count, u32 total, count x u32 parts
// - size-agnostic (a peer with fewer / more parts compares the common prefix). Each pair does it once per
// net_sync_arm, so the per-tick message pattern of every later exchange is the original's + the 8-byte
// checksum. Both sides then print the first differing part to stderr and call the dump hook once.
// The checksum / parts / dump are of the state every instance reached after the previous tick (the
// exchange is inside player_commands_process, before any packet of this tick is applied).
void net_sync_set_parts(void (*fn)(NetChecksumParts *));   // null = net_state_checksum_parts (tests: per peer)
// Called once per peer on its first parts exchange: (exchange number, local player). Return true when a
// dump was written. Null = no dump. replay_check.h desync_dump_install() installs the savestate dump
// <save dir>/desync_<exchange>_p<player>.mcs.
void net_sync_set_dump(bool (*fn)(uint32_t exchange, int local_player));
// The result of the last parts exchange (false when none happened since net_sync_arm).
bool net_sync_last_parts(NetChecksumParts *local, NetChecksumParts *remote, int *remote_player, uint32_t *exchange);
// NetSyncStats (above) carries the summary: first_part / part_peer / parts_differ / dumped.

// ---- per-thread contexts (tests run several simulated peers in one process, one thread each) -------
struct NetContext;
NetContext *net_context_create();
void        net_context_destroy(NetContext *ctx);
// Selects the calling thread's context (default: the process-wide one). Returns the previous one.
NetContext *net_context_select(NetContext *ctx);
