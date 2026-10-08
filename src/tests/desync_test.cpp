// Desync tooling test (port round 10, task E; docs/analysis/port_desync.md).
//  1. net_state_checksum keeps its exact value: a verbatim copy of the round-9 function (+ the round-10 mode
//     block) against the new one and against NetChecksumParts.total, every tick of level 38 (300 ticks), of
//     movie 0 (600 ticks), of an extended-pool level (3000 slots) and with a mode block active. The part
//     mapping: a change in each region (rng, g_rng16, the stacks, a Thing of each kind, a P block field, each
//     map, the cell heads, the mode block, the AI seed) changes exactly the expected part; the per-instance
//     fields (start_tick, HUD flash counters, PlayerRec.tick) change nothing.
//  2. MemLan (net.h, the in-memory NetBIOS of net_test) with 3 peers in one process sharing one simulation:
//     player 2 sees a perturbed state (a Thing's health) from exchange K on. The mismatch is detected at
//     exchange K by the host and player 2 (not player 1), both exchange their parts once and name the same
//     part (things.<class of that Thing>, 1 part differs), both write desync_K_p<n>.mcs, the exchange goes on
//     (later rounds keep their message pattern: mismatches counted, no second parts exchange); before K the
//     checksums equal the round-9 values. tools/reference/diff_state.py names the field (things[slot].health)
//     when Python is installed (else that check prints SKIP).
//  3. Two processes over TCP (mcport/net_tcp.cpp, as net_test part 2) playing level 50 in lockstep for 300
//     ticks; process 1 changes a creature's health before tick 100: both detect it at exchange 100, both
//     name things.creature (or the class of the Thing chosen), both dump, both play on to tick 300 (the
//     lockstep survives the extra messages), diff_state.py names the field in the two dumps.
// argv[1] = game dir. Exit 0 = pass.
#define _CRT_SECURE_NO_WARNINGS
#include "net.h"
#include "net_tcp.h"
#include "replay_check.h"
#include "savegame.h"
#include "sim.h"
#include "player.h"
#include "input.h"
#include "thing.h"
#include "demo.h"
#include "mode.h"
#include "settings.h"
#include "ai_wizard.h"
#include "mc_globals.h"
#include "mc_math.h"
#include "crash_handler.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

#ifndef MC_TOOLS_DIR
#define MC_TOOLS_DIR MC_REPO_DIR "/tools"
#endif

