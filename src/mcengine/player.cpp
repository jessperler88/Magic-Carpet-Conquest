// Players of carpet.exe: records, spawning, command packets, the flying wizard (class 3 type 0).
// Translated from the disassembly (addresses in the comments); see docs/analysis/port_player.md.
#define _CRT_SECURE_NO_WARNINGS
#include "player.h"
#include "demo.h"
#include "text.h"
#include "mc_math.h"
#include "gen/player_tables.h"
#include "palette_fx.h"
#include "net.h"
#include "world_set.h"
#include "world.h"
#include "tick_profile.h"
#include <cstdio>
#include <cstring>

void (*g_hook_castle_stamp_footprint)(Thing *) = nullptr;
void (*g_hook_thing_set_castle_extents)(Thing *, int) = nullptr;
void (*g_hook_player_local_input)() = nullptr;
void (*g_hook_sound_update)() = nullptr;
void (*g_hook_music_update)(int mood) = nullptr;
void (*g_hook_frame_state)(int player) = nullptr;
void (*g_hook_creature_kill_all)() = nullptr;
void (*g_hook_input_mouse_center)() = nullptr;
void (*g_hook_player_mouse_cursor)(int entry) = nullptr;
void (*g_hook_port_command)(int, Thing *, CmdPacket *) = nullptr;
void (*g_hook_debug_tick)() = nullptr;
uint32_t g_timer_ticks = 0;

// Flight tuning dwords of the data segment (0x94384..0x943b0).
static const int32_t &k_speed_step   = g_player_flight_consts[1];    // DAT_00094384  0x10
static const int32_t &k_speed_min    = g_player_flight_consts[2];    // DAT_00094388 -0x50
static const int32_t &k_speed_max    = g_player_flight_consts[3];    // DAT_0009438c  0x50
static const int32_t &k_knock_decay  = g_player_flight_consts[4];    // DAT_00094390 -4
static const int32_t &k_strafe_step  = g_player_flight_consts[9];    // DAT_000943a4  0x10
static const int32_t &k_strafe_min   = g_player_flight_consts[10];   // DAT_000943a8 -0x50
static const int32_t &k_strafe_max   = g_player_flight_consts[11];   // DAT_000943ac  0x50
static const int32_t &k_strafe_decay = g_player_flight_consts[12];   // DAT_000943b0 -4

static inline int iabs(int v) { return v < 0 ? -v : v; }
// The original computes v / |v| (cdq / xor / sub / idiv) with 0 for 0.
static inline int isign(int v) { return v == 0 ? 0 : v / iabs(v); }
static inline uint8_t *cfg_bytes() { return reinterpret_cast<uint8_t *>(g_cfg); }

// sound_request_49720(thing, player, sound) / sound_fade_player_sound_49c40 / music_update_1f800:
// sound is not ported; the request goes to the hook when the sound system installs one.
static inline void snd_request(int thing, int player, int sound) {
    sound_request(thing, player, sound);
}
static inline void snd_fade(int thing, int player, int sound) {
    sound_fade(thing, player, sound);
}
static inline void music_update(int mood) {           // music_update_1f800(mood): 1 = calm, 2 = combat
    if (g_hook_music_update) g_hook_music_update(mood);
}
static inline void player_cd_check_quit() {
    // TODO(port): player_cd_check_quit_3bbd0(): CD-ROM copy protection (sets the local quit flag to 2)
}

// The spell Thing index a book slot holds, read the way the original does: P+0x214 + slot*4 with no
// range check. slot_left / slot_right are 0xff until a spell is chosen, which reads 0x3fc bytes
// further on, i.e. inside the next player's record (its unused camera log) - emulated by reading the
// GameState at the same offset.
static int32_t spell_slot_raw(const Thing *t, int slot) {
    const uint8_t *P = thing_player_block(t);
    if (slot >= 0 && slot < 24) {
        int32_t v;
        std::memcpy(&v, P + offsetof(PlayerBlock, spell_slot) + slot * 4, 4);
        return v;
    }
    const uint8_t *lo = reinterpret_cast<const uint8_t *>(g_state);
    const uint8_t *hi = lo + sizeof(GameState);
    if (P < lo || P >= hi) return 0;                    // dummy block: outside the state in the port
    // round 11: the address in the original's layout (mc_image.h; P blocks lie in front of the Things)
    const intptr_t off = (P - lo) + (intptr_t)offsetof(PlayerBlock, spell_slot) + (intptr_t)slot * 4;
    if (off < 0 || off + 4 > (intptr_t)MC_STATE_IMAGE_SIZE) return 0;
    int32_t v;
    state_image_read(*g_state, off, &v, 4);
    return v;
}
// Thing pointer from a raw index the original multiplies by 0xa4 unchecked; null for "not above
// the pool base" (index <= 0) and for values that would leave the pool.
static Thing *thing_from_raw(int32_t idx) {
    return (idx > 0 && idx < thing_pool_slots()) ? thing_at((unsigned)idx) : nullptr;
}

// ---- small helpers -----------------------------------------------------------------------------

// player_set_combat_music_timer_40b50
void player_set_combat_music_timer(Thing *t) {
    player_block(t)->combat_music = 100;       // the pointer is never null (dummy block)
}

// player_set_palette_effect_3f210
void player_set_palette_effect(Thing *t, int effect) {
    if (g_state->local_player == player_block(t)->player_no) g_cfg->palette_effect = (uint8_t)effect;
}

// player_note_fire_distance_3f240
void player_note_fire_distance(Thing *fire) {
    Thing *lp = thing_at(g_state->players[g_state->local_player & 7].thing);
    PlayerBlock *P = player_block(lp);
    int d = pos_dist_xyz(thing_pos(fire), thing_pos(lp));
    if (d <= P->fire_dist) P->fire_dist = d;
}

// player_note_ridge_distance_3f2c0
void player_note_ridge_distance(Thing *node) {
    Thing *lp = thing_at(g_state->players[g_state->local_player & 7].thing);
    PlayerBlock *P = player_block(lp);
    int d = pos_dist_xyz(thing_pos(node), thing_pos(lp));
    if (d <= P->ridge_dist) P->ridge_dist = d;
}

// chat_message_show_3bb50(rec, mode): despite the name it switches the player's input mode
// (0 flight, 2 spell book, 3 text entry); for the local player it also swaps the mouse cursor.
void player_set_input_mode(PlayerRec *rec, int mode) {
    rec->input_mode = (uint8_t)mode;
    if (rec->index == (uint16_t)g_state->local_player) {
        // mouse_cursor_set_sprite_5ba5c(DAT_000adfc0 + 6) in mode 2, (DAT_000adfc0) otherwise (0x3bb6f..0x3bb97)
        if (g_hook_player_mouse_cursor) g_hook_player_mouse_cursor(mode == 2 ? 1 : 0);
        if (mode == 2) mapmode_palette_save();                                      // mapmode_palette_save_30350
        else {
            if (g_hook_input_mouse_center) g_hook_input_mouse_center();             // input_mouse_center_4a000
            mapmode_palette_restore();                                              // mapmode_palette_restore_303b0
        }
        snd_request(0, (int16_t)rec->index, 0xe);
    }
    player_cd_check_quit();
}

// player_rebuild_spell_index_40240
void player_rebuild_spell_index(Thing *t) {
    PlayerBlock *P = player_block(t);
    std::memset(P->spell_thing, 0, sizeof P->spell_thing);
    for (int i = 0; i < 24; i++) {
        int32_t idx = P->spell_slot[i];
        if (idx == 0) continue;
        Thing *s = thing_from_raw(idx);
        if (!s) continue;                               // (the original indexes the pool unchecked)
        int id = (int8_t)s->type;
        if (id >= 0 && id < 24) P->spell_thing[id] = (uint16_t)idx;
    }
}

