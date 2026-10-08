// Debug command language and scenario files (scenario.h; round 10 task B, docs/analysis/port_console.md).
#define _CRT_SECURE_NO_WARNINGS
#include "scenario.h"
#include "debug_cmd.h"
#include "player.h"
#include "mc_globals.h"
#include "mcfile.h"
#include "level.h"
#include "world.h"
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

// ---- the command table (parser, help, completion) ------------------------------------------------------------
struct CmdInfo {
    const char *name;
    const char *usage;
    const char *summary;
    const char *group;
};
const CmdInfo k_cmds[] = {
    // simulation (command packets)
    {"teleport", "teleport X Y [Z] [@P]", "move the wizard to cell X,Y (Z: height in world units, default ground + 0x100)", "sim"},
    {"spawn", "spawn CLASS [TYPE] [X Y] [xN] [ahead D] [@P]",
     "create N Things (level-file spawn) at cell X,Y, or D cells (default 4) in front of the wizard; "
     "CLASS TYPE by number or name: spawn dragon x3, spawn creature 2 40 52, spawn spell fireball", "sim"},
    {"give", "give mana N [wizard|castle|ball] [@P]  |  give spells [@P]",
     "mana: add N to the wizard (default, up to its mana_total), to the castle, or as a mana ball (cheat 2); "
     "spells: every spell (cheat 1)", "sim"},
    {"god", "god [on|off|toggle] [@P]", "health and mana kept full every tick, damage discarded (default toggle)", "sim"},
    {"kill", "kill #S | kill CLASS [TYPE] | kill creatures | kill wizard [N] | kill castle [N]",
     "kill Thing slot S, every Thing of a class / type (never your own wizard), or a player's wizard / castle", "sim"},
    {"claim", "claim #S [@P]", "Thing slot S now belongs to the player (a mana ball: its mana)", "sim"},
    {"heal", "heal [wizard|castle|all] [@P]", "health back to max", "sim"},
    {"spells", "spells [@P]", "every spell the wizard lacks (cheat 1, mana cost 0)", "sim"},
    {"win", "win [mark] [@P]", "level won and left (mark: only marked as won, the level goes on)", "sim"},
    {"lose", "lose [mark] [@P]", "level lost (the campaign restarts it; mark: only marked)", "sim"},
    {"damage", "damage N [wizard|castle] [@P]", "N damage through the game's damage path (shield / god apply)", "sim"},
    {"cmd", "cmd C [A [P2]]", "a raw command packet of the original (command C, arg A, byte 2 P2)", "sim"},
    {"cheat", "cheat N", "the original's cheat N (command 0x1e): 1 spells, 2 mana, 3-5 destroy wizards / castles / balloons, 6 heal, 7 kill creatures", "sim"},
    {"book", "book", "open the spell book (command 0x14 arg 2)", "sim"},
    {"close", "close", "close the spell book (command 0x14 arg 0)", "sim"},
    {"packet", "packet B0 B1 .. B9", "a raw 10-byte command packet (debug_cmd.h layout)", "sim"},
    // host
    {"time", "time pause|resume|toggle|step [N]|speed X|normal|run", "pause, single-step N ticks, speed x2 .. x64, 1/2 .. 1/16, max", "host"},
    {"shot", "shot [NAME]", "screenshot of the next presented frame", "host"},
    {"save", "save N", "save the state into slot N (0..9)", "host"},
    {"load", "load N", "load the state of slot N", "host"},
    {"sync", "sync", "network sync status", "host"},
    {"dump", "dump", "write a JSON dump of the game now", "host"},
    {"cam", "cam free|follow [S]|to X Y|off", "debug camera: free flight, follow Thing slot S, jump to cell X,Y, back to the wizard", "host"},
    {"overlay", "overlay NAME [on|off] | overlay list", "debug overlays (grid, labels, ...)", "host"},
    {"inspect", "inspect S | inspect cursor | inspect off", "Thing inspector panel for slot S / the Thing under the pointer", "host"},
    {"run", "run FILE", "run a scenario file from now (its level / rts header is ignored)", "host"},
    {"quit", "quit", "leave the port (a scenario: end it)", "host"},
    {"echo", "echo TEXT", "print TEXT", "host"},
    // queries
    {"where", "where [WHO]", "position, health and mana of a player / castle / slot", "query"},
    {"find", "find CLASS [TYPE]", "list the live Things of a class / type (slot, cell, health)", "query"},
    {"count", "count CLASS [TYPE]", "number of live Things of a class / type", "query"},
    {"players", "players", "every player record: wizard, health, mana, castle, god, status", "query"},
    {"queue", "queue", "commands waiting for the command slot", "query"},
    {"help", "help [COMMAND]", "this list / one command", "query"},
    // assertions
    {"assert_health", "assert_health WHO OP V", "health of WHO compared with V (a number or max)", "assert"},
    {"assert_mana", "assert_mana WHO OP V", "mana of WHO compared with V (a number or max)", "assert"},
    {"assert_count", "assert_count CLASS [TYPE] OP N", "number of live Things of a class / type", "assert"},
    {"assert_alive", "assert_alive WHO", "WHO exists and its health is >= 0", "assert"},
    {"assert_dead", "assert_dead WHO", "WHO is gone or its health is < 0", "assert"},
    {"assert_pos", "assert_pos WHO X Y [R]", "WHO within R cells (default 1) of cell X,Y", "assert"},
    {"assert_status", "assert_status WHO won|lost|running", "the player's level status", "assert"},
    {"assert_god", "assert_god WHO on|off", "god mode of the player", "assert"},
    // scenario only
    {"wait", "wait N", "scenario files: later untimed lines run N ticks later", "scenario"},
    {"steer", "T[-T2] steer X Y", "scenario input: steering bytes of the local packet", "scenario"},
    {"keys", "T[-T2] keys B", "scenario input: input bits (1 faster, 2 slower, 4 / 8 strafe, 0x10 / 0x20 cast)", "scenario"},
    {"cast", "T[-T2] cast left|right", "scenario input: = keys 0x10 / 0x20", "scenario"},
    {"left", "T left S", "scenario input: the book slot holding spell id S into the left hand", "scenario"},
    {"right", "T right S", "scenario input: ... into the right hand", "scenario"},
    {"respawn", "T[-T2] respawn", "scenario input: respawn while dead (and the castle stands)", "scenario"},
    {"rebuild", "T[-T2] rebuild", "scenario input: no castle: castle spell to the right hand and cast it", "scenario"},
    {"face", "T[-T2] face X Y [Z]", "scenario input: steer towards world point X,Y", "scenario"},
    {"fly", "T[-T2] fly X Y", "scenario input: face the world point and fly there", "scenario"},
    {"faceth", "T[-T2] faceth S", "scenario input: face Thing slot S", "scenario"},
    {"face_class", "T[-T2] face_class C TYPE [D]", "scenario input: face the nearest Thing of class C / TYPE (-1 any); D: chase it", "scenario"},
};

const CmdInfo *find_cmd(const std::string &name) {
    for (const CmdInfo &c : k_cmds)
        if (name == c.name) return &c;
    return nullptr;
}

