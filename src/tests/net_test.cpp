// Network test (port round 6, task B): net.h over two transports.
//  1. In-memory NetBIOS ("MemLan", below) with simulated peers in one process, one thread and one
//     NetContext each: the join of 3 peers (distinct player numbers, every pair connected, host 0), the
//     per-tick exchange (packet contents, the original's order: the host receives from every client,
//     then sends to all; a client sends, then receives), the 0x801-byte record exchange (chunked
//     messages), the desync side channel (an injected mismatch is seen by the host and by that client
//     only), a graceful leave of the host (net_player_disconnect on every peer: player 1 becomes host
//     and the game goes on), a crashed client (its slot stays empty, the host goes on), and an aborted
//     join (cancel hook) that leaves no name behind.
//  2. Two processes over the TCP transport (mcport/net_tcp.cpp): the test starts itself again with
//     --peer; both join the session "NETTEST", exchange the level choice as the lobby does (player 0's
//     wins), load that level with Config.flags 0x10 and run 2000 ticks with scripted mouse / key input
//     for their own player through player_local_input. Every tick each process records
//     net_state_checksum(); the desync side channel compares them online, the parent compares the files
//     afterwards: identical = pass.
//  3. Three processes, the host (player 0) leaves at tick 1200 with command 0x1d (Esc): player 1 becomes
//     the host, players 1 and 2 go on to tick 2000 in lockstep.
//  4. The lobby screen (frontend.cpp) joining a session of two through the in-memory transport as player
//     1 (the other player is there first and answers slowly): the frame-stepped join, the slot
//     animations (slot 0 blinks while the call is pending), the level exchange (player 0's choice wins),
//     FE_START_LEVEL; PPMs of the lobby while joining and when both slots are connected (next to the
//     executable: net_lobby_joining_a/b.ppm, net_lobby_joined.ppm).
//  5. The config screen's sound summary with a sndsetup.inf / .dat pair in the save directory: the card
//     names and the I/O / IRQ / DMA / music port lines (texts 23..26); net_config_*.ppm.
// Order of the run: 1, 4, 5, 2, 3. argv[1] = game dir.
#define _CRT_SECURE_NO_WARNINGS
#include "net.h"
#include "net_tcp.h"
#include "frontend.h"
#include "text.h"
#include "palette_fx.h"
#include "sim.h"
#include "player.h"
#include "input.h"
#include "thing.h"
#include "settings.h"
#include "mc_globals.h"
#include "crash_handler.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

