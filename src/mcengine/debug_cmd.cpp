// Port-only debug commands (debug_cmd.h; round 10 task B, docs/analysis/port_console.md).
#define _CRT_SECURE_NO_WARNINGS
#include "debug_cmd.h"
#include "player.h"
#include "mode.h"
#include "mc_globals.h"
#include "mc_math.h"
#include "level.h"
#include "world.h"
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>

namespace {

std::deque<CmdPacket> s_queue;
std::vector<std::string> s_messages;

void put16(CmdPacket *p, int off, int v) {
    uint8_t *b = reinterpret_cast<uint8_t *>(p);
    b[off] = (uint8_t)(v & 0xff);
    b[off + 1] = (uint8_t)((v >> 8) & 0xff);
}
void put32(CmdPacket *p, int off, int32_t v) {
    put16(p, off, v & 0xffff);
    put16(p, off + 2, (int)(((uint32_t)v >> 16) & 0xffff));
}
uint8_t b(const CmdPacket *p, int off) { return reinterpret_cast<const uint8_t *>(p)[off]; }
int get16u(const CmdPacket *p, int off) { return b(p, off) | (b(p, off + 1) << 8); }
int get16s(const CmdPacket *p, int off) { return (int16_t)(uint16_t)get16u(p, off); }
int32_t get32(const CmdPacket *p, int off) { return (int32_t)((uint32_t)get16u(p, off) | ((uint32_t)get16u(p, off + 2) << 16)); }

CmdPacket packet(uint8_t id) {
    CmdPacket p;
    std::memset(&p, 0, sizeof p);
    p.cmd = id;
    return p;
}

void say(const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (s_messages.size() < 256) s_messages.emplace_back(buf);
}

int player_count() {
    int n = (int)(uint16_t)g_state->player_count;
    return n < 0 ? 0 : n > 8 ? 8 : n;
}

// The target player record of a packet sent by `sender` (-1 = invalid).
int target_of(int sender, uint8_t t) {
    const int p = t == DEBUG_TARGET_SELF ? sender : (int)t;
    return p >= 0 && p < 8 ? p : -1;
}

// The player's Thing when it is a wizard (class 3 type 0 / 1), else null.
Thing *wizard_of(int p) {
    if (p < 0 || p > 7) return nullptr;
    Thing *t = thing_at(thing_wrap(g_state->players[p].thing));
    if (t == thing_at(0) || t->cls != 3 || (t->type != 0 && t->type != 1)) return nullptr;
    return t;
}
Thing *castle_of(int p) {
    Thing *w = wizard_of(p);
    if (!w) return nullptr;
    const unsigned c = player_block(w)->castle;
    if (c == 0 || c >= (unsigned)thing_pool_slots()) return nullptr;
    Thing *t = thing_at(c);
    return t->cls == 3 && t->type == 2 ? t : nullptr;
}

const char *class_name(int cls) {
    switch (cls) {
    case 9: return "Projectile";
    default: return mc_class_name(cls);
    }
}
const char *model_name(int cls, int type) {
    if (cls == 3) {
        static const char *k[] = {"Wizard", "Computer wizard", "Castle", "Balloon"};
        return type >= 0 && type < 4 ? k[type] : "?";
    }
    return mc_model_name(cls, type);
}

// The Thing a packet names by slot + expected class / type (null: no such live Thing).
Thing *named_thing(const CmdPacket *p) {
    const int slot = get16u(p, 3);
    if (slot <= 0 || slot >= thing_pool_slots()) return nullptr;
    Thing *t = thing_at((unsigned)slot);
    if (t->cls == 0) return nullptr;
    if (b(p, 5) != DEBUG_ANY && t->cls != b(p, 5)) return nullptr;
    if (b(p, 6) != DEBUG_ANY && t->type != b(p, 6)) return nullptr;
    return t;
}

// A wizard dies the way its update makes it die (player_type0_s0_update / the AI's update: health < 0 ->
// state 2, the dying handler). Setting only the health is not enough: an invulnerable wizard (spawn, inside
// its castle) regenerates before its update looks at the health.
void kill_wizard(Thing *t) {
    t->health = -1;
    t->state = 2;
    t->z_vel = 0;
    std::memset(t->damage_slots, 0, sizeof t->damage_slots);
}
void kill_thing(Thing *t) {
    if (t->cls == 3 && (t->type == 0 || t->type == 1)) kill_wizard(t);
    else if (t->cls == 3 || t->cls == 5) t->health = -1;
    else thing_mark_delete(t);
}

void set_god(int p, int mode) {
    bool on = mode == DEBUG_GOD_ON || (mode == DEBUG_GOD_TOGGLE && !debug_god(p));
    if (mode_active()) {
        if (on) g_mode.players[p].flags |= MODE_PLAYER_GOD;
        else    g_mode.players[p].flags &= ~MODE_PLAYER_GOD;
    } else {
        uint8_t &f = debug_player_flags(g_state->players[p]);
        f = (uint8_t)(on ? (f | DEBUG_PLAYER_GOD) : (f & ~DEBUG_PLAYER_GOD));
    }
    say("god %s for player %d", on ? "on" : "off", p);
}

// Cheat 1's spell part (player_cheat case 1, player.cpp): one spell Thing per missing spell id into the first
// empty book slot and the first free quick-select key.
int give_all_spells(Thing *t) {
    PlayerBlock *P = player_block(t);
    int added = 0;
    for (int id = 0; id < 24; id++) {
        if (P->spell_thing[id] != 0) continue;
        for (int slot = 0; slot < 24; slot++) {
            if (P->spell_slot[slot] != 0) continue;
            ThingCreateFn fn = thing_create_fn(0xc, id);
            Thing *s = fn ? fn(thing_pos(t)) : nullptr;
            if (!s) continue;
            s->flags |= 0x40001;
            s->mana_cost = 0;
            s->caster = thing_index(t);
            P->spell_slot[slot] = thing_index(s);
            for (int k = 0; k < 10; k++)
                if (P->hotkey_slot[k] == 0xff) { P->hotkey_slot[k] = (uint8_t)slot; break; }
            added++;
            break;
        }
    }
    player_rebuild_spell_index(t);
    return added;
}

void apply_spawn(int sender, const CmdPacket *p) {
    const int cls = b(p, 1), type = b(p, 2);
    int count = b(p, 5);
    if (count <= 0) count = 1;
    if (count > 64) count = 64;
    int cx, cy;
    if (b(p, 6) & 1) {
        cx = world_wrap_cell(b(p, 3) | (b(p, 9) & 0x0f) << 8);  // round 11: b9 = the cells' high nibbles (debug_cmd.h)
        cy = world_wrap_cell(b(p, 4) | (b(p, 9) >> 4) << 8);
    } else {
        Thing *w = wizard_of(target_of(sender, b(p, 7)));
        if (!w) { say("spawn: no wizard to spawn in front of"); return; }
        const int ahead = (b(p, 8) ? b(p, 8) : 4) * 0x100;
        // yaw 0 looks towards -y, 0x200 towards +x (render.h); mc_sin / mc_cos are 16.16
        const int32_t x = (int32_t)w->x + (int32_t)(((int64_t)mc_sin(w->yaw) * ahead) >> 16);
        const int32_t y = (int32_t)w->y - (int32_t)(((int64_t)mc_cos(w->yaw) * ahead) >> 16);
        cx = world_cx(x);                               // round 11: was (x >> 8) & 0xff
        cy = world_cx(y);
    }
    if (!thing_table_b_enabled(cls, type)) { say("spawn: class %d type %d has no constructor", cls, type); return; }
    const int before = thing_pool_live_count();
    int side = 1;
    while (side * side < count) side++;
    for (int i = 0; i < count; i++) {
        ThingInit rec;
        std::memset(&rec, 0, sizeof rec);
        rec.cls = (uint16_t)cls;
        rec.model = (uint16_t)type;
        rec.x = (uint16_t)world_wrap_cell(cx + i % side - side / 2);     // ThingInit cells are u16 (< 4096)
        rec.y = (uint16_t)world_wrap_cell(cy + i / side - side / 2);
        level_spawn_thing_record(&rec);
    }
    say("spawned %d x %s %s (%d,%d) at cell %d,%d: %d new Things", count, class_name(cls), model_name(cls, type), cls, type, cx,
        cy, thing_pool_live_count() - before);
}

} // namespace