std::string lower(std::string s) {
    for (char &c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}
std::string norm_name(const char *s) {
    std::string r;
    for (; *s; s++) {
        char c = (char)std::tolower((unsigned char)*s);
        r += (c == ' ' || c == '-') ? '_' : c;
    }
    return r;
}

bool parse_int(const std::string &s, int64_t *v) {
    if (s.empty()) return false;
    char *end = nullptr;
    const long long x = std::strtoll(s.c_str(), &end, 0);
    if (!end || *end) return false;
    *v = x;
    return true;
}
bool parse_int(const std::string &s, int *v) {
    int64_t x;
    if (!parse_int(s, &x) || x < INT32_MIN || x > INT32_MAX) return false;
    *v = (int)x;
    return true;
}
// A coordinate: cells (cell centre) or world units with a `w` suffix. Result in world units.
// Round 11: masked to the largest world (MC_WORLD_MAX cells, 20 bits of units) instead of 256 cells - a
// command is parsed before the world it runs in may exist; the packet / the store wraps it onto the world
// (identical to the old & 0xff / & 0xffff in a 256-cell world).
bool parse_coord(const std::string &s, int *world) {
    constexpr int kUnitMask = MC_WORLD_MAX * 256 - 1, kCellMask = MC_WORLD_MAX - 1;
    if (s.size() > 1 && (s.back() == 'w' || s.back() == 'W')) {
        int v;
        if (!parse_int(s.substr(0, s.size() - 1), &v)) return false;
        *world = v & kUnitMask;
        return true;
    }
    int c;
    if (!parse_int(s, &c)) return false;
    *world = ((c & kCellMask) << 8) | 0x80;
    return true;
}

bool parse_op(const std::string &s, int *op) {
    if (s == "==" || s == "=") *op = OP_EQ;
    else if (s == "!=") *op = OP_NE;
    else if (s == "<") *op = OP_LT;
    else if (s == "<=") *op = OP_LE;
    else if (s == ">") *op = OP_GT;
    else if (s == ">=") *op = OP_GE;
    else return false;
    return true;
}
const char *op_text(int op) {
    static const char *k[] = {"==", "!=", "<", "<=", ">", ">="};
    return op >= 0 && op < 6 ? k[op] : "?";
}
bool compare(int64_t a, int op, int64_t b) {
    switch (op) {
    case OP_EQ: return a == b;
    case OP_NE: return a != b;
    case OP_LT: return a < b;
    case OP_LE: return a <= b;
    case OP_GT: return a > b;
    default: return a >= b;
    }
}

// "me" | "pN" | "N" | "castle" | "castleN" | "#S" | "slotS"
bool parse_who(const std::string &w0, CmdWho *who) {
    const std::string w = lower(w0);
    int n = 0;
    if (w == "me") { *who = CmdWho{CmdWho::PLAYER, -1}; return true; }
    if (w.size() > 1 && w[0] == 'p' && parse_int(w.substr(1), &n) && n >= 0 && n < 8) { *who = CmdWho{CmdWho::PLAYER, n}; return true; }
    if (w == "castle") { *who = CmdWho{CmdWho::CASTLE, -1}; return true; }
    if (w.rfind("castle", 0) == 0 && parse_int(w.substr(6), &n) && n >= 0 && n < 8) { *who = CmdWho{CmdWho::CASTLE, n}; return true; }
    if (w.size() > 1 && w[0] == '#' && parse_int(w.substr(1), &n) && n > 0) { *who = CmdWho{CmdWho::SLOT, n}; return true; }
    if (w.rfind("slot", 0) == 0 && parse_int(w.substr(4), &n) && n > 0) { *who = CmdWho{CmdWho::SLOT, n}; return true; }
    return false;
}

struct ClassName { const char *name; int cls; };
const ClassName k_classes[] = {
    {"scenery", 2}, {"player", 3}, {"players", 3}, {"creature", 5}, {"creatures", 5}, {"weather", 7},
    {"projectile", 9}, {"projectiles", 9}, {"effect", 10}, {"effects", 10}, {"switch", 11}, {"switches", 11},
    {"spell", 12}, {"spells", 12},
};
const char *k_player_types[] = {"wizard", "computer_wizard", "castle", "balloon"};

const char *model_name_raw(int cls, int type) {
    if (cls == 3) return type >= 0 && type < 4 ? k_player_types[type] : "?";
    return mc_model_name(cls, type);
}
// The type `name` names inside class `cls` (-1: none).
int type_in_class(int cls, const std::string &name) {
    if (cls == 3 && (name == "ai" || name == "computer" || name == "bot")) return 1;
    if (cls == 3 && name == "flyer") return 0;
    for (int t = 0; t < 64; t++) {
        const char *m = model_name_raw(cls, t);
        if (!m || !std::strcmp(m, "?") || (cls == 10 && !std::strcmp(m, "Effect"))) continue;
        if (norm_name(m) == name) return t;
    }
    return -1;
}

// Splits a line into words; '#' followed by a non-digit (or the end) starts a comment, "#12" is a slot.
std::vector<std::string> split(const std::string &line, std::string *src) {
    std::string s = line;
    for (size_t i = 0; i < s.size(); i++)
        if (s[i] == '#' && (i + 1 >= s.size() || !std::isdigit((unsigned char)s[i + 1]))) { s.resize(i); break; }
    std::vector<std::string> w;
    for (size_t i = 0; i < s.size();) {
        while (i < s.size() && std::isspace((unsigned char)s[i])) i++;
        size_t j = i;
        while (j < s.size() && !std::isspace((unsigned char)s[j])) j++;
        if (j > i) w.push_back(s.substr(i, j - i));
        i = j;
    }
    if (src) {
        size_t a = 0, e = s.size();
        while (a < e && std::isspace((unsigned char)s[a])) a++;
        while (e > a && std::isspace((unsigned char)s[e - 1])) e--;
        *src = s.substr(a, e - a);
    }
    return w;
}

// Removes an "@N" / "@me" / "@pN" target word; -1 = bad.
int take_target(std::vector<std::string> *w, std::string *err) {
    int target = DEBUG_TARGET_SELF;
    for (size_t i = 1; i < w->size(); i++) {
        const std::string &x = (*w)[i];
        if (x.empty() || x[0] != '@') continue;
        std::string v = lower(x.substr(1));
        if (!v.empty() && v[0] == 'p') v = v.substr(1);
        int n = 0;
        if (v == "me" || v == "e") target = DEBUG_TARGET_SELF;
        else if (parse_int(v, &n) && n >= 0 && n < 8) target = n;
        else { *err = "bad target " + x + " (@0..@7 or @me)"; return -1; }
        w->erase(w->begin() + (long)i);
        i--;
    }
    return target;
}

ParsedCommand fail(ParsedCommand c, const std::string &why) {
    c.kind = CmdKind::INVALID;
    const CmdInfo *ci = find_cmd(c.verb);
    c.error = why + (ci ? std::string(" - usage: ") + ci->usage : std::string());
    return c;
}

ParsedCommand packet_cmd(ParsedCommand c, const CmdPacket &p) {
    c.kind = CmdKind::PACKET;
    c.packet = p;
    return c;
}

// The player block of a living wizard Thing of player p (null otherwise).
int local_player() { return g_state->local_player & 7; }
Thing *wizard_thing(int p) {
    if (p < 0) p = local_player();
    if (p > 7) return nullptr;
    Thing *t = thing_at(thing_wrap(g_state->players[p].thing));
    if (t == thing_at(0) || t->cls != 3 || (t->type != 0 && t->type != 1)) return nullptr;
    return t;
}
Thing *castle_thing(int p) {
    Thing *w = wizard_thing(p);
    if (!w) return nullptr;
    const unsigned c = player_block(w)->castle;
    if (c == 0 || c >= (unsigned)thing_pool_slots()) return nullptr;
    Thing *t = thing_at(c);
    return t->cls == 3 && t->type == 2 ? t : nullptr;
}
Thing *who_thing(const CmdWho &w) {
    switch (w.kind) {
    case CmdWho::PLAYER: return wizard_thing(w.n);
    case CmdWho::CASTLE: return castle_thing(w.n);
    default: {
        if (w.n <= 0 || w.n >= thing_pool_slots()) return nullptr;
        Thing *t = thing_at((unsigned)w.n);
        return t->cls ? t : nullptr;
    }
    }
}
std::string who_text(const CmdWho &w) {
    char b[32];
    switch (w.kind) {
    case CmdWho::PLAYER: if (w.n < 0) return "me"; std::snprintf(b, sizeof b, "p%d", w.n); break;
    case CmdWho::CASTLE: if (w.n < 0) return "castle"; std::snprintf(b, sizeof b, "castle%d", w.n); break;
    default: std::snprintf(b, sizeof b, "#%d", w.n); break;
    }
    return b;
}
// A body segment of a dragon / worm / kraken: chained to the Thing before it through parent / child
// (constructors.cpp creature_create_tail16). Counted with its head, not on its own.
bool is_segment(const Thing *t) {
    if (t->cls != 5 || t->parent == 0 || t->parent >= (unsigned)thing_pool_slots()) return false;
    const Thing *p = thing_at(t->parent);
    return p->cls == 5 && p->type == t->type && p->child == thing_index(t);
}
bool live(const Thing *t) {
    if (t->cls == 0 || (t->flags & 0x400) || is_segment(t)) return false;
    if ((t->cls == 3 || t->cls == 5) && t->health < 0) return false;
    return true;
}
int count_live(int cls, int type) {
    int n = 0;
    for (int i = 1; i < thing_pool_slots(); i++) {
        const Thing *t = thing_at((unsigned)i);
        if (t->cls != cls || (type >= 0 && t->type != type) || !live(t)) continue;
        n++;
    }
    return n;
}

} // namespace