static std::atomic<int> g_fail{0};
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)
#define CHECK_EQ(a, b) do { long long _a = (long long)(a), _b = (long long)(b); if (_a != _b) { \
    std::printf("FAIL %s:%d: %s == %s (%lld != %lld)\n", __FILE__, __LINE__, #a, #b, _a, _b); g_fail++; } } while (0)

static std::string g_exe, g_game, g_outdir;

// =====================================================================================================
// MemLan: an in-memory NetBIOS for simulated peers (one thread each). Same semantics as the TCP
// transport: names, LISTEN / CALL with the "smaller name wins" rule for crossing calls, message
// sessions, hang-up, cancel. Completions happen in the owner's poll().
// =====================================================================================================
struct MemTransport;
struct MemMsg {
    enum Kind { CALL, ACK, DATA, CLOSE } kind;
    int from = 0;
    uint64_t op = 0;
    std::string caller, callee;
    uint8_t lsn = 0, code = 0, peer_lsn = 0;
    std::vector<uint8_t> data;
};
struct MemLan {
    std::mutex mu;
    std::condition_variable cv;
    std::map<std::string, int> names;        // name -> peer id
    std::vector<MemTransport *> peers;
};

static std::string key15(const char *n) {
    char b[16];
    std::memset(b, ' ', 15);
    for (int i = 0; i < 15 && n[i]; i++) b[i] = n[i];
    return std::string(b, 15);
}

struct MemTransport : NetTransport {
    struct Pend { NetNcb *ncb; int kind; uint64_t op; };   // kind 0 listen, 1 call, 2 receive
    struct Sess { int peer = -1; uint8_t peer_lsn = 0; std::string local, remote; std::deque<std::vector<uint8_t>> q; bool closed = false, open = false; };
    MemLan *lan;
    int id;
    bool crashed = false;
    std::vector<Pend> pend;
    std::map<uint8_t, Sess> sess;
    uint8_t next_lsn = 1;
    uint64_t next_op = 1;
    std::vector<std::string> names;
    std::deque<MemMsg> box;                   // guarded by lan->mu
    std::vector<std::string> log;             // "S<digit>" / "R<digit>" of the remote name, for order checks
    bool logging = false;

    explicit MemTransport(MemLan *l) : lan(l) {
        std::lock_guard<std::mutex> g(lan->mu);
        id = (int)lan->peers.size();
        lan->peers.push_back(this);
    }
    void post(int to, MemMsg m) {             // lan->mu held
        lan->peers[(size_t)to]->box.push_back(std::move(m));
        lan->cv.notify_all();
    }
    void drop(NetNcb *n) {
        for (size_t i = 0; i < pend.size(); i++)
            if (pend[i].ncb == n) { pend.erase(pend.begin() + (long)i); return; }
    }
    bool pending(NetNcb *n) { for (auto &p : pend) if (p.ncb == n) return true; return false; }
    void finish(uint64_t op, uint8_t code) {
        for (size_t i = 0; i < pend.size(); i++)
            if (pend[i].op == op) { pend[i].ncb->cplt = code; pend.erase(pend.begin() + (long)i); return; }
    }
    uint8_t new_lsn() {
        for (int i = 0; i < 254; i++) {
            uint8_t l = next_lsn;
            next_lsn = (uint8_t)(next_lsn == 254 ? 1 : next_lsn + 1);
            if (!sess.count(l)) return l;
        }
        return 0;
    }
    char digit(const std::string &name) {
        for (int i = 14; i >= 0; i--) if (name[(size_t)i] != ' ') return name[(size_t)i];
        return '?';
    }
    bool present() override { return !crashed; }
    int submit(NetNcb *n) override {
        if (crashed) return -1;
        drop(n);
        const uint64_t op = next_op++;
        std::lock_guard<std::mutex> g(lan->mu);
        switch (n->cmd) {
        case NCB_INVALID: n->retcode = NRC_ILLCMD; n->cplt = NRC_ILLCMD; return 0;
        case NCB_ADD_NAME: {
            const std::string k = key15(n->name);
            for (auto &x : names) if (x == k) { n->cplt = NRC_DUPNAME; return 0; }
            if (lan->names.count(k)) { n->cplt = NRC_INUSE; return 0; }
            lan->names[k] = id;
            names.push_back(k);
            n->cplt = 0;
            return 0;
        }
        case NCB_DELETE_NAME: {
            const std::string k = key15(n->name);
            for (size_t i = 0; i < names.size(); i++) if (names[i] == k) { names.erase(names.begin() + (long)i); lan->names.erase(k); break; }
            n->cplt = 0;
            return 0;
        }
        case NCB_LISTEN: pend.push_back(Pend{n, 0, op}); n->cplt = NRC_PENDING; return 0;
        case NCB_CALL: {
            auto it = lan->names.find(key15(n->callname));
            if (it == lan->names.end()) { n->cplt = NRC_NOCALL; return 0; }
            const uint8_t l = new_lsn();
            Sess &s = sess[l];
            s.local = key15(n->name);
            s.remote = key15(n->callname);
            s.peer = it->second;
            pend.push_back(Pend{n, 1, op});
            n->cplt = NRC_PENDING;
            n->lsn = l;
            MemMsg m{MemMsg::CALL};
            m.from = id; m.op = op; m.caller = s.local; m.callee = s.remote; m.lsn = l;
            post(it->second, std::move(m));
            return 0;
        }
        case NCB_HANGUP: {
            auto it = sess.find(n->lsn);
            if (it == sess.end() || n->lsn == 0) { n->cplt = 0x08; return 0; }
            if (it->second.open && !it->second.closed) { MemMsg m{MemMsg::CLOSE}; m.lsn = it->second.peer_lsn; post(it->second.peer, std::move(m)); }
            sess.erase(it);
            n->cplt = 0;
            return 0;
        }
        case NCB_SEND: {
            auto it = sess.find(n->lsn);
            if (it == sess.end() || n->lsn == 0 || !it->second.open) { n->cplt = 0x08; return 0; }
            if (it->second.closed) { n->cplt = NRC_SCLOSED; return 0; }
            if (logging) log.push_back(std::string("S") + digit(it->second.remote));
            MemMsg m{MemMsg::DATA};
            m.lsn = it->second.peer_lsn;
            m.data.assign(n->buffer, n->buffer + n->length);
            post(it->second.peer, std::move(m));
            n->cplt = 0;
            return 0;
        }
        case NCB_RECEIVE: {
            auto it = sess.find(n->lsn);
            if (it == sess.end() || n->lsn == 0 || !it->second.open) { n->cplt = 0x08; return 0; }
            if (logging) log.push_back(std::string("R") + digit(it->second.remote));
            pend.push_back(Pend{n, 2, op});
            n->cplt = NRC_PENDING;
            deliver_all();
            return 0;
        }
        case NCB_CANCEL: {
            NetNcb *t = n->cancel_target;
            if (t && pending(t)) {
                for (auto &p : pend) if (p.ncb == t && p.kind == 1) { auto it = sess.find(t->lsn); if (it != sess.end() && !it->second.open) sess.erase(it); }
                drop(t);
                t->cplt = NRC_CMDCAN;
                n->cplt = 0;
            } else n->cplt = 0x24;
            return 0;
        }
        default: n->cplt = NRC_ILLCMD; return 0;
        }
    }
    bool calling(const std::string &local, const std::string &remote) {
        for (auto &p : pend) if (p.kind == 1 && key15(p.ncb->name) == local && key15(p.ncb->callname) == remote) return true;
        return false;
    }
    bool has_session(const std::string &local, const std::string &remote) {
        for (auto &kv : sess) if (kv.second.open && !kv.second.closed && kv.second.local == local && kv.second.remote == remote) return true;
        return false;
    }
    void deliver_all() {
        for (size_t i = 0; i < pend.size();) {
            Pend &p = pend[i];
            if (p.kind == 2) {
                auto it = sess.find(p.ncb->lsn);
                if (it == sess.end()) { p.ncb->cplt = 0x08; pend.erase(pend.begin() + (long)i); continue; }
                Sess &s = it->second;
                if (!s.q.empty()) {
                    std::vector<uint8_t> m = std::move(s.q.front());
                    s.q.pop_front();
                    size_t len = m.size();
                    uint8_t code = 0;
                    if (len > p.ncb->length) { len = p.ncb->length; code = NRC_INCOMP; }
                    std::memcpy(p.ncb->buffer, m.data(), len);
                    p.ncb->length = (uint16_t)len;
                    p.ncb->cplt = code;
                    pend.erase(pend.begin() + (long)i);
                    continue;
                }
                if (s.closed) { p.ncb->cplt = NRC_SCLOSED; pend.erase(pend.begin() + (long)i); continue; }
            }
            i++;
        }
    }
    void poll(int timeout_ms) override {
        std::deque<MemMsg> in;
        {
            std::unique_lock<std::mutex> lk(lan->mu);
            if (box.empty() && timeout_ms > 0)
                lan->cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&] { return !box.empty(); });
            in.swap(box);
        }
        for (MemMsg &m : in) {
            std::lock_guard<std::mutex> g(lan->mu);
            switch (m.kind) {
            case MemMsg::CALL: {
                Pend *listen = nullptr;
                for (auto &p : pend)
                    if (p.kind == 0 && key15(p.ncb->name) == m.callee && (p.ncb->callname[0] == '*' || key15(p.ncb->callname) == m.caller)) { listen = &p; break; }
                bool ok = listen && !crashed && !has_session(m.callee, m.caller);
                if (ok && calling(m.callee, m.caller) && !(m.caller < m.callee)) ok = false;
                MemMsg a{MemMsg::ACK};
                a.op = m.op; a.lsn = m.lsn;
                if (!ok) { a.code = NRC_REJECTED; post(m.from, std::move(a)); break; }
                const uint8_t l = new_lsn();
                Sess &s = sess[l];
                s.peer = m.from; s.peer_lsn = m.lsn; s.local = m.callee; s.remote = m.caller; s.open = true;
                a.code = 0; a.peer_lsn = l;
                post(m.from, std::move(a));
                listen->ncb->lsn = l;
                std::memcpy(listen->ncb->callname, m.caller.data(), 15);
                finish(listen->op, 0);
                break;
            }
            case MemMsg::ACK: {
                auto it = sess.find(m.lsn);
                bool want = false;
                for (auto &p : pend) if (p.op == m.op) want = true;
                if (m.code != 0 || !want) {
                    const int peer = it != sess.end() ? it->second.peer : -1;
                    if (it != sess.end() && !it->second.open) sess.erase(it);
                    if (m.code == 0 && peer >= 0) {   // cancelled meanwhile: hang the new session up again
                        MemMsg c{MemMsg::CLOSE};
                        c.lsn = m.peer_lsn;
                        post(peer, std::move(c));
                    }
                    finish(m.op, m.code);
                    break;
                }
                if (it != sess.end()) { it->second.peer_lsn = m.peer_lsn; it->second.open = true; }
                finish(m.op, 0);
                break;
            }
            case MemMsg::DATA: {
                auto it = sess.find(m.lsn);
                if (it != sess.end()) it->second.q.push_back(std::move(m.data));
                break;
            }
            case MemMsg::CLOSE: {
                auto it = sess.find(m.lsn);
                if (it != sess.end()) it->second.closed = true;
                break;
            }
            }
        }
        deliver_all();
    }
    // The process of this peer dies: names gone, every session closed for the others.
    void crash() {
        std::lock_guard<std::mutex> g(lan->mu);
        crashed = true;
        for (auto &n : names) lan->names.erase(n);
        names.clear();
        for (auto &kv : sess) if (kv.second.open && !kv.second.closed) { MemMsg m{MemMsg::CLOSE}; m.lsn = kv.second.peer_lsn; post(kv.second.peer, std::move(m)); }
        sess.clear();
        for (auto &p : pend) p.ncb->cplt = NRC_CMDCAN;
        pend.clear();
    }
};