// ---- builders ------------------------------------------------------------------------------------------------
CmdPacket debug_cmd_teleport(int x, int y, int z, int target) {
    CmdPacket p = packet(DEBUG_CMD_TELEPORT);
    p.arg = (uint8_t)target;
    p.pad2 = z == DEBUG_Z_GROUND ? 0 : 1;
    put16(&p, 3, x);
    put16(&p, 5, y);
    put16(&p, 7, z == DEBUG_Z_GROUND ? 0 : z);
    // round 11: bits 16..19 of x / y (a world up to 4096 cells = 2^20 units; 0 in a 256-cell world)
    reinterpret_cast<uint8_t *>(&p)[9] = (uint8_t)((((uint32_t)x >> 16) & 0x0f) | ((((uint32_t)y >> 16) & 0x0f) << 4));
    return p;
}
namespace {
int32_t teleport_x(const CmdPacket *p) { return get16u(p, 3) | (b(p, 9) & 0x0f) << 16; }
int32_t teleport_y(const CmdPacket *p) { return get16u(p, 5) | (b(p, 9) >> 4) << 16; }
}
CmdPacket debug_cmd_spawn(int cls, int type, int cx, int cy, bool at_cell, int count, int target, int ahead) {
    CmdPacket p = packet(DEBUG_CMD_SPAWN);
    p.arg = (uint8_t)cls;
    p.pad2 = (uint8_t)type;
    uint8_t *q = reinterpret_cast<uint8_t *>(&p);
    q[3] = at_cell ? (uint8_t)cx : 0;
    q[4] = at_cell ? (uint8_t)cy : 0;
    q[5] = (uint8_t)(count < 1 ? 1 : count > 64 ? 64 : count);
    q[6] = at_cell ? 1 : 0;
    q[7] = (uint8_t)target;
    q[8] = (uint8_t)(ahead < 0 ? 0 : ahead > 255 ? 255 : ahead);
    // round 11: bits 8..11 of the cells (a world up to 4096 cells; 0 in a 256-cell world)
    q[9] = at_cell ? (uint8_t)((((unsigned)cx >> 8) & 0x0f) | ((((unsigned)cy >> 8) & 0x0f) << 4)) : 0;
    return p;
}
CmdPacket debug_cmd_give_mana(int amount, int where, int target) {
    CmdPacket p = packet(DEBUG_CMD_GIVE_MANA);
    p.arg = (uint8_t)target;
    p.pad2 = (uint8_t)where;
    put32(&p, 3, amount);
    return p;
}
CmdPacket debug_cmd_god(int mode, int target) {
    CmdPacket p = packet(DEBUG_CMD_GOD);
    p.arg = (uint8_t)target;
    p.pad2 = (uint8_t)mode;
    return p;
}
CmdPacket debug_cmd_kill_slot(int slot, int cls, int type) {
    CmdPacket p = packet(DEBUG_CMD_KILL);
    p.arg = DEBUG_TARGET_SELF;
    p.pad2 = DEBUG_KILL_SLOT;
    put16(&p, 3, slot);
    reinterpret_cast<uint8_t *>(&p)[5] = (uint8_t)cls;
    reinterpret_cast<uint8_t *>(&p)[6] = (uint8_t)type;
    return p;
}
CmdPacket debug_cmd_kill_class(int cls, int type) {
    CmdPacket p = packet(DEBUG_CMD_KILL);
    p.arg = DEBUG_TARGET_SELF;
    p.pad2 = DEBUG_KILL_CLASS;
    reinterpret_cast<uint8_t *>(&p)[5] = (uint8_t)cls;
    reinterpret_cast<uint8_t *>(&p)[6] = (uint8_t)type;
    return p;
}
CmdPacket debug_cmd_kill_wizard(int target) {
    CmdPacket p = packet(DEBUG_CMD_KILL);
    p.arg = (uint8_t)target;
    p.pad2 = DEBUG_KILL_WIZARD;
    return p;
}
CmdPacket debug_cmd_kill_castle(int target) {
    CmdPacket p = packet(DEBUG_CMD_KILL);
    p.arg = (uint8_t)target;
    p.pad2 = DEBUG_KILL_CASTLE;
    return p;
}
CmdPacket debug_cmd_claim(int slot, int cls, int type, int target) {
    CmdPacket p = packet(DEBUG_CMD_CLAIM);
    p.arg = (uint8_t)target;
    put16(&p, 3, slot);
    reinterpret_cast<uint8_t *>(&p)[5] = (uint8_t)cls;
    reinterpret_cast<uint8_t *>(&p)[6] = (uint8_t)type;
    return p;
}
CmdPacket debug_cmd_heal(int what, int target) {
    CmdPacket p = packet(DEBUG_CMD_HEAL);
    p.arg = (uint8_t)target;
    p.pad2 = (uint8_t)what;
    return p;
}
CmdPacket debug_cmd_spells(int target) {
    CmdPacket p = packet(DEBUG_CMD_SPELLS);
    p.arg = (uint8_t)target;
    return p;
}
CmdPacket debug_cmd_level_end(int how, bool mark_only, int target) {
    CmdPacket p = packet(DEBUG_CMD_LEVEL_END);
    p.arg = (uint8_t)target;
    p.pad2 = (uint8_t)how;
    reinterpret_cast<uint8_t *>(&p)[3] = mark_only ? 1 : 0;
    return p;
}
CmdPacket debug_cmd_damage(int amount, int what, int target) {
    CmdPacket p = packet(DEBUG_CMD_DAMAGE);
    p.arg = (uint8_t)target;
    p.pad2 = (uint8_t)what;
    put32(&p, 3, amount);
    return p;
}