// ---- names --------------------------------------------------------------------------------------------------
bool cmd_parse_class_type(const std::vector<std::string> &w, size_t *i, int *cls, int *type, bool type_required) {
    *cls = *type = -1;
    if (*i >= w.size()) return false;
    const std::string a = lower(w[*i]);
    int n = 0;
    bool have_cls = false;
    for (const ClassName &c : k_classes)
        if (a == c.name) { *cls = c.cls; have_cls = true; break; }
    if (!have_cls && parse_int(a, &n) && n >= 0 && n < 16) { *cls = n; have_cls = true; }
    if (have_cls) {
        (*i)++;
        if (*i < w.size()) {
            const std::string t = lower(w[*i]);
            int tn;
            if (t == "any") { *type = -1; (*i)++; return !type_required; }
            if (parse_int(t, &tn) && tn >= 0 && tn < 256) { *type = tn; (*i)++; return true; }
            const int byname = type_in_class(*cls, t);
            if (byname >= 0) { *type = byname; (*i)++; return true; }
        }
        return !type_required;
    }
    // a type name alone: player types, creatures, spells, scenery, effects
    static const int order[] = {3, 5, 12, 2, 10};
    for (int c : order) {
        const int t = type_in_class(c, a);
        if (t >= 0) { *cls = c; *type = t; (*i)++; return true; }
    }
    return false;
}

std::string cmd_thing_name(int cls, int type) {
    char b[96];
    const char *cn = cls == 9 ? "Projectile" : cls == 3 ? "Player" : mc_class_name(cls);
    if (type < 0) std::snprintf(b, sizeof b, "%s (%d)", cn, cls);
    else std::snprintf(b, sizeof b, "%s %s (%d,%d)", cn, model_name_raw(cls, type), cls, type);
    return b;
}

