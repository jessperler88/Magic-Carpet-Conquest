// Network layer (round 6, task B): net_check_cancel_4e4b0 .. net_exchange_block_4f620 over a NetTransport
// (net.h). Every function keeps the original's NCB sequence, the order of the per-tick exchange, the
// host choice and the drop handling; only int 5Ch is replaced by NetTransport::submit / poll.
//
// Translated: net_check_cancel_4e4b0, net_add_name_4e530, net_call_4e600, net_cancel_4e870,
// net_netbios_detect_4e8f0, net_delete_name_4e940, net_hangup_4e9e0, net_listen_4ea10, net_receive_4eb30,
// net_receive_chunked_4ebb0, net_send_4ec80, net_send_chunked_4ecf0, net_build_player_status_4ed50,
// net_player_status_4ed70, net_netbios_submit_4eda0 (= NetTransport::submit), net_init_4ee70,
// net_shutdown_4efc0, net_session_join_4f030, net_player_disconnect_4f360, net_send_to_player_4f470,
// net_receive_from_player_4f4d0, net_exchange_frame_4f530, net_exchange_block_4f620.
#define _CRT_SECURE_NO_WARNINGS
#include "net.h"
#include "mode.h"
#include "settings.h"
#include "mc_types.h"
#include "mc_globals.h"
#include "thing.h"
#include "mc_math.h"
#include <cstdio>
#include <cstring>

namespace {

constexpr int NET_BUF = 0x800;      // DAT_0009e3d0 / 3d4 / 3d8[]: the DOS transfer buffers

enum JoinPhase { J_IDLE, J_NAME_START, J_NAME_WAIT, J_CALLS, J_CALL_WAIT, J_WAIT_ALL };

} // namespace

struct NetContext {
    NetTransport *tr = nullptr;
    NetLobbyHooks hooks;
    uint8_t present = 0;            // DAT_0009e3c8
    uint8_t joined = 0;             // DAT_0009e3c9
    int16_t local = 0;              // DAT_0009e3ca
    int16_t count = 0;              // DAT_0009e3cc
    int16_t abort = 0;              // DAT_0009e3f8
    int16_t host = 0;               // DAT_0009e3fa
    bool    allocated = false;      // DAT_0009e3fc != 0
    NetNcb  ctl;                    // DAT_0009e3fc: control NCB (cancel, presence test)
    NetNcb  ncb[8];                 // DAT_0009e400[8]: one NCB per player slot
    uint8_t send_buf[NET_BUF];      // DAT_0009e3d0
    uint8_t recv_buf[NET_BUF];      // DAT_0009e3d4
    uint8_t status[8] = {};         // DAT_0009e420
    char    session[32] = {};       // DAT_0009e428
    // the frame-stepped join (net_session_join_4f030's loops)
    JoinPhase phase = J_IDLE;
    int  b = 0;                     // the slot the current loop is at
    int  wait_ms = 0;               // poll timeout inside the join's waits (0 = one look per step)
    // desync check
    int      sync_every = 1;
    uint32_t (*sync_fn)() = nullptr;
    bool     sync_armed = false;
    NetSyncStats sync{0, 0, 0, -1, 0, -1, -1, 0, 0, 0};
    // round 10 task E: the parts exchange (net.h net_sync_set_parts)
    void   (*parts_fn)(NetChecksumParts *) = nullptr;
    bool   (*dump_fn)(uint32_t, int) = nullptr;
    uint8_t parts_done[8] = {};         // per remote player: parts already exchanged since net_sync_arm
    bool    have_parts = false;
    NetChecksumParts parts_local{}, parts_remote{};
    int      parts_peer = -1;
    uint32_t parts_exchange = 0;
};