static std::atomic<int> g_fail{0};
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)
#define CHECK_EQ(a, b) do { long long _a = (long long)(a), _b = (long long)(b); if (_a != _b) { \
    std::printf("FAIL %s:%d: %s == %s (%lld != %lld)\n", __FILE__, __LINE__, #a, #b, _a, _b); g_fail++; } } while (0)

static std::string g_exe, g_game, g_outdir;

// (MemLan: the in-memory NetBIOS of tests/net_test.cpp, copied: tests cannot share sources)

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
// The round-9 net_state_checksum, verbatim (+ the round-10 mode block): the value must not change.
// =====================================================================================================
static uint32_t legacy_fnv(uint32_t h, const void *p, size_t n) {
    const uint8_t *b = static_cast<const uint8_t *>(p);
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; }
    return h;
}
static uint32_t legacy_checksum() {
    if (!g_state) return 0;
    const GameState *s = g_state;
    uint32_t h = 2166136261u;
    h = legacy_fnv(h, &s->rng, sizeof s->rng);
    h = legacy_fnv(h, &g_rng16, sizeof g_rng16);
    h = legacy_fnv(h, &s->free_top, sizeof s->free_top);
    h = legacy_fnv(h, s->free_list, sizeof s->free_list);
    h = legacy_fnv(h, &s->active_top, sizeof s->active_top);
    h = legacy_fnv(h, s->active_list, sizeof s->active_list);
    const int ext = thing_pool_ext_count();
    if (ext > 0) {
        const int32_t slots = thing_pool_slots();
        h = legacy_fnv(h, &slots, sizeof slots);
        h = legacy_fnv(h, thing_pool_ext_free_stack(), (size_t)ext * sizeof(int32_t));
        h = legacy_fnv(h, thing_pool_ext_active_stack(), (size_t)ext * sizeof(int32_t));
    }
    for (int i = 0; i < MC_THING_SLOTS + ext; i++) {
        // round 11: the round-9 function hashed the original's 0xa4-byte record (mc_image.h)
        ThingImage t;
        thing_to_image(i < MC_THING_SLOTS ? s->things[i] : *thing_at((unsigned)i), &t);
        // round 11 session 3: free slots keep the local-instance bit of the spell / flyer they were (net.cpp)
        if ((t.cls == 3 && t.type == 0) || t.cls == 0xc || t.cls == 0) t.flags &= ~1u;
        h = legacy_fnv(h, &t, sizeof t);
    }
    for (int p = 0; p < 8; p++) {
        const PlayerRec &r = s->players[p];
        h = legacy_fnv(h, &r.win_timer, sizeof r.win_timer);
        h = legacy_fnv(h, &r.status, sizeof r.status);
        h = legacy_fnv(h, &r.active, sizeof r.active);
        h = legacy_fnv(h, &r.index, sizeof r.index);
        h = legacy_fnv(h, &r.is_computer, sizeof r.is_computer);
        h = legacy_fnv(h, &r.thing, sizeof r.thing);
        uint8_t P[sizeof r.p];
        std::memcpy(P, r.p, sizeof P);
        std::memset(P + offsetof(PlayerBlock, start_tick), 0, 4);
        P[offsetof(PlayerBlock, castle_hit_flash)] = 0;
        P[offsetof(PlayerBlock, hit_flash)] = 0;
        P[offsetof(PlayerBlock, damage_flash)] = 0;
        std::memset(P + offsetof(PlayerBlock, spell_flash), 0, sizeof r.blk.spell_flash);
        std::memset(P + offsetof(PlayerBlock, combat_music), 0, sizeof r.blk.combat_music);   // round 11 session 3
        h = legacy_fnv(h, P, sizeof P);
    }
    h = legacy_fnv(h, g_map_type, MC_MAP_CELLS);
    h = legacy_fnv(h, g_map_height, MC_MAP_CELLS);
    h = legacy_fnv(h, g_map_light, MC_MAP_CELLS);
    h = legacy_fnv(h, g_map_flags, MC_MAP_CELLS);
    h = legacy_fnv(h, g_cell_things, sizeof(uint16_t) * MC_MAP_CELLS);
    if (mode_active()) h = mode_checksum(h);
    return h;
}

// =====================================================================================================
// part 1: the value and the part mapping
// =====================================================================================================
static int g_value_checks = 0, g_value_bad = 0;
static void check_value(const char *what, int tick) {
    NetChecksumParts p;
    net_state_checksum_parts(&p);
    const uint32_t legacy = legacy_checksum(), now = net_state_checksum();
    g_value_checks++;
    if (legacy != now || p.total != now) {
        if (g_value_bad++ < 5)
            std::printf("FAIL %s tick %d: legacy %08x, net_state_checksum %08x, parts.total %08x\n", what, tick,
                        (unsigned)legacy, (unsigned)now, (unsigned)p.total);
    }
}

static bool load_level(int level, int slots) {
    g_settings.thing_slots = slots;
    g_cfg->flags = 0;
    g_cfg->paused = 0;
    input_reset();
    return sim_load_level(level);
}

// Applies `change`, expects exactly `part` to differ (-1: none) and the total to change or not.
template <class F>
static void expect_part(const char *what, F change, int part, bool total_changes) {
    NetChecksumParts a, b, c;
    net_state_checksum_parts(&a);
    change();
    net_state_checksum_parts(&b);
    change();                                        // every change is an xor: applying it again undoes it
    net_state_checksum_parts(&c);
    int count = 0;
    const int first = net_checksum_parts_first_diff(a, b, &count);
    const bool ok = first == part && count == (part < 0 ? 0 : 1) && (a.total != b.total) == total_changes &&
                    std::memcmp(&a, &c, sizeof a) == 0;
    std::printf("  %-34s -> %-18s (%d part(s), total %s)%s\n", what, first < 0 ? "-" : net_checksum_part_name(first), count,
                a.total != b.total ? "changes" : "unchanged", ok ? "" : "   FAIL");
    if (!ok) {
        std::printf("FAIL part mapping %s: expected %s, total %s\n", what, part < 0 ? "-" : net_checksum_part_name(part),
                    total_changes ? "changes" : "unchanged");
        g_fail++;
    }
}