// ---- parsing ------------------------------------------------------------------------------------------------
ParsedCommand cmd_parse(const std::string &line) {
    ParsedCommand c;
    std::vector<std::string> w = split(line, &c.source);
    if (w.empty()) return c;
    c.verb = lower(w[0]);
    const std::string &v = c.verb;
    std::string err;
    auto num = [&](size_t i, int *out) { return i < w.size() && parse_int(w[i], out); };
    auto word = [&](size_t i) { return i < w.size() ? lower(w[i]) : std::string(); };

    // ---- simulation commands ----
    if (v == "teleport" || v == "tp") {
        c.verb = "teleport";
        const int target = take_target(&w, &err);
        if (target < 0) return fail(c, err);
        int x, y, z = DEBUG_Z_GROUND;
        if (w.size() < 3 || !parse_coord(w[1], &x) || !parse_coord(w[2], &y)) return fail(c, "teleport needs X Y");
        if (w.size() > 3 && !parse_int(w[3], &z)) return fail(c, "bad Z " + w[3]);
        if (w.size() > 4) return fail(c, "too many arguments");
        return packet_cmd(c, debug_cmd_teleport(x, y, z, target));
    }
    if (v == "spawn") {
        const int target = take_target(&w, &err);
        if (target < 0) return fail(c, err);
        size_t i = 1;
        int cls, type;
        if (!cmd_parse_class_type(w, &i, &cls, &type, true) || type < 0) return fail(c, "unknown class / type");
        int count = 1, ahead = 0, x = 0, y = 0;
        bool at = false;
        std::vector<std::string> rest(w.begin() + (long)i, w.end());
        std::vector<std::string> coords;
        for (size_t k = 0; k < rest.size(); k++) {
            const std::string r = lower(rest[k]);
            if (r.size() > 1 && r[0] == 'x' && parse_int(r.substr(1), &count)) continue;
            if (r == "count" && k + 1 < rest.size() && parse_int(rest[k + 1], &count)) { k++; continue; }
            if (r == "ahead" && k + 1 < rest.size() && parse_int(rest[k + 1], &ahead)) { k++; continue; }
            coords.push_back(rest[k]);
        }
        if (coords.size() == 2) {
            if (!parse_coord(coords[0], &x) || !parse_coord(coords[1], &y)) return fail(c, "bad X Y");
            at = true;
        } else if (!coords.empty()) {
            return fail(c, "unexpected " + coords[0]);
        }
        if (count < 1 || count > 64) return fail(c, "count 1..64");
        if (ahead < 0 || ahead > 255) return fail(c, "ahead 0..255");
        return packet_cmd(c, debug_cmd_spawn(cls, type, x >> 8, y >> 8, at, count, target, ahead));
    }
    if (v == "give") {
        const int target = take_target(&w, &err);
        if (target < 0) return fail(c, err);
        const std::string what = word(1);
        if (what == "spells") return packet_cmd(c, debug_cmd_spells(target));
        int n = 0;
        if (what != "mana" || !num(2, &n)) return fail(c, "give mana N / give spells");
        int where = DEBUG_MANA_WIZARD;
        const std::string to = word(3);
        if (to == "castle") where = DEBUG_MANA_CASTLE;
        else if (to == "ball") where = DEBUG_MANA_BALL;
        else if (!to.empty() && to != "wizard") return fail(c, "give mana N wizard|castle|ball");
        return packet_cmd(c, debug_cmd_give_mana(n, where, target));
    }
    if (v == "god") {
        const int target = take_target(&w, &err);
        if (target < 0) return fail(c, err);
        const std::string m = word(1);
        int mode = DEBUG_GOD_TOGGLE;
        if (m == "on" || m == "1") mode = DEBUG_GOD_ON;
        else if (m == "off" || m == "0") mode = DEBUG_GOD_OFF;
        else if (!m.empty() && m != "toggle") return fail(c, "god on|off|toggle");
        return packet_cmd(c, debug_cmd_god(mode, target));
    }
    if (v == "kill") {
        const int target = take_target(&w, &err);
        if (target < 0) return fail(c, err);
        const std::string a = word(1);
        CmdWho who;
        if (a.empty()) return fail(c, "kill what?");
        if (a == "wizard" || a == "castle") {
            int p = target;
            if (w.size() > 2) {
                CmdWho wp;
                int n = 0;
                if (parse_int(w[2], &n) && n >= 0 && n < 8) p = n;
                else if (parse_who(w[2], &wp) && wp.kind == CmdWho::PLAYER) p = wp.n < 0 ? DEBUG_TARGET_SELF : wp.n;
                else return fail(c, "bad player " + w[2]);
            }
            return packet_cmd(c, a == "wizard" ? debug_cmd_kill_wizard(p) : debug_cmd_kill_castle(p));
        }
        if (parse_who(a, &who) && who.kind == CmdWho::SLOT) {
            int cls = DEBUG_ANY, type = DEBUG_ANY;
            size_t i = 2;
            if (i < w.size()) {
                if (!cmd_parse_class_type(w, &i, &cls, &type, false)) return fail(c, "bad class / type");
                if (type < 0) type = DEBUG_ANY;
            }
            return packet_cmd(c, debug_cmd_kill_slot(who.n, cls, type));
        }
        if (a == "all") return packet_cmd(c, debug_cmd_kill_class(5, DEBUG_ANY));
        size_t i = 1;
        int cls, type;
        if (!cmd_parse_class_type(w, &i, &cls, &type, false) || i != w.size()) return fail(c, "unknown class / type");
        return packet_cmd(c, debug_cmd_kill_class(cls, type < 0 ? DEBUG_ANY : type));
    }
    if (v == "claim") {
        const int target = take_target(&w, &err);
        if (target < 0) return fail(c, err);
        CmdWho who;
        int n = 0;
        if (w.size() > 1 && parse_who(w[1], &who) && who.kind == CmdWho::SLOT) n = who.n;
        else if (!num(1, &n) || n <= 0) return fail(c, "claim #S");
        return packet_cmd(c, debug_cmd_claim(n, DEBUG_ANY, DEBUG_ANY, target));
    }
    if (v == "heal") {
        const int target = take_target(&w, &err);
        if (target < 0) return fail(c, err);
        const std::string a = word(1);
        int what = DEBUG_HEAL_WIZARD;
        if (a == "castle") what = DEBUG_HEAL_CASTLE;
        else if (a == "all") what = DEBUG_HEAL_WIZARD | DEBUG_HEAL_CASTLE;
        else if (!a.empty() && a != "wizard") return fail(c, "heal wizard|castle|all");
        return packet_cmd(c, debug_cmd_heal(what, target));
    }
    if (v == "spells") {
        const int target = take_target(&w, &err);
        if (target < 0) return fail(c, err);
        if (w.size() > 1) return fail(c, "too many arguments");
        return packet_cmd(c, debug_cmd_spells(target));
    }
    if (v == "win" || v == "lose") {
        const int target = take_target(&w, &err);
        if (target < 0) return fail(c, err);
        const std::string a = word(1);
        if (!a.empty() && a != "mark") return fail(c, "only `mark` may follow");
        return packet_cmd(c, debug_cmd_level_end(v == "win" ? DEBUG_END_WIN : DEBUG_END_LOSE, a == "mark", target));
    }
    if (v == "damage") {
        const int target = take_target(&w, &err);
        if (target < 0) return fail(c, err);
        int n = 0;
        if (!num(1, &n)) return fail(c, "damage N");
        const std::string a = word(2);
        if (!a.empty() && a != "wizard" && a != "castle") return fail(c, "damage N wizard|castle");
        return packet_cmd(c, debug_cmd_damage(n, a == "castle" ? 1 : 0, target));
    }
    if (v == "cmd" || v == "cheat" || v == "book" || v == "close") {
        CmdPacket p;
        std::memset(&p, 0, sizeof p);
        int a = 0, b2 = 0, cc = 0;
        if (v == "cmd") {
            if (!num(1, &a) || a < 0 || a > 255) return fail(c, "cmd C");
            if (w.size() > 2 && !num(2, &b2)) return fail(c, "bad A");
            if (w.size() > 3 && !num(3, &cc)) return fail(c, "bad P2");
            p.cmd = (uint8_t)a; p.arg = (uint8_t)b2; p.pad2 = (uint8_t)cc;
        } else if (v == "cheat") {
            if (!num(1, &a) || a < 1 || a > 7) return fail(c, "cheat 1..7");
            p.cmd = 0x1e; p.arg = (uint8_t)a;
        } else {
            p.cmd = 0x14; p.arg = v == "book" ? 2 : 0;
        }
        return packet_cmd(c, p);
    }
    if (v == "packet") {
        if (w.size() < 2 || w.size() > 11) return fail(c, "packet B0 [B1 .. B9]");
        CmdPacket p;
        std::memset(&p, 0, sizeof p);
        uint8_t *q = reinterpret_cast<uint8_t *>(&p);
        for (size_t i = 1; i < w.size(); i++) {
            int x;
            if (!parse_int(w[i], &x) || x < -128 || x > 255) return fail(c, "bad byte " + w[i]);
            q[i - 1] = (uint8_t)x;
        }
        return packet_cmd(c, p);
    }

    // ---- host commands ----
    auto host = [&](HostOp op) {
        c.kind = CmdKind::HOST;
        c.host.op = op;
        return c;
    };
    if (v == "time") {
        const std::string a = word(1);
        c.host.sub = a.empty() ? "toggle" : a;
        if (a == "step") {
            int n = 1;
            if (w.size() > 2 && (!num(2, &n) || n < 1)) return fail(c, "time step N (N >= 1)");
            c.host.n = n;
        } else if (a == "speed") {
            if (w.size() < 3) return fail(c, "time speed X");
            c.host.text = w[2];
        } else if (a == "run") {
            c.host.sub = "resume";
        } else if (!a.empty() && a != "pause" && a != "resume" && a != "toggle" && a != "normal") {
            return fail(c, "unknown time command " + a);
        }
        return host(HostOp::TIME);
    }
    if (v == "shot" || v == "screenshot") {
        c.verb = "shot";
        if (w.size() > 2) return fail(c, "shot [NAME]");
        c.host.text = w.size() > 1 ? w[1] : "";
        return host(HostOp::SHOT);
    }
    if (v == "save" || v == "load") {
        int n = 0;
        if (!num(1, &n) || n < 0 || n > 9) return fail(c, v + " N (0..9)");
        c.host.n = n;
        return host(v == "save" ? HostOp::SAVE : HostOp::LOAD);
    }
    if (v == "sync") return host(HostOp::SYNC);
    if (v == "dump") return host(HostOp::DUMP);
    if (v == "cam" || v == "camera") {
        c.verb = "cam";
        const std::string a = word(1);
        c.host.sub = a;
        c.host.n = -1;
        if (a == "free" || a == "off") return host(HostOp::CAM);
        if (a == "follow") {
            if (w.size() > 2) {
                CmdWho who;
                int n = 0;
                if (parse_who(w[2], &who) && who.kind == CmdWho::SLOT) c.host.n = who.n;
                else if (parse_int(w[2], &n) && n > 0) c.host.n = n;
                else return fail(c, "cam follow [#S]");
            }
            return host(HostOp::CAM);
        }
        if (a == "to") {
            if (w.size() != 4 || !parse_coord(w[2], &c.host.x) || !parse_coord(w[3], &c.host.y)) return fail(c, "cam to X Y");
            return host(HostOp::CAM);
        }
        return fail(c, "cam free|follow|to|off");
    }
    if (v == "overlay") {
        if (w.size() < 2) return fail(c, "overlay NAME [on|off]");
        c.host.sub = lower(w[1]);
        const std::string a = word(2);
        if (a == "on" || a == "1") c.host.on = 1;
        else if (a == "off" || a == "0") c.host.on = 0;
        else if (!a.empty() && a != "toggle") return fail(c, "overlay NAME on|off");
        return host(HostOp::OVERLAY);
    }
    if (v == "inspect") {
        const std::string a = word(1);
        CmdWho who;
        int n = 0;
        if (a == "cursor" || a == "off") { c.host.sub = a; c.host.n = -1; return host(HostOp::INSPECT); }
        if (w.size() > 1 && parse_who(w[1], &who) && who.kind == CmdWho::SLOT) n = who.n;
        else if (!num(1, &n) || n <= 0) return fail(c, "inspect S|cursor|off");
        c.host.sub = "slot";
        c.host.n = n;
        return host(HostOp::INSPECT);
    }
    if (v == "run") {
        if (w.size() != 2) return fail(c, "run FILE");
        c.host.text = w[1];
        return host(HostOp::RUN);
    }
    if (v == "quit" || v == "exit") { c.verb = "quit"; return host(HostOp::QUIT); }
    if (v == "echo") {
        const size_t p = c.source.find_first_of(" \t");
        c.host.text = p == std::string::npos ? "" : c.source.substr(c.source.find_first_not_of(" \t", p));
        return host(HostOp::ECHO);
    }

    // ---- queries / help ----
    if (v == "help" || v == "?") {
        c.verb = "help";
        c.kind = CmdKind::TEXT;
        c.text = cmd_help(word(1));
        return c;
    }
    if (v == "where" || v == "find" || v == "count" || v == "players" || v == "queue") {
        if ((v == "find" || v == "count")) {
            size_t i = 1;
            int cls, type;
            if (!cmd_parse_class_type(w, &i, &cls, &type, false) || i != w.size()) return fail(c, "unknown class / type");
        }
        if (v == "where" && w.size() > 1) {
            CmdWho who;
            if (!parse_who(w[1], &who)) return fail(c, "bad WHO " + w[1]);
        }
        c.kind = CmdKind::QUERY;
        c.text = c.source;
        return c;
    }

    // ---- assertions ----
    if (v.rfind("assert_", 0) == 0) {
        Assertion &a = c.check;
        if (v == "assert_health" || v == "assert_mana") {
            a.kind = v == "assert_health" ? AssertKind::HEALTH : AssertKind::MANA;
            if (w.size() != 4 || !parse_who(w[1], &a.who) || !parse_op(w[2], &a.op)) return fail(c, "bad arguments");
            if (lower(w[3]) == "max") a.value_max = true;
            else if (!parse_int(w[3], &a.value)) return fail(c, "bad value " + w[3]);
        } else if (v == "assert_count") {
            a.kind = AssertKind::COUNT;
            size_t i = 1;
            if (!cmd_parse_class_type(w, &i, &a.cls, &a.type, false)) return fail(c, "unknown class / type");
            if (w.size() != i + 2 || !parse_op(w[i], &a.op) || !parse_int(w[i + 1], &a.value)) return fail(c, "bad arguments");
        } else if (v == "assert_alive" || v == "assert_dead") {
            a.kind = v == "assert_alive" ? AssertKind::ALIVE : AssertKind::DEAD;
            if (w.size() != 2 || !parse_who(w[1], &a.who)) return fail(c, "bad arguments");
        } else if (v == "assert_pos") {
            a.kind = AssertKind::POS;
            int r = 1;
            if (w.size() < 4 || w.size() > 5 || !parse_who(w[1], &a.who) || !parse_coord(w[2], &a.x) || !parse_coord(w[3], &a.y) ||
                (w.size() == 5 && (!parse_int(w[4], &r) || r < 0)))
                return fail(c, "bad arguments");
            a.r = r * 0x100;
        } else if (v == "assert_status") {
            a.kind = AssertKind::STATUS;
            const std::string s = word(2);
            if (w.size() != 3 || !parse_who(w[1], &a.who) || a.who.kind != CmdWho::PLAYER) return fail(c, "bad arguments");
            if (s == "won") a.status = 2;
            else if (s == "lost") a.status = 4;
            else if (s == "running") a.status = 0;
            else return fail(c, "won|lost|running");
        } else if (v == "assert_god") {
            a.kind = AssertKind::GOD;
            const std::string s = word(2);
            if (w.size() != 3 || !parse_who(w[1], &a.who) || a.who.kind != CmdWho::PLAYER || (s != "on" && s != "off"))
                return fail(c, "bad arguments");
            a.on = s == "on";
        } else {
            return fail(c, "unknown assertion " + v);
        }
        c.kind = CmdKind::ASSERT;
        return c;
    }

    // ---- scenario only ----
    if (v == "wait") {
        int n = 0;
        if (!num(1, &n) || n < 0) return fail(c, "wait N");
        c.kind = CmdKind::WAIT;
        c.wait = n;
        return c;
    }
    {
        InputLine &l = c.input;
        auto arg = [&](size_t i, int def) { int x; return i < w.size() && parse_int(w[i], &x) ? x : def; };
        bool ok = true;
        if (v == "steer") { l.verb = InputVerb::STEER; l.a = arg(1, 0); l.b = arg(2, 0); }
        else if (v == "keys") { l.verb = InputVerb::KEYS; l.a = arg(1, 0); }
        else if (v == "cast") { l.verb = InputVerb::KEYS; l.a = word(1) == "right" ? 0x20 : 0x10; }
        else if (v == "left") { l.verb = InputVerb::LEFT; l.a = arg(1, 0); }
        else if (v == "right") { l.verb = InputVerb::RIGHT; l.a = arg(1, 0); }
        else if (v == "respawn") { l.verb = InputVerb::RESPAWN; }
        else if (v == "rebuild") { l.verb = InputVerb::REBUILD; }
        else if (v == "face") { l.verb = InputVerb::FACE; l.a = arg(1, 0); l.b = arg(2, 0); l.c = arg(3, 0); l.has_c = w.size() > 3; }
        else if (v == "fly") { l.verb = InputVerb::FLY; l.a = arg(1, 0); l.b = arg(2, 0); }
        else if (v == "faceth") { l.verb = InputVerb::FACE_THING; l.a = arg(1, 0); }
        else if (v == "face_class") { l.verb = InputVerb::FACE_CLASS; l.a = arg(1, 0); l.b = arg(2, -1); l.c = arg(3, 0); }
        else ok = false;
        if (ok) { c.kind = CmdKind::INPUT; return c; }
    }
    c.kind = CmdKind::INVALID;
    c.error = "unknown command '" + w[0] + "' (help lists the commands)";
    return c;
}