namespace {

NetContext s_default;
thread_local NetContext *t_ctx = &s_default;
NetContext &C() { return *t_ctx; }

void idle_wait(int ms) {
    if (C().hooks.idle) C().hooks.idle();
    if (C().tr) C().tr->poll(ms);
}
// The busy-wait of the original: `while (ncb->cplt == 0xff) ;`
void spin(const NetNcb &n) { while (n.cplt == NRC_PENDING) idle_wait(10); }

// net_netbios_submit_4eda0: clears cmd_cplt and issues int 5Ch through DPMI 0300h.
int submit(NetNcb &n) {
    n.cplt = 0;
    if (!C().tr) return -1;
    return C().tr->submit(&n);
}

// sprintf(buf, "%s%d", session, p) (format 0x92e40) padded with ' ' (0x92e3c) to 15 characters.
void make_name(char out[NET_NAME_LEN], const char *session, int p) {
    char tmp[64];
    std::snprintf(tmp, sizeof tmp, "%s%d", session, p);
    std::memset(out, 0, NET_NAME_LEN);
    std::strncpy(out, tmp, NET_NAME_LEN - 1);
    size_t n = std::strlen(out);
    while (n < 15) out[n++] = ' ';
}
void pad_name(char out[NET_NAME_LEN], const char *in) {
    char tmp[NET_NAME_LEN];
    std::memset(tmp, 0, sizeof tmp);
    std::strncpy(tmp, in, NET_NAME_LEN - 1);
    size_t n = std::strlen(tmp);
    while (n < 15) tmp[n++] = ' ';
    std::memcpy(out, tmp, NET_NAME_LEN);
}

void cb_connecting(int p) { if (C().hooks.slot_connecting) C().hooks.slot_connecting(p); }
void cb_connected(int p)  { if (C().hooks.slot_connected) C().hooks.slot_connected(p); }
void cb_clear(int p)      { if (C().hooks.slot_clear) C().hooks.slot_clear(p); }

// net_cancel_4e870(p): cancel the pending NCB of slot p and wait until both have completed.
int net_cancel(int p) {
    NetContext &c = C();
    if (c.ncb[p].cplt == NRC_PENDING) {
        c.ctl.cmd = NCB_CANCEL;
        c.ctl.cancel_target = &c.ncb[p];
        if (submit(c.ctl) == -1) return -99;
        while (c.ctl.cplt == NRC_PENDING || c.ncb[p].cplt == NRC_PENDING) idle_wait(10);
    }
    return -(int)c.ctl.cplt;
}

// net_netbios_detect_4e8f0: the int 5Ch vector exists and command 0x7f is answered with "illegal command".
int net_present_test() {
    NetContext &c = C();
    if (!c.tr || !c.tr->present()) return -1;
    c.ctl.cmd = NCB_INVALID;
    c.ctl.retcode = 0;
    if (submit(c.ctl) == -1) return -1;
    return c.ctl.retcode == NRC_ILLCMD ? 0 : -1;
}

// net_delete_name_4e940(ncb, name)
int net_delete_name(NetNcb &n, const char *name) {
    pad_name(n.name, name);
    n.cmd = NCB_DELETE_NAME;
    if (submit(n) == -1) return -99;
    spin(n);
    return -(int)n.cplt;
}

// net_hangup_4e9e0(ncb)
int net_hangup(NetNcb &n) {
    n.cmd = NCB_HANGUP;
    if (submit(n) == -1) return -99;
    spin(n);
    n.lsn = 0;
    return -(int)n.cplt;
}

// net_listen_4ea10(p): (re)start a LISTEN for player p's name on slot p unless one is pending.
int net_listen(int p) {
    NetContext &c = C();
    NetNcb &n = c.ncb[p];
    if (n.cplt != NRC_PENDING) {
        n.cmd = NCB_LISTEN;
        make_name(n.callname, c.session, p);
        n.rto = 0;
        n.sto = 0;
        if (submit(n) == -1) return -99;
        n.buffer = nullptr;              // +4 = 0
    }
    return -(int)n.cplt;
}

// net_receive_4eb30(ncb, dst): one message (at most 0x800 bytes) through the DOS buffer.
int net_receive(NetNcb &n, uint8_t *dst) {
    NetContext &c = C();
    n.cmd = NCB_RECEIVE;
    n.length = NET_BUF;
    n.buffer = c.recv_buf;
    if (submit(n) == -1) return -99;
    spin(n);
    if (n.cplt != 0) return -(int)n.cplt;
    std::memcpy(dst, c.recv_buf, n.length);
    return n.length;
}
// net_receive_chunked_4ebb0(ncb, dst, size): size / 0x800 full messages, then the remainder (also when
// it is 0). Returns size when everything arrived.
int net_receive_chunked(NetNcb &n, uint8_t *dst, uint32_t size) {
    const uint32_t full = size >> 11;
    for (uint32_t i = 0; i < full; i++) {
        int r = net_receive(n, dst);
        if (r != NET_BUF) return r;
        dst += NET_BUF;
    }
    int r = net_receive(n, dst);
    if (r == (int)(size & 0x7ff)) return (int)size;
    return r;
}

// net_send_4ec80(ncb, src, len)
int net_send(NetNcb &n, const uint8_t *src, uint32_t len) {
    NetContext &c = C();
    std::memcpy(c.send_buf, src, len);
    n.cmd = NCB_SEND;
    n.length = (uint16_t)len;
    n.buffer = c.send_buf;
    if (submit(n) == -1) return -99;
    spin(n);
    return -(int)n.cplt;
}
// net_send_chunked_4ecf0(ncb, src, size)
int net_send_chunked(NetNcb &n, const uint8_t *src, uint32_t size) {
    const uint32_t full = size >> 11;
    for (uint32_t i = 0; i < full; i++) {
        int r = net_send(n, src, NET_BUF);
        if ((int16_t)r != 0) return (int16_t)r;
        src += NET_BUF;
    }
    return net_send(n, src, size & 0x7ff);
}

// net_player_status_4ed70(p): 2 local, 1 session up (lsn != 0 and the last NCB completed), 0 none.
uint8_t net_player_status_of(int p) {
    NetContext &c = C();
    if (p == c.local) return 2;
    if (c.ncb[p].lsn != 0 && c.ncb[p].cplt == 0) return 1;
    return 0;
}
// net_build_player_status_4ed50
void net_build_player_status() {
    for (int p = 0; p < 8; p++) C().status[p] = net_player_status_of(p);
}

// net_send_to_player_4f470(p, buf, size)
void net_send_to_player(int p, const uint8_t *buf, uint32_t size) {
    NetContext &c = C();
    if (!c.present || c.status[p & 7] != 1) return;
    net_send_chunked(c.ncb[p], buf, size);
    if (c.ncb[p].cplt != 0) net_listen(p);
}
// net_receive_from_player_4f4d0(p, buf, size)
void net_receive_from_player(int p, uint8_t *buf, uint32_t size) {
    NetContext &c = C();
    if (!c.present || c.status[p & 7] != 1) return;
    net_receive_chunked(c.ncb[p], buf, size);
    if (c.ncb[p].cplt != 0) net_listen(p);
}

// ---- the join (net_session_join_4f030) as resumable steps ------------------------------------------

// net_add_name_4e530 + the result handling of the name loop (0x4f0cd..0x4f109).
void join_name_result() {
    NetContext &c = C();
    NetNcb &n = c.ncb[c.b];
    if (n.cplt == 0) cb_connected(c.b); else cb_clear(c.b);
    const int r = -(int)n.cplt;
    if (r == 0) c.local = (int16_t)c.b;
    else if (r == -(int)NRC_DUPNAME) {               // already in the local table: delete it and retry the slot
        char name[NET_NAME_LEN];
        make_name(name, c.session, c.b);
        net_delete_name(n, name);
        c.b--;
    }
    c.b++;
}

// net_call_4e600(b)'s epilogue, after its wait loop ended without an abort.
void join_call_result(int b) {
    NetContext &c = C();
    NetNcb &me = c.ncb[c.local];
    NetNcb &nb = c.ncb[b];
    bool done = false;
    if (nb.cplt == 0) { net_cancel(c.local); done = true; }     // our LISTEN was answered: drop the CALL
    if (me.cplt == 0) { net_cancel(b); done = true; }          // our CALL got through: drop the LISTEN
    if (done) cb_connected(b);
    if (me.cplt == 0) {                                        // the session lives in slot b from now on
        net_cancel(b);
        std::memcpy(nb.callname, me.callname, NET_NAME_LEN);
        nb.lsn = me.lsn;
        nb.cplt = me.cplt;
    }
}

int join_finish() {
    NetContext &c = C();
    c.phase = J_IDLE;
    if (c.abort) return -1;
    net_build_player_status();
    c.host = 0;
    c.joined = 1;
    return c.local;
}

} // namespace