static int first_slot_of(int cls) {
    for (int i = 1; i < MC_THING_SLOTS; i++) if (g_state->things[i].cls == cls) return i;
    return -1;
}

static void part1_values() {
    std::printf("== part 1: net_state_checksum unchanged, parts\n");
    // part names: every index has its own
    for (int i = 0; i < NCP_COUNT; i++) {
        CHECK(std::strcmp(net_checksum_part_name(i), "?") != 0);
        for (int j = 0; j < i; j++) CHECK(std::strcmp(net_checksum_part_name(i), net_checksum_part_name(j)) != 0);
    }
    CHECK(std::strcmp(net_checksum_part_name(NCP_COUNT), "?") == 0);
    desync_tools_install();                          // the ai_seed reader

    CHECK(load_level(38, 1000));
    check_value("level 38", 0);
    for (int t = 1; t <= 300; t++) { game_tick_sim(); check_value("level 38", t); }
    std::printf("  level 38: 300 ticks, value unchanged\n");

    // the part mapping on the running level
    GameState *s = g_state;
    expect_part("GameState.rng", [&] { s->rng ^= 1; }, NCP_RNG, true);
    expect_part("g_rng16", [&] { g_rng16 ^= 1; }, NCP_RNG16, true);
    expect_part("free_list[3]", [&] { s->free_list[3] ^= 1; }, NCP_POOL, true);
    expect_part("active_top", [&] { s->active_top ^= 1; }, NCP_POOL, true);
    int classes = 0;
    for (int c = 0; c < 14; c++) {
        const int slot = first_slot_of(c);
        if (slot < 0) continue;
        char what[64];
        std::snprintf(what, sizeof what, "things[%d] (class %d).health", slot, c);
        expect_part(what, [&] { s->things[slot].health ^= 1; }, NCP_THINGS + c, true);
        classes++;
    }
    CHECK(classes >= 5);
    {
        int flyer = -1;
        for (int i = 1; i < MC_THING_SLOTS; i++) if (s->things[i].cls == 3 && s->things[i].type == 0) { flyer = i; break; }
        if (flyer > 0) expect_part("local flyer flags bit 0", [&] { s->things[flyer].flags ^= 1; }, -1, false);
    }
    expect_part("players[1].blk.mana", [&] { s->players[1].blk.mana ^= 1; }, NCP_PLAYERS + 1, true);
    expect_part("players[7].status", [&] { s->players[7].status ^= 1; }, NCP_PLAYERS + 7, true);
    expect_part("players[0].blk.start_tick", [&] { s->players[0].blk.start_tick ^= 1; }, -1, false);
    expect_part("players[0].blk.hit_flash", [&] { s->players[0].blk.hit_flash ^= 1; }, -1, false);
    expect_part("players[0].blk.spell_flash[3]", [&] { s->players[0].blk.spell_flash[3] ^= 1; }, -1, false);
    expect_part("players[0].tick", [&] { s->players[0].tick ^= 1; }, -1, false);
    expect_part("GameState.local_player", [&] { s->local_player ^= 1; }, -1, false);
    expect_part("map type", [&] { g_map_type[1234] ^= 1; }, NCP_MAP_TYPE, true);
    expect_part("map height", [&] { g_map_height[1234] ^= 1; }, NCP_MAP_HEIGHT, true);
    expect_part("map light", [&] { g_map_light[1234] ^= 1; }, NCP_MAP_LIGHT, true);
    expect_part("map flags", [&] { g_map_flags[1234] ^= 1; }, NCP_MAP_FLAGS, true);
    expect_part("cell heads", [&] { g_cell_things[777] ^= 1; }, NCP_CELL_HEADS, true);
    expect_part("g_ai_rand_seed (not in the total)", [&] { g_ai_rand_seed ^= 1; }, NCP_AI_SEED, false);
    // a mode block (only hashed while a mode runs)
    {
        ModeParams mp;
        mp.seed = 7;
        mp.bots = 3;
        mode_begin(GAME_MODE_CONQUEST, mp);
        CHECK(mode_active());
        check_value("mode active", 0);
        expect_part("mode block rng", [&] { g_mode.rng ^= 1; }, NCP_MODE, true);
        mode_end();
        CHECK(!mode_active());
        NetChecksumParts p;
        net_state_checksum_parts(&p);
        CHECK_EQ(p.part[NCP_MODE], 2166136261u);     // basis while no mode runs
        check_value("mode ended", 0);
    }
    // format of the tick-log line
    {
        char line[2048];
        const int n = tick_log_format(42, true, line, sizeof line);
        CHECK(n > 0 && n < (int)sizeof line);
        CHECK(std::strncmp(line, "42 ", 3) == 0);
        CHECK(std::strstr(line, " things.creature=") != nullptr && std::strstr(line, " ai_seed=") != nullptr);
        char plain[64];
        tick_log_format(42, false, plain, sizeof plain);
        CHECK(std::strncmp(line, plain, std::strlen(plain)) == 0 && line[std::strlen(plain)] == ' ');
        std::printf("  tick log line: %.100s...\n", line);
    }

    // movie 0 (the reference movie)
    g_cfg->flags = 0;
    g_cfg->paused = 0;
    sim_prepare_movie();
    sim_load_level(38);
    if (demo_open(g_game.c_str(), 0)) {
        int t = 0;
        while (demo_step() && t < 600) check_value("movie 0", ++t);
        demo_close();
        std::printf("  movie 0: %d ticks, value unchanged\n", t);
    } else std::printf("  movie 0 missing: skipped\n");

    // extended pool
    CHECK(load_level(20, 3000));
    CHECK(thing_pool_ext_count() > 0);
    for (int t = 1; t <= 300; t++) { game_tick_sim(); check_value("level 20 / 3000 slots", t); }
    {
        const int ext_slot = MC_THING_SLOTS + 5;
        expect_part("pool extension Thing (free)", [&] { thing_at((unsigned)ext_slot)->health ^= 1; },
                    NCP_THINGS + thing_at((unsigned)ext_slot)->cls, true);
        expect_part("pool extension free stack", [&] { thing_pool_ext_free_stack()[2] ^= 1; }, NCP_POOL_EXT, true);
    }
    std::printf("  level 20 with 3000 slots: 300 ticks, value unchanged\n");
    g_settings.thing_slots = 1000;
    CHECK(load_level(38, 1000));

    std::printf("  %d values compared, %d differ\n", g_value_checks, g_value_bad);
    CHECK_EQ(g_value_bad, 0);
}