const char *debug_cmd_name(uint8_t id) {
    static const char *k[] = {"teleport", "spawn", "give_mana", "god", "kill", "claim", "heal", "spells", "level_end", "damage"};
    return id >= DEBUG_CMD_FIRST && id <= DEBUG_CMD_DAMAGE ? k[id - DEBUG_CMD_FIRST] : nullptr;
}

std::string debug_cmd_describe(const CmdPacket &pk) {
    const CmdPacket *p = &pk;
    char buf[200];
    auto who = [](uint8_t t) {
        static char w[8];
        if (t == DEBUG_TARGET_SELF) return "self";
        std::snprintf(w, sizeof w, "p%d", t);
        return (const char *)w;
    };
    switch (pk.cmd) {
    case DEBUG_CMD_TELEPORT:
        if (pk.pad2 & 1) std::snprintf(buf, sizeof buf, "teleport %s -> %d,%d z %d", who(pk.arg), teleport_x(p), teleport_y(p), get16s(p, 7));
        else std::snprintf(buf, sizeof buf, "teleport %s -> %d,%d (ground)", who(pk.arg), teleport_x(p), teleport_y(p));
        break;
    case DEBUG_CMD_SPAWN:
        if (b(p, 6) & 1) std::snprintf(buf, sizeof buf, "spawn %d x class %d type %d at cell %d,%d", b(p, 5), pk.arg, pk.pad2,
                                       b(p, 3) | (b(p, 9) & 0x0f) << 8, b(p, 4) | (b(p, 9) >> 4) << 8);
        else std::snprintf(buf, sizeof buf, "spawn %d x class %d type %d %d cells in front of %s", b(p, 5), pk.arg, pk.pad2, b(p, 8) ? b(p, 8) : 4, who(b(p, 7)));
        break;
    case DEBUG_CMD_GIVE_MANA:
        std::snprintf(buf, sizeof buf, "give mana %d to %s's %s", (int)get32(p, 3), who(pk.arg),
                      pk.pad2 == DEBUG_MANA_CASTLE ? "castle" : pk.pad2 == DEBUG_MANA_BALL ? "mana ball" : "wizard");
        break;
    case DEBUG_CMD_GOD:
        std::snprintf(buf, sizeof buf, "god %s for %s", pk.pad2 == DEBUG_GOD_ON ? "on" : pk.pad2 == DEBUG_GOD_OFF ? "off" : "toggle", who(pk.arg));
        break;
    case DEBUG_CMD_KILL:
        switch (pk.pad2) {
        case DEBUG_KILL_SLOT: std::snprintf(buf, sizeof buf, "kill slot %d (class %d type %d)", get16u(p, 3), b(p, 5), b(p, 6)); break;
        case DEBUG_KILL_CLASS: std::snprintf(buf, sizeof buf, "kill every class %d type %d", b(p, 5), b(p, 6)); break;
        case DEBUG_KILL_WIZARD: std::snprintf(buf, sizeof buf, "kill the wizard of %s", who(pk.arg)); break;
        default: std::snprintf(buf, sizeof buf, "kill the castle of %s", who(pk.arg)); break;
        }
        break;
    case DEBUG_CMD_CLAIM: std::snprintf(buf, sizeof buf, "claim slot %d for %s", get16u(p, 3), who(pk.arg)); break;
    case DEBUG_CMD_HEAL: std::snprintf(buf, sizeof buf, "heal %s (%d)", who(pk.arg), pk.pad2); break;
    case DEBUG_CMD_SPELLS: std::snprintf(buf, sizeof buf, "all spells for %s", who(pk.arg)); break;
    case DEBUG_CMD_LEVEL_END:
        std::snprintf(buf, sizeof buf, "%s%s for %s", pk.pad2 == DEBUG_END_WIN ? "win" : "lose", b(p, 3) & 1 ? " (mark)" : "", who(pk.arg));
        break;
    case DEBUG_CMD_DAMAGE:
        std::snprintf(buf, sizeof buf, "damage %d to %s's %s", (int)get32(p, 3), who(pk.arg), pk.pad2 ? "castle" : "wizard");
        break;
    default: {
        const uint8_t *q = reinterpret_cast<const uint8_t *>(p);
        std::snprintf(buf, sizeof buf, "packet %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x", q[0], q[1], q[2], q[3], q[4], q[5], q[6],
                      q[7], q[8], q[9]);
        break;
    }
    }
    return buf;
}

