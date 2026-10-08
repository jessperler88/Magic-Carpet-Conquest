// References with an active local player (port round 6, task C): scripted recordings made by the port.
//
// A recording directory (extracted/reference/player/<name>/) holds
//   script.txt                the scripted input of the local player (format below),
//   movie/{mvi,gam,map}M.dat  the recording the port made from it (demo.cpp's recorder: the original's
//                             movie format, pointers in the DOSBox run's layout),
//   tick%05d.gam / .map       the original's per-tick dumps while it plays the recording back
//                             (tools/reference/run_player.py), index.json.
// reference_test compares the port's playback of the recording with the dumps (suite mode, ctest
// `reference_player`). This program makes the recordings and checks the round trip:
//
//   reference_player_test <game> record <script> <out dir>   record: the port plays level L from its first
//                                                           tick, the script drives the local player,
//                                                           and the recorder writes <out>/movie/
//   reference_player_test <game> [<dir>]                     round trip (ctest `reference_player_test`):
//                                                           for every <dir>/<name>/script.txt (or <dir>
//                                                           itself): record again into the build dir, play
//                                                           that recording back in the port and compare the
//                                                           states tick by tick with the record run; also
//                                                           report whether the new recording equals the
//                                                           stored one (it changes when game logic changes).
//
// The record run reproduces the original's record run of tools/reference/run_level.py: `-roll` flags
// 0x100, level_load_file resets, sim_load_level(L), and once tick 1 is done (player 0's Thing exists) the
// record bit Config.flags |= 2 - the next tick's first packet opens the movie and saves the snapshot pair.
// At tick STOP the recording is closed (demo_close), like the record cave does.
//
// Script (one command per line, '#' comments; ticks are PlayerRec.tick values: the packet is executed in
// that tick, the same numbering as the dumps):
//   level L | movie M | stop T                     header
//   T[-T2] steer X Y                               steering bytes of the packet (held over the range)
//   T[-T2] keys B                                  input bits (1 faster, 2 slower, 4/8 strafe, 0x10 / 0x20
//                                                  cast left / right), sent as command 6 like the game does
//   T[-T2] cast left|right                         = keys 0x10 / 0x20
//   T cmd C [A [P2]]                               a raw command packet
//   T cheat N                                      command 0x1e (1 all spells, 2 mana, 3..5 destroy, 6 heal, 7)
//   T left S | T right S                           commands 0x15 / 0x16 with the book slot that holds spell id S
//   T book | T close                               command 0x14 arg 2 / 0
//   T[-T2] respawn                                 command 0xf (only while dead and the castle stands: without a
//                                                  castle the level is lost and the original leaves it)
//   T[-T2] rebuild                                 no castle and alive: castle spell -> right hand, cast it
//   T[-T2] face X Y [Z]                            steer towards the world point (yaw: steer_x, pitch: steer_y)
//   T[-T2] fly X Y                                 face the point and accelerate (keys 1), slow down near it
//   T[-T2] faceth N                                face Thing slot N (or "faceth mine" ...) - see below
//   T[-T2] face_class C TYPE [D]                   face the nearest live Thing of class C / type TYPE (-1 any);
//                                                  D > 0: also fly towards it until D away (keys 1 / 2)
// Commands of the same tick: the explicit command wins, bits only travel with command 0 / 6.
#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "sim.h"
#include "thing.h"
#include "player.h"
#include "demo.h"
#include "hud.h"
#include "mc_globals.h"
#include "mcfile.h"
#include "crash_handler.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

#ifndef MC_PLAYER_REFERENCE_DIR
#define MC_PLAYER_REFERENCE_DIR MC_REPO_DIR "/extracted/reference/player"
#endif

// ---- the script -----------------------------------------------------------------------------------

enum Verb { V_STEER, V_KEYS, V_CMD, V_LEFT, V_RIGHT, V_FACE, V_FLY, V_FACE_THING, V_FACE_CLASS, V_REBUILD };
struct Line {
    int t0 = 0, t1 = 0;
    Verb verb = V_CMD;
    int a = 0, b = 0, c = 0;
    bool has_c = false;
};
struct Script {
    int level = -1, movie = -1, stop = 3000;
    std::vector<Line> lines;
    std::string error;
};