// =====================================================================================================
// diff_state.py (only when Python is installed)
// =====================================================================================================
static bool python_available() {
    static int have = -1;
    if (have < 0) have = std::system("python --version >nul 2>&1") == 0 ? 1 : 0;
    return have == 1;
}
// Runs diff_state.py on two dumps; returns its output ("" when Python is missing) and the exit code.
static std::string run_diff_state(const std::string &a, const std::string &b, int *code) {
    *code = -1;
    if (!python_available()) return "";
    const std::string out = g_outdir + "/desync_test_diff.txt";
    const std::string cmd = std::string("python -I \"") + MC_TOOLS_DIR + "/reference/diff_state.py\" \"" + a + "\" \"" + b +
                            "\" --max 10 > \"" + out + "\" 2>&1";
    *code = std::system(cmd.c_str());
    std::ifstream f(out);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
static void check_diff_names(const std::string &a, const std::string &b, int slot) {
    int code = 0;
    const std::string text = run_diff_state(a, b, &code);
    if (code == -1) { std::printf("  diff_state.py: SKIP (python not installed)\n"); return; }
    char want[64];
    std::snprintf(want, sizeof want, "first difference (hashed): things[%d].health", slot);
    const bool named = text.find(want) != std::string::npos;
    const size_t eol = text.find('\n', text.find("first difference"));
    std::printf("  diff_state.py (exit %d): %s\n", code,
                text.find("first difference") != std::string::npos
                    ? text.substr(text.find("first difference"), eol == std::string::npos ? std::string::npos : eol - text.find("first difference")).c_str()
                    : text.substr(0, 300).c_str());
    CHECK(named);
    CHECK_EQ(code, 1);
}

// =====================================================================================================
// part 2: MemLan peers sharing one simulation; player 2 sees a perturbed Thing from exchange K on
// =====================================================================================================
struct Barrier {
    std::mutex mu; std::condition_variable cv; int n, count = 0, gen = 0;
    explicit Barrier(int k) : n(k) {}
    void wait() {
        std::unique_lock<std::mutex> lk(mu);
        const int g = gen;
        if (++count == n) { count = 0; gen++; cv.notify_all(); return; }
        cv.wait(lk, [&] { return gen != g; });
    }
};

static std::mutex g_sim_mu;
static thread_local int t_me = -1, t_round = -1;
static int g_perturb_slot = 0, g_perturb_from = 0;
static constexpr int PERTURB_DELTA = 7;
static std::vector<std::pair<uint32_t, uint32_t>> g_host_values;   // (checksum sent, legacy) per round, player 0

struct PerturbScope {      // player 2's view of the shared state (caller holds g_sim_mu)
    bool on = false;
    PerturbScope() {
        if (t_me == 2 && t_round >= g_perturb_from) { g_state->things[g_perturb_slot].health += PERTURB_DELTA; on = true; }
    }
    ~PerturbScope() { if (on) g_state->things[g_perturb_slot].health -= PERTURB_DELTA; }
};
static uint32_t peer_checksum() {
    std::lock_guard<std::mutex> g(g_sim_mu);
    PerturbScope p;
    const uint32_t c = net_state_checksum();
    if (t_me == 0) g_host_values.emplace_back(c, legacy_checksum());
    return c;
}
static void peer_parts(NetChecksumParts *out) {
    std::lock_guard<std::mutex> g(g_sim_mu);
    PerturbScope p;
    net_state_checksum_parts(out);
}
static bool peer_dump(uint32_t exchange, int player) {
    std::lock_guard<std::mutex> g(g_sim_mu);
    PerturbScope p;
    return desync_dump_state(exchange, player);
}

static void part2_memlan() {
    std::printf("== part 2: MemLan, 3 peers, player 2 perturbed from exchange K\n");
    CHECK(load_level(38, 1000));
    for (int t = 0; t < 20; t++) game_tick_sim();
    g_perturb_slot = first_slot_of(5);
    if (g_perturb_slot < 0) g_perturb_slot = first_slot_of(2);
    const int cls = g_state->things[g_perturb_slot].cls;
    const int R = 40, K = 15;
    g_perturb_from = K;
    const std::string dir = g_outdir + "/desync_test_memlan";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    desync_set_dump_dir(dir.c_str());
    g_host_values.clear();

    MemLan lan;
    const int N = 3;
    MemTransport *tr[N];
    for (int i = 0; i < N; i++) tr[i] = new MemTransport(&lan);
    int local[N] = {-9, -9, -9};
    NetSyncStats stats[N];
    NetChecksumParts lp[N], rp[N];
    bool have[N] = {};
    Barrier bar(N);
    std::atomic<int> errors{0};
    auto peer = [&](int t) {
        NetContext *ctx = net_context_create();
        net_context_select(ctx);
        net_set_transport(tr[t]);
        if (net_init() != 1) { errors++; return; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20 * t));
        const int me = net_session_join("DESYNC", N);
        local[t] = me;
        t_me = me;
        if (me < 0) { errors++; return; }
        net_sync_set_checksum(peer_checksum);
        net_sync_set_parts(peer_parts);
        net_sync_set_dump(peer_dump);
        net_sync_arm();
        uint8_t buf[80];
        for (int round = 0; round < R; round++) {
            bar.wait();
            if (me == 0) { std::lock_guard<std::mutex> g(g_sim_mu); game_tick_sim(); }
            bar.wait();
            t_round = round;
            std::memset(buf, 0, sizeof buf);
            buf[me * 10] = (uint8_t)round;
            net_exchange_frame(buf, 10);
            for (int p = 0; p < N; p++) if (buf[p * 10] != (uint8_t)round) { errors++; break; }
        }
        stats[t] = net_sync_stats();
        int peer_no = -1;
        uint32_t ex = 0;
        have[t] = net_sync_last_parts(&lp[t], &rp[t], &peer_no, &ex);
        if (have[t] && ex != (uint32_t)K) errors++;
        net_context_select(nullptr);
        net_context_destroy(ctx);
    };
    std::vector<std::thread> th;
    for (int t = 0; t < N; t++) th.emplace_back(peer, t);
    for (auto &x : th) x.join();
    for (int i = 0; i < N; i++) delete tr[i];
    CHECK_EQ(errors.load(), 0);

    std::printf("  perturbed: things[%d] (class %d) health +%d as seen by player 2 from exchange %d of %d\n", g_perturb_slot,
                cls, PERTURB_DELTA, K, R);
    for (int t = 0; t < N; t++) {
        const int me = local[t];
        const NetSyncStats &s = stats[t];
        std::printf("  player %d: checks %u mismatches %u first %d | parts exchanges %u, first part %s with player %d, %u differ, dumped %u\n",
                    me, s.checks, s.mismatches, s.first_mismatch, s.parts_exchanges, s.first_part >= 0 ? net_checksum_part_name(s.first_part) : "-",
                    s.part_peer, s.parts_differ, s.dumped);
        if (me == 1) {
            CHECK_EQ(s.mismatches, 0);
            CHECK_EQ(s.parts_exchanges, 0);
            CHECK_EQ(s.first_part, -1);
            CHECK(!have[t]);
            continue;
        }
        CHECK_EQ(s.first_mismatch, K);
        CHECK_EQ(s.mismatches, R - K);
        CHECK_EQ(s.parts_exchanges, 1);
        CHECK_EQ(s.first_part, NCP_THINGS + cls);
        CHECK_EQ(s.part_peer, me == 0 ? 2 : 0);
        CHECK_EQ(s.parts_differ, 1);
        CHECK_EQ(s.dumped, 1);
        CHECK(have[t]);
        if (have[t]) {
            int count = 0;
            CHECK_EQ(net_checksum_parts_first_diff(lp[t], rp[t], &count), NCP_THINGS + cls);
            CHECK_EQ(count, 1);
        }
    }
    // the checksums the host sent before K are the round-9 values, after K as well (its own view is unperturbed)
    int bad = 0;
    for (auto &v : g_host_values) bad += v.first != v.second;
    std::printf("  host checksums: %zu sent, %d differ from the round-9 function\n", g_host_values.size(), bad);
    CHECK_EQ(g_host_values.size(), (size_t)R);
    CHECK_EQ(bad, 0);
    char p0[1024], p1[1024], p2[1024];
    desync_dump_path((uint32_t)K, 0, p0, sizeof p0);
    desync_dump_path((uint32_t)K, 1, p1, sizeof p1);
    desync_dump_path((uint32_t)K, 2, p2, sizeof p2);
    CHECK(std::filesystem::exists(p0));
    CHECK(std::filesystem::exists(p2));
    CHECK(!std::filesystem::exists(p1));
    SaveStateHeader h{};
    CHECK(savestate_read_header(p2, &h));
    if (std::filesystem::exists(p0) && std::filesystem::exists(p2)) check_diff_names(p0, p2, g_perturb_slot);
    desync_set_dump_dir(nullptr);
}