// ---- applying ------------------------------------------------------------------------------------------------
bool debug_cmd_allowed() {
    if (mode_active()) return (g_mode.params.flags & MODE_PARAM_DEBUG) != 0;
    return !(g_cfg->flags & 0x10);      // not in a network game
}

bool debug_god(int p) {
    if (p < 0 || p > 7) return false;
    if (mode_active()) return (g_mode.players[p].flags & MODE_PLAYER_GOD) != 0;
    return (debug_player_flags(g_state->players[p]) & DEBUG_PLAYER_GOD) != 0;
}

void debug_cmd_apply(int sender, Thing *, CmdPacket *cmd) {
    if (cmd->cmd < DEBUG_CMD_FIRST || cmd->cmd > DEBUG_CMD_LAST || !debug_cmd_allowed()) return;
    const CmdPacket *p = cmd;
    const int target = target_of(sender, cmd->arg);
    switch (cmd->cmd) {
    case DEBUG_CMD_TELEPORT: {
        Thing *w = wizard_of(target);
        if (!w || w->health < 0) { say("teleport: player %d has no living wizard", target); break; }
        Pos pos;
        pos.x = teleport_x(p);                          // round 11: + b9's high nibbles, WCoord wraps
        pos.y = teleport_y(p);
        pos.z = (cmd->pad2 & 1) ? (int16_t)get16s(p, 7) : (int16_t)(terrain_height_at(&pos) + 0x100);
        thing_move_to(w, &pos);
        player_log_position(&g_state->players[target], w);
        say("teleported player %d to %d,%d z %d (cell %d,%d)", target, (int)pos.x, (int)pos.y, pos.z, (int)pos.x >> 8,
            (int)pos.y >> 8);
        break;
    }
    case DEBUG_CMD_SPAWN:
        apply_spawn(sender, p);
        break;
    case DEBUG_CMD_GIVE_MANA: {
        Thing *w = wizard_of(target);
        const int32_t n = get32(p, 3);
        if (!w) { say("give mana: player %d has no wizard", target); break; }
        if (cmd->pad2 == DEBUG_MANA_CASTLE) {
            Thing *c = castle_of(target);
            if (!c) { say("give mana: player %d has no castle", target); break; }
            int64_t m = (int64_t)c->mana + n;
            if (c->mana_total > 0 && m > c->mana_total) m = c->mana_total;
            if (m < 0) m = 0;
            c->mana = (int32_t)m;
            say("castle of player %d: mana %d / %d", target, c->mana, c->mana_total);
        } else if (cmd->pad2 == DEBUG_MANA_BALL) {
            Thing *ball = thing_create(thing_pos(w), 10, 0x27);          // cheat 2 (player_cheat, player.cpp)
            if (ball) {
                ball->mana = n;
                ball->mana_owner = thing_index(w);
            }
            w->mana = w->mana_total;
            say("mana ball of %d for player %d%s", n, target, ball ? "" : " (pool full)");
        } else {
            int64_t m = (int64_t)w->mana + n;
            if (m > w->mana_total) m = w->mana_total;
            if (m < 0) m = 0;
            w->mana = (int32_t)m;
            say("wizard of player %d: mana %d / %d", target, w->mana, w->mana_total);
        }
        break;
    }
    case DEBUG_CMD_GOD:
        if (target < 0) break;
        set_god(target, cmd->pad2);
        break;
    case DEBUG_CMD_KILL: {
        switch (cmd->pad2) {
        case DEBUG_KILL_SLOT: {
            Thing *t = named_thing(p);
            if (!t) { say("kill: slot %d holds no such Thing", get16u(p, 3)); break; }
            kill_thing(t);
            say("killed slot %d (%s %s)", get16u(p, 3), class_name(t->cls), model_name(t->cls, t->type));
            break;
        }
        case DEBUG_KILL_CLASS: {
            Thing *own = wizard_of(sender);
            int n = 0;
            for (int i = 1; i < thing_pool_slots(); i++) {
                Thing *t = thing_at((unsigned)i);
                if (t->cls == 0 || t == own) continue;
                if (b(p, 5) != DEBUG_ANY && t->cls != b(p, 5)) continue;
                if (b(p, 6) != DEBUG_ANY && t->type != b(p, 6)) continue;
                if ((t->cls == 3 || t->cls == 5) && t->health < 0) continue;     // already dead
                if (t->flags & 0x400) continue;                                  // already marked
                if (t->cls == 5 && t->parent != 0 && t->parent < (unsigned)thing_pool_slots() &&
                    thing_at(t->parent)->cls == 5 && thing_at(t->parent)->child == (unsigned)i)
                    continue;                                                     // a body segment: its head dies
                kill_thing(t);
                n++;
            }
            say("killed %d Things of class %d type %d", n, b(p, 5), b(p, 6));
            break;
        }
        case DEBUG_KILL_WIZARD: {
            Thing *w = wizard_of(target);
            if (!w || w->health < 0) { say("kill: player %d has no living wizard", target); break; }
            kill_wizard(w);
            say("killed the wizard of player %d", target);
            break;
        }
        default: {
            Thing *c = castle_of(target);
            if (!c) { say("kill: player %d has no castle", target); break; }
            c->health = -1;
            say("destroyed the castle of player %d", target);
            break;
        }
        }
        break;
    }
    case DEBUG_CMD_CLAIM: {
        Thing *w = wizard_of(target);
        Thing *t = named_thing(p);
        if (!w || !t) { say("claim: no such Thing / wizard"); break; }
        if (t->cls == 10 && t->type == 0x27) t->mana_owner = w->owner;
        else t->owner = w->owner;
        say("slot %d (%s %s) now belongs to player %d", get16u(p, 3), class_name(t->cls), model_name(t->cls, t->type), target);
        break;
    }
    case DEBUG_CMD_HEAL: {
        const int what = cmd->pad2 ? cmd->pad2 : DEBUG_HEAL_WIZARD;
        if (what & DEBUG_HEAL_WIZARD) {
            Thing *w = wizard_of(target);
            if (w && w->health >= 0) { w->health = w->max_health; say("healed the wizard of player %d", target); }
        }
        if (what & DEBUG_HEAL_CASTLE) {
            Thing *c = castle_of(target);
            if (c && c->health >= 0) { c->health = c->max_health; say("healed the castle of player %d", target); }
        }
        break;
    }
    case DEBUG_CMD_SPELLS: {
        Thing *w = wizard_of(target);
        if (!w) { say("spells: player %d has no wizard", target); break; }
        say("player %d: %d spells added", target, give_all_spells(w));
        break;
    }
    case DEBUG_CMD_LEVEL_END: {
        if (target < 0) break;
        PlayerRec &rec = g_state->players[target];
        const bool mark = (b(p, 3) & 1) != 0;
        if (cmd->pad2 == DEBUG_END_WIN) rec.status = (uint16_t)(mark ? (rec.status | 2) : 10);
        else                            rec.status = (uint16_t)(mark ? (rec.status | 4) : 0xc);
        say("player %d: level %s%s", target, cmd->pad2 == DEBUG_END_WIN ? "won" : "lost", mark ? " (marked)" : "");
        break;
    }
    case DEBUG_CMD_DAMAGE: {
        Thing *t = cmd->pad2 ? castle_of(target) : wizard_of(target);
        if (!t) { say("damage: player %d has no %s", target, cmd->pad2 ? "castle" : "wizard"); break; }
        const int32_t n = get32(p, 3);
        if (t->damage_slots[0].attacker != 0) t->damage_slots[0].amount += n;
        else                                  t->damage_slots[0].amount = n;
        t->damage_slots[0].attacker = thing_index(t);
        say("damage %d pending on the %s of player %d", n, cmd->pad2 ? "castle" : "wizard", target);
        break;
    }
    default:
        return;         // reserved ids: nothing (and the packet's steering stays as it is)
    }
    // the data bytes are no steering / keys
    cmd->steer_x = 0;
    cmd->steer_y = 0;
    cmd->bits = 0;
}