// ---- mana totals -------------------------------------------------------------------------------

// mana_add_to_owner_428e0
Thing *mana_add_to_owner(Thing *t) {
    Thing *owner = nullptr;
    if (t->mana_owner != 0) {
        owner = thing_at(t->mana_owner);
        owner->mana_total += t->mana;
    }
    g_cfg->total_mana += (uint32_t)t->mana;
    return owner;
}

// mana_totals_update_427d0
void mana_totals_update(Thing *local_player) {
    for (int p = 0; (uint16_t)p < (uint16_t)g_state->player_count && p < 8; p++) {
        Thing *t = thing_at(g_state->players[p].thing);
        PlayerBlock *P = player_block(t);
        t->mana_total = P->mana;
        P->mana_in_transit = 0;
    }
    std::memset(cfg_bytes() + 0xc0, 0, 4);                  // Config+0xc0 (u32) = 0
    g_cfg->total_mana = (uint32_t)player_block(local_player)->mana;
    for (int i = 1; i < thing_pool_slots(); i++) {
        Thing *t = thing_at(i);
        switch (t->cls) {
        case 5:
            mana_add_to_owner(t);
            break;
        case 3:
            if (t->type == 2 || t->type == 3) mana_add_to_owner(t);     // castle, balloon
            break;
        case 10:
            if (t->type == 0x27) {
                mana_add_to_owner(t);
            } else if (t->type == 0x2d) {
                Thing *o = mana_add_to_owner(t);
                if (o) player_block(o)->mana_in_transit += t->mana;
            }
            break;
        default:
            break;
        }
    }
}

// ---- castle helpers ----------------------------------------------------------------------------

static Thing *castle_owner_spell(Thing *castle) {
    Thing *owner = thing_at(thing_wrap((unsigned)(int16_t)castle->owner));
    if (owner->state != 0 && owner->state != 1) return nullptr;
    int16_t idx = (int16_t)player_block(owner)->spell_thing[16];    // P+0x2c4: the Castle spell
    return idx != 0 ? thing_from_raw(idx) : nullptr;
}

// castle_apply_level_stats_42170
void castle_apply_level_stats(Thing *castle, Thing *spell, int health, int mana) {
    if (health != 0) {
        int32_t old = castle->health, lost = 0;
        castle->max_health = health;
        if (old < 0) {
            lost = -old;
            if (lost > health / 2) lost = health / 2;
        }
        castle->health = castle->max_health - lost;
    }
    if (spell) {
        spell->mana_total = mana;
        if (spell->duration != 0) spell->mana = mana / spell->duration;    // idiv; 0 would fault
    }
    castle->mana_total = mana;
}

// castle_set_level_stats_42200 (switch on the castle level in Thing.aux)
void castle_set_level_stats(Thing *castle) {
    static const int32_t health[8] = {0, 20000, 40000, 40000, 60000, 60000, 80000, 80000};
    static const int32_t mana[8]   = {5000, 10000, 20000, 40000, 80000, 160000, 320000, 30000000};
    Thing *spell = castle_owner_spell(castle);
    unsigned lvl = (uint16_t)castle->aux;
    if (lvl > 7) return;
    castle_apply_level_stats(castle, spell, health[lvl], mana[lvl]);
}

// castle_spell_set_capacity_42370
void castle_spell_set_capacity(Thing *castle) {
    static const int32_t mana[8] = {5000, 10000, 20000, 40000, 80000, 160000, 320000, 30000000};
    Thing *spell = castle_owner_spell(castle);
    if (!spell) return;
    unsigned lvl = (uint16_t)castle->aux;
    int32_t cap = lvl > 7 ? 0 : mana[lvl];
    spell->mana_total = cap;
    if (spell->duration != 0) spell->mana = cap / spell->duration;
}

// ---- records -----------------------------------------------------------------------------------

static const char *player_default_name(int p) {
    uint32_t off = g_player_name_ptrs[p & 7] - 0x90e70u;
    return off < sizeof g_player_name_pool ? reinterpret_cast<const char *>(g_player_name_pool) + off : "";
}

// players_clear_records_3bf60
void players_clear_records() {
    for (int p = 0; p < 8; p++) {
        std::memset(&g_state->commands[p], 0, sizeof(CmdPacket));
        PlayerRec *rec = &g_state->players[p];
        uint32_t keep = rec->cheat;
        std::memset(rec, 0, sizeof *rec);
        rec->cheat = keep;
    }
}

// players_init_records_3bc10
void players_init_records() {
    uint8_t *state = reinterpret_cast<uint8_t *>(g_state);
    uint8_t *backup = state + 0x2c0a;                   // one PlayerRec of scratch in front of the records
    for (int p = 0; p < 8; p++) {
        PlayerRec *rec = &g_state->players[p];
        PlayerBlock *P = &rec->blk;
        CmdPacket *cmd = &g_state->commands[p];
        std::memset(cmd, 0, sizeof *cmd);
        std::memcpy(backup, rec, sizeof *rec);
        std::memset(rec, 0, sizeof *rec);
        // the spells found so far and the +0x18 dword survive the reset
        std::memcpy(P->spell_found, backup + 0x7cb, 0x18);
        std::memcpy(&rec->cheat, backup + 0x18, 4);
        cmd->cmd = 1;                                   // "join": the first tick spawns the player
        rec->index = (uint16_t)p;
        if (!(g_cfg->flags & 0x10) && p != g_state->local_player) rec->is_computer = 1;
        rec->log_count = 0x20;
        rec->view_entry = (uint16_t)(rec->log_count - 1);
        rec->log_template.zoom = 0x80;                  // rec+0x248
        std::strcpy(rec->name, player_default_name(p));
        for (int i = 0; (int16_t)i < (int)rec->log_count; i++) rec->log[i] = rec->log_template;
        for (int i = 0; i < 24; i++) P->spell_slot[i] = -1;
        P->slot_left = 0xff;
        P->slot_right = 0xff;
        // Level player block: +0x10[24] spells the player starts with, +0x74[24] spells allowed.
        int q = (g_cfg->flags & 0x10) ? 0 : p;
        const uint8_t *blk_q = g_state->level.player_block[q];
        const uint8_t *blk_p = g_state->level.player_block[p];
        int slot = 0;
        for (int i = 0; i < 24; i++) {
            int id = g_spell_slot_order[i];
            bool give = false;
            P->hotkey_slot[i] = 0xff;
            if (rec->is_computer == 1) {
                P->ai_allowed[id] = blk_q[0x74 + id];
                if (blk_p[0x10 + id] != 0 && P->ai_allowed[id] != 0) give = true;
            } else if (blk_q[0x74 + id] == 1) {
                if (!(g_cfg->flags & 0x10)) {
                    if (P->spell_found[id] != 0) give = true;
                } else if (blk_q[0x10 + id] != 0 && blk_q[0x74 + id] != 0) {
                    give = true;
                }
            }
            if (give) {
                P->spell_slot[slot] = id;               // spell id until player_spawn creates the Thing
                if (P->slot_left == 0xff)       P->slot_left = (int16_t)slot;
                else if (P->slot_right == 0xff) P->slot_right = (int16_t)slot;
                P->hotkey_slot[slot] = (uint8_t)slot;
                slot++;
            }
        }
    }
}