// ---- public API -------------------------------------------------------------------------------------

void net_set_transport(NetTransport *t) { C().tr = t; }
NetTransport *net_transport() { return C().tr; }
void net_set_lobby_hooks(const NetLobbyHooks &hooks) { C().hooks = hooks; }
NetLobbyHooks net_lobby_hooks() { return C().hooks; }

NetContext *net_context_create() { return new NetContext(); }
void net_context_destroy(NetContext *ctx) { if (ctx != &s_default) delete ctx; }
NetContext *net_context_select(NetContext *ctx) {
    NetContext *prev = t_ctx;
    t_ctx = ctx ? ctx : &s_default;
    return prev;
}

bool net_present() { return C().present != 0; }
bool net_joined() { return C().joined != 0; }
int  net_local_player() { return C().local; }
int  net_player_count() { return C().count; }
int  net_host() { return C().host; }
bool net_aborted() { return C().abort != 0; }
int  net_player_status(int p) { return (p >= 0 && p < 8) ? C().status[p] : 0; }

// net_check_cancel_4e4b0
void net_check_cancel() {
    if (C().hooks.cancel_requested && C().hooks.cancel_requested()) C().abort = 1;
}

// net_init_4ee70
int net_init() {
    NetContext &c = C();
    int result = 1;
    if (c.present == 0 && !c.allocated) {
        // system("netbios") loads the DOS NetBIOS driver: the transport's present() does its own start-up
        c.allocated = true;
        if (net_present_test() == -1) { c.allocated = false; return -1; }
        for (NetNcb &n : c.ncb) n = NetNcb();
        c.present = 1;
    }
    return result;
}

// net_shutdown_4efc0: frees the NCBs and buffers. The port also closes the transport's sessions and
// names, and clears DAT_0009e3c9 (the original leaves it set; it only shuts down at program end).
void net_shutdown() {
    NetContext &c = C();
    net_release_rules();
    if (!c.present) return;
    if (c.tr) c.tr->close_all();
    c.present = 0;
    c.allocated = false;
    c.joined = 0;
    c.phase = J_IDLE;
}