// ---- help ---------------------------------------------------------------------------------------------------
std::string cmd_help(const std::string &topic0) {
    const std::string topic = lower(topic0);
    std::string out;
    if (!topic.empty()) {
        const CmdInfo *ci = find_cmd(topic);
        if (!ci) {
            for (const CmdInfo &c : k_cmds)
                if (topic == c.group) out += std::string(c.usage) + "  - " + c.summary + "\n";
            if (out.empty()) return "no command '" + topic0 + "' (help lists them)";
            return out.substr(0, out.size() - 1);
        }
        return std::string(ci->usage) + "\n  " + ci->summary;
    }
    static const struct { const char *group, *title; } groups[] = {
        {"sim", "game (command packets)"}, {"host", "host"}, {"query", "queries"}, {"assert", "assertions"},
        {"scenario", "scenario files only"}};
    for (const auto &g : groups) {
        out += std::string(g.title) + ":";
        for (const CmdInfo &c : k_cmds)
            if (!std::strcmp(c.group, g.group)) out += std::string(" ") + c.name;
        out += "\n";
    }
    out += "help COMMAND or help sim|host|query|assert|scenario for details; WHO = me pN castle castleN #SLOT; "
           "X Y = cells (or 1234w world units); @N = target player";
    return out;
}

std::vector<std::string> cmd_complete(const std::string &prefix) {
    std::vector<std::string> r;
    const std::string p = lower(prefix);
    for (const CmdInfo &c : k_cmds)
        if (std::strncmp(c.name, p.c_str(), p.size()) == 0) r.push_back(c.name);
    return r;
}