// player_spawn_3f360
void player_spawn(PlayerRec *rec, Thing *t) {
    bool created = false;
    models_initialise();
    int p = (int)(rec - g_state->players);
    Pos pos = world_start_pos(p);
    pos.z = (int16_t)(terrain_height_at(&pos) + 0x100);
    if (t == thing_at(0)) {
        t = thing_create(&pos, 3, rec->is_computer == 1 ? 1 : 0);
        if (!t) return;         // the original does not test: the wizard constructors cannot fail there
        created = true;
    } else {
        t->state = rec->is_computer == 1 ? 1 : 0;
        t->flags &= ~0x20u;
        PlayerBlock *old = player_block(t);
        if (old->castle != 0) pos = *thing_pos(thing_at(old->castle));
        thing_move_to(t, &pos);
    }
    t->player = player_block_offset(p);
    PlayerBlock *P = &rec->blk;
    P->player_no = (int16_t)p;
    P->invuln_timer = 100;
    P->countdown15f = 2000;
    P->target_speed = 0;
    P->knock_yaw = 0;
    P->knock_pitch = 0;
    P->knock_speed = 0;
    P->strafe_speed = 0;
    // The spell book: every slot holds a spell id (>= 0) or -1 and gets its spell Thing.
    for (int i = 0; i < 24; i++) {
        int32_t id = P->spell_slot[i];
        if (id < 0) { P->spell_slot[i] = 0; continue; }
        Thing *s = thing_create(thing_pos(t), 0xc, id);
        if (!s) { P->spell_slot[i] = 0; continue; }
        P->spell_slot[i] = thing_index(s);
        s->caster = thing_index(t);
        s->flags |= 1;
        int sid = (int8_t)s->type;
        if (sid >= 0 && sid < 24 && P->spell_sealed[sid] != 0) {
            s->sprite = 0x118;
            s->mana_cost = 0;
            s->flags |= 0x40000;
        }
    }
    if (created) {
        static const uint16_t sprite[8] = {0x2c, 0x111, 0x112, 0x113, 0x114, 0x115, 0x116, 0x117};   // jump table 0x3f334
        P->start_tick = g_timer_ticks;
        if ((uint16_t)P->player_no <= 7) thing_set_sprite(t, sprite[P->player_no]);
        player_rebuild_spell_index(t);
        if (rec->is_computer == 1) {
            const uint8_t *blk = g_state->level.player_block[P->player_no & 7];
            std::memcpy(&P->ai_aggression, blk + 4, 2);         // state+0x385d7
            std::memcpy(&P->ai_accuracy, blk + 0xc, 2);         // state+0x385df
            std::memcpy(&P->ai_reaction, blk + 8, 2);           // state+0x385db
            int lvl_count = g_state->level.castle_level[P->player_no & 7];
            if (P->spell_thing[16] != 0 && lvl_count > 0) {
                Thing *c = thing_create(thing_pos(t), 3, 2);
                if (c) {
                    c->owner = t->owner;
                    P->castle = thing_index(c);
                    snd_request((int16_t)thing_index(t), -1, 0x1e);
                    // One footprint stamp per level through the scratch Thing (things[0]).
                    for (int lvl = 0; lvl < (int)g_state->level.castle_level[P->player_no & 7]; lvl++) {
                        Thing *s0 = thing_at(0);
                        s0->x = c->home.x; s0->y = c->home.y; s0->z = c->home.z;
                        s0->type = 0;
                        s0->aux = 0;
                        s0->owner = c->owner;
                        s0->castle_size = (uint8_t)lvl;
                        if (g_hook_castle_stamp_footprint) g_hook_castle_stamp_footprint(s0);
                    }
                    c->aux = (int16_t)(lvl_count - 1);
                    if (g_hook_thing_set_castle_extents) g_hook_thing_set_castle_extents(c, c->aux);
                    castle_set_level_stats(c);
                    c->mana = c->mana_total;
                    if (c->mana < 0) c->mana = 0;
                    if (c->mana > 0x4e200) c->mana = 0x4e200;
                }
            }
        }
        P->kills = 0;
    }
    rec->thing = thing_index(t);
    if (g_state->local_player == P->player_no) t->flags |= 1;
    if (rec->cheat == 0xae89e) {            // a pointer constant: the record of a special (debug?) player
        t->mana_total = 1000000;
        t->max_health = 1000000;
    } else {
        t->mana_total = 1000;
        t->max_health = 10000;
    }
    t->health = t->max_health;
    t->mana = t->mana_total;
    P->mana = t->mana_total;
    player_rebuild_spell_index(t);
    if (P->castle != 0) castle_spell_set_capacity(thing_at(P->castle));
    // Every other wizard starts to consider this player a threat.
    for (uint32_t i = g_cfg->player_list; i != 0 && i < (uint32_t)thing_pool_slots(); i = thing_at(i)->next) {
        Thing *o = thing_at(i);
        if (o->owner != t->owner && (o->type == 0 || o->type == 1))
            *player_threat(player_block(o), P->player_no) = 0x9fdf;
    }
    P->tether_target = 0;
    P->tether_timer = 0;
    if (t->type == 1) {
        P->ai_mode = 0;
        for (int k = 0; k < 8; k++) *player_threat(P, k) = 0x601f;
        P->spell_cooldown[16] = (uint16_t)(P->player_no << 2);     // P+0x2f4
    }
    P->fire_dist = 0x800;
    P->ridge_dist = 0x800;
    P->combat_music = 0;
    std::memset(P->unk14d, 0x10, sizeof P->unk14d);
    g_state->active_top = -1;
    if (P->player_no == g_state->local_player) g_cfg->substeps = 0;
}

// ---- the flyer ---------------------------------------------------------------------------------

// player_terrain_collide_3fa40: the move target is in g_pos_scratch. A target on an impassable cell
// (terrain bit 0x100) is replaced by the projection of the step on the two neighbouring axis
// directions; returns 0 when both are blocked. The target is then lifted to the minimum clearance.
int player_terrain_collide(Thing *t) {
    int result = 1;
    if (terrain_type_mask_at(&g_pos_scratch) == 0x100) {
        const Pos *tp = thing_pos(t);
        int ang   = pos_angle_to(tp, &g_pos_scratch) & 0xffff;
        int pitch = pos_pitch_to(tp, &g_pos_scratch) & 0xffff;
        int dist  = pos_dist_xyz(tp, &g_pos_scratch) & 0xffff;
        int q = ang / 512;
        g_pos_scratch = *tp;
        int a0 = (q << 9) & 0xffff;
        int a1 = ((q + 1) << 9) & 0x7ff;
        int d0 = (int16_t)(((0x200 - (angle_diff(ang, a0) & 0xffff)) * dist) / 512);
        math_rotate_offset(&g_pos_scratch, a0, pitch, d0);
        if (terrain_type_mask_at(&g_pos_scratch) == 0x100) {
            g_pos_scratch = *tp;
            int d1 = (int16_t)(((0x200 - (angle_diff(ang, a1) & 0xffff)) * dist) / 512);
            math_rotate_offset(&g_pos_scratch, a1, pitch, d1);
            if (terrain_type_mask_at(&g_pos_scratch) == 0x100) result = 0;
        }
    }
    int clear = mc_move_desc(t->desc)->clear_hi;
    int ground = terrain_height_at(&g_pos_scratch);
    if ((int)g_pos_scratch.z < clear + (int16_t)ground) g_pos_scratch.z = (int16_t)(ground + clear);
    return result;
}