static Script parse_script(const std::string &path) {
    Script s;
    mc_blob b;
    if (!mc_read_file(path.c_str(), &b)) { s.error = "cannot read " + path; return s; }
    std::string text(reinterpret_cast<const char *>(b.data), b.len);
    mc_blob_free(&b);
    size_t pos = 0;
    int lineno = 0;
    while (pos < text.size()) {
        size_t e = text.find('\n', pos);
        if (e == std::string::npos) e = text.size();
        std::string ln = text.substr(pos, e - pos);
        pos = e + 1;
        lineno++;
        size_t h = ln.find('#');
        if (h != std::string::npos) ln.resize(h);
        std::vector<std::string> w;
        for (size_t i = 0; i < ln.size();) {
            while (i < ln.size() && (ln[i] == ' ' || ln[i] == '\t' || ln[i] == '\r')) i++;
            size_t j = i;
            while (j < ln.size() && !(ln[j] == ' ' || ln[j] == '\t' || ln[j] == '\r')) j++;
            if (j > i) w.push_back(ln.substr(i, j - i));
            i = j;
        }
        if (w.empty()) continue;
        auto num = [](const std::string &x) { return (int)std::strtol(x.c_str(), nullptr, 0); };
        if (w[0] == "level" && w.size() > 1) { s.level = num(w[1]); continue; }
        if (w[0] == "movie" && w.size() > 1) { s.movie = num(w[1]); continue; }
        if (w[0] == "stop" && w.size() > 1) { s.stop = num(w[1]); continue; }
        if (w.size() < 2) { s.error = "line " + std::to_string(lineno) + ": no verb"; return s; }
        Line l;
        size_t dash = w[0].find('-');
        l.t0 = num(w[0].substr(0, dash));
        l.t1 = dash == std::string::npos ? l.t0 : num(w[0].substr(dash + 1));
        const std::string &v = w[1];
        auto arg = [&](size_t i, int def) { return w.size() > i ? num(w[i]) : def; };
        if (v == "steer") { l.verb = V_STEER; l.a = arg(2, 0); l.b = arg(3, 0); }
        else if (v == "keys") { l.verb = V_KEYS; l.a = arg(2, 0); }
        else if (v == "cast") { l.verb = V_KEYS; l.a = w.size() > 2 && w[2] == "right" ? 0x20 : 0x10; }
        else if (v == "cmd") { l.verb = V_CMD; l.a = arg(2, 0); l.b = arg(3, 0); l.c = arg(4, 0); l.has_c = w.size() > 4; }
        else if (v == "cheat") { l.verb = V_CMD; l.a = 0x1e; l.b = arg(2, 1); }
        else if (v == "book") { l.verb = V_CMD; l.a = 0x14; l.b = 2; }
        else if (v == "close") { l.verb = V_CMD; l.a = 0x14; l.b = 0; }
        else if (v == "respawn") { l.verb = V_CMD; l.a = 0xf; }
        else if (v == "rebuild") { l.verb = V_REBUILD; }
        else if (v == "left") { l.verb = V_LEFT; l.a = arg(2, 0); }
        else if (v == "right") { l.verb = V_RIGHT; l.a = arg(2, 0); }
        else if (v == "face") { l.verb = V_FACE; l.a = arg(2, 0); l.b = arg(3, 0); l.c = arg(4, 0); l.has_c = w.size() > 4; }
        else if (v == "fly") { l.verb = V_FLY; l.a = arg(2, 0); l.b = arg(3, 0); }
        else if (v == "faceth") { l.verb = V_FACE_THING; l.a = arg(2, 0); }
        else if (v == "face_class") { l.verb = V_FACE_CLASS; l.a = arg(2, 0); l.b = arg(3, -1); l.c = arg(4, 0); }
        else { s.error = "line " + std::to_string(lineno) + ": unknown verb " + v; return s; }
        s.lines.push_back(l);
    }
    if (s.level < 0) s.error = "no level";
    if (s.movie < 0) s.movie = 30000 + (s.level < 0 ? 0 : s.level);
    return s;
}