void debug_cmd_tick() {
    const int n = player_count();
    for (int p = 0; p < n; p++) {
        if (!debug_god(p)) continue;
        Thing *w = wizard_of(p);
        if (!w || w->health < 0) continue;
        w->health = w->max_health;
        w->mana = w->mana_total;
        PlayerBlock *P = player_block(w);
        if (P->invuln_timer < 2) P->invuln_timer = 2;
    }
}

void debug_cmd_register() {
    g_hook_port_command = debug_cmd_apply;
#ifdef MC_HAVE_DEBUG_TICK_HOOK
    g_hook_debug_tick = debug_cmd_tick;     // player.h (requested shared change, docs/analysis/port_console.md)
#endif
}

// ---- host-side queue -----------------------------------------------------------------------------------------
bool debug_cmd_queue(const CmdPacket &p, std::string *why) {
    if (!debug_cmd_allowed()) {
        if (why) *why = mode_active() ? "debug commands are off in this run (start it with --debug)"
                                      : "debug commands are disabled in a network game";
        return false;
    }
    if (g_cfg->flags & 4) {
        if (why) *why = "a movie is playing (its own packets drive the game)";
        return false;
    }
    if (s_queue.size() >= 4096) {
        if (why) *why = "too many queued commands";
        return false;
    }
    s_queue.push_back(p);
    return true;
}

void debug_cmd_pump() {
    if (s_queue.empty() || !g_state || (g_cfg->flags & 4)) return;
    CmdPacket *slot = &g_state->commands[g_state->local_player & 7];
    if (slot->cmd != 0) return;
    *slot = s_queue.front();
    s_queue.pop_front();
}

int debug_cmd_pending() { return (int)s_queue.size(); }
void debug_cmd_clear_queue() { s_queue.clear(); }

std::vector<std::string> debug_cmd_take_messages() {
    std::vector<std::string> m;
    m.swap(s_messages);
    return m;
}