// player_flyer_move_3fc00
void player_flyer_move(Thing *t) {
    PlayerBlock *P = player_block(t);
    g_pos_scratch = *thing_pos(t);
    P->yaw_rate = (int16_t)(P->yaw_rate + P->steer_dx);
    P->pitch_acc = (int16_t)(P->pitch_acc + P->steer_dy);
    t->yaw = (uint16_t)(((int)P->yaw_rate / 8 + (int)t->yaw) & 0x7ff);

    int diff = (int)P->target_speed - (int)t->speed_cur;
    t->speed_cur = (int16_t)(t->speed_cur + k_speed_step * isign(diff));

    // Height above the ground relative to the cruise height (descriptor +0xa) limits the climb.
    int ground = terrain_height_at(&g_pos_scratch);
    int cruise = mc_move_desc(t->desc)->clear_lo;
    int ratio = cruise != 0 ? (((int)g_pos_scratch.z - (int16_t)ground - cruise) << 10) / cruise : 0;   // idiv (0 would fault)
    if (ratio < -0x100) ratio = -0x100;
    if (ratio > 0x100) ratio = 0x100;
    uint16_t pw = (uint16_t)((uint16_t)P->pitch_acc & 0x7ff);
    t->pitch = pw;
    int16_t sp = (int16_t)pw;
    if (sp > 0x400) sp = (int16_t)(pw - 0x800);
    int16_t spd = t->speed_cur;
    if (spd < 0 && sp > 0)       P->move_pitch = (uint16_t)((-ratio * sp) / 256);
    else if (spd < 0 && sp < 0)  P->move_pitch = (uint16_t)P->pitch_acc;
    else if (spd > 0 && sp < 0)  P->move_pitch = (uint16_t)((-ratio * sp) / 256);
    else if (spd > 0 && sp > 0)  P->move_pitch = (uint16_t)P->pitch_acc;
    else if (spd == 0) {
        if ((int)g_pos_scratch.z > (int16_t)ground + cruise) g_pos_scratch.z = (int16_t)(g_pos_scratch.z - 8);
    }
    P->move_pitch &= 0x7ff;

    math_rotate_offset(&g_pos_scratch, t->yaw, P->move_pitch, t->speed_cur);
    if (P->strafe_speed != 0)
        math_rotate_offset(&g_pos_scratch, (uint16_t)(t->yaw + 0x200), 0, P->strafe_speed);
    if (P->knock_speed != 0) {
        if (P->knock_speed > 0x80) P->knock_speed = 0x80;
        math_rotate_offset(&g_pos_scratch, P->knock_yaw, 0, P->knock_speed);
        P->knock_speed = (int16_t)(P->knock_speed + k_knock_decay * isign(P->knock_speed));
        if (iabs(P->knock_speed) < 4) P->knock_speed = 0;
    }
    g_pos_scratch.x = (uint16_t)(g_pos_scratch.x + P->push_x);
    g_pos_scratch.y = (uint16_t)(g_pos_scratch.y + P->push_y);
    g_pos_scratch.z = (int16_t)(g_pos_scratch.z + P->push_z);
    P->push_x = 0;
    P->push_y = 0;
    P->push_z = 0;

    // Rubber band (spell hit slot 4): pulled toward the Thing that holds the other end.
    if (P->tether_target != 0) {
        Thing *o = thing_at(thing_wrap(P->tether_target));
        int dist = pos_dist_xyz(thing_pos(t), thing_pos(o));
        if (dist >= 0x1400 || o->health < 0 || P->tether_timer == 1000) {
            P->tether_target = 0;
            P->tether_timer = 0;
        }
        int lim = ((int)t->speed_base * 3) / 2;
        int unit = (int16_t)lim != 0 ? 0x400 / (int16_t)lim : 0;       // idiv (0 would fault)
        int pull = unit != 0 ? (dist - P->tether_length) / unit : 0;
        if ((int16_t)pull < -(int)(int16_t)lim) pull = -lim;
        if ((int16_t)pull > (int16_t)lim) pull = lim;
        int ang = pos_angle_to(thing_pos(t), thing_pos(o)) & 0xffff;
        t->yaw = (uint16_t)((t->yaw + angle_turn_step(t->yaw, ang, 5, 0x82)) & 0x7ff);
        math_rotate_offset(&g_pos_scratch, ang, t->pitch, (int16_t)pull);
        P->tether_timer++;
    }

    if ((int16_t)player_terrain_collide(t) != 0) thing_move_to(t, &g_pos_scratch);

    // Flying sound (water / land), fire and ridge proximity, combat music, ambient noise.
    int lp = g_state->local_player;
    if (terrain_type_mask_at(thing_pos(t)) == 1) {
        snd_request(0, lp, 1);
        snd_fade(0, lp, 2);
    } else {
        snd_request(0, lp, 2);
        snd_fade(0, lp, 1);
    }
    if (P->fire_dist < 0x600) {
        snd_request(0, lp, 5);
        P->fire_dist = 0x800;
    } else {
        snd_fade(0, lp, 5);
    }
    if (P->ridge_dist < 0x600) {
        snd_request(0, lp, 0x1f);
        P->ridge_dist = 0x800;
    } else {
        snd_fade(0, lp, 0x1f);
    }
    if (P->player_no == g_state->local_player) {
        if (P->combat_music > 0) {
            P->combat_music--;
            music_update(2);
        } else {
            music_update(1);
        }
    }
    if ((t->tick & 0x3f) == 0) {
        t->rng = mc_lcg(t->rng);
        if (t->rng % 0xb == 0) snd_request((int16_t)thing_index(t), -1, 0x2e);
    }
}

// player_cast_spell_410f0(thing, spell, hand flag 0x100 / 0x200, input bit 0x10 / 0x20)
void player_cast_spell(Thing *t, Thing *spell, uint32_t hand_flag, uint32_t input_bit) {
    if (!spell || spell == thing_at(0)) return;
    PlayerBlock *P = player_block(t);
    if (P->input_bits & input_bit) {
        uint8_t id = spell->type;
        if (id == 0x10) {                                // Castle: one at a time
            if (spell->cast_ticks != 0) {
                snd_request(0, P->player_no, 0x1d);
                return;
            }
            if (t->mana < spell->mana_total) return;
            goto start;
        }
        if (id == 0x15 || id == 2) {                     // spells 2 and 0x15 cancel each other
            Thing *other = thing_from_raw((int16_t)P->spell_thing[id == 0x15 ? 2 : 0x15]);
            if (!other) other = thing_at(0);
            if (other->cast_ticks != 0) other->cast_ticks = 0;
            if (t->mana < spell->mana_total) return;
        }
        if (spell->unk3e != 0) {                         // multi-shot spell: queue a burst
            spell->burst++;
            int n = spell->mana_total != 0 ? t->mana / spell->mana_total : 0;   // idiv (0 would fault)
            int16_t shots = (int16_t)n;
            int16_t most = (int8_t)spell->unk3e;
            if (shots > most) shots = most;
            if ((int8_t)spell->burst < 0) spell->burst = 0;
            if ((int16_t)(int8_t)spell->burst > shots) spell->burst = (uint8_t)shots;
            t->flags = (t->flags & ~0x300u) | hand_flag;
            return;
        }
        if (t->mana < spell->mana_total) return;
    } else {
        if (spell->burst == 0) return;
        if (!(t->flags & hand_flag)) return;
    }
start:
    spell->cast_ticks = spell->duration;
    t->flags = ((t->flags & ~0x300u) | hand_flag) & ~0x20u;
}