int net_session_join_begin(const char *session, int player_count) {
    NetContext &c = C();
    if (!c.present) return -1;
    if (c.joined) return c.local;
    c.abort = 0;
    c.count = (int16_t)player_count;
    if (c.count > 8) c.count = 8;
    std::snprintf(c.session, sizeof c.session, "%s", session ? session : "");
    std::memset(c.status, 0, sizeof c.status);
    c.local = -1;
    c.b = 0;
    c.phase = J_NAME_START;
    c.sync_armed = false;
    return NET_JOIN_PENDING;
}

int net_session_join_step() {
    NetContext &c = C();
    for (;;) {
        switch (c.phase) {
        case J_IDLE:
            return c.joined ? c.local : -1;

        case J_NAME_START: {                       // 0x4f09e: try the names <session>0 .. <session>7
            if (c.local != -1 || c.abort != 0 || c.b >= 8) {
                if (c.local == -1) { c.phase = J_IDLE; return -1; }
                if (c.abort != 0) {                // 0x4f149: aborted after the name was added
                    char name[NET_NAME_LEN];
                    make_name(name, c.session, c.local);
                    net_delete_name(c.ncb[c.local], name);   // (the original passes slot local + 1)
                    c.phase = J_IDLE;
                    return -1;
                }
                // 0x4f18b: every slot NCB carries the local name
                for (int p = 0; p < 8; p++) {
                    if (p == c.local) continue;
                    std::memcpy(c.ncb[p].name, c.ncb[c.local].name, NET_NAME_LEN);
                    c.ncb[p].num = c.ncb[c.local].num;
                }
                for (int p = 0; p < c.count; p++) if (p != c.local) net_listen(p);
                c.b = 0;
                c.phase = J_CALLS;
                continue;
            }
            NetNcb &n = c.ncb[c.b];                // net_add_name_4e530(ncb[b], name, b)
            make_name(n.name, c.session, c.b);
            n.cmd = NCB_ADD_NAME;
            if (submit(n) == -1) { c.b++; continue; }   // -99: the next slot
            c.phase = J_NAME_WAIT;
            continue;
        }
        case J_NAME_WAIT: {
            NetNcb &n = c.ncb[c.b];
            if (n.cplt == NRC_PENDING) {
                net_check_cancel();
                cb_connecting(c.b);
                idle_wait(c.wait_ms);
                if (n.cplt == NRC_PENDING) return NET_JOIN_PENDING;
            }
            join_name_result();
            c.phase = J_NAME_START;
            continue;
        }
        case J_CALLS: {                            // 0x4f211: call every player whose LISTEN is still open
            while (c.b < c.count && (c.b == c.local || c.ncb[c.b].cplt != NRC_PENDING)) c.b++;
            if (c.b >= c.count) { c.phase = J_WAIT_ALL; continue; }
            NetNcb &me = c.ncb[c.local];           // net_call_4e600(b)
            me.cmd = NCB_CALL;
            make_name(me.callname, c.session, c.b);
            me.rto = 0;
            me.sto = 0;
            if (submit(me) == -1) {                // -99; the caller only tests the abort flag
                if (c.abort) { c.phase = J_WAIT_ALL; continue; }
                c.b++;
                continue;
            }
            c.phase = J_CALL_WAIT;
            continue;
        }
        case J_CALL_WAIT: {
            NetNcb &me = c.ncb[c.local];
            NetNcb &nb = c.ncb[c.b];
            if (me.cplt == NRC_PENDING && nb.cplt == NRC_PENDING) {
                cb_connecting(c.b);
                net_check_cancel();
                if (c.abort == 1) {
                    net_cancel(c.local);
                    c.phase = J_WAIT_ALL;          // 0x4f243: the abort ends the call loop
                    continue;
                }
                idle_wait(c.wait_ms);
                if (me.cplt == NRC_PENDING && nb.cplt == NRC_PENDING) return NET_JOIN_PENDING;
            }
            join_call_result(c.b);
            if (c.abort) { c.phase = J_WAIT_ALL; continue; }
            c.b++;
            c.phase = J_CALLS;
            continue;
        }
        case J_WAIT_ALL: {                         // 0x4f259: until every slot has its session
            net_check_cancel();
            int connected = 0;
            for (int p = 0; p < c.count; p++) {
                if (p == c.local || c.ncb[p].cplt == 0) {
                    cb_connected(p);
                    if (c.abort) {
                        if (p != c.local) net_hangup(c.ncb[p]);
                        else {
                            char name[NET_NAME_LEN];
                            make_name(name, c.session, c.local);
                            net_delete_name(c.ncb[p], name);
                        }
                    }
                    connected++;
                } else if (c.abort) {
                    net_cancel(p);
                }
            }
            if (connected == c.count || c.abort) return join_finish();
            idle_wait(c.wait_ms);
            return NET_JOIN_PENDING;
        }
        }
    }
}

// net_session_join_4f030
int net_session_join(const char *session, int player_count) {
    int r = net_session_join_begin(session, player_count);
    if (r != NET_JOIN_PENDING) return r;
    const int keep = C().wait_ms;
    C().wait_ms = 10;
    while ((r = net_session_join_step()) == NET_JOIN_PENDING) {}
    C().wait_ms = keep;
    return r;
}