// =====================================================================================================
// part 1: simulated peers
// =====================================================================================================
static uint8_t pattern(int round, int p, int i) { return (uint8_t)(round * 31 + p * 7 + i + 1); }

struct Barrier {
    std::mutex mu; std::condition_variable cv; int n, count = 0, gen = 0;
    explicit Barrier(int k) : n(k) {}
    void wait() {
        std::unique_lock<std::mutex> lk(mu);
        const int g = gen;
        if (++count == n) { count = 0; gen++; cv.notify_all(); return; }
        cv.wait(lk, [&] { return gen != g; });
    }
    void shrink() { std::lock_guard<std::mutex> g(mu); n--; if (count >= n && n > 0) { count = 0; gen++; cv.notify_all(); } }
};

static std::atomic<int> g_cancel_a{0}, g_cancel_b{0};
static thread_local uint32_t t_fake_crc = 0;
static uint32_t fake_crc() { return t_fake_crc; }

static void part1_memory() {
    std::printf("== part 1: in-memory NetBIOS, 3 simulated peers\n");
    MemLan lan;
    const int N = 3;
    MemTransport *tr[N];
    for (int i = 0; i < N; i++) tr[i] = new MemTransport(&lan);
    int local[N] = {-9, -9, -9};
    int host_after[N] = {-9, -9, -9};
    NetSyncStats stats[N];
    std::vector<std::string> order[N];
    Barrier bar(N);
    std::atomic<int> errors{0};

    auto peer = [&](int t) {
        NetContext *ctx = net_context_create();
        net_context_select(ctx);
        net_set_transport(tr[t]);
        if (net_init() != 1) { errors++; return; }
        // stagger the start a little: the first to add its name becomes player 0
        std::this_thread::sleep_for(std::chrono::milliseconds(20 * t));
        const int me = net_session_join("TEST", N);
        local[t] = me;
        if (me < 0) { errors++; return; }
        bar.wait();
        // the 0x801 records after the join (chunked: 0x800 + 1, the host sends 3 * 0x801)
        std::vector<uint8_t> rec(8 * 0x801, 0);
        for (int i = 0; i < 0x801; i++) rec[(size_t)(me * 0x801 + i)] = pattern(99, me, i);
        net_exchange_frame(rec.data(), 0x801);
        for (int p = 0; p < N; p++)
            for (int i = 0; i < 0x801; i++)
                if (rec[(size_t)(p * 0x801 + i)] != pattern(99, p, i)) { errors++; p = N; break; }
        // 50 rounds of command packets, desync check armed, one injected mismatch by player 2 at round 20
        net_sync_set_checksum(fake_crc);
        net_sync_arm();
        uint8_t buf[80];
        for (int round = 0; round < 50; round++) {
            t_fake_crc = 0x1234u + (uint32_t)round + ((me == 2 && round == 20) ? 1u : 0u);
            std::memset(buf, 0, sizeof buf);
            for (int i = 0; i < 10; i++) buf[me * 10 + i] = pattern(round, me, i);
            tr[t]->logging = round == 7;
            net_exchange_frame(buf, 10);
            tr[t]->logging = false;
            for (int p = 0; p < N; p++)
                for (int i = 0; i < 10; i++) if (buf[p * 10 + i] != pattern(round, p, i)) { errors++; p = N; break; }
        }
        order[t] = tr[t]->log;
        stats[t] = net_sync_stats();
        net_sync_disarm();
        bar.wait();
        // the host (player 0) leaves: every peer processes its command 0x1d / 2 the same tick
        net_player_disconnect(0);
        host_after[t] = net_host();
        if (me == 0) { bar.shrink(); net_context_select(nullptr); net_context_destroy(ctx); return; }
        for (int round = 50; round < 60; round++) {
            std::memset(buf, 0, sizeof buf);
            for (int i = 0; i < 10; i++) buf[me * 10 + i] = pattern(round, me, i);
            net_exchange_frame(buf, 10);
            for (int i = 0; i < 10; i++) {
                if (buf[i] != 0) { errors++; break; }                                   // player 0 is gone
                if (buf[10 + i] != pattern(round, 1, i) || buf[20 + i] != pattern(round, 2, i)) { errors++; break; }
            }
        }
        bar.wait();
        // player 2 crashes (no disconnect); the host notices only through the failing session
        if (me == 2) { tr[t]->crash(); net_context_select(nullptr); net_context_destroy(ctx); return; }
        for (int round = 60; round < 65; round++) {
            std::memset(buf, 0, sizeof buf);
            for (int i = 0; i < 10; i++) buf[me * 10 + i] = pattern(round, me, i);
            net_exchange_frame(buf, 10);
            for (int i = 0; i < 10; i++) if (buf[10 + i] != pattern(round, 1, i) || buf[20 + i] != 0) { errors++; break; }
        }
        net_context_select(nullptr);
        net_context_destroy(ctx);
    };
    std::vector<std::thread> th;
    for (int t = 0; t < N; t++) th.emplace_back(peer, t);
    for (auto &x : th) x.join();
    CHECK_EQ(errors.load(), 0);
    int seen = 0;
    for (int t = 0; t < N; t++) if (local[t] >= 0 && local[t] < N) seen |= 1 << local[t];
    CHECK_EQ(seen, 7);
    std::printf("  join: player numbers %d %d %d\n", local[0], local[1], local[2]);
    for (int t = 0; t < N; t++) {
        const int me = local[t];
        std::string s;
        for (auto &x : order[t]) s += x + " ";
        std::printf("  player %d exchange order: %s| sync checks %u mismatches %u first %d\n", me, s.c_str(),
                    stats[t].checks, stats[t].mismatches, stats[t].first_mismatch);
        if (me == 0) {
            CHECK(s == "R1 R1 R2 R2 S1 S1 S2 S2 ");            // packet + sync message per client
            CHECK_EQ(stats[t].checks, 100);
            CHECK_EQ(stats[t].mismatches, 1);
            CHECK_EQ(stats[t].first_mismatch, 20);
        } else {
            CHECK(s == "S0 S0 R0 R0 ");
            CHECK_EQ(stats[t].checks, 50);
            CHECK_EQ(stats[t].mismatches, me == 2 ? 1 : 0);
        }
        CHECK_EQ(host_after[t], me == 0 ? 0 : 1);         // the one who left only has itself
    }
    for (int i = 0; i < N; i++) delete tr[i];

    // an aborted join: two of three players; one cancels, then the other
    std::printf("  aborted join\n");
    MemLan lan2;
    MemTransport a(&lan2), b(&lan2);
    int ra = -9, rb = -9;
    std::thread ta([&] {
        NetContext *ctx = net_context_create(); net_context_select(ctx);
        net_set_transport(&a); net_init();
        NetLobbyHooks h;
        h.cancel_requested = [] { return g_cancel_a.load() != 0; };
        net_set_lobby_hooks(h);
        ra = net_session_join("ABORT", 3);
        net_context_select(nullptr); net_context_destroy(ctx);
    });
    std::thread tb([&] {
        NetContext *ctx = net_context_create(); net_context_select(ctx);
        net_set_transport(&b); net_init();
        NetLobbyHooks h;
        h.cancel_requested = [] { return g_cancel_b.load() != 0; };
        net_set_lobby_hooks(h);
        rb = net_session_join("ABORT", 3);
        net_context_select(nullptr); net_context_destroy(ctx);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    g_cancel_a = 1;
    ta.join();
    g_cancel_b = 1;
    tb.join();
    CHECK_EQ(ra, -1);
    CHECK_EQ(rb, -1);
    {
        std::lock_guard<std::mutex> g(lan2.mu);
        CHECK_EQ(lan2.names.size(), 0);                    // both names deleted again
    }
}

// =====================================================================================================
// part 2 / 3: processes over TCP, the real simulation
// =====================================================================================================
static void script_input(int tick, int me) {
    const int ph = tick + me * 37;
    const int dx = (((ph / 50) & 1) ? 1 : -1) * ((ph * 7) % 120);
    const int dy = ((ph / 80) % 3 - 1) * 40;
    input_mouse_move(320 + dx, 200 + dy);
    if (ph % 45 == 0) input_mouse_button(0, true);
    if (ph % 45 == 3) input_mouse_button(0, false);
    if (ph % 70 == 10) input_mouse_button(1, true);
    if (ph % 70 == 12) input_mouse_button(1, false);
    if (ph % 300 == 0) input_key_event(MC_SC_UP | MC_SC_EXT, true);
    if (ph % 300 == 150) input_key_event(MC_SC_UP | MC_SC_EXT, false);
    if (ph % 400 == 399) { input_key_event(MC_SC_SPACE, true); }
    if (ph % 400 == 0 && tick > 0) { input_key_event(MC_SC_SPACE, false); }
}

// One player of a network game: join, lobby level exchange, the level, `ticks` ticks. Writes one line
// per tick ("tick crc") to `out`. leave_tick >= 0: the local player leaves the game then (command 0x1d).
static int run_player(TcpTransport *tr, int players, int ticks, int leave_tick, const char *out) {
    net_set_transport(tr);
    if (net_init() != 1) { std::printf("  peer: no network (%s)\n", tr->last_error()); return 2; }
    const int me = net_session_join("NETTEST", players);
    if (me < 0) { std::printf("  peer: join failed\n"); return 2; }
    // fe_screen_multiplayer_54bd0 after the join: count, flags, the level choice (player 0's wins)
    g_state->player_count = (int16_t)players;
    g_state->local_player = (int16_t)me;
    // the lobby's name dialog: Config+0x1d "Enter your name", +0x3d "Enter your call-name"
    std::snprintf(g_cfg->save_str_a, sizeof g_cfg->save_str_a, "Player %d", me);
    std::snprintf(g_cfg->save_str_b, sizeof g_cfg->save_str_b, "CALL%d", me);
    g_cfg->flags |= 0x10;
    g_state->commands[me].arg = (uint8_t)(50 + me);
    net_exchange_frame(g_state->commands, 10);
    const int level = (int8_t)g_state->commands[0].arg;
    if (level != 50) { std::printf("  peer %d: level %d, expected player 0's 50\n", me, level); return 2; }
    g_cfg->level = (uint16_t)level;
    // the lobby's rules exchange (frontend.cpp, net.h NetGameRules): player 0 (the host) plays with an
    // extended pool and a longer Possession, the others with the original's - everybody adopts the host's
    NetGameRules mine{};
    mine.thing_slots = me == 0 ? 3000u : 1000u;
    mine.possession_range_pct = me == 0 ? 150u : 100u;
    bool differs = false;
    const NetGameRules agreed = net_agree_rules(mine, &differs);
    if (agreed.thing_slots != 3000 || agreed.possession_range_pct != 150 || (players > 1 && !differs)) {
        std::printf("  peer %d: rules %u / %u (differs %d), expected the host's 3000 / 150\n", me, agreed.thing_slots,
                    agreed.possession_range_pct, (int)differs);
        return 2;
    }
    net_apply_rules(agreed);
    input_reset();
    if (!sim_load_level(level)) { std::printf("  peer %d: level %d not loaded\n", me, level); return 2; }
    input_mouse_center();
    net_sync_arm();
    FILE *f = std::fopen(out, "w");
    if (!f) return 2;
    int t = 0;
    for (; t < ticks; t++) {
        g_timer_ticks = (uint32_t)(t * 5 + me * 1000);     // real time differs per machine: kept out of the checksum
        script_input(t, me);
        if (t == leave_tick) player_queue_command(0x1d, 0);
        game_tick_sim();
        std::fprintf(f, "%d %08x\n", t, net_state_checksum());
        if (const char *d = std::getenv("MC_NET_DUMP")) {               // diagnosis: the raw state after tick d
            if (std::atoi(d) == t) {
                FILE *g = std::fopen((std::string(out) + ".gam").c_str(), "wb");
                if (g) {
                    std::fwrite(g_state, 1, sizeof(GameState), g);
                    std::fwrite(g_map_type, 1, MC_MAP_CELLS, g); std::fwrite(g_map_height, 1, MC_MAP_CELLS, g);
                    std::fwrite(g_map_light, 1, MC_MAP_CELLS, g); std::fwrite(g_map_flags, 1, MC_MAP_CELLS, g);
                    std::fwrite(g_cell_things, 2, MC_MAP_CELLS, g);
                    std::fclose(g);
                }
            }
        }
        if (g_state->players[me].status & 8) { t++; break; }       // left the game
    }
    const NetSyncStats s = net_sync_stats();
    std::fprintf(f, "end %d sync %u %u %d\n", t, s.checks, s.mismatches, s.first_mismatch);
    std::fclose(f);
    // one write, so the lines of the processes do not interleave
    std::string msg;
    char line[256];
    std::snprintf(line, sizeof line, "  player %d: %d ticks, %u checksums compared online, %u mismatches, host now %d, %d live things\n",
                  me, t, s.checks, s.mismatches, net_host(), 999 - (g_state->free_top + 1));
    msg += line;
    for (int p = 0; p < players; p++) {                 // what the scripted input did
        const PlayerRec &r = g_state->players[p];
        const Thing *pt = thing_at(r.thing % MC_THING_SLOTS);
        std::snprintf(line, sizeof line, "    as seen by %d: player %d active %d status %d at %04x,%04x,%d health %d mana %d\n", me, p,
                      r.active, r.status, (unsigned)pt->x, (unsigned)pt->y, (int)pt->z, (int)pt->health, (int)pt->mana);
        msg += line;
    }
    std::fwrite(msg.data(), 1, msg.size(), stdout);
    std::fflush(stdout);
    return 0;
}

#ifdef _WIN32
static HANDLE spawn_peer(int players, int port, int ticks, int leave, const std::string &out) {
    char cmd[4096];
    std::snprintf(cmd, sizeof cmd, "\"%s\" \"%s\" --peer %d %d %d %d \"%s\"", g_exe.c_str(), g_game.c_str(),
                  players, port, ticks, leave, out.c_str());
    STARTUPINFOA si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessA(nullptr, cmd, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) return nullptr;
    CloseHandle(pi.hThread);
    return pi.hProcess;
}
#endif

static int free_port() {
    // a TcpTransport on port 0 is not possible (it is the name server's port): probe from the high range
    static int next = 30600 + (int)(std::chrono::steady_clock::now().time_since_epoch().count() % 2000);
    return next++;
}

static bool read_crcs(const std::string &path, std::vector<std::string> &lines, std::string &end) {
    FILE *f = std::fopen(path.c_str(), "r");
    if (!f) return false;
    char buf[256];
    while (std::fgets(buf, sizeof buf, f)) {
        std::string s(buf);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        if (s.rfind("end", 0) == 0) end = s; else lines.push_back(s);
    }
    std::fclose(f);
    return true;
}

static void part_tcp(int players, int ticks, int host_leave) {
    std::printf("== part %d: %d processes over TCP, %d ticks%s\n", players == 2 ? 2 : 3, players, ticks,
                host_leave >= 0 ? ", the host leaves" : "");
#ifndef _WIN32
    std::printf("  SKIP: process spawning is implemented for Windows only\n");
    return;
#else
    NetContext *ctx = net_context_create();
    NetContext *prev = net_context_select(ctx);
    int port = 0;
    TcpTransport *tr = nullptr;
    for (int tries = 0; tries < 20; tries++) {
        port = free_port();
        tr = new TcpTransport("", (uint16_t)port);
        if (tr->present() && tr->is_name_server()) break;
        delete tr;
        tr = nullptr;
    }
    if (!tr) { std::printf("FAIL: no free TCP port for the name server\n"); g_fail++; return; }
    std::vector<std::string> files;
    std::vector<HANDLE> kids;
    for (int i = 0; i < players; i++) files.push_back(g_outdir + "/net_peer" + std::to_string(players) + "_" + std::to_string(i) + ".txt");
    for (int i = 1; i < players; i++) {
        HANDLE h = spawn_peer(players, port, ticks, -1, files[(size_t)i]);
        CHECK(h != nullptr);
        if (h) kids.push_back(h);
    }
    const auto t0 = std::chrono::steady_clock::now();
    const int r = run_player(tr, players, ticks, host_leave, files[0].c_str());
    CHECK_EQ(r, 0);
    for (HANDLE h : kids) {
        const DWORD w = WaitForSingleObject(h, 180000);
        DWORD code = 99;
        if (w == WAIT_OBJECT_0) GetExitCodeProcess(h, &code);
        else TerminateProcess(h, 98);
        CHECK_EQ(code, 0);
        CloseHandle(h);
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    net_shutdown();
    delete tr;
    net_context_select(prev);
    net_context_destroy(ctx);
    // the files: one checksum per tick and player; every player that played tick t has the same one
    std::vector<std::vector<std::string>> crc((size_t)players);
    std::vector<std::string> ends((size_t)players);
    for (int i = 0; i < players; i++) CHECK(read_crcs(files[(size_t)i], crc[(size_t)i], ends[(size_t)i]));
    int compared = 0, differ = 0, first = -1;
    for (int t = 0; t < ticks; t++) {
        const std::string *ref = nullptr;
        for (int i = 0; i < players; i++) {
            if ((size_t)t >= crc[(size_t)i].size()) continue;
            if (!ref) { ref = &crc[(size_t)i][(size_t)t]; continue; }
            compared++;
            if (*ref != crc[(size_t)i][(size_t)t]) { differ++; if (first < 0) first = t; }
        }
    }
    for (int i = 0; i < players; i++) std::printf("  player %d file: %zu ticks, %s\n", i, crc[(size_t)i].size(), ends[(size_t)i].c_str());
    std::printf("  %d per-tick checksum pairs compared, %d differ (first at tick %d), %.1f s\n", compared, differ, first, secs);
    {
        std::vector<std::string> v = crc[1];
        std::sort(v.begin(), v.end());
        const size_t distinct = (size_t)(std::unique(v.begin(), v.end()) - v.begin());
        std::printf("  player 1: %zu distinct checksums in %zu ticks (the state changes every tick)\n", distinct, crc[1].size());
        CHECK(distinct + 10 >= crc[1].size());
    }
    CHECK_EQ(differ, 0);
    if (host_leave < 0) for (int i = 0; i < players; i++) CHECK_EQ(crc[(size_t)i].size(), (size_t)ticks);
    else {
        CHECK_EQ(crc[0].size(), (size_t)host_leave + 1);
        for (int i = 1; i < players; i++) CHECK_EQ(crc[(size_t)i].size(), (size_t)ticks);
    }
    CHECK(compared >= ticks * (players - 1) - (host_leave >= 0 ? ticks - host_leave : 0));
    // the online side channel agrees: no mismatch anywhere
    for (int i = 0; i < players; i++) {
        unsigned c = 0, m = 0; int fm = 0, n = 0;
        if (std::sscanf(ends[(size_t)i].c_str(), "end %d sync %u %u %d", &n, &c, &m, &fm) == 4) { CHECK_EQ(m, 0); CHECK(c > 0); }
        else CHECK(false);
    }
#endif
}

// =====================================================================================================
// part 4: the lobby screen
// =====================================================================================================
static uint8_t g_fb[320 * 200];
static uint32_t g_tick = 1000;
static FeResult g_last = FE_CONTINUE;
static int g_level_out = -1;
static FeResult frame() { g_tick += 2; g_last = fe_frame(FrameBuffer{g_fb, 320, 200}, g_tick, &g_level_out); return g_last; }

static void write_ppm(const char *name) {
    const std::string path = g_outdir + "/net_" + name + ".ppm";
    FILE *f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    std::fprintf(f, "P6\n320 200\n255\n");
    const uint8_t *px = fe_screen_pixels(), *pal = fe_palette6();
    for (int i = 0; i < 64000; i++) {
        const uint8_t *c = pal + px[i] * 3;
        const uint8_t rgb[3] = {(uint8_t)(c[0] << 2 | c[0] >> 4), (uint8_t)(c[1] << 2 | c[1] >> 4), (uint8_t)(c[2] << 2 | c[2] >> 4)};
        std::fwrite(rgb, 1, 3, f);
    }
    std::fclose(f);
    std::printf("  wrote %s\n", path.c_str());
}

static void part4_lobby() {
    std::printf("== part 4: the lobby joining a session of two (in-memory transport)\n");
    MemLan lan;
    MemTransport mine(&lan), other(&lan);
    NetContext *ctx = net_context_create();
    NetContext *prev = net_context_select(ctx);
    net_set_transport(&mine);
    CHECK_EQ(net_init(), 1);
    g_fe_network = 1;
    if (!fe_init(g_game.c_str(), 4)) { std::printf("  SKIP: front-end resources missing\n"); net_context_select(prev); return; }
    input_reset();
    for (int i = 0; i < 200; i++) frame();
    CHECK_EQ(fe_state(), 4);
    // The other player is there first (player 0, "CARPET00") and polls its NetBIOS slowly (an idle hook
    // of 300 ms), so this lobby's CALL to it stays pending for a while: slot 0 blinks (connecting).
    int other_local = -9, other_level = -9;
    NetGameRules other_rules{};
    std::thread th([&] {
        NetContext *c2 = net_context_create();
        net_context_select(c2);
        net_set_transport(&other);
        net_init();
        NetLobbyHooks h;
        h.idle = [] { std::this_thread::sleep_for(std::chrono::milliseconds(300)); };
        net_set_lobby_hooks(h);
        other_local = net_session_join("CARPET0", 2);
        uint8_t cmds[80] = {};
        if (other_local >= 0) {
            cmds[other_local * 10 + 1] = 0x33;               // player 0 chose level 51: everybody plays it
            net_exchange_frame(cmds, 10);
            other_level = (int8_t)cmds[other_local * 10 + 1];
            NetGameRules r{};                                // the host's rules: everybody adopts them
            r.thing_slots = 4000; r.possession_range_pct = 140;
            other_rules = net_agree_rules(r, nullptr);
        }
        net_context_select(nullptr);
        net_context_destroy(c2);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    // Start (0x11c..0x12f x 0x7e..0x91 in 320 space)
    input_mouse_move(0x125 * 2, 0x87 * 2);
    frame();
    input_mouse_button(0, true);
    frame();
    input_mouse_button(0, false);
    bool wrote_joined = false;
    int frames = 0;
    for (; frames < 3000 && g_last != FE_START_LEVEL; frames++) {
        frame();
        if (frames == 10) write_ppm("lobby_joining_a");          // 16 progress callbacks apart: the two blink phases
        if (frames == 26) write_ppm("lobby_joining_b");
        if (!wrote_joined && net_joined()) { write_ppm("lobby_joined"); wrote_joined = true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    th.join();
    CHECK_EQ(g_last, FE_START_LEVEL);
    CHECK(wrote_joined);
    CHECK_EQ(other_local, 0);
    CHECK_EQ(g_state->local_player, 1);
    CHECK_EQ(g_level_out, 51);                             // player 0's choice wins over this lobby's 50
    CHECK_EQ(other_level, 51);
    CHECK(g_cfg->flags & 0x10);
    CHECK_EQ(g_state->player_count, 2);
    CHECK_EQ(net_player_status(0), 1);
    CHECK_EQ(net_player_status(1), 2);
    CHECK_EQ(net_host(), 0);
    CHECK_EQ(other_rules.thing_slots, 4000u);              // the lobby (1000 / 100 here) took the host's rules
    CHECK_EQ(thing_pool_wanted_slots(), 4000);
    CHECK_EQ(gameplay_rules().possession_range_pct, 140);
    std::printf("  joined after %d frames: local %d, other %d, level %d\n", frames, g_state->local_player, other_local, g_level_out);
    // the exit fade, then the front end has left
    for (int i = 0; i < 60; i++) frame();
    fe_shutdown();
    g_cfg->flags &= ~0x10;
    g_state->local_player = 0;
    net_shutdown();
    CHECK_EQ(thing_pool_wanted_slots(), 1000);             // released with the session
    CHECK_EQ(gameplay_rules().possession_range_pct, 100);
    net_context_select(prev);
    net_context_destroy(ctx);
    g_fe_network = 0;
}

// =====================================================================================================
// part 5: the config screen's sound summary with a sndsetup.inf / .dat pair in the save directory
// (fe_sndsetup_read_57af0, fe_config_draw_summary_52e20: texts 23..26)
// =====================================================================================================
static int config_summary_pixels(const char *save_dir, const char *ppm) {
    fe_set_save_dir(save_dir, nullptr);
    if (!fe_init(g_game.c_str(), 1)) return -1;
    input_reset();
    input_mouse_move(0, 0);
    for (int i = 0; i < 200; i++) frame();
    int n = 0;
    std::vector<uint8_t> px(fe_screen_pixels(), fe_screen_pixels() + 64000);
    static std::vector<uint8_t> first;
    if (first.empty()) first = px;
    else for (int y = 0x40; y < 0x40 + 0x4e; y++) for (int x = 0x38; x < 0x38 + 0x5a; x++) n += px[(size_t)(y * 320 + x)] != first[(size_t)(y * 320 + x)];
    write_ppm(ppm);
    fe_shutdown();
    return n;
}
static void part5_summary() {
    std::printf("== part 5: config screen summary with sndsetup.dat\n");
    text_load(g_game.c_str(), 0);
    const std::string dir = g_outdir + "/net_test_sndsetup";
#ifdef _WIN32
    CreateDirectoryA(dir.c_str(), nullptr);
#endif
    std::remove((dir + "/sndsetup.inf").c_str());
    std::remove((dir + "/sndsetup.dat").c_str());
    CHECK_EQ(config_summary_pixels(dir.c_str(), "config_default"), 0);
    FILE *f = std::fopen((dir + "/sndsetup.inf").c_str(), "wb");
    if (f) { std::fputs("SOUNDFX = SB16 220 5 1\n MUSIC = SB16FM 388 0 0 \n", f); std::fclose(f); }
    f = std::fopen((dir + "/sndsetup.dat").c_str(), "wb");
    if (f) {
        char rec[0x80 + 80] = {};
        std::strcpy(rec + 0x00, "SB16");
        std::strcpy(rec + 0x20, "Sound Blaster 16");
        std::strcpy(rec + 0x40, "SB16FM");
        std::strcpy(rec + 0x60, "Sound Blaster FM");
        const char *fields[8] = {"0", "220", "0", "5", "0", "1", "0", "388"};
        for (int i = 0; i < 8; i++) std::strcpy(rec + 0x80 + i * 10, fields[i]);
        std::fwrite(rec, 1, sizeof rec, f);
        std::fclose(f);
    }
    const int changed = config_summary_pixels(dir.c_str(), "config_sndsetup");
    std::printf("  %d pixels of the summary box changed (card names + I/O, IRQ, DMA, music port lines)\n", changed);
    CHECK(changed > 200);
    fe_set_save_dir(nullptr, nullptr);
}

int main(int argc, char **argv) {
    mc_install_crash_handler();
    g_exe = argv[0];
    g_game = argc > 1 ? argv[1] : MC_DEFAULT_GAME_DIR;
#ifdef _WIN32
    { char p[MAX_PATH]; if (GetModuleFileNameA(nullptr, p, MAX_PATH)) g_exe = p; }
#endif
    g_outdir = g_exe.substr(0, g_exe.find_last_of("/\\"));
    if (g_outdir == g_exe) g_outdir = ".";
    setvbuf(stdout, nullptr, _IONBF, 0);

    if (argc > 2 && std::strcmp(argv[2], "--peer") == 0) {             // a child of part 2 / 3
        if (argc < 8) return 2;
        std::thread([] { std::this_thread::sleep_for(std::chrono::seconds(170)); std::printf("peer: watchdog\n"); std::_Exit(4); }).detach();
        if (!sim_init(g_game.c_str())) return 2;
        sim_register_gameplay();
        g_video_mode_flags = 1;
        TcpTransport tr("127.0.0.1", (uint16_t)std::atoi(argv[4]));
        const int r = run_player(&tr, std::atoi(argv[3]), std::atoi(argv[5]), std::atoi(argv[6]), argv[7]);
        net_shutdown();
        return r;
    }

    if (!sim_init(g_game.c_str())) { std::printf("SKIP: game data not found in %s\n", g_game.c_str()); return 0; }
    sim_register_gameplay();
    g_video_mode_flags = 1;
    std::thread([] { std::this_thread::sleep_for(std::chrono::seconds(600)); std::printf("net_test: watchdog\n"); std::_Exit(4); }).detach();

    part1_memory();
    part4_lobby();
    part5_summary();
    part_tcp(2, 2000, -1);
    part_tcp(3, 2000, 1200);

    if (g_fail) { std::printf("net_test: %d failure(s)\n", g_fail.load()); return 1; }
    std::printf("net_test: OK\n");
    return 0;
}