// player_apply_controls_40e70
void player_apply_controls(Thing *t) {
    PlayerBlock *P = player_block(t);
    if (engine1995() && P->input_bits == 0x30) {
        // 1995 (CD 0x46848): exactly "fire left + fire right" and nothing else (also what the 1995
        // debug key 0x26 queues) sets the own castle's health to -1 and applies no controls.
        if (P->castle != 0) thing_at(thing_wrap(P->castle))->health = -1;
        return;
    }
    P->accelerating = 0;
    int dir = 0;
    if ((P->input_bits & 1) && (int)P->target_speed < k_speed_max) dir = 1;
    if ((P->input_bits & 2) && (int)P->target_speed > k_speed_min) dir = -1;
    if (dir != 0) {
        P->target_speed = (int16_t)(P->target_speed + (int16_t)(dir * (int16_t)k_speed_step));
        if ((int)P->target_speed < k_speed_min) P->target_speed = (int16_t)k_speed_min;
        if ((int)P->target_speed > k_speed_max) P->target_speed = (int16_t)k_speed_max;
        P->accelerating = 1;
    }
    dir = 0;
    if (P->input_bits & 4) dir = -1;
    if (P->input_bits & 8) dir = 1;
    if (dir == 0) {
        int s = isign(P->strafe_speed);
        P->strafe_speed = (int16_t)(P->strafe_speed + (int16_t)k_strafe_decay * s);
        if (s != isign(P->strafe_speed)) P->strafe_speed = 0;
    } else {
        P->strafe_speed = (int16_t)(P->strafe_speed + (int16_t)(dir * (int16_t)k_strafe_step));
    }
    if ((int)P->strafe_speed < k_strafe_min) P->strafe_speed = (int16_t)k_strafe_min;
    if ((int)P->strafe_speed > k_strafe_max) P->strafe_speed = (int16_t)k_strafe_max;

    player_cast_spell(t, thing_from_raw(spell_slot_raw(t, P->slot_left)), 0x100, 0x10);
    player_cast_spell(t, thing_from_raw(spell_slot_raw(t, P->slot_right)), 0x200, 0x20);
}

// player_apply_hits_40b70: consumes damage slots 4 (rubber band), 3 (mana steal) and 0 (damage).
int player_apply_hits(Thing *t) {
    PlayerBlock *P = player_block(t);
    int result = 0;
    if (t->health < 0) return 2;
    if (t->damage_slots[4].attacker != 0) {
        Thing *a = thing_at(thing_wrap(t->damage_slots[4].attacker));
        PlayerBlock *aP = player_block(a);
        aP->tether_target = thing_index(t);
        aP->tether_timer = 200;
        aP->tether_length = pos_dist_xyz(thing_pos(t), thing_pos(a));
        if (aP->tether_length < 0x400) aP->tether_length = 0x400;
        if (aP->tether_length > 0xc00) aP->tether_length = 0xc00;
        t->damage_slots[4].attacker = 0;
        P->hit_flash = 4;
        P->regen_pause = 0x10;
        player_set_combat_music_timer(t);
    }
    if (t->damage_slots[3].attacker != 0) {
        Thing *a = thing_at(thing_wrap(t->damage_slots[3].attacker));
        if (a->cls == 3) a->mana += t->damage_slots[3].amount;
        t->mana -= t->damage_slots[3].amount;
        P->hit_flash = 4;
        P->regen_pause = 0x10;
        player_set_combat_music_timer(t);
        t->damage_slots[3].attacker = 0;
    }
    if (t->damage_slots[0].attacker != 0) {
        if (t->flags & 0x4000) {                         // shield: a quarter gets through, paid in mana
            int32_t q = t->damage_slots[0].amount / 4;
            t->mana -= q;
            t->damage_slots[0].amount = q;
            t->flags &= ~0x4000u;
        }
        t->health -= t->damage_slots[0].amount;
        Thing *a = thing_at(thing_wrap(t->damage_slots[0].attacker));
        if (a != thing_at(0)) {
            P->knock_yaw = (uint16_t)pos_angle_to(thing_pos(a), thing_pos(t));
            P->knock_pitch = (uint16_t)pos_pitch_to(thing_pos(a), thing_pos(t));
            P->knock_speed = (int16_t)(t->damage_slots[0].amount / 10);
            if (P->knock_speed < 0) P->knock_speed = 0;
            if (P->knock_speed > 0x50) P->knock_speed = 0x50;
        }
        player_set_palette_effect(t, 2);
        P->hit_flash = 4;
        P->regen_pause = 0x10;
        snd_request((int16_t)thing_index(t), -1, 0x11);
        if (t->health < 0) {
            t->killer = t->damage_slots[0].attacker;
            return 2;
        }
        result = 1;
        player_set_combat_music_timer(t);
        t->damage_slots[0].attacker = 0;
    }
    return result;
}

// player_take_damage_42770
int player_take_damage(Thing *t) {
    if (t->health < 0) return 2;
    int result = 0;
    if (t->damage_slots[0].attacker != 0) {
        t->health -= t->damage_slots[0].amount;
        player_block(t)->damage_flash = 4;
        if (t->health < 0) {
            t->killer = t->damage_slots[0].attacker;
            return 2;
        }
        result = 1;
        t->damage_slots[0].attacker = 0;
    }
    return result;
}

// player_type0_s0_update_402c0: the living flyer.
void player_type0_s0_update(Thing *t) {
    bool in_castle = false;
    const bool paused = (g_cfg->paused & 1) != 0;
    if (!paused) player_rebuild_spell_index(t);
    t->speed_base = (int16_t)k_speed_max;
    PlayerBlock *P = player_block(t);
    if (!paused && P->castle != 0) {
        if (thing_collide(t, thing_at(thing_wrap(P->castle)))) in_castle = true;
    }
    player_apply_controls(t);
    if (!paused && in_castle) {
        // Inside the own castle: hits go to the castle and the wizard is untouchable.
        if (t->damage_slots[0].attacker != 0) {
            Thing *c = thing_at(thing_wrap(P->castle));
            if (c->damage_slots[0].attacker != 0) c->damage_slots[0].amount += t->damage_slots[0].amount;
            else                                  c->damage_slots[0].amount = t->damage_slots[0].amount;
            c->damage_slots[0].attacker = t->damage_slots[0].attacker;
        }
        P->invuln_timer = 2;
    }
    if (!paused) {
        if (P->invuln_timer == 0) {
            player_apply_hits(t);
        } else {
            std::memset(t->damage_slots, 0, sizeof t->damage_slots);
            P->invuln_timer--;
        }
        if (P->aim_charge < 200) P->aim_charge++;
    }
    player_flyer_move(t);
    if (t->health < 0) {
        t->state = 2;
        t->z_vel = 0;
        snd_request((int16_t)thing_index(t), -1, 0x10);
        return;
    }
    if (paused) return;
    t->mana += t->mana_cost;
    if (P->regen_pause == 0) t->health += P->health_regen;
    else                     P->regen_pause--;
    if (t->mana < 0) t->mana = 0;
    if (t->mana > t->mana_total) t->mana = t->mana_total;
    if (t->health < -1) t->health = -1;
    if (t->health > t->max_health) t->health = t->max_health;
    if (P->countdown15f != 0) P->countdown15f--;
    if (P->timer210 != 0) P->timer210--;
    if (in_castle || (t->flags & 0x1000)) {              // 0x1000: standing on a dolmen
        t->mana_cost = t->mana_total / 200;
        P->health_regen = (int16_t)(t->max_health / 250);
        if (t->mana_cost < 1000) t->mana_cost = 1000;
        t->flags &= ~0x1000u;
    } else {
        t->mana_cost = t->mana_total / 2000;
        P->health_regen = (int16_t)(t->max_health / 2000);
        if (t->mana_cost < 100) t->mana_cost = 100;
    }
}