// net_player_disconnect_4f360(p): the local player leaves (hang up everything, drop the name) or
// another one left (hang up its session); then the host is the first player still there.
void net_player_disconnect(int player) {
    NetContext &c = C();
    if (!c.present || !c.joined) return;
    if (player == c.local) {
        for (int b = 0; b < c.count; b++) {
            if (b == c.local) continue;
            net_cancel(b);
            net_hangup(c.ncb[b]);
        }
        char name[NET_NAME_LEN];
        make_name(name, c.session, c.local);
        net_delete_name(c.ncb[player & 7], name);
        c.joined = 0;
    } else {
        net_cancel(player & 7);
        net_hangup(c.ncb[player & 7]);
    }
    net_build_player_status();
    for (int b = 0; b < c.count; b++) {
        if (c.status[b] != 0) { c.host = (int16_t)b; break; }
    }
}

// ---- desync side channel --------------------------------------------------------------------------

uint32_t (*g_hook_net_ai_seed)() = nullptr;

namespace {

constexpr uint32_t FNV_BASIS = 2166136261u;

uint32_t fnv(uint32_t h, const void *p, size_t n) {
    const uint8_t *b = static_cast<const uint8_t *>(p);
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; }
    return h;
}

void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

// Returns true on a mismatch.
bool sync_compare(uint32_t n, uint32_t mine, uint32_t theirs, int from) {
    NetSyncStats &s = C().sync;
    s.checks++;
    if (mine == theirs) return false;
    s.mismatches++;
    if (s.first_mismatch < 0) {
        s.first_mismatch = (int32_t)n;
        std::fprintf(stderr, "net: DESYNC at exchange %u: player %d has %08x, local player %d has %08x\n",
                     n, from, theirs, C().local, mine);
    }
    return true;
}

// The total and (when `parts` is set) every part in one pass over the state. The total is exactly the
// checksum of rounds 6..9 (+ the mode block of round 10): the same bytes in the same order.
struct Hasher {
    uint32_t h = FNV_BASIS;
    NetChecksumParts *parts;
    explicit Hasher(NetChecksumParts *p) : parts(p) {
        if (parts) for (uint32_t &v : parts->part) v = FNV_BASIS;
    }
    void add(int part, const void *p, size_t n) {
        h = fnv(h, p, n);
        if (parts) parts->part[part] = fnv(parts->part[part], p, n);
    }
    void part_only(int part, const void *p, size_t n) {
        if (parts) parts->part[part] = fnv(parts->part[part], p, n);
    }
};