// =====================================================================================================
// part 3: two processes over TCP, process 1 perturbs a creature before tick K
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
}

static int pick_slot() {
    for (int i = 1; i < MC_THING_SLOTS; i++) if (g_state->things[i].cls == 5 && g_state->things[i].health > 0) return i;
    for (int i = 1; i < MC_THING_SLOTS; i++) if (g_state->things[i].cls != 0) return i;
    return 1;
}

// One process: join, level choice, `ticks` ticks; player 1 perturbs before tick `k`. Writes one result line.
static int run_peer(TcpTransport *tr, int players, int ticks, int k, const char *dir, const char *result) {
    net_set_transport(tr);
    if (net_init() != 1) { std::printf("  peer: no network (%s)\n", tr->last_error()); return 2; }
    const int me = net_session_join("DESYNCT", players);
    if (me < 0) { std::printf("  peer: join failed\n"); return 2; }
    g_state->player_count = (int16_t)players;
    g_state->local_player = (int16_t)me;
    std::snprintf(g_cfg->save_str_a, sizeof g_cfg->save_str_a, "Player %d", me);
    std::snprintf(g_cfg->save_str_b, sizeof g_cfg->save_str_b, "CALL%d", me);
    g_cfg->flags |= 0x10;
    g_state->commands[me].arg = (uint8_t)(50 + me);
    net_exchange_frame(g_state->commands, 10);
    const int level = (int8_t)g_state->commands[0].arg;
    g_cfg->level = (uint16_t)level;
    input_reset();
    if (!sim_load_level(level)) { std::printf("  peer %d: level %d not loaded\n", me, level); return 2; }
    input_mouse_center();
    desync_tools_install();
    desync_set_dump_dir(dir);
    net_sync_arm();
    int t = 0, slot = 0, cls = 0;
    for (; t < ticks; t++) {
        g_timer_ticks = (uint32_t)(t * 5 + me * 1000);
        script_input(t, me);
        if (t == k) {
            slot = pick_slot();
            cls = g_state->things[slot].cls;
            if (me == 1) g_state->things[slot].health += PERTURB_DELTA;
        }
        game_tick_sim();
    }
    const NetSyncStats s = net_sync_stats();
    FILE *f = std::fopen(result, "w");
    if (!f) return 2;
    std::fprintf(f, "%d %d %d %d %d %d %d %u %u %u %u\n", me, t, slot, cls, s.first_mismatch, s.first_part, s.part_peer,
                 s.parts_differ, s.dumped, s.mismatches, s.parts_exchanges);
    std::fclose(f);
    return 0;
}