static const Script *g_script = nullptr;
static int g_script_tick = 0;

static int clamp127(double v) {
    if (v > 127) return 127;
    if (v < -127) return -127;
    return (int)std::lround(v);
}
static int wrap_angle(int a) {          // -0x400..0x3ff
    a &= 0x7ff;
    return a >= 0x400 ? a - 0x800 : a;
}

// Steering bytes that turn the flyer towards (x, y, z): yaw through steer_x (velocity: the yaw changes by
// about steer_x / 4 per tick at steady state), pitch through steer_y (position: pitch_acc -> 2 * steer_y,
// positive = down). z < 0: aim at the ground under the point.
static void face_point(const Thing *t, int x, int y, int z, bool z_given, int8_t *sx, int8_t *sy) {
    const PlayerBlock *P = player_block(const_cast<Thing *>(t));
    Pos to{(uint16_t)x, (uint16_t)y, (int16_t)z};
    if (!z_given) to.z = (int16_t)terrain_height_at(&to);
    int want = pos_angle_to(thing_pos(t), &to);
    int diff = wrap_angle(want - (int)t->yaw);
    // predicted remaining turn with the current rate: yaw_rate decays towards 2 * steer_x by 1/4 per tick
    double v = diff / 2.0 - P->yaw_rate / 2.0;
    *sx = (int8_t)clamp127(v);
    double dx = (double)(int16_t)(to.x - t->x), dy = (double)(int16_t)(to.y - t->y);
    double dist = std::sqrt(dx * dx + dy * dy);
    double dz = (double)t->z - (double)to.z;                     // > 0: the target is below
    double pitch = std::atan2(dz, dist < 1 ? 1 : dist) * 0x400 / 3.14159265358979;   // angle units, + = down
    *sy = (int8_t)clamp127(pitch / 2.0);
}