uint32_t state_checksum(NetChecksumParts *parts) {
    if (parts) std::memset(parts, 0, sizeof *parts);
    if (!g_state) return 0;
    const GameState *s = g_state;
    Hasher H(parts);
    H.add(NCP_RNG, &s->rng, sizeof s->rng);
    H.add(NCP_RNG16, &g_rng16, sizeof g_rng16);
    H.add(NCP_POOL, &s->free_top, sizeof s->free_top);
    H.add(NCP_POOL, s->free_list, sizeof s->free_list);
    H.add(NCP_POOL, &s->active_top, sizeof s->active_top);
    H.add(NCP_POOL, s->active_list, sizeof s->active_list);
    // Thing.flags bit 0 is view state of the local instance on two classes: the local player's own
    // flyer (player_spawn_3f360) and the spells it already owns (spell_pickup_update, the HUD marks them).
    // Phase 3: an extended pool (thing.h) adds its size, its slots and its stack parts; the original's
    // pool hashes exactly as before. Peers with different pool sizes desync at the first exchange.
    const int ext = thing_pool_ext_count();
    if (ext > 0) {
        const int32_t slots = thing_pool_slots();
        H.add(NCP_POOL_EXT, &slots, sizeof slots);
        H.add(NCP_POOL_EXT, thing_pool_ext_free_stack(), (size_t)ext * sizeof(int32_t));
        H.add(NCP_POOL_EXT, thing_pool_ext_active_stack(), (size_t)ext * sizeof(int32_t));
    }
    // Round 11: each Thing is hashed in the original's byte layout (mc_image.h), so the value is the one of
    // rounds 6..10; a world wider than 256 cells also hashes the coordinates' high halves.
    const bool wide = g_world.size > 256;
    for (int i = 0; i < MC_THING_SLOTS + ext; i++) {
        const Thing &src = i < MC_THING_SLOTS ? s->things[i] : *thing_at((unsigned)i);
        ThingImage t;
        thing_to_image(src, &t);
        if ((t.cls == 3 && t.type == 0) || t.cls == 0xc) t.flags &= ~1u;
        const int part = NCP_THINGS + (t.cls < 15 ? t.cls : 15);
        const uint32_t slot = (uint32_t)i;
        H.part_only(part, &slot, sizeof slot);
        H.add(part, &t, sizeof t);
        if (wide) { const ThingHi hi = thing_hi(src); H.add(part, &hi, sizeof hi); }
    }
    for (int p = 0; p < 8; p++) {
        const PlayerRec &r = s->players[p];
        const int part = NCP_PLAYERS + p;
        H.add(part, &r.win_timer, sizeof r.win_timer);
        H.add(part, &r.status, sizeof r.status);
        H.add(part, &r.active, sizeof r.active);
        H.add(part, &r.index, sizeof r.index);
        H.add(part, &r.is_computer, sizeof r.is_computer);
        H.add(part, &r.thing, sizeof r.thing);
        // The P block without what only one instance writes: start_tick (the real-time stamp of the first
        // spawn) and the HUD's flash counters, which hud_tick_state counts down for the local player only.
        uint8_t P[sizeof r.p];
        std::memcpy(P, r.p, sizeof P);
        std::memset(P + offsetof(PlayerBlock, start_tick), 0, 4);
        P[offsetof(PlayerBlock, castle_hit_flash)] = 0;
        P[offsetof(PlayerBlock, hit_flash)] = 0;
        P[offsetof(PlayerBlock, damage_flash)] = 0;
        std::memset(P + offsetof(PlayerBlock, spell_flash), 0, sizeof r.blk.spell_flash);
        H.add(part, P, sizeof P);
        if (wide) {                                         // round 11: start position high halves (world.h)
            const uint16_t hi[2] = {g_world_hi.start_x[p], g_world_hi.start_y[p]};
            H.add(part, hi, sizeof hi);
        }
    }
    // Round 11: the maps of the world (g_world.cells cells; 0x10000 = the original's bytes with 256 cells). A
    // wider world also hashes its size first, so peers with different world sizes differ in "map.type" at once
    // (the camera log is not hashed at any size, its high halves neither).
    if (wide) {
        const uint32_t size = (uint32_t)g_world.size;
        H.add(NCP_MAP_TYPE, &size, sizeof size);
    }
    H.add(NCP_MAP_TYPE, g_map_type, g_world.cells);
    H.add(NCP_MAP_HEIGHT, g_map_height, g_world.cells);
    H.add(NCP_MAP_LIGHT, g_map_light, g_world.cells);
    H.add(NCP_MAP_FLAGS, g_map_flags, g_world.cells);
    H.add(NCP_CELL_HEADS, g_cell_things, sizeof(uint16_t) * g_world.cells);
    // Phase 4: the game mode's block (mode.h), only while a mode runs - the original's checksum is unchanged.
    if (mode_active()) {
        H.h = mode_checksum(H.h);
        if (parts) parts->part[NCP_MODE] = mode_checksum(FNV_BASIS);
    }
    if (parts) {
        parts->part[NCP_AI_SEED] = g_hook_net_ai_seed ? g_hook_net_ai_seed() : 0u;
        parts->total = H.h;
    }
    return H.h;
}

const char *const k_part_names[] = {
    "rng", "rng16", "pool", "pool_ext",
    "things.free", "things.c1", "things.scenery", "things.player", "things.c4", "things.creature", "things.c6",
    "things.c7", "things.c8", "things.projectile", "things.effect", "things.switch", "things.spell", "things.c13",
    "things.c14", "things.c15+",
    "player0", "player1", "player2", "player3", "player4", "player5", "player6", "player7",
    "map.type", "map.height", "map.light", "map.flags", "map.cells", "mode", "ai_seed",
};
static_assert(sizeof k_part_names / sizeof k_part_names[0] == NCP_COUNT);

constexpr uint32_t PARTS_MAGIC = 0x53545250u;   // "PRTS"
constexpr int PARTS_MSG_MAX = 16 + 4 * 400;     // a peer may carry up to 400 parts (one message < 0x800)

int encode_parts(uint8_t *out, uint32_t exchange, const NetChecksumParts &p) {
    put32(out, PARTS_MAGIC);
    put32(out + 4, exchange);
    put32(out + 8, (uint32_t)NCP_COUNT);
    put32(out + 12, p.total);
    for (int i = 0; i < NCP_COUNT; i++) put32(out + 16 + 4 * i, p.part[i]);
    return 16 + 4 * NCP_COUNT;
}
// Parts not in the message (an older peer) are taken as equal to ours.
bool decode_parts(const uint8_t *in, int len, uint32_t exchange, const NetChecksumParts &mine, NetChecksumParts *out) {
    if (len < 16 || get32(in) != PARTS_MAGIC || get32(in + 4) != exchange) return false;
    uint32_t count = get32(in + 8);
    if ((int)(16 + 4 * count) > len) return false;
    *out = mine;
    out->total = get32(in + 12);
    for (uint32_t i = 0; i < count && i < (uint32_t)NCP_COUNT; i++) out->part[i] = get32(in + 16 + 4 * i);
    return true;
}