// player_dying_update_405f0 (state 2): fall to the ground, then drop the spells and the mana.
void player_dying_update(Thing *t) {
    player_flyer_move(t);
    if (cfg_tick_bit(6) != 0) player_set_palette_effect(t, 7);       // Config+0x63
    PlayerBlock *P = player_block(t);
    t->z = (int16_t)(t->z + t->z_vel);
    t->z_vel = (int16_t)(t->z_vel - 2);
    if (t->z_vel < -0x100) t->z_vel = -0x100;
    if (t->z_vel > 0) t->z_vel = 0;
    int clear = mc_move_desc(t->desc)->clear_hi;
    int ground = terrain_height_at(thing_pos(t));
    if ((int)t->z < clear + (int16_t)ground) t->z = (int16_t)(ground + clear);
    Thing *smoke = thing_create(&g_pos_scratch, 10, 1);
    if (smoke) {
        smoke->flags |= 0x80;
        smoke->owner = t->owner;
    }
    if ((int)t->z != clear + (int16_t)ground) return;

    models_initialise();
    if (t->killer != 0) {
        Thing *k = thing_at(thing_wrap(t->killer));
        if (k->cls == 3 && (k->type == 0 || k->type == 1))
            player_block(k)->kills_of_player[P->player_no & 7]++;
    }
    {
        PlayerMsg *m = &g_state->players[g_state->local_player & 7].messages[P->player_no & 7];
        text_copy(m->text, sizeof m->text, MC_TEXT_HAS_DIED);   // strcpy(m->text, *(char **)0xaddc0)
        m->ticks = 100;
    }
    std::memset(t->damage_slots, 0, sizeof t->damage_slots);
    for (int i = 0; i < 24; i++) {
        Thing *s = thing_from_raw(P->spell_slot[i]);
        if (!s) { P->spell_slot[i] = -1; continue; }
        int id = (int8_t)s->type;
        P->spell_slot[i] = id;                           // back to "spell id" for the respawn
        if (id >= 0 && id < 24) P->spell_sealed[id] = (s->flags & 0x40000) ? 1 : 0;
        s->flags &= ~1u;
        s->state++;                                      // the dropped-spell phase
        g_pos_scratch = *thing_pos(t);
        t->rng = mc_lcg(t->rng);
        g_pos_scratch.x = (uint16_t)((int16_t)g_pos_scratch.x + (int)(t->rng & 0x1ff) - 0x100);
        t->rng = mc_lcg(t->rng);
        g_pos_scratch.y = (uint16_t)((int16_t)g_pos_scratch.y + (int)(t->rng & 0x1ff) - 0x100);
        thing_move_to(s, &g_pos_scratch);
        t->rng = mc_lcg(t->rng);
        s->health = (int32_t)(t->rng % 0x5a + 200);
    }
    Thing *ball = thing_create(thing_pos(t), 10, 0x28);
    if (ball) {
        t->state = 3;
        t->aux = (int16_t)((((0xff - (int)P->ai_reaction) / 8) << 5) + 0x20);   // respawn delay of an AI wizard
        for (uint32_t i = g_cfg->mana_ball_list; i != 0 && i < (uint32_t)thing_pool_slots(); i = thing_at(i)->next) {
            Thing *b = thing_at(i);
            if (b->type == 0x27 && b->mana_owner == thing_index(t)) b->mana_owner = thing_index(ball);
        }
    }
    t->flags |= 0x20;
    g_state->active_top--;
}

// player_look_at_killer_409e0
void player_look_at_killer(Thing *t) {
    Thing *k = thing_at(thing_wrap(t->killer));
    t->target_yaw = (uint16_t)pos_angle_to(thing_pos(t), thing_pos(k));
    t->target_pitch = (uint16_t)pos_pitch_to(thing_pos(t), thing_pos(k));
    t->yaw = (uint16_t)((t->yaw + angle_turn_step(t->yaw, t->target_yaw, 5, 0x16)) & 0x7ff);
    t->pitch = (uint16_t)((t->pitch + angle_turn_step(t->pitch, t->target_pitch, 5, 0x16)) & 0x7ff);
    t->pitch = 0;
    PlayerBlock *P = player_block(t);
    P->pitch_acc = (int16_t)t->pitch;
    P->yaw_rate = 0;
}

// player_type3_s3_update_40ab0 (state 3: dead). An AI wizard respawns at its castle after
// Thing.aux ticks (or leaves the game without one); a human watches the killer until command 0xf.
void player_type3_s3_update(Thing *t) {
    PlayerBlock *P = player_block(t);
    P->knock_speed = 0;
    PlayerRec *rec = &g_state->players[P->player_no & 7];
    if (rec->is_computer == 1) {
        if (g_cfg->flags & 0x200) return;
        if (P->castle != 0) {
            if (t->aux == 0) player_spawn(rec, t);
            else             t->aux--;
            return;
        }
        rec->active = 0;
        return;
    }
    player_set_palette_effect(t, 7);
    player_look_at_killer(t);
}

// ---- per tick ----------------------------------------------------------------------------------

// player_log_position_3e080
void player_log_position(PlayerRec *rec, Thing *t) {
    if (g_cfg->paused & 1) return;
    unsigned idx = (uint16_t)(rec->log_count - 1);
    if (idx >= 32) return;                              // log_count is 0x20 from players_init_records
    PosLogEntry *e = &rec->log[idx];
    PlayerBlock *P = player_block(t);
    world_log_set_xy((int)(rec - g_state->players), e, t->x, t->y);    // round 11: + the high halves (world.h)
    (void)terrain_height_at(thing_pos(t));              // called, result discarded
    e->z = t->z;
    e->yaw = t->yaw;
    e->zoom = rec->log_template.zoom;
    e->roll = P->yaw_rate;
    int k = P->knock_speed;
    e->pitch = (int16_t)((int)cfg_tick_bit(1) * (k / 16) + (int)P->pitch_acc / 2 - k / 8);
}

// The 8 arguments render_frame_1fab0 pushes for render_view_2f6e0 (0x1fbb9..0x1fc2d).
Camera player_camera(int player) {
    const PlayerRec *rec = &g_state->players[player & 7];
    const PosLogEntry *e = &rec->log[rec->view_entry < 32 ? rec->view_entry : 31];
    Camera c;
    // round 11: the original sign-extends the words (the renderers keep the low 16 bits); a wider world
    // needs the whole coordinate (world.h)
    c.cam_x = world_wide() ? world_log_x(player, *e) : (int16_t)e->x;
    c.cam_y = world_wide() ? world_log_y(player, *e) : (int16_t)e->y;
    c.yaw   = (int16_t)e->yaw;
    c.cam_z = (int)e->z + 0x80;
    c.pitch = e->pitch;
    c.roll  = e->roll;
    c.zoom  = e->zoom;
    return c;
}

// game_check_level_won_3db20
void game_check_level_won() {
    if (g_cfg->flags & 0x110) return;                   // not in network games and movies
    for (int p = 0; p < (int)(uint16_t)g_state->player_count && p < 8; p++) {
        PlayerRec *rec = &g_state->players[p];
        if (rec->active == 0) continue;
        PlayerBlock *P = player_block(thing_at(rec->thing));
        if (P->castle == 0) continue;
        int32_t mana = thing_at(thing_wrap(P->castle))->mana + P->mana_in_transit;
        if (g_cfg->total_mana == 0) continue;
        int32_t pct = (int32_t)((uint32_t)mana * 100u) / (int32_t)g_cfg->total_mana;     // imul 32-bit, idiv
        if (pct > (int)g_state->level.win_percent) {
            if ((int16_t)rec->win_timer >= 0x10) rec->status |= 2;
            else                                 rec->win_timer++;
        } else {
            rec->win_timer = 0;
        }
    }
}

static void set_notice(int p, const char *text) {
    // The cheat notices go to players[local].messages[p] (p is the local player there).
    PlayerMsg *m = &g_state->players[g_state->local_player & 7].messages[p & 7];
    m->ticks = 100;
    m->arg = 0;
    std::memcpy(m->text, text, std::strlen(text) + 1);           // strcpy: the tail of a longer old text stays
}