#ifdef _WIN32
static HANDLE spawn_peer(int players, int port, int ticks, int k, const std::string &dir, const std::string &out) {
    char cmd[4096];
    std::snprintf(cmd, sizeof cmd, "\"%s\" \"%s\" --peer %d %d %d %d \"%s\" \"%s\"", g_exe.c_str(), g_game.c_str(), players,
                  port, ticks, k, dir.c_str(), out.c_str());
    STARTUPINFOA si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessA(nullptr, cmd, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) return nullptr;
    CloseHandle(pi.hThread);
    return pi.hProcess;
}
#endif

static void part3_tcp() {
    std::printf("== part 3: two processes over TCP, process 1 perturbs a creature before tick K\n");
#ifndef _WIN32
    std::printf("  SKIP: process spawning is implemented for Windows only\n");
#else
    const int ticks = 300, K = 100;
    const std::string dir = g_outdir + "/desync_test_tcp";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    NetContext *ctx = net_context_create();
    NetContext *prev = net_context_select(ctx);
    TcpTransport *tr = nullptr;
    int port = 0;
    static int next = 31600 + (int)(std::chrono::steady_clock::now().time_since_epoch().count() % 2000);
    for (int tries = 0; tries < 20; tries++) {
        port = next++;
        tr = new TcpTransport("", (uint16_t)port);
        if (tr->present() && tr->is_name_server()) break;
        delete tr;
        tr = nullptr;
    }
    if (!tr) { std::printf("FAIL: no free TCP port for the name server\n"); g_fail++; net_context_select(prev); return; }
    const std::string r0 = dir + "/result0.txt", r1 = dir + "/result1.txt";
    HANDLE kid = spawn_peer(2, port, ticks, K, dir, r1);
    CHECK(kid != nullptr);
    const int r = run_peer(tr, 2, ticks, K, dir.c_str(), r0.c_str());
    CHECK_EQ(r, 0);
    if (kid) {
        const DWORD w = WaitForSingleObject(kid, 180000);
        DWORD code = 99;
        if (w == WAIT_OBJECT_0) GetExitCodeProcess(kid, &code);
        else TerminateProcess(kid, 98);
        CHECK_EQ(code, 0);
        CloseHandle(kid);
    }
    net_shutdown();
    delete tr;
    net_context_select(prev);
    net_context_destroy(ctx);
    g_cfg->flags &= ~0x10;
    g_state->local_player = 0;
    desync_set_dump_dir(nullptr);
    int res[2][11] = {};
    for (int i = 0; i < 2; i++) {
        FILE *f = std::fopen((i ? r1 : r0).c_str(), "r");
        CHECK(f != nullptr);
        if (!f) continue;
        int *v = res[i];
        const int got = std::fscanf(f, "%d %d %d %d %d %d %d %d %d %d %d", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7],
                                    &v[8], &v[9], &v[10]);
        std::fclose(f);
        CHECK_EQ(got, 11);
        std::printf("  process %d: player %d, %d ticks, perturbed slot %d (class %d), first mismatch %d, first part %s with player %d, "
                    "%d part(s) differ, dumped %d, %d mismatches, %d parts exchange(s)\n", i, v[0], v[1], v[2], v[3], v[4],
                    v[5] >= 0 ? net_checksum_part_name(v[5]) : "-", v[6], v[7], v[8], v[9], v[10]);
    }
    for (int i = 0; i < 2; i++) {
        const int *v = res[i];
        CHECK_EQ(v[1], ticks);                       // the lockstep survived the parts exchange
        CHECK_EQ(v[2], res[0][2]);                   // both chose the same Thing
        CHECK_EQ(v[4], K);
        CHECK_EQ(v[5], NCP_THINGS + v[3]);
        CHECK_EQ(v[6], 1 - v[0]);
        CHECK_EQ(v[7], 1);
        CHECK_EQ(v[8], 1);
        CHECK_EQ(v[10], 1);
    }
    const std::string d0 = dir + "/desync_" + std::to_string(K) + "_p0.mcs", d1 = dir + "/desync_" + std::to_string(K) + "_p1.mcs";
    CHECK(std::filesystem::exists(d0));
    CHECK(std::filesystem::exists(d1));
    if (std::filesystem::exists(d0) && std::filesystem::exists(d1)) check_diff_names(d0, d1, res[0][2]);