// ---- queries and assertions -----------------------------------------------------------------------------------
std::string cmd_query(const ParsedCommand &q) {
    std::vector<std::string> w = split(q.text, nullptr);
    if (w.empty() || !g_state) return "";
    const std::string v = lower(w[0]);
    char b[256];
    if (v == "queue") {
        std::snprintf(b, sizeof b, "%d command(s) waiting for the command slot", debug_cmd_pending());
        return b;
    }
    if (v == "players") {
        std::string out;
        const int n = (int)(uint16_t)g_state->player_count;
        for (int p = 0; p < 8 && p < n; p++) {
            const PlayerRec &r = g_state->players[p];
            Thing *t = wizard_thing(p);
            Thing *c = castle_thing(p);
            std::snprintf(b, sizeof b, "p%d%s %s%s slot %d health %d/%d mana %d/%d castle %d (level %d, mana %d)%s status %d\n", p,
                          p == local_player() ? "*" : "", r.active ? "active" : "inactive", r.is_computer == 1 ? " computer" : "",
                          t ? (int)thing_index(t) : 0, t ? t->health : 0, t ? t->max_health : 0, t ? t->mana : 0,
                          t ? t->mana_total : 0, c ? (int)thing_index(c) : 0, c ? c->aux + 1 : 0, c ? c->mana : 0,
                          debug_god(p) ? " GOD" : "", r.status);
            out += b;
        }
        if (!out.empty()) out.pop_back();
        return out;
    }
    if (v == "where") {
        CmdWho who;
        if (w.size() > 1) parse_who(w[1], &who);
        Thing *t = who_thing(who);
        if (!t) return who_text(who) + ": nothing there";
        std::snprintf(b, sizeof b, "%s: slot %d %s at cell %d,%d (%dw,%dw) z %d yaw %d health %d/%d mana %d/%d state %d",
                      who_text(who).c_str(), (int)thing_index(t), cmd_thing_name(t->cls, t->type).c_str(), t->x >> 8, t->y >> 8,
                      (int)t->x, (int)t->y, t->z, t->yaw, t->health, t->max_health, t->mana, t->mana_total, t->state);
        return b;
    }
    size_t i = 1;
    int cls, type;
    if (!cmd_parse_class_type(w, &i, &cls, &type, false)) return "unknown class / type";
    const int n = count_live(cls, type);
    if (v == "count") {
        std::snprintf(b, sizeof b, "%d live %s", n, cmd_thing_name(cls, type).c_str());
        return b;
    }
    std::string out;
    std::snprintf(b, sizeof b, "%d live %s%s", n, cmd_thing_name(cls, type).c_str(), n > 20 ? " (first 20)" : "");
    out = b;
    int shown = 0;
    for (int s = 1; s < thing_pool_slots() && shown < 20; s++) {
        const Thing *t = thing_at((unsigned)s);
        if (t->cls != cls || (type >= 0 && t->type != type) || !live(t)) continue;
        std::snprintf(b, sizeof b, "\n  #%d %s cell %d,%d z %d health %d/%d state %d owner %d", s, model_name_raw(t->cls, t->type),
                      t->x >> 8, t->y >> 8, t->z, t->health, t->max_health, t->state, t->owner);
        out += b;
        shown++;
    }
    return out;
}

bool cmd_check(const Assertion &a, std::string *detail) {
    char b[256];
    b[0] = 0;
    bool ok = false;
    if (!g_state) { if (detail) *detail = "no game"; return false; }
    switch (a.kind) {
    case AssertKind::COUNT: {
        const int n = count_live(a.cls, a.type);
        ok = compare(n, a.op, a.value);
        std::snprintf(b, sizeof b, "count %s = %d (%s %lld)", cmd_thing_name(a.cls, a.type).c_str(), n, op_text(a.op), (long long)a.value);
        break;
    }
    case AssertKind::HEALTH: case AssertKind::MANA: {
        const Thing *t = who_thing(a.who);
        const bool h = a.kind == AssertKind::HEALTH;
        if (!t) { std::snprintf(b, sizeof b, "%s: nothing there", who_text(a.who).c_str()); break; }
        const int64_t v = h ? t->health : t->mana;
        const int64_t ref = a.value_max ? (h ? t->max_health : t->mana_total) : a.value;
        ok = compare(v, a.op, ref);
        std::snprintf(b, sizeof b, "%s of %s = %lld (%s %lld%s)", h ? "health" : "mana", who_text(a.who).c_str(), (long long)v, op_text(a.op),
                      (long long)ref, a.value_max ? " = max" : "");
        break;
    }
    case AssertKind::ALIVE: case AssertKind::DEAD: {
        const Thing *t = who_thing(a.who);
        const bool alive = t && live(t) && t->health >= 0;
        ok = (a.kind == AssertKind::ALIVE) == alive;
        if (t) std::snprintf(b, sizeof b, "%s: health %d state %d -> %s", who_text(a.who).c_str(), t->health, t->state, alive ? "alive" : "dead");
        else std::snprintf(b, sizeof b, "%s: nothing there -> dead", who_text(a.who).c_str());
        break;
    }
    case AssertKind::POS: {
        const Thing *t = who_thing(a.who);
        if (!t) { std::snprintf(b, sizeof b, "%s: nothing there", who_text(a.who).c_str()); break; }
        const int dx = std::abs(world_signed(t->x - a.x)), dy = std::abs(world_signed(t->y - a.y));   // round 11: was (int16_t)
        ok = dx <= a.r && dy <= a.r;
        std::snprintf(b, sizeof b, "%s at %d,%d (cell %d,%d), wanted %d,%d +- %d", who_text(a.who).c_str(), (int)t->x, (int)t->y, t->x >> 8, t->y >> 8,
                      a.x, a.y, a.r);
        break;
    }
    case AssertKind::STATUS: {
        const int p = a.who.n < 0 ? local_player() : a.who.n;
        const int st = g_state->players[p].status;
        const int bits = st & 6;
        ok = a.status == 0 ? bits == 0 : a.status == 2 ? (st & 2) != 0 : (st & 6) == 4;
        std::snprintf(b, sizeof b, "status of p%d = %d", p, st);
        break;
    }
    case AssertKind::GOD: {
        const int p = a.who.n < 0 ? local_player() : a.who.n;
        ok = debug_god(p) == a.on;
        std::snprintf(b, sizeof b, "god of p%d is %s", p, debug_god(p) ? "on" : "off");
        break;
    }
    }
    if (detail) *detail = b;
    return ok;
}