// Command 0x1e: the "chronicle" cheats (queued by player_queue_command_17270 after the name check).
static void player_cheat(int p, Thing *t, const CmdPacket *cmd) {
    PlayerBlock *P = player_block(t);
    const bool local = g_state->local_player == p;
    switch (cmd->arg) {
    case 1: {   // all spells: one new spell Thing per missing spell id into the first empty slot
        for (int id = 0; id < 24; id++) {
            if (P->spell_thing[id] != 0) continue;
            for (int slot = 0; slot < 24; slot++) {
                if (P->spell_slot[slot] != 0) continue;
                ThingCreateFn fn = thing_create_fn(0xc, id);         // class-12 Table B, no enabled test
                Thing *s = fn ? fn(thing_pos(t)) : nullptr;
                if (!s) continue;
                s->flags |= 0x40001;
                s->mana_cost = 0;
                s->caster = thing_index(t);
                P->spell_slot[slot] = thing_index(s);
                for (int k = 0; k < 10; k++)
                    if (P->hotkey_slot[k] == 0xff) { P->hotkey_slot[k] = (uint8_t)slot; break; }
                break;
            }
        }
        for (int i = 1; i < thing_pool_slots(); i++)
            if (thing_at(i)->cls == 0xb) thing_at(i)->flags &= ~1u;      // every switch becomes usable
        if (local) set_notice(p, ".. CHEAT: access all spells");
        break;
    }
    case 2: {   // more mana: a 100000 mana ball owned by the player
        Thing *ball = thing_create(thing_pos(t), 10, 0x27);
        if (ball) {
            ball->mana = 100000;
            ball->mana_owner = thing_index(t);
        }
        t->mana = t->mana_total;
        if (local) set_notice(p, ".. CHEAT: more mana");
        break;
    }
    case 3: case 4: case 5: {   // destroy all other wizards / castles / balloons
        for (uint32_t i = g_cfg->player_list; i != 0 && i < (uint32_t)thing_pool_slots(); i = thing_at(i)->next) {
            Thing *o = thing_at(i);
            if (o->owner == t->owner) continue;
            bool hit = cmd->arg == 3 ? (o->type == 0 || o->type == 1) : cmd->arg == 4 ? o->type == 2 : o->type == 3;
            if (hit) o->health = -1;
        }
        if (local) set_notice(p, cmd->arg == 3 ? ".. CHEAT: destroy all players"
                               : cmd->arg == 4 ? ".. CHEAT: destroy all castles" : ".. CHEAT: destroy all balloons");
        break;
    }
    case 6:
        t->health = t->max_health;
        if (local) set_notice(p, ".. CHEAT: heal");
        break;
    case 7:
        if (g_hook_creature_kill_all) g_hook_creature_kill_all();           // creature_kill_all_17ff0
        if (local) set_notice(p, ".. CHEAT: Kill all creatures");
        break;
    default:
        break;
    }
}