// net_send_to_player / net_receive_from_player for one message of any length up to PARTS_MSG_MAX.
void send_msg(int p, const uint8_t *buf, int len) { net_send_to_player(p, buf, (uint32_t)len); }
int recv_msg(int p, uint8_t *buf) {
    NetContext &c = C();
    if (!c.present || c.status[p & 7] != 1) return -1;
    const int r = net_receive(c.ncb[p], buf);
    if (c.ncb[p].cplt != 0) { net_listen(p); return -1; }
    return r;
}

// One peer's half of the parts exchange with `peer` (exchange n): send first (client) or receive first (host).
void parts_exchange(uint32_t n, int peer, bool host) {
    NetContext &c = C();
    c.parts_done[peer & 7] = 1;
    NetChecksumParts mine{};
    if (c.parts_fn) c.parts_fn(&mine); else net_state_checksum_parts(&mine);
    uint8_t out[16 + 4 * NCP_COUNT];
    const int out_len = encode_parts(out, n, mine);
    static_assert(16 + 4 * NCP_COUNT <= PARTS_MSG_MAX);
    uint8_t in[0x800];
    int in_len = -1;
    if (host) { in_len = recv_msg(peer, in); send_msg(peer, out, out_len); }
    else      { send_msg(peer, out, out_len); in_len = recv_msg(peer, in); }
    NetChecksumParts theirs{};
    NetSyncStats &s = c.sync;
    s.parts_exchanges++;
    if (in_len < 0 || !decode_parts(in, in_len, n, mine, &theirs)) {
        std::fprintf(stderr, "net: DESYNC parts at exchange %u: no parts message from player %d\n", n, peer);
        return;
    }
    int count = 0;
    const int first = net_checksum_parts_first_diff(mine, theirs, &count);
    if (!c.have_parts) {
        c.have_parts = true;
        c.parts_local = mine;
        c.parts_remote = theirs;
        c.parts_peer = peer;
        c.parts_exchange = n;
        s.first_part = first;
        s.part_peer = peer;
        s.parts_differ = (uint32_t)count;
    }
    if (first < 0) {
        std::fprintf(stderr, "net: DESYNC parts at exchange %u with player %d: every part agrees (totals %08x / %08x)\n",
                     n, peer, mine.total, theirs.total);
    } else {
        std::fprintf(stderr, "net: DESYNC parts at exchange %u with player %d: first differing part %s (local %08x, "
                     "player %d %08x), %d part(s) differ:", n, peer, net_checksum_part_name(first), mine.part[first],
                     peer, theirs.part[first], count);
        for (int i = 0; i < NCP_COUNT; i++)
            if (mine.part[i] != theirs.part[i]) std::fprintf(stderr, " %s", net_checksum_part_name(i));
        std::fprintf(stderr, "\n");
    }
    if (c.dump_fn && !s.dumped && c.dump_fn(n, c.local)) s.dumped = 1;
}

} // namespace

uint32_t net_state_checksum() { return state_checksum(nullptr); }
void net_state_checksum_parts(NetChecksumParts *out) { if (out) state_checksum(out); }
const char *net_checksum_part_name(int part) { return part >= 0 && part < NCP_COUNT ? k_part_names[part] : "?"; }
int net_checksum_parts_first_diff(const NetChecksumParts &a, const NetChecksumParts &b, int *count) {
    int first = -1, n = 0;
    for (int i = 0; i < NCP_COUNT; i++) {
        if (a.part[i] == b.part[i]) continue;
        if (first < 0) first = i;
        n++;
    }
    if (count) *count = n;
    return first;
}
int net_checksum_parts_format(const NetChecksumParts &p, char *buf, size_t cap) {
    if (!buf || cap == 0) return 0;
    size_t at = std::strlen(buf);
    const size_t start = at;
    for (int i = 0; i < NCP_COUNT && at < cap; i++) {
        const int w = std::snprintf(buf + at, cap - at, " %s=%08x", k_part_names[i], (unsigned)p.part[i]);
        if (w < 0) break;
        at += (size_t)w;
        if (at >= cap) { at = cap - 1; break; }
    }
    return (int)(at - start);
}

void net_sync_set_interval(int every) { C().sync_every = every < 0 ? 0 : every; }
void net_sync_arm() {
    NetContext &c = C();
    c.sync_armed = true;
    c.sync = NetSyncStats{0, 0, 0, -1, 0, -1, -1, 0, 0, 0};
    std::memset(c.parts_done, 0, sizeof c.parts_done);
    c.have_parts = false;
    c.parts_peer = -1;
}
void net_sync_disarm() { C().sync_armed = false; }
void net_sync_set_checksum(uint32_t (*fn)()) { C().sync_fn = fn; }
void net_sync_set_parts(void (*fn)(NetChecksumParts *)) { C().parts_fn = fn; }
void net_sync_set_dump(bool (*fn)(uint32_t, int)) { C().dump_fn = fn; }
NetSyncStats net_sync_stats() { return C().sync; }
bool net_sync_last_parts(NetChecksumParts *local, NetChecksumParts *remote, int *remote_player, uint32_t *exchange) {
    const NetContext &c = C();
    if (!c.have_parts) return false;
    if (local) *local = c.parts_local;
    if (remote) *remote = c.parts_remote;
    if (remote_player) *remote_player = c.parts_peer;
    if (exchange) *exchange = c.parts_exchange;
    return true;
}