// ---- scenario files ------------------------------------------------------------------------------------------
bool scenario_parse(const std::string &text, const std::string &name, Scenario *out) {
    *out = Scenario{};
    out->name = name;
    int cursor = 1, lineno = 0, order = 0;
    size_t pos = 0;
    auto err = [&](const std::string &m) {
        out->error = name + ":" + std::to_string(lineno) + ": " + m;
        return false;
    };
    while (pos <= text.size()) {
        size_t e = text.find('\n', pos);
        if (e == std::string::npos) e = text.size();
        std::string ln = text.substr(pos, e - pos);
        pos = e + 1;
        lineno++;
        std::vector<std::string> w = split(ln, nullptr);
        if (w.empty()) { if (e >= text.size()) break; continue; }
        const std::string h = lower(w[0]);
        int n = 0;
        if (h == "level") {
            if (w.size() != 2 || !parse_int(w[1], &n) || n < 0 || n > 69) return err("level L (0..69)");
            out->level = n;
            continue;
        }
        if (h == "movie") continue;                     // reference_player_test header, no meaning here
        if (h == "stop") {
            if (w.size() != 2 || !parse_int(w[1], &n) || n < 1) return err("stop T");
            out->stop = n;
            continue;
        }
        if (h == "name") {
            const size_t p0 = ln.find_first_not_of(" \t", ln.find("name") + 4);
            if (p0 != std::string::npos) out->title = ln.substr(p0);
            while (!out->title.empty() && std::isspace((unsigned char)out->title.back())) out->title.pop_back();
            continue;
        }
        if (h == "rts") {
            ModeParams p;
            p.seed = 1;
            p.world_size = 256;
            p.bots = 3;
            p.humans = 1;
            p.flags = MODE_PARAM_DEBUG;
            for (size_t i = 1; i < w.size(); i++) {
                const std::string k = lower(w[i]);
                int x = 0;
                if (k == "reseed") { p.flags |= MODE_PARAM_RESEED; continue; }
                if (i + 1 >= w.size() || !parse_int(w[i + 1], &x) || x < 0) return err("rts seed S [bots B] [size N] [map M] [humans H] [reseed]");
                i++;
                if (k == "seed") p.seed = (uint32_t)x;
                else if (k == "bots") p.bots = (uint32_t)(x > 7 ? 7 : x);
                else if (k == "size") p.world_size = (uint32_t)x;
                else if (k == "map") p.map = (uint32_t)x;
                else if (k == "humans") p.humans = (uint32_t)(x < 1 ? 1 : x > 8 ? 8 : x);
                else return err("unknown rts option " + w[i - 1]);
            }
            if (p.world_size != 256) return err("rts size: only 256 until round 11");
            out->rts = true;
            out->rts_params = p;
            continue;
        }
        // tick prefix
        int t0 = -1, t1 = -1;
        if (std::isdigit((unsigned char)w[0][0])) {
            const size_t dash = w[0].find('-');
            int a, b2;
            if (!parse_int(w[0].substr(0, dash), &a) || a < 0) return err("bad tick " + w[0]);
            b2 = a;
            if (dash != std::string::npos && (!parse_int(w[0].substr(dash + 1), &b2) || b2 < a)) return err("bad tick range " + w[0]);
            if (b2 - a > 100000) return err("tick range too long");
            t0 = a;
            t1 = b2;
        }
        // the command text after the tick word
        std::string cmdtext = ln;
        if (t0 >= 0) {
            const size_t p0 = cmdtext.find(w[0]);
            cmdtext = cmdtext.substr(p0 + w[0].size());
        }
        ParsedCommand c = cmd_parse(cmdtext);
        if (c.kind == CmdKind::EMPTY) return err("no command after the tick");
        if (c.kind == CmdKind::INVALID) return err(c.error);
        if (c.kind == CmdKind::WAIT) {
            if (t0 >= 0) return err("wait takes no tick");
            cursor += c.wait;
            continue;
        }
        if (t0 < 0) t0 = t1 = cursor;
        ScenarioItem it;
        it.line = lineno;
        it.cmd = c;
        if (c.kind == CmdKind::INPUT) {
            it.tick = t0;
            it.tick_end = t1;
            it.order = order++;
            out->inputs.push_back(it);
            continue;
        }
        if (c.kind == CmdKind::HOST && c.host.op == HostOp::RUN) return err("run is not allowed inside a scenario");
        for (int t = t0; t <= t1; t++) {
            it.tick = it.tick_end = t;
            it.order = order++;
            out->items.push_back(it);
            if (out->items.size() > 200000) return err("too many lines");
        }
        if (e >= text.size()) break;
    }
    std::stable_sort(out->items.begin(), out->items.end(), [](const ScenarioItem &a, const ScenarioItem &b) {
        return a.tick != b.tick ? a.tick < b.tick : a.order < b.order;
    });
    int packets = 0;
    for (ScenarioItem &it : out->items) {
        it.packets_before = packets;
        if (it.cmd.kind == CmdKind::PACKET) packets++;
    }
    return true;
}

bool scenario_load(const std::string &path, Scenario *out) {
    mc_blob b;
    if (!mc_read_file(path.c_str(), &b)) {
        *out = Scenario{};
        out->error = "cannot read " + path;
        return false;
    }
    std::string text(reinterpret_cast<const char *>(b.data), b.len);
    mc_blob_free(&b);
    std::string name = path;
    const size_t sl = name.find_last_of("/\\");
    if (sl != std::string::npos) name = name.substr(sl + 1);
    return scenario_parse(text, name, out);
}

// ---- the runner -----------------------------------------------------------------------------------------------
void ScenarioRunner::start(const Scenario &s) {
    s_ = s;
    next_ = 0;
    tick_ = 0;
    running_ = true;
    done_ = false;
    failures_ = passed_ = 0;
    queued_ = refused_ = 0;
}

void ScenarioRunner::stop() {
    running_ = false;
    done_ = true;
}

std::string ScenarioRunner::summary() const {
    char b[256];
    std::snprintf(b, sizeof b, "%s%s%s%s: %d assertion(s) passed, %d failed%s", s_.name.c_str(), s_.title.empty() ? "" : " (",
                  s_.title.c_str(), s_.title.empty() ? "" : ")", passed_, failures_,
                  done_ ? "" : " (still running)");
    return b;
}