// player_commands_process_3a8b0: one command packet per player per tick.
void player_commands_process() {
    bool stop_recording = false;
    if (g_cfg->flags & 0x10) {
        g_cfg->net_time = g_timer_ticks;                                     // timed into Config+0x9d
        net_exchange_frame(g_state->commands, 10);                          // net_exchange_frame_4f530
        g_cfg->net_time = g_timer_ticks - g_cfg->net_time;
        bool joined = false;
        for (int p = 0; p < (int)(uint16_t)g_state->player_count && p < 8; p++) {
            if (g_state->commands[p].cmd != 1) continue;
            PlayerRec *rec = &g_state->players[p];
            rec->active = 1;
            if (p == g_state->local_player) {
                if (g_cfg->save_str_a[0] == 0) std::strcpy(g_cfg->save_str_a, player_default_name(p));
                else                           std::memcpy(rec->name, g_cfg->save_str_b, 10);
                rec->name[10] = 0;
            }
            joined = true;
        }
        if (joined) {
            net_exchange_frame(g_state->players, 0x801);                    // net_exchange_frame_4f530: the records
        }
    }

    auto local_rec = []() { return &g_state->players[g_state->local_player & 7]; };
    local_rec()->tick++;
    g_cfg->tick++;
    for (int k = 1; k < 0x10; k++)
        cfg_tick_bit(k) = (uint8_t)((local_rec()->tick / (uint32_t)k) & 1);

    for (int p = 0; p < (int)(uint16_t)g_state->player_count && p < 8; p++) {
        PlayerRec *rec = &g_state->players[p];
        CmdPacket *cmd = &g_state->commands[p];
        // The Thing pointer is taken before the demo step, which may replace the whole state.
        Thing *t = thing_at(thing_wrap(rec->thing));
        demo_record_playback_step(cmd);
        rec->unk44b = 0;
        PlayerBlock *P = player_block(t);
        switch (cmd->cmd) {
        case 1:     // join
            rec->active = 1;
            if (p == g_state->local_player && g_cfg->save_str_a[0] != 0) {
                std::memcpy(rec->name, g_cfg->save_str_b, 10);
                rec->name[10] = 0;
            }
            [[fallthrough]];
        case 3:     // (re)spawn
        spawn:
            player_spawn(rec, t);
            player_set_input_mode(rec, 0);
            break;
        case 2:     // quit
            player_cd_check_quit();
            if (g_state->local_player == p) rec->quit = 1;
            t->health = -1;
            net_player_disconnect(p);
            rec->active = 0;
            break;
        case 4:
            rec->flags ^= cmd->arg;
            break;
        case 7: {   // camera log entry to view (never changes in practice: only one entry is live)
            int v = (int)rec->view_entry + (int8_t)cmd->arg;
            if (v >= 0 && v < (int)rec->log_count - 1) rec->view_entry = (uint16_t)(rec->view_entry + (int8_t)cmd->arg);
            break;
        }
        case 8:     // zoom
            rec->log_template.zoom = (int16_t)(rec->log_template.zoom + (int8_t)cmd->arg);
            break;
        case 10:    // quick save
            if (!(g_cfg->flags & 0x10)) {
                demo_save_state(10000);                                     // demo_save_state_3c2c0(10000)
            }
            break;
        case 0xb:   // quick load
            if (!(g_cfg->flags & 0x10)) demo_load_state(10000);
            break;
        case 0xc:   // start recording
            if (!(g_cfg->flags & 0x20) && p == g_state->local_player) {
                std::memset(cfg_bytes() + 0x8e1a, 0, 4);
                g_cfg->flags |= 2;
            }
            break;
        case 0xe:   // stop recording / playback
            stop_recording = true;
            break;
        case 0xf:   // respawn request of a dead player; without a castle the level is lost
            if (P->castle == 0) {
                if (g_cfg->flags & 0x10) goto leave_game;
                rec->status |= 0xc;
            }
            goto spawn;
        case 0x10:  // chat: begin a line
            std::memset(&local_rec()->messages[p], 0, sizeof(PlayerMsg));
            player_set_input_mode(rec, 3);
            rec->messages[p].ticks = 32000;
            rec->messages[p].arg = 1;
            break;
        case 0x11: { // chat: one key (8 = backspace)
            char *text = local_rec()->messages[p].text;
            size_t len = std::strlen(text);
            if (cmd->arg == 8) {
                if (len != 0) text[len - 1] = 0;
            } else if (cmd->arg != 0 && len < 0x3f) {
                text[len] = (char)cmd->arg;              // sprintf("%c") + strcat
                text[len + 1] = 0;
            }
            rec->messages[p].ticks = 32000;
            rec->messages[p].arg = 1;
            break;
        }
        case 0x12:  // chat: cancel
            std::memset(&local_rec()->messages[p], 0, sizeof(PlayerMsg));
            player_set_input_mode(rec, 0);
            break;
        case 0x13: { // chat: send. "RATTY" opens the cheat gate instead of being shown.
            PlayerMsg *m = &local_rec()->messages[p];
            auto is = [&](int i, char c) { return m->text[i] == c || m->text[i] == (char)(c + 0x20); };
            // 1995 (CD 0x3d187): the word is "QUICK"
            const bool gate = engine1995() ? (is(0, 'Q') && is(1, 'U') && is(2, 'I') && is(3, 'C') && is(4, 'K'))
                                           : (is(0, 'R') && is(1, 'A') && is(2, 'T') && is(3, 'T') && is(4, 'Y'));
            if (gate) {
                m->ticks = 0;
                m->arg = 0;
                if (p == g_state->local_player) g_cfg->flags |= 0x8000;
            } else {
                m->ticks = 200;
                m->arg = 3;
            }
            player_set_input_mode(rec, 0);
            break;
        }
        case 0x14:  // input mode (2 = open the spell book)
            player_set_input_mode(rec, (int8_t)cmd->arg);
            break;
        case 0x15:  // book slot for the left hand
            if ((int8_t)cmd->arg != -1) {
                P->slot_left = (int8_t)cmd->arg;
                snd_request(0, (int16_t)rec->index, 0xe);
                player_set_input_mode(rec, 0);
            }
            break;
        case 0x16:  // book slot for the right hand
            if ((int8_t)cmd->arg != -1) {
                P->slot_right = (int8_t)cmd->arg;
                snd_request(0, (int16_t)rec->index, 0xe);
                player_set_input_mode(rec, 0);
            }
            break;
        case 0x17:  // assign book slot pad2 to quick-select key arg
            if ((int8_t)cmd->pad2 != -1) {
                for (int k = 0; k < 0x18; k++)
                    if (P->hotkey_slot[k] == cmd->pad2) P->hotkey_slot[k] = 0xff;
                int key = (int8_t)cmd->arg;
                if (key >= 0 && key < 24) P->hotkey_slot[key] = cmd->pad2;
                snd_request(0, (int16_t)rec->index, 0xe);
            }
            break;
        case 0x18:  // quick-select key -> left hand
        case 0x19: { // quick-select key -> right hand
            player_cd_check_quit();
            int key = (int8_t)cmd->arg;
            int slot = (key >= 0 && key < 24) ? (int8_t)P->hotkey_slot[key] : -1;
            if (slot != -1) {
                if (cmd->cmd == 0x18) P->slot_left = (int16_t)slot;
                else                  P->slot_right = (int16_t)slot;
                Thing *s = thing_from_raw(spell_slot_raw(t, slot));
                int id = s ? (int8_t)s->type : (int8_t)thing_at(0)->type;
                if (id >= 0 && id < 24) P->spell_flash[id] = 0x20;
            }
            snd_request(0, (int16_t)rec->index, 0xe);
            break;
        }
        case 0x1a:  // everybody quits
            for (int q = 0; q < (int)(uint16_t)g_state->player_count && q < 8; q++) {
                if (g_state->players[q].active == 0) continue;
                g_state->players[q].quit = 1;
                net_player_disconnect(q);
            }
            break;
        case 0x1b:
            player_cd_check_quit();
            rec->status = 10;
            net_player_disconnect(p);
            break;
        case 0x1c:
            rec->status = 0xc;
            net_player_disconnect(p);
            break;
        case 0x1d:  // leaves the (network) game
        leave_game: {
            PlayerMsg *m = &local_rec()->messages[P->player_no & 7];
            text_copy(m->text, sizeof m->text, MC_TEXT_ELIMINATED);   // strcpy(m->text, *(char **)0xadde0)
            m->ticks = 100;
            m->arg = 0;
            rec->status = 8;
            net_player_disconnect(p);
            rec->active = 0;
            break;
        }
        case 0x1e:
            player_cd_check_quit();
            player_cheat(p, t, cmd);
            break;
        default:    // 0, 5, 6, 9, 0xd: nothing (6 only marks a packet that carries steering / keys)
            // port (round 10): ids >= 0x20 are port commands (debug commands, mode orders), unknown to the original
            if (cmd->cmd >= 0x20 && g_hook_port_command) g_hook_port_command(p, t, cmd);
            break;
        }

        // Steering and keys of the packet go to the player's (possibly new) Thing.
        t = thing_at(thing_wrap(rec->thing));
        P = player_block(t);
        P->steer_dx = (int16_t)(((int)cmd->steer_x * 2 - (int)P->yaw_rate) / 4);
        P->steer_dy = (int16_t)(((int)cmd->steer_y * 2 - (int)P->pitch_acc) / 4);
        P->input_bits = cmd->bits;
        if (local_rec()->quit != 0 || stop_recording) {
            demo_close();
            if (stop_recording) {
                g_cfg->flags &= ~2;
                g_cfg->movie++;
            }
        }
        player_log_position(rec, thing_at(thing_wrap(rec->thing)));
        std::memset(cmd, 0, sizeof *cmd);
    }
}

// The simulation half of game_tick_update_32e80.
void game_tick_sim() {
    // port (round 10): tick profiler markers (tick_profile.h) - clock reads only, off when g_tick_profile is null
    TickProfile *const tp = g_tick_profile;
    int64_t tp_t = tp ? tick_profile_now_ns() : 0;
    if (g_state->mode_3d == 0) palette_effect_update();                     // palette_effect_update_33010
    if (tp) tick_profile_mark(tp, TP_PALETTE, &tp_t);
    // texture_anim_update_4be50 (not paused) runs in engine_tick (renderer side)
    if (!(g_cfg->flags & 4) && g_hook_player_local_input) g_hook_player_local_input();   // player_local_input_16660
    if (tp) tick_profile_mark(tp, TP_INPUT, &tp_t);
    player_commands_process();
    if (tp) tick_profile_mark(tp, TP_COMMANDS, &tp_t);
    if (!(g_cfg->paused & 1)) game_check_level_won();
    if (tp) tick_profile_mark(tp, TP_WIN, &tp_t);
    switch (g_cfg->substeps) {
    case 0: thing_update_all(); break;
    case 1: for (int i = 0; i < 4; i++) thing_update_all(); break;
    case 2: for (int i = 0; i < 16; i++) thing_update_all(); break;
    default: break;                                     // any other value: no update at all
    }
    if (tp) tick_profile_mark(tp, TP_THINGS, &tp_t);
    if (g_hook_mode_tick) g_hook_mode_tick();                            // port: the game mode's tick (mode.h)
    if (g_hook_debug_tick) g_hook_debug_tick();                          // port: debug commands (god mode, debug_cmd.h)
    if (tp) tick_profile_mark(tp, TP_MODE, &tp_t);
    if (g_hook_sound_update) g_hook_sound_update();                      // sound_update_494b0
    if (tp) tick_profile_mark(tp, TP_SOUND, &tp_t);
    // The render half (render_frame_1fab0 ... vga_present_frame_2f480) is the platform's, but
    // render_frame writes game state once per tick (HUD counters, book selection): hud_tick_state.
    if (g_hook_frame_state) g_hook_frame_state(g_state->local_player);
    if (tp) tick_profile_mark(tp, TP_FRAME, &tp_t);
}

void player_register_handlers() {
    thing_register_update(0x402c0, player_type0_s0_update);
    thing_register_update(0x405f0, player_dying_update);
    thing_register_update(0x40ab0, player_type3_s3_update);
    g_hook_mana_totals_update = mana_totals_update;
}