// net_exchange_frame_4f530(base, size)
void net_exchange_frame(void *base_v, int size) {
    NetContext &c = C();
    if (!c.present) return;
    uint8_t *base = static_cast<uint8_t *>(base_v);
    // port: the desync side channel (one extra 8-byte message per direction after the original's)
    bool sync = false;
    uint32_t n = 0, mine = 0;
    if (size == 10 && c.sync_armed && c.sync_every > 0) {
        n = c.sync.exchanges++;
        sync = (n % (uint32_t)c.sync_every) == 0;
        if (sync) { mine = c.sync_fn ? c.sync_fn() : net_state_checksum(); c.sync.last_local = mine; }
    }
    uint8_t msg[8], in[8];
    put32(msg, n);
    put32(msg + 4, mine);
    bool parts_with[8] = {};            // round 10: first mismatch with that peer -> parts exchange below
    if (c.local == c.host) {
        for (int b = 0; b < c.count; b++) {
            if (b == c.local) continue;
            net_receive_from_player(b, base + b * size, (uint32_t)size);
            if (sync && c.status[b] == 1) {
                std::memset(in, 0, sizeof in);
                net_receive_from_player(b, in, sizeof in);
                if (c.ncb[b].cplt == 0 && get32(in) == n && sync_compare(n, mine, get32(in + 4), b) && !c.parts_done[b & 7])
                    parts_with[b & 7] = true;
            }
        }
        for (int b = 0; b < c.count; b++) {
            if (b == c.local) continue;
            net_send_to_player(b, base, (uint32_t)(c.count * size));
            if (sync) net_send_to_player(b, msg, sizeof msg);
        }
        for (int b = 0; b < c.count && b < 8; b++)
            if (parts_with[b]) parts_exchange(n, b, true);
    } else {
        net_send_to_player(c.host, base + c.local * size, (uint32_t)size);
        if (sync) net_send_to_player(c.host, msg, sizeof msg);
        net_receive_from_player(c.host, base, (uint32_t)(c.count * size));
        if (sync && c.status[c.host] == 1) {
            std::memset(in, 0, sizeof in);
            net_receive_from_player(c.host, in, sizeof in);
            if (c.ncb[c.host].cplt == 0 && get32(in) == n && sync_compare(n, mine, get32(in + 4), c.host) &&
                !c.parts_done[c.host & 7])
                parts_exchange(n, c.host, false);
        }
    }
}

// net_exchange_block_4f620(base, size): player s = 0, 1, .. in turn sends its block to everybody
// (send_to skips itself: its status is 2), everybody else receives it from s.
void net_exchange_block(void *base_v, int size) {
    NetContext &c = C();
    if (!c.present) return;
    uint8_t *base = static_cast<uint8_t *>(base_v);
    for (int s = 0; s < c.count; s++) {
        if (s == c.local) {
            for (int b = 0; b < c.count; b++) net_send_to_player(b, base + c.local * size, (uint32_t)size);
        } else if (c.status[s] == 1) {
            net_receive_from_player(s, base + s * size, (uint32_t)size);
        }
    }
}

// Port only (round 8, net.h NetGameRules): agree on the simulation settings before a level starts.
NetGameRules net_local_rules() {
    NetGameRules r{};
    r.thing_slots = (uint32_t)thing_pool_wanted_slots();
    r.possession_range_pct = (uint32_t)gameplay_rules().possession_range_pct;
    r.mode = (uint32_t)gameplay_rules().mode;
    return r;
}

NetGameRules net_agree_rules(const NetGameRules &mine, bool *differs) {
    NetContext &c = C();
    if (differs) *differs = false;
    if (!c.present || !c.joined) return mine;
    NetGameRules v[8] = {};
    v[c.local & 7] = mine;
    net_exchange_block(v, (int)sizeof v[0]);
    const NetGameRules agreed = v[c.host & 7];
    for (int p = 0; p < c.count && p < 8; p++)
        if (c.status[p] != 0 && std::memcmp(&v[p], &agreed, sizeof agreed) != 0 && differs) *differs = true;
    return agreed;
}

static bool s_rules_applied = false;
void net_apply_rules(const NetGameRules &rules) {
    thing_pool_force_slots((int)rules.thing_slots);
    GameplayRules g;
    g.possession_range_pct = (int)rules.possession_range_pct;
    g.mode = (int)rules.mode;
    gameplay_force_rules(&g);
    s_rules_applied = true;
}
void net_release_rules() {
    if (!s_rules_applied) return;
    thing_pool_force_slots(0);
    gameplay_force_rules(nullptr);
    s_rules_applied = false;
}