// player_local_input_16660 replaced by the script (installed on g_hook_player_local_input).
static void script_input() {
    GameState *st = g_state;
    const int lp = st->local_player & 7;
    CmdPacket *pk = &st->commands[lp];
    const PlayerRec *rec = &st->players[lp];
    // The tick this packet is executed in, counted by the record loop: PlayerRec.tick + 1 unless a quick
    // load (command 0xb) set the level clock back - script lines never run twice.
    const int tick = g_script_tick;
    // local_input_flight 0x16dcb: a command that is still pending (the join packet of the level start)
    // is left alone, steering included.
    if (pk->cmd != 0) return;
    Thing *t = thing_at(rec->thing % MC_THING_SLOTS);
    PlayerBlock *P = player_block(t);
    int cmd = -1, arg = 0, pad2 = 0, bits = 0;
    bool steer = false, fly = false;
    int8_t sx = 0, sy = 0;
    for (const Line &l : g_script->lines) {
        if (tick < l.t0 || tick > l.t1) continue;
        switch (l.verb) {
        case V_STEER: sx = (int8_t)l.a; sy = (int8_t)l.b; steer = true; break;
        case V_KEYS: bits |= l.a; break;
        case V_CMD:
            // player_queue_command_17270 0x174f2: the respawn request only while the wizard lies dead
            // The script also never sends it without a castle: player_commands_process then sets status
            // 0xc (level lost), and the original's game_main leaves the level - the playback would stop.
            if (l.a == 0xf && !(t->health < 0 && t->state == 3 && P->castle != 0)) break;
            cmd = l.a; arg = l.b; pad2 = l.has_c ? l.c : 0;
            break;
        case V_LEFT: case V_RIGHT: {
            int slot = 0xff;
            unsigned id = (unsigned)l.a;
            if (id < 24 && P->spell_thing[id] != 0)
                for (int k = 0; k < 24; k++) if (P->spell_slot[k] == (int32_t)P->spell_thing[id]) { slot = k; break; }
            cmd = l.verb == V_LEFT ? 0x15 : 0x16;
            arg = slot;
            break;
        }
        case V_REBUILD: {
            // no castle and alive: castle spell into the right hand, then cast it (16 cells ahead)
            if (P->castle != 0 || t->health < 0 || P->spell_thing[16] == 0) break;
            int slot = -1;
            for (int k = 0; k < 24; k++) if (P->spell_slot[k] == (int32_t)P->spell_thing[16]) { slot = k; break; }
            if (slot < 0) break;
            if (P->slot_right != slot) { cmd = 0x16; arg = slot; }
            else if (thing_at(P->spell_thing[16] % MC_THING_SLOTS)->cast_ticks == 0) bits |= 0x20;
            break;
        }
        case V_FACE: face_point(t, l.a, l.b, l.c, l.has_c, &sx, &sy); steer = true; break;
        case V_FLY: {
            face_point(t, l.a, l.b, 0, false, &sx, &sy);
            sy = 0;
            steer = true;
            Pos to{(uint16_t)l.a, (uint16_t)l.b, 0};
            int d = pos_dist_xy(thing_pos(t), &to);
            if (d > 0x600) bits |= P->target_speed < 0x30 ? 1 : 0;
            else if (P->target_speed > 0) bits |= 2;
            fly = true;
            break;
        }
        case V_FACE_THING: {
            const Thing *o = thing_at((unsigned)l.a % MC_THING_SLOTS);
            if (o->cls) { face_point(t, o->x, o->y, o->z, true, &sx, &sy); steer = true; }
            break;
        }
        case V_FACE_CLASS: {
            int best = -1, bd = 1 << 30;
            for (int i = 1; i < MC_THING_SLOTS; i++) {
                const Thing *o = thing_at(i);
                if (o->cls != l.a || (l.b >= 0 && o->type != l.b) || o == t ||
                    ((o->cls == 3 || o->cls == 5) && o->health < 0)) continue;
                int d = pos_dist_xy(thing_pos(t), thing_pos(o));
                if (d < bd) { bd = d; best = i; }
            }
            if (best > 0) {
                const Thing *o = thing_at(best);
                face_point(t, o->x, o->y, o->z, true, &sx, &sy);
                steer = true;
                if (l.c > 0) {                          // chase: keep about l.c away
                    if (bd > l.c) { if (P->target_speed < 0x30) bits |= 1; }
                    else if (P->target_speed > 0) bits |= 2;
                }
            }
            break;
        }
        }
    }
    (void)fly;
    std::memset(pk, 0, sizeof *pk);
    if (cmd >= 0) {
        pk->cmd = (uint8_t)cmd;
        pk->arg = (uint8_t)arg;
        pk->pad2 = (uint8_t)pad2;
    } else if (bits) {
        pk->cmd = 6;
        pk->bits = (uint8_t)bits;
    }
    if (steer) { pk->steer_x = sx; pk->steer_y = sy; }
}

// ---- state comparison (round trip) ----------------------------------------------------------------