ScenarioStep ScenarioRunner::step(int tick) {
    ScenarioStep r;
    if (!running_) { r.done = done_; return r; }
    tick_ = tick;
    auto where = [&](const ScenarioItem &it) { return s_.name + ":" + std::to_string(it.line) + ": "; };
    while (next_ < s_.items.size()) {
        const ScenarioItem &it = s_.items[next_];
        const ParsedCommand &c = it.cmd;
        if (c.kind == CmdKind::PACKET) {
            if (it.tick > tick + 1) break;
            std::string why;
            if (debug_cmd_queue(c.packet, &why)) { r.packets.push_back(c.packet); queued_++; }
            else { r.failures.push_back(where(it) + c.source + ": refused: " + why); failures_++; refused_++; }
            next_++;
            continue;
        }
        // everything else looks at the state after its tick, once the packets before it ran (the queue is
        // FIFO: this run's packets that left it = queued - still pending)
        const int ran = queued_ - std::min(queued_, debug_cmd_pending()) + refused_;
        if (it.tick > tick || ran < it.packets_before) break;
        next_++;
        switch (c.kind) {
        case CmdKind::ASSERT: {
            std::string d;
            if (cmd_check(c.check, &d)) { passed_++; r.output.push_back(where(it) + "ok: " + c.source + " - " + d); }
            else { failures_++; r.failures.push_back(where(it) + "FAILED: " + c.source + " - " + d); }
            break;
        }
        case CmdKind::QUERY: r.output.push_back(cmd_query(c)); break;
        case CmdKind::TEXT: r.output.push_back(c.text); break;
        case CmdKind::HOST:
            if (c.host.op == HostOp::ECHO) r.output.push_back(c.host.text);
            else if (c.host.op == HostOp::QUIT) { next_ = s_.items.size(); s_.stop = 0; }
            else r.host.push_back(c.host);
            break;
        default: break;
        }
    }
    int input_end = 0;
    for (const ScenarioItem &it : s_.inputs) input_end = std::max(input_end, it.tick_end);
    const bool items_done = next_ >= s_.items.size() && tick >= input_end;
    if ((s_.stop > 0 && tick >= s_.stop) || (s_.stop <= 0 && items_done && debug_cmd_pending() == 0)) {
        for (size_t k = next_; k < s_.items.size(); k++)
            if (s_.items[k].cmd.kind == CmdKind::ASSERT) {
                failures_++;
                r.failures.push_back(where(s_.items[k]) + "NOT REACHED (stop " + std::to_string(s_.stop) + "): " + s_.items[k].cmd.source);
            }
        running_ = false;
        done_ = true;
    }
    r.done = done_;
    return r;
}

// ---- the local player's input of a scenario (reference_player_test's script_input) -------------------------
namespace {
int clamp127(double v) {
    if (v > 127) return 127;
    if (v < -127) return -127;
    return (int)std::lround(v);
}
int wrap_angle(int a) {
    a &= 0x7ff;
    return a >= 0x400 ? a - 0x800 : a;
}
// Steering bytes that turn the flyer towards (x, y, z) (reference_player_test.cpp face_point). Input
// generation (what a player's hand would do), recorded in the packet: floating point is fine here.
void face_point(const Thing *t, int x, int y, int z, bool z_given, int8_t *sx, int8_t *sy) {
    const PlayerBlock *P = player_block(const_cast<Thing *>(t));
    Pos to{x, y, (int16_t)z};                   // round 11: WCoord wraps (was (uint16_t))
    if (!z_given) to.z = (int16_t)terrain_height_at(&to);
    const int want = pos_angle_to(thing_pos(t), &to);
    const int diff = wrap_angle(want - (int)t->yaw);
    const double v = diff / 2.0 - P->yaw_rate / 2.0;
    *sx = (int8_t)clamp127(v);
    const double dx = (double)world_signed(to.x - t->x), dy = (double)world_signed(to.y - t->y);
    const double dist = std::sqrt(dx * dx + dy * dy);
    const double dz = (double)t->z - (double)to.z;
    const double pitch = std::atan2(dz, dist < 1 ? 1 : dist) * 0x400 / 3.14159265358979;
    *sy = (int8_t)clamp127(pitch / 2.0);
}
} // namespace

void ScenarioRunner::local_input() {
    GameState *st = g_state;
    const int lp = st->local_player & 7;
    CmdPacket *pk = &st->commands[lp];
    const PlayerRec *rec = &st->players[lp];
    const int tick = tick_ + 1;                 // the tick being run
    if (pk->cmd != 0) return;                   // a pending command (join packet, a queued debug packet)
    Thing *t = thing_at(thing_wrap(rec->thing));
    PlayerBlock *P = player_block(t);
    int cmd = -1, arg = 0, bits = 0;
    bool steer = false;
    int8_t sx = 0, sy = 0;
    for (const ScenarioItem &it : s_.inputs) {
        if (tick < it.tick || tick > it.tick_end) continue;
        const InputLine &l = it.cmd.input;
        switch (l.verb) {
        case InputVerb::STEER: sx = (int8_t)l.a; sy = (int8_t)l.b; steer = true; break;
        case InputVerb::KEYS: bits |= l.a; break;
        case InputVerb::RESPAWN:
            if (t->health < 0 && t->state == 3 && P->castle != 0) cmd = 0xf;
            break;
        case InputVerb::LEFT: case InputVerb::RIGHT: {
            int slot = 0xff;
            const unsigned id = (unsigned)l.a;
            if (id < 24 && P->spell_thing[id] != 0)
                for (int k = 0; k < 24; k++) if (P->spell_slot[k] == (int32_t)P->spell_thing[id]) { slot = k; break; }
            cmd = l.verb == InputVerb::LEFT ? 0x15 : 0x16;
            arg = slot;
            break;
        }
        case InputVerb::REBUILD: {
            if (P->castle != 0 || t->health < 0 || P->spell_thing[16] == 0) break;
            int slot = -1;
            for (int k = 0; k < 24; k++) if (P->spell_slot[k] == (int32_t)P->spell_thing[16]) { slot = k; break; }
            if (slot < 0) break;
            if (P->slot_right != slot) { cmd = 0x16; arg = slot; }
            else if (thing_at(thing_wrap(P->spell_thing[16]))->cast_ticks == 0) bits |= 0x20;
            break;
        }
        case InputVerb::FACE: face_point(t, l.a, l.b, l.c, l.has_c, &sx, &sy); steer = true; break;
        case InputVerb::FLY: {
            face_point(t, l.a, l.b, 0, false, &sx, &sy);
            sy = 0;
            steer = true;
            Pos to{l.a, l.b, 0};
            const int d = pos_dist_xy(thing_pos(t), &to);
            if (d > 0x600) bits |= P->target_speed < 0x30 ? 1 : 0;
            else if (P->target_speed > 0) bits |= 2;
            break;
        }
        case InputVerb::FACE_THING: {
            const Thing *o = thing_at(thing_wrap((unsigned)l.a));
            if (o->cls) { face_point(t, o->x, o->y, o->z, true, &sx, &sy); steer = true; }
            break;
        }
        case InputVerb::FACE_CLASS: {
            int best = -1, bd = 1 << 30;
            for (int i = 1; i < thing_pool_slots(); i++) {
                const Thing *o = thing_at((unsigned)i);
                if (o->cls != l.a || (l.b >= 0 && o->type != l.b) || o == t || ((o->cls == 3 || o->cls == 5) && o->health < 0)) continue;
                const int d = pos_dist_xy(thing_pos(t), thing_pos(o));
                if (d < bd) { bd = d; best = i; }
            }
            if (best > 0) {
                const Thing *o = thing_at((unsigned)best);
                face_point(t, o->x, o->y, o->z, true, &sx, &sy);
                steer = true;
                if (l.c > 0) {
                    if (bd > l.c) { if (P->target_speed < 0x30) bits |= 1; }
                    else if (P->target_speed > 0) bits |= 2;
                }
            }
            break;
        }
        }
    }
    std::memset(pk, 0, sizeof *pk);
    if (cmd >= 0) {
        pk->cmd = (uint8_t)cmd;
        pk->arg = (uint8_t)arg;
    } else if (bits) {
        pk->cmd = 6;
        pk->bits = (uint8_t)bits;
    }
    if (steer) { pk->steer_x = sx; pk->steer_y = sy; }
}

ScenarioRunner &scenario_runner() {
    static ScenarioRunner r;
    return r;
}
void scenario_local_input() { scenario_runner().local_input(); }