#endif
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

    if (argc > 2 && std::strcmp(argv[2], "--peer") == 0) {             // the child of part 3
        if (argc < 9) return 2;
        std::thread([] { std::this_thread::sleep_for(std::chrono::seconds(170)); std::printf("peer: watchdog\n"); std::_Exit(4); }).detach();
        if (!sim_init(g_game.c_str())) return 2;
        sim_register_gameplay();
        g_video_mode_flags = 1;
        TcpTransport tr("127.0.0.1", (uint16_t)std::atoi(argv[4]));
        const int r = run_peer(&tr, std::atoi(argv[3]), std::atoi(argv[5]), std::atoi(argv[6]), argv[7], argv[8]);
        net_shutdown();
        return r;
    }

    if (!sim_init(g_game.c_str())) { std::printf("SKIP: game data not found in %s\n", g_game.c_str()); return 0; }
    sim_register_gameplay();
    g_video_mode_flags = 1;
    std::thread([] { std::this_thread::sleep_for(std::chrono::seconds(900)); std::printf("desync_test: watchdog\n"); std::_Exit(4); }).detach();

    // part 3 first: the parent must start its level from the same history as the child (a process that played
    // another level before keeps stale entries above GameState.active_top, which the checksum hashes - see
    // docs/analysis/port_desync.md "findings")
    part3_tcp();
    part1_values();
    part2_memlan();

    if (g_fail) { std::printf("desync_test: %d failure(s)\n", g_fail.load()); return 1; }
    std::printf("desync_test: OK\n");
    return 0;
}