// The GameState with what a snapshot load cannot reproduce cleared: the fields of free Thing slots
// thing_relink_snapshot zeroes (next / desc / player) and the stack entries above the tops.
static void normalise(GameState *s) {
    for (int i = 1; i < MC_THING_SLOTS; i++) {
        Thing &t = s->things[i];
        if (t.cls == 0) { t.next = 0; t.desc = 0; t.player = 0; }
    }
    // The HUD's game-state writes (hud_tick_state): render_frame_1fab0 draws the status panels, the hand
    // labels and the message lines only while no movie is played (Config.flags & 4), so their counters
    // run down in a record run and stay put in its playback - in the original as in the port (round 5
    // saw P+0x188 differ between the original's own record and play runs).
    for (int p = 0; p < 8; p++) {
        PlayerRec &r = s->players[p];
        for (PlayerMsg &m : r.messages) m.ticks = 0;
        r.blk.castle_hit_flash = 0;
        r.blk.hit_flash = 0;
        r.blk.damage_flash = 0;
        std::memset(r.blk.spell_flash, 0, sizeof r.blk.spell_flash);
    }
    for (int i = s->free_top + 1; i < 1000; i++) if (i >= 0) s->free_list[i] = 0;
    for (int i = s->active_top + 1; i < 1000; i++) if (i >= 0) s->active_list[i] = 0;
}
static uint64_t state_hash(const GameState *s) {
    static GameState tmp;
    std::memcpy(&tmp, s, sizeof tmp);
    normalise(&tmp);
    const uint8_t *p = reinterpret_cast<const uint8_t *>(&tmp);
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < sizeof tmp; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

// The Config / GameState part of level_load_file_3d160 that sim_load_level does not do (same as
// reference_test.cpp's level_reset_config_local; requested for sim_load_level in round 5).
static void level_reset_config_local() {
    uint8_t *st = reinterpret_cast<uint8_t *>(g_state);
    uint8_t *cf = reinterpret_cast<uint8_t *>(g_cfg);
    st[0x244] = 0;
    g_cfg->fade_stage = 0;
    std::memset(cf + 0x5d, 0, 0x10);
    g_cfg->substeps = 0;
    g_cfg->palette_effect = 0;
    std::memset(cf + 0xb8, 0, 0xe);
    std::memset(cf + 0x8e1a, 0, 4);
    std::memset(g_cfg->creature_lists, 0, sizeof g_cfg->creature_lists);
    g_cfg->player_list = 0;
    g_cfg->mana_ball_list = 0;
    g_cfg->wizard_list = 0;
    g_cfg->projectile_list = 0;
    g_cfg->flags &= 0x3fff;
    g_cfg->paused &= ~1;
}

static void start_level(int level) {
    g_cfg->flags = 0x100;
    g_cfg->paused = 0;
    sim_prepare_movie();
    level_reset_config_local();
    sim_load_level(level);
}

struct Snap { int tick; uint64_t hash; };

// MC_RT_DUMP=tick: the normalised state of that tick of the record run / the playback into <file>.
static void rt_dump(const char *file, int tick) {
    const char *e = std::getenv("MC_RT_DUMP");
    if (!e || std::atoi(e) != tick) return;
    static GameState tmp;
    std::memcpy(&tmp, g_state, sizeof tmp);
    normalise(&tmp);
    if (FILE *f = std::fopen(file, "wb")) { std::fwrite(&tmp, 1, sizeof tmp, f); std::fclose(f); }
}

// Event log of the record run: new Things owned / cast by the local player, the player's state changes.
struct Watch {
    uint8_t cls[MC_THING_SLOTS] = {};
    uint8_t type[MC_THING_SLOTS] = {};
    int last_health = 0, last_castle = 0, last_state = -1;
    int32_t last_mana = 0;
    int cast_prev[24] = {};
    int casts[24] = {};
};

static void watch_tick(Watch &w, int tick, bool verbose) {
    const int lp = g_state->local_player & 7;
    Thing *pt = thing_at(g_state->players[lp].thing % MC_THING_SLOTS);
    const int pidx = (int)thing_index(pt);
    PlayerBlock *P = player_block(pt);
    static int count[16][64], first[16][64];
    std::memset(count, 0, sizeof count);
    for (int i = 1; i < MC_THING_SLOTS; i++) {
        const Thing *o = thing_at(i);
        bool fresh = o->cls != 0 && (w.cls[i] != o->cls || w.type[i] != o->type);
        if (fresh && (o->owner == pidx || o->caster == pidx || o->mana_owner == pidx) && o->cls != 12 && o->cls < 16 &&
            o->type < 64) {
            if (!count[o->cls][o->type]++) first[o->cls][o->type] = i;
        }
        w.cls[i] = o->cls;
        w.type[i] = o->type;
    }
    if (verbose)
        for (int c = 0; c < 16; c++)
            for (int ty = 0; ty < 64; ty++) {
                if (!count[c][ty]) continue;
                const Thing *o = thing_at(first[c][ty]);
                std::printf("  %5d new cls %2d type %2d x%d (slot %d state %d at %d,%d,%d)\n", tick, c, ty, count[c][ty],
                            first[c][ty], o->state, (int)o->x, (int)o->y, o->z);
            }
    if (verbose && (pt->state != w.last_state || P->castle != w.last_castle))
        std::printf("  %5d player thing %d state %d health %d/%d mana %d/%d castle %d (level %d) pos %d,%d,%d\n", tick, pidx,
                    pt->state, pt->health, pt->max_health, pt->mana, pt->mana_total, P->castle, P->castle_level, (int)pt->x,
                    (int)pt->y, pt->z);
    for (int id = 0; id < 24; id++) {
        const Thing *sp = P->spell_thing[id] ? thing_at(P->spell_thing[id] % MC_THING_SLOTS) : nullptr;
        int ct = sp ? sp->cast_ticks : 0;
        if (ct > 0 && w.cast_prev[id] <= 0) {
            w.casts[id]++;
            if (verbose)
                std::printf("  %5d CAST spell %2d (cast_ticks %d) mana %d/%d pos %d,%d,%d yaw %d pitch %d\n", tick, id, ct, pt->mana,
                            pt->mana_total, (int)pt->x, (int)pt->y, pt->z, pt->yaw, pt->pitch);
        }
        w.cast_prev[id] = ct;
    }
    w.last_state = pt->state;
    w.last_castle = P->castle;
    w.last_health = pt->health;
    w.last_mana = pt->mana;
}

// Record run. Returns the per-tick hashes (ticks >= 2) in *snaps.
static bool record(const Script &s, const std::string &out, std::vector<Snap> *snaps, bool verbose) {
    fs::create_directories(fs::path(out) / "movie");
    demo_set_record_dir(out.c_str());
    start_level(s.level);
    g_script = &s;
    g_hook_player_local_input = script_input;
    g_cfg->movie = (uint16_t)s.movie;
    Watch w;
    bool started = false;
    for (int steps = 0; steps < 1000000; steps++) {
        if (steps > 0) hud_tick_state(g_state->local_player);
        g_script_tick = steps + 1;
        game_tick_sim();
        const int tick = steps + 1;              // = PlayerRec.tick unless a quick load set it back
        if (verbose) watch_tick(w, tick, verbose);
        if (std::getenv("MC_REC_DEBUG") && steps < 5)
            std::printf("step %d tick %d flags %#x file %u p0 thing %d\n", steps, tick, g_cfg->flags, g_cfg->demo_file,
                        g_state->players[0].thing);
        if (g_cfg->flags & 2) {
            started = true;
            if (snaps) snaps->push_back({tick, state_hash(g_state)});
            rt_dump((out + "/rt_rec.bin").c_str(), tick);
        }
        // the record cave of tools/reference/patch_carpet.py (build_record_cave)
        if ((g_cfg->flags & 2) && g_cfg->demo_file != 0) {
            if (tick >= s.stop) { demo_close(); break; }
        } else if (started) {
            break;                                      // the recording ended by itself (quit packet / error)
        } else if (tick < s.stop && g_state->players[0].thing != 0) {
            g_cfg->flags |= 2;
        }
    }
    g_hook_player_local_input = nullptr;
    std::printf("recorded %ld packets into %s/movie/mvi%05d.dat (level %d, ticks 2..%d)\n", demo_packets_written(),
                out.c_str(), s.movie, s.level, s.stop);
    return started;
}

// Playback of <dir>/movie/mviM.dat in the port, compared with the record run's hashes (in order: playback
// step n executes the same tick as the n-th recorded tick).
static int playback_compare(const Script &s, const std::string &dir, const std::vector<Snap> &snaps) {
    start_level(s.level);
    if (!demo_open(dir.c_str(), s.movie)) { std::printf("  playback: %s/movie/mvi%05d.dat missing\n", dir.c_str(), s.movie); return 1; }
    size_t k = 0;
    int first_bad = 0, compared = 0;
    for (int steps = 0; steps < 1000000; steps++) {
        if (steps > 0) hud_tick_state(g_state->local_player);
        bool more = demo_step();
        const int tick = (int)g_state->players[g_state->local_player & 7].tick;
        (void)tick;
        rt_dump((dir + "/rt_play.bin").c_str(), k < snaps.size() ? snaps[k].tick : -1);
        if (k < snaps.size()) {
            compared++;
            if (state_hash(g_state) != snaps[k].hash && !first_bad) first_bad = snaps[k].tick;
            k++;
        }
        if (!more) break;
    }
    demo_close();
    std::printf("  round trip: %d ticks compared, first tick where playback != record run: %d (0 = none)\n", compared,
                first_bad);
    return first_bad != 0 || compared == 0;
}

static bool same_file(const fs::path &a, const fs::path &b) {
    mc_blob x, y;
    bool ok = mc_read_file(a.string().c_str(), &x);
    bool ok2 = mc_read_file(b.string().c_str(), &y);
    bool same = ok && ok2 && x.len == y.len && std::memcmp(x.data, y.data, x.len) == 0;
    if (ok) mc_blob_free(&x);
    if (ok2) mc_blob_free(&y);
    return same;
}

static int roundtrip_dir(const fs::path &dir, const std::string &scratch) {
    Script s = parse_script((dir / "script.txt").string());
    if (!s.error.empty()) { std::printf("%s: script error: %s\n", dir.string().c_str(), s.error.c_str()); return 1; }
    std::string out = scratch + "/" + dir.filename().string();
    std::vector<Snap> snaps;
    std::printf("%s: level %d movie %d stop %d\n", dir.filename().string().c_str(), s.level, s.movie, s.stop);
    if (!record(s, out, &snaps, false)) { std::printf("  recording did not start\n"); return 1; }
    int rc = playback_compare(s, out, snaps);
    char name[32];
    std::snprintf(name, sizeof name, "mvi%05d.dat", s.movie);
    fs::path stored = dir / "movie" / name;
    if (fs::exists(stored))
        std::printf("  new recording %s the stored one (the original played the stored one)\n",
                    same_file(stored, fs::path(out) / "movie" / name) ? "is identical to" : "DIFFERS from");
    return rc;
}

int main(int argc, char **argv) {
    mc_install_crash_handler();
    setvbuf(stdout, nullptr, _IONBF, 0);
    const char *game_dir = argc > 1 ? argv[1] : MC_DEFAULT_GAME_DIR;
    if (!sim_init(game_dir)) { std::printf("sim_init failed\n"); return 2; }
    sim_register_gameplay();
    if (argc > 4 && std::strcmp(argv[2], "record") == 0) {
        Script s = parse_script(argv[3]);
        if (!s.error.empty()) { std::printf("script error: %s\n", s.error.c_str()); return 2; }
        std::vector<Snap> snaps;
        const bool verbose = std::getenv("MC_REC_QUIET") == nullptr;
        if (!record(s, argv[4], &snaps, verbose)) { std::printf("recording did not start\n"); return 1; }
        fs::copy_file(argv[3], fs::path(argv[4]) / "script.txt", fs::copy_options::overwrite_existing);
        return playback_compare(s, argv[4], snaps);
    }
    fs::path root = argc > 2 ? fs::path(argv[2]) : fs::path(MC_PLAYER_REFERENCE_DIR);
    std::vector<fs::path> dirs;
    std::error_code ec;
    if (fs::exists(root / "script.txt", ec)) dirs.push_back(root);
    else if (fs::is_directory(root, ec))
        for (const auto &e : fs::directory_iterator(root, ec))
            if (e.is_directory() && fs::exists(e.path() / "script.txt")) dirs.push_back(e.path());
    if (dirs.empty()) {
        std::printf("SKIP: no scripted recordings under %s (tools/reference/run_player.py makes them)\n", root.string().c_str());
        return 0;
    }
    std::sort(dirs.begin(), dirs.end());
    std::string scratch = (fs::temp_directory_path() / "mc_reference_player_roundtrip").string();
    int failed = 0;
    for (const fs::path &d : dirs) failed += roundtrip_dir(d, scratch) != 0;
    std::printf("%s: %d of %zu round trips differ\n", failed ? "FAIL" : "OK", failed, dirs.size());
    return failed ? 1 : 0;
}
