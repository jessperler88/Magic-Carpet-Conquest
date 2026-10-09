// Class-10 effect handlers of carpet.exe, 0x2376f..0x284c0 (everything that is not in
// level_features.cpp). Translated from the disassembly (python tools/analysis/img.py dis <addr> <end>);
// the original name and address of every function is in the comment on its definition.
//
// Conventions of the range: every handler takes the Thing on the stack; `life = health--` followed
// by `if (life < 0) delete` is the common lifetime test (the health is decremented first, the *old*
// value is tested); Thing.aux (+0x1a) is the per-effect timer / ring counter; flag 2 = "first tick
// done"; Thing.home.x / .y (+0x96 / +0x98) double as a signed x / y velocity for everything that
// rolls (mana balls, lava blobs, type 0x2c).
#include "effects.h"
#include "world_set.h"
#include "constructors.h"   // mana_ball_update_sprite
#include "mode_rules.h"     // same_side (round 12)
#include "terrain_paint.h"
#include "level_features.h"
#include "player.h"
#include "mc_math.h"
#include "gen/effects_tables.h"
#include <cstring>
#include <vector>

namespace {

// Round 11: cell coordinates are ints wrapped onto the world (the original's uint8_t arithmetic at 256).
inline unsigned cell_xy(int x, int y) { return world_cell(x, y); }
// Cell of a thing as the effect handlers compute it: (coordinate + 0x80) >> 8 on the signed word.
inline int thing_cell_x(const Thing *t) { return world_wrap_cell((world_signed(t->x) + 0x80) >> 8); }
inline int thing_cell_y(const Thing *t) { return world_wrap_cell((world_signed(t->y) + 0x80) >> 8); }
// t->rng = t->rng * 0x24a1 + 0x24df; returns the new value.
inline uint32_t rng_next(Thing *t) { t->rng = mc_lcg(t->rng); return t->rng; }
// A thing index read from a field (the original multiplies by 0xa4 without a range check).
inline Thing *thing_ref(unsigned idx) { return thing_at(thing_wrap(idx)); }
// (thing - base) / 0xa4 followed by cwde: the index as sound_request_49720 gets it.
inline int snd_index(const Thing *t) { return (int16_t)thing_index(t); }
// `life = health; health = life - 1; life < 0`
inline bool life_expired(Thing *t) {
    int32_t life = t->health;
    t->health = life - 1;
    return life < 0;
}
// GameState.volcano_thing / volcano_smoke (+0x24 / +0x26, inside the per-level zeroed block): the
// erupting volcano (effect 0x12) and its smoke column (effect 0x13).

// Signed x / y velocity kept in Thing.home (+0x96 / +0x98). Round 11: a velocity is a 16-bit quantity in
// any world size - only its low 16 bits are meaningful (a negative one stored through WCoord in a wider world
// keeps them), so every read goes through vel_x / vel_y and a position adds the sign-extended value.
inline int  vel_x(const Thing *t) { return (int16_t)t->home.x; }
inline int  vel_y(const Thing *t) { return (int16_t)t->home.y; }
inline void clamp_velocity(Thing *t, int limit) {
    if (vel_x(t) < -limit) t->home.x = (uint16_t)-limit;
    if (vel_x(t) > limit)  t->home.x = (uint16_t)limit;
    if (vel_y(t) < -limit) t->home.y = (uint16_t)-limit;
    if (vel_y(t) > limit)  t->home.y = (uint16_t)limit;
}
// The slope push + friction shared by 243b0 and 25980: v += terrain slope vector (written into the
// position scratch), v = v * 250 / 256 (signed, truncating).
inline void roll_on_slope(Thing *t) {
    int16_t slope[2];
    terrain_slope_vector(thing_pos(t), slope);
    g_pos_scratch.x = (uint16_t)slope[0];
    g_pos_scratch.y = (uint16_t)slope[1];
    t->home.x = (uint16_t)(t->home.x + g_pos_scratch.x);
    uint16_t vy = (uint16_t)(t->home.y + g_pos_scratch.y);
    t->home.x = (uint16_t)((int32_t)(int16_t)t->home.x * 250 / 256);
    t->home.y = vy;
    t->home.y = (uint16_t)((int32_t)(int16_t)t->home.y * 250 / 256);
}

// Random position on the spiral offset (dx, dy) * spacing around t, as 23a80 / 24630 / 24a90 / 280d0
// build it (two RNG draws, x first): coordinate + d * spacing - 0x60 + rnd % 0x81 - 0x40.
inline Pos scatter_pos(Thing *t, int dx, int dy, int spacing) {
    Pos p;
    uint32_t r = rng_next(t) % 0x81u;
    p.x = dx * spacing + t->x - 0x60 + (int32_t)r - 0x40;          // WCoord wraps (the original's u16 store)
    r = rng_next(t) % 0x81u;
    p.y = dy * spacing + t->y - 0x60 + (int32_t)r - 0x40;
    p.z = t->z;
    return p;
}

// Castle footprint dimensions as every user of the table reads them: halved in 320x200.
inline void footprint_dims(const CastleFootprint *fp, uint32_t *w, uint32_t *h) {
    *w = fp->w;
    *h = fp->h;
    if (g_video_mode_flags == 1) { *w >>= 1; *h >>= 1; }
}
// `movsx eax, byte [t+0x47]` * 6 into the footprint table.
inline const CastleFootprint *thing_footprint(const Thing *t) {
    return castle_footprint((unsigned)(int)(int8_t)t->castle_size);
}

// mana_ball_update_sprite_25e20 is in constructors.cpp (constructors.h), shared with the constructor.

}  // namespace

// ---- helpers ---------------------------------------------------------------------------------------

// terrain_max_drop_around_2376f (entry 0x23780)
int terrain_max_drop_around(const Pos *pos) {
    int x = world_cx(pos->x - 0x80);
    int y = world_cx(pos->y - 0x80);
    int32_t centre = g_map_height[cell_xy(x, y)];
    int32_t drop = 0;
    // the walk order of the original: N, NE, E, SE, S, SW, W, NW (y - 1 first)
    static const int8_t step[8][2] = { {0, -1}, {1, 0}, {0, 1}, {0, 1}, {-1, 0}, {-1, 0}, {0, -1}, {0, -1} };
    for (const auto &s : step) {
        x = world_wrap_cell(x + s[0]);
        y = world_wrap_cell(y + s[1]);
        int32_t h = g_map_height[cell_xy(x, y)];
        if (h < centre && h - centre < drop) drop = h - centre;
    }
    return drop;
}

// effect_age_tick_25600
void effect_age_tick(Thing *t) {
    t->aux++;
    if (life_expired(t)) thing_mark_delete(t);
}

// terrain_ring_find_height_ne8_24d70 (jump table 0x24d54: +x, +y, -x, -y)
unsigned terrain_ring_find_height_ne8(unsigned cell_in, unsigned start_radius) {
    const uint32_t start = cell_in & (g_world.cells - 1);
    for (uint8_t r = (uint8_t)start_radius; r < 0x1e; r++) {
        int x = world_cell_x(start) - r, y = world_cell_y(start) - r;
        static const int8_t side[4][2] = { {1, 0}, {0, 1}, {-1, 0}, {0, -1} };
        for (const auto &s : side) {
            for (uint8_t n = r; n != 0; n--) {
                unsigned c = cell_xy(x, y);
                if (g_map_height[c] != 8) return c;
                x = world_wrap_cell(x + s[0]);
                y = world_wrap_cell(y + s[1]);
            }
        }
    }
    return start;
}

namespace {

// ---- explosions, fire, splash, smoke ----------------------------------------------------------------

// effect_explosion_update_238b0 (state 0): waits while aux & 3, then on the first live tick deals
// its area damage, scorches / dents the ground and picks a random vertical speed; afterwards it only
// rises / sinks and animates.
void effect_explosion_update(Thing *t) {
    if (t->aux & 3) {
        t->aux--;
        return;
    }
    if (life_expired(t)) {
        thing_mark_delete(t);
        return;
    }
    t->flags &= ~1u;
    int ground = (int16_t)terrain_height_at(thing_pos(t));
    if (!(t->flags & 2)) {
        unsigned cell = cell_xy(thing_cell_x(t), thing_cell_y(t));
        if (!(t->flags & 0x10000)) thing_area_damage(t, 0, t->damage);
        uint8_t tex = g_map_type[cell];
        if (tex != 0) {
            // DL / CL at the three paint calls are leftovers of the calls before (only read for a
            // quad that is completely flat at height 0, see terrain_paint.h): the defaults are used.
            if (tex == 0x1a)      terrain_paint_cell(cell, 0x14);
            else if (tex == 0x0a) terrain_paint_cell(cell, 0x15);
            else if (tex == 0x0b) terrain_paint_cell(cell, 0x16);
            else if (tex >= 6 && tex <= 0x22) { /* other castle textures: untouched */ }
            else if ((g_map_flags[cell] & 7) == 1) { }
            else if ((int16_t)t->z - ground > 0x80) { }
            else if (terrain_cell_flag_bit(thing_pos(t)) & 1) { }
            else {
                int r = (int)(rng_next(t) % 7u);
                terrain_find_cell_spiral(t, 0, 0, (int16_t)-r, 1);
            }
        }
        uint32_t r = rng_next(t);
        t->flags |= 2;
        t->z_vel = (int16_t)(r % 0x41u - 0x20);
        sound_request(snd_index(t), -1, 3);
    }
    pos_follow_ground(thing_pos(t), ground, 0, 0, (int16_t)t->z_vel);
    thing_anim_advance(t);
}

// effect_big_explosion_s1_update_23a80 (state 1): every tick, a ring of explosions (type 0) on about
// half of the spiral cells of ring `aux`, 0xc0 apart.
void effect_big_explosion_update(Thing *t) {
    if (life_expired(t)) {
        thing_mark_delete(t);
        return;
    }
    if (!(t->flags & 2)) {
        t->flags |= 2;
        sound_request(snd_index(t), -1, 3);
    }
    SpiralSearch s;
    int dx, dy;
    if (!spiral_search_begin(&s, (int16_t)t->aux, (int16_t)t->aux)) return;
    while (spiral_search_next(&s, &dx, &dy) == 1) {
        uint32_t r = rng_next(t) % 0x9du;
        if ((int32_t)(r / 0x4fu) * 2 - 1 <= 0) continue;
        Pos p = scatter_pos(t, dx, dy, 0xc0);
        Thing *c = thing_create(&p, 10, 0);
        if (!c) continue;
        c->owner = t->owner;
        c->yaw = t->yaw;
        c->flags |= (t->flags & 0x10000u) | 0x80u;
    }
}

// effect_type2_s2_update_23c00 (state 2) and effect_type3_s3_update_23d40 (state 3): only a lifetime.
void effect_lifetime_update(Thing *t) {
    if (life_expired(t)) thing_mark_delete(t);
}

// effect_fire_update_23c20 (state 6): grows for 7 ticks (sprite++), shrinks during the last 12
// (sprite--, a puff of white smoke with chance 1/7 unless flag 0x80), sits on the ground + z_vel,
// dies on water and burns what it touches (trees take a tenth).
void effect_fire_update(Thing *t) {
    bool dead = life_expired(t);
    if (!dead) {
        player_note_fire_distance(t);
        if (t->health >= 0xc) {
            if (t->aux <= 6) {
                t->sprite++;
                t->aux++;
            }
        } else if (t->aux > 0) {
            t->sprite--;
            t->aux--;
            if (!(t->flags & 0x80)) {
                if (rng_next(t) % 7u == 0) {
                    Thing *c = thing_create(thing_pos(t), 10, 0xd);
                    if (c) {
                        c->aux = 0x64;
                        c->health = 0xf;
                        c->owner = t->owner;
                        c->sprite = (uint16_t)(c->sprite + 2);
                    }
                }
            }
        }
        t->z = (int16_t)(terrain_height_at(thing_pos(t)) + t->z_vel);
        if (terrain_type_mask_at(thing_pos(t)) == 1) dead = true;
    }
    if (dead) thing_mark_delete(t);
    // (the damage is dealt on the tick the fire dies, too)
    if (!(t->flags & 0x10000)) thing_area_damage_fire(t, 0, t->damage);
}

// effect_splash_s5_update_23d60 (state 5)
void effect_splash_update(Thing *t) {
    if (life_expired(t)) {
        thing_mark_delete(t);
        return;
    }
    thing_anim_advance(t);
    if (!(t->flags & 2)) {
        t->flags |= 2;
        sound_request(snd_index(t), -1, 0x1b);
    }
}

// effect_type12_s12_update_240b0 (state 0xc): animated, damage type 1 every tick.
void effect_type12_update(Thing *t) {
    t->aux++;
    if (life_expired(t)) {
        thing_mark_delete(t);
        return;
    }
    thing_anim_advance(t);
    thing_area_damage(t, 1, t->damage);
}

// effect_white_smoke_update_24100 (state 0xd) / effect_black_smoke_update_241f0 (state 0xe): rises at
// speed_cur (decaying by 4 to 0x40, capped at 0x80), never below the ground under its *old*
// position, drifts 0x1e along its yaw for the first 15 ticks while the sprite advances every other
// tick, and runs the sprite back during the last 6 ticks of its life (white: not below 0x44).
void smoke_update(Thing *t, bool white) {
    if (life_expired(t)) {
        thing_mark_delete(t);
        return;
    }
    g_pos_scratch = *thing_pos(t);
    t->speed_cur = (int16_t)(t->speed_cur - 4);
    if (t->speed_cur < 0x40) t->speed_cur = 0x40;
    if (t->speed_cur > 0x80) t->speed_cur = 0x80;
    g_pos_scratch.z = (int16_t)(g_pos_scratch.z + t->speed_cur);
    int ground = (int16_t)terrain_height_at(thing_pos(t));
    if (g_pos_scratch.z < ground) g_pos_scratch.z = (int16_t)ground;
    t->aux++;
    if (t->aux < 0x10) {
        math_rotate_offset(&g_pos_scratch, t->yaw, 0, 0x1e);
        if (!(t->aux & 1)) t->sprite++;
    }
    if (t->health < 6) {
        if (!white || (int16_t)t->sprite > 0x43) t->sprite--;
    }
    // weather_update_nop_4b570(&g_pos_scratch): a lone ret
    thing_move_to(t, &g_pos_scratch);
}
void effect_white_smoke_update(Thing *t) { smoke_update(t, true); }
void effect_black_smoke_update(Thing *t) { smoke_update(t, false); }

// effect_type4_s4_update_242d0 (state 4), effect_update_shared_24c10 (states 0x14..0x16),
// effect_type22_s24_update_24ca0 (state 0x18), effect_type59_s61_update_284c0 (state 0x3d = type 0x38):
// a lone ret.
void effect_nop_update(Thing *) {}

// ---- earthquake, lava, meteor, eruption ---------------------------------------------------------------

// effect_earthquake_s15_update_242e0 (state 0xf): a crack that wanders (yaw +-0x2d per step, one
// cell per tick) and leaves a 10-tick crater with its own extents on every step. aux counts up on
// cells whose flag nibble is 0 (terrain_cell_flag_bit & 1: class-0 water without the animated-sea
// bit) and back down elsewhere; above 8 the crack gives up. The position is changed in place: the
// thing is not relinked into its new cell.
void effect_earthquake_update(Thing *t) {
    if (terrain_cell_flag_bit(thing_pos(t)) & 1) t->aux++;
    else if (t->aux > 0) t->aux--;
    if (life_expired(t) || t->aux > 8) {
        thing_mark_delete(t);
        return;
    }
    uint32_t r = rng_next(t) % 0x5bu;
    t->yaw = (uint16_t)((t->yaw + r - 0x2d) & 0x7ff);
    math_rotate_offset(thing_pos(t), t->yaw, 0, 0x100);
    Thing *c = thing_create(thing_pos(t), 10, 0xb);
    if (c) {
        c->ext_x = t->ext_x;
        c->ext_y = t->ext_y;
        c->ext_h = t->ext_h;
        c->health = 10;
        c->owner = t->owner;
    }
}

// effect_type16_s16_update_243b0 (state 0x10): a lava blob thrown by the erupting volcano. Flies
// with the velocity in home.x / home.y (clamped to +-0x50) under gravity 0x1c; where it lands it
// bounces at a quarter of the speed, becomes a splash on water or lights a triple-damage fire (unless
// one burns within 0x80 already), and it rolls downhill while it lies on the ground.
void effect_lava_blob_update(Thing *t) {
    if (life_expired(t)) {
        thing_mark_delete(t);
        return;
    }
    if (!(t->flags & 2)) t->flags |= 2;
    clamp_velocity(t, 0x50);
    g_pos_scratch = *thing_pos(t);
    g_pos_scratch.x = t->x + vel_x(t);
    g_pos_scratch.y = t->y + vel_y(t);
    g_pos_scratch.z = (int16_t)(g_pos_scratch.z + t->z_vel);
    t->z_vel = (int16_t)(t->z_vel - 0x1c);
    if (t->z_vel < -0x180) t->z_vel = -0x180;
    if (t->z_vel > 0x100) t->z_vel = 0x100;
    int16_t ground = (int16_t)terrain_height_at(&g_pos_scratch);
    if (ground > g_pos_scratch.z) {
        g_pos_scratch.z = ground;
        t->z_vel = (int16_t)-((int32_t)t->z_vel / 4);
        if (terrain_type_mask_at(thing_pos(t)) == 1) {
            Thing *c = thing_create(&g_pos_scratch, 10, 5);
            if (c) {
                c->owner = t->owner;
                thing_mark_delete(t);
            }
        } else {
            if (!thing_exists_near_pos(&g_pos_scratch, 10, 6)) {
                Thing *c = thing_create(&g_pos_scratch, 10, 6);
                if (c) {
                    c->owner = t->owner;
                    c->health = 0x1e;
                    c->damage = (uint16_t)(c->damage * 3);
                    t->aux = 0;
                }
            }
            if (t->z_vel <= 0x1c) t->z_vel = 0;
        }
    }
    t->aux++;
    thing_move_to(t, &g_pos_scratch);
    if (ground == g_pos_scratch.z) roll_on_slope(t);
}

// effect_meteor_s17_update_24630 (state 0x11): the impact of a meteor / lava bomb. A shock wave of
// explosions (type 0, no area damage of their own) on ring aux, damage / max_health dealt to
// everything within 0xc0 * aux; aux runs 0, 2, 4, .. modulo 11.
// The ring modulus is the only difference to Hidden Worlds' Fire Wall impact (effect_fire_pillar_update).
static void shock_ring_update(Thing *t, int modulus) {
    if (life_expired(t)) {
        thing_mark_delete(t);
        return;
    }
    if (!(t->flags & 2)) {
        t->flags |= 0x10002u;
        sound_request(snd_index(t), -1, 0x1e);
    }
    thing_set_extents(t, ((int32_t)t->aux * 3 * 256) / 4, 0x200);
    // idiv by max_health (0 would fault in the original; the constructor sets it)
    int32_t per_tick = t->max_health != 0 ? (int32_t)t->damage / t->max_health : 0;
    thing_area_damage(t, 0, (unsigned)per_tick & 0xffff);
    SpiralSearch s;
    int dx, dy;
    if (spiral_search_begin(&s, (int16_t)t->aux, (int16_t)t->aux)) {
        rng_next(t);                                   // one draw that is not used
        while (spiral_search_next(&s, &dx, &dy) == 1) {
            Pos p = scatter_pos(t, dx, dy, 0xa0);
            Thing *c = thing_create(&p, 10, 0);
            if (!c) continue;
            c->owner = t->owner;
            c->yaw = t->yaw;
            c->flags |= 0x10080u;
            thing_set_extents(c, 0x200, 0x200);
            c->aux = 0;
        }
    }
    t->aux = (int16_t)((int16_t)(t->aux + 2) % modulus);
}

void effect_meteor_update(Thing *t) { shock_ring_update(t, 0xb); }

// effect_type18_s18_update_24810 (state 0x12): the crater a volcano leaves, which keeps erupting.
// aux is its clock: at 0 it registers as *the* erupting volcano (GameState+0x24; the previous one is
// sent to sleep with aux = 0xfa), gets a smoke column (effect 0x13, GameState+0x26) and fires one
// projectile that lands as a meteor impact 0x600 away; while aux < 0x80 it throws a lava blob (effect
// 0x10) on a fifth of the ticks that are not multiples of 16. At 0x7f it ends (deleted when the
// last roll succeeds, dormant otherwise), and a dormant one starts over with chance 1/100 per tick
// once aux is above 2500 and no other volcano is erupting. It dies when the ground under it moved.
void effect_eruption_update(Thing *t) {
    if (t->aux > 0x9c4 && rng_next(t) % 0x64u == 0 && g_state->volcano_thing == 0) {
        int16_t old_z = t->z;
        t->z = (int16_t)terrain_height_at(thing_pos(t));
        if (old_z != t->z) {
            thing_mark_delete(t);
            return;
        }
        t->aux = 0;
    }
    bool act = false;
    if (t->aux < 0x80 && (t->aux & 0xf) != 0) act = rng_next(t) % 5u == 0;
    if (!act && t->aux != 0) {
        t->aux++;
        return;
    }
    int16_t old_z = t->z;
    t->z = (int16_t)terrain_height_at(thing_pos(t));
    if (old_z != t->z) {
        thing_mark_delete(t);
        return;
    }
    if (t->aux == 0) {
        uint16_t prev = g_state->volcano_thing;
        if (prev != 0) thing_ref(prev)->aux = 0xfa;
        g_state->volcano_thing = (uint16_t)thing_index(t);
        Thing *smoke = thing_create(thing_pos(t), 10, 0x13);
        if (smoke) {
            smoke->owner = t->owner;
            uint16_t old = g_state->volcano_smoke;
            if (old != 0) thing_mark_delete(thing_ref(old));
            g_state->volcano_smoke = (uint16_t)thing_index(smoke);
        }
    }
    Thing *blob = thing_create(thing_pos(t), 10, 0x10);
    if (blob) {
        blob->owner = t->owner;
        blob->rng = rng_next(t);
    }
    t->yaw = (uint16_t)(t->yaw + 0x500);               // add byte [t+0x1f], 5
    if (t->aux == 0) {
        Thing *p = thing_create(thing_pos(t), 9, 0);
        if (p) {
            p->owner = t->owner;
            p->pitch = 0xfe7e;
            p->impact_cls = 10;
            p->impact_type = 0x11;
            p->health = 1;
            p->yaw = (uint16_t)(t->yaw & 0x7ff);
            p->home = *thing_pos(t);
            math_rotate_offset(&p->home, p->yaw, 0, 0x600);
            p->home.z = (int16_t)terrain_height_at(&p->home);
        }
    }
    if (t->aux >= 0x7f) {
        thing_mark_delete(t);
        g_state->volcano_thing = 0;
    }
    t->aux++;
}

// effect_type19_s19_update_24a90 (state 0x13): the smoke column over the erupting volcano. On odd
// health ticks about half of the centre-quad cells puff four white smoke clouds (yaws 0x000 / 0x100
// + k * 0x200); deals its damage every tick, the last one included.
void effect_volcano_smoke_update(Thing *t) {
    if (life_expired(t)) {
        thing_mark_delete(t);
    } else {
        t->aux = 0;
        SpiralSearch s;
        int dx, dy;
        if (spiral_search_begin(&s, 0, (int16_t)t->aux)) {
            while (spiral_search_next(&s, &dx, &dy) == 1) {
                uint32_t r = rng_next(t) % 0x9du;
                if ((int32_t)(r / 0x4fu) * 2 - 1 <= 0) continue;
                Pos p = scatter_pos(t, dx, dy, 0xc0);
                if (!(t->health & 1)) continue;
                uint16_t yaw = (uint16_t)(((t->health / 2) & 1) << 8);
                for (; yaw < 0x800; yaw = (uint16_t)(yaw + 0x200)) {
                    Thing *c = thing_create(&p, 10, 0xd);
                    if (c) {
                        c->owner = t->owner;
                        c->yaw = yaw;
                    }
                }
            }
        }
        t->z = (int16_t)terrain_height_at(thing_pos(t));
    }
    thing_area_damage(t, 0, t->damage);
}

// effect_type21_s23_update_24c20 (state 0x17 = type 0x17, the Lightning strike a bolt leaves where it
// hits): one burst of damage with sound 0x18, then one more tick of life.
void effect_lightning_update(Thing *t) {
    t->aux++;
    if (life_expired(t)) {
        thing_mark_delete(t);
        return;
    }
    if (!(t->flags & 2)) {
        thing_area_damage(t, 0, t->damage);
        sound_request(snd_index(t), -1, 0x18);
        t->health = 1;
        t->flags |= 2;
    }
}

// effect_lightning_s25_update_24cb0 (state 0x19 = type 0x19, "Steal mana" in the level editor's model
// list; the "lightning" of the table name is the label of model 23 shifted by two): animated, writes
// damage slot 3 (amount = its damage, 2000) once.
void effect_steal_mana_update(Thing *t) {
    t->aux++;
    if (life_expired(t)) {
        thing_mark_delete(t);
        return;
    }
    thing_anim_advance(t);
    if (!(t->flags & 2)) {
        t->flags |= 2;
        thing_area_damage(t, 3, t->damage);
    }
}

// effect_rain_of_fire_s26_update_24d10 (state 0x1a = type 0x1a; "Rain of fire" is model 24, whose
// state 0x18 is a lone ret): animated, writes damage slot 4 every tick.
void effect_type26_update(Thing *t) {
    t->aux++;
    if (life_expired(t)) {
        thing_mark_delete(t);
        return;
    }
    thing_anim_advance(t);
    thing_area_damage(t, 4, t->damage);
}

// ---- teleport, orbiters, storm, skeleton army --------------------------------------------------------

// effect_teleport_s36_update_253b0 (state 0x24): every player thing that touches the gate and looks
// at it (within 0xaa of the direction to it) is moved to home (+0x96) at the clearance height of
// the gate's MoveDesc. A gate with a lifetime (health > 0 at creation) closes when it reaches 0.
void effect_teleport_update(Thing *t) {
    if (!(t->flags & 2)) {
        t->flags |= 2;
        sound_request(snd_index(t), -1, 0x15);
    }
    if (t->health > 0) {
        t->health--;
        if (t->health == 0) {
            thing_mark_delete(t);
            sound_request(snd_index(t), -1, 0x14);
            return;
        }
    }
    for (uint16_t i = 0; i < (uint16_t)g_state->player_count && i < 8; i++) {
        Thing *p = thing_ref(g_state->players[i].thing);
        if (!thing_collide(t, p)) continue;
        int to_gate = pos_angle_to(thing_pos(p), thing_pos(t)) & 0xffff;
        if ((uint16_t)angle_diff(p->yaw, to_gate) >= 0xaa) continue;
        const MoveDesc *desc = mc_move_desc(t->desc);
        t->home.z = (int16_t)(terrain_height_at(&t->home) + desc->clear_hi);
        sound_request(snd_index(t), -1, 0x16);
        thing_move_to(p, &t->home);
        player_set_palette_effect(p, 6);
    }
    t->z = (int16_t)terrain_height_at(thing_pos(t));
}

// effect_type35_s37_update_25550 (state 0x25, type 0x23): circles its caster (+0x2a) at distance
// speed_cur, 0x100 above the ground, 0x2d per tick; gone when the caster's health is exactly 1.
void effect_orbiter_update(Thing *t) {
    if (t->caster == 0) return;
    Thing *c = thing_ref(t->caster);
    if (c->health == 1) {
        thing_mark_delete(t);
        return;
    }
    g_pos_scratch = *thing_pos(c);
    math_rotate_offset(&g_pos_scratch, t->yaw, t->pitch, (int16_t)t->speed_cur);
    t->yaw = (uint16_t)((t->yaw + 0x2d) & 0x7ff);
    g_pos_scratch.z = (int16_t)(terrain_height_at(&g_pos_scratch) + 0x100);
    thing_move_to(t, &g_pos_scratch);
}

// effect_type33_s35_update_25630 (state 0x23, type 0x21): effect_age_tick plus the first-tick flag.
void effect_type33_update(Thing *t) {
    t->aux++;
    if (life_expired(t)) {
        thing_mark_delete(t);
        return;
    }
    if (!(t->flags & 2)) t->flags |= 2;
}

// effect_type38_s40_update_25670 (state 0x28, type 0x26): a storm cloud. Climbs 0x40 per tick to 0x400
// above the ground; once there it fires two things of its impact class / type (+0x44 / +0x45) per
// tick in opposite random directions, pitch 0x38, with a third of their normal life, its own damage
// and a lightning strike (10 / 0x17) as their impact.
void effect_storm_update(Thing *t) {
    bool moved = false;
    int ground = terrain_height_at(thing_pos(t));
    if ((int16_t)t->z < (int16_t)ground + 0x400) {
        t->z = (int16_t)(t->z + 0x40);
        moved = true;
    }
    if ((int16_t)t->z > (int16_t)ground + 0x400) {
        t->z = (int16_t)((uint16_t)ground + 0x400);
        moved = true;
    }
    if (moved) return;
    if (life_expired(t)) {
        thing_mark_delete(t);
        return;
    }
    uint32_t r = rng_next(t);
    t->pitch = 0x38;
    t->yaw = (uint16_t)(r & 0x7ff);
    Thing *last = nullptr;
    for (int i = 0; i < 2; i++) {
        t->yaw = (uint16_t)((t->yaw + 0x400) & 0x7ff);
        // the scratch position (raised by ext_z0) is prepared but the constructor gets t's own position
        g_pos_scratch = *thing_pos(t);
        g_pos_scratch.z = (int16_t)(g_pos_scratch.z + t->ext_z0);
        Thing *c = thing_create(thing_pos(t), (int8_t)t->impact_cls, (int8_t)t->impact_type);
        last = c;
        if (c) {
            c->owner = t->owner;
            c->health = c->health / 3;
            c->yaw = t->yaw;
            c->pitch = t->pitch;
            c->impact_cls = 10;
            c->impact_type = 0x17;
            c->damage = t->damage;
        }
    }
    // (the original passes a wild index when the second create failed)
    sound_request(last ? snd_index(last) : 0, -1, 0x17);
}

// effect_type36_s38_update_257e0 (state 0x26, type 0x24): raises up to 8 skeletons (creature type 9)
// in a circle of radius 0x200, limited by the free pool slots and by 64 skeletons per owner (+0x90).
// The mana each gets is the *remainder* of the 10000 budget by the per-skeleton share, as written.
void effect_skeleton_army_update(Thing *t) {
    t->damage = 0x2710;
    int16_t n = (int16_t)thing_free_count();
    if (n < 0) n = 0;
    if (n > 8) n = 8;
    uint32_t owned = 0;
    int guard = 0;
    for (uint32_t i = g_cfg->creature_lists[9]; i != 0 && i < (uint32_t)thing_pool_slots() && guard < thing_pool_slots(); guard++) {
        const Thing *p = thing_at(i);
        if ((int32_t)p->mana_owner == (int32_t)(int16_t)t->owner) owned++;
        i = p->next;
    }
    if (n < 0) n = 0;
    if ((int32_t)n > 0x40 - (int32_t)(owned & 0xffff)) n = (int16_t)(0x40 - owned);
    if (n > 0) {
        int32_t budget = t->damage;                    // 16-bit, read signed below
        int32_t share = (int32_t)(int16_t)budget / n;
        int32_t step = 0x800 / n;
        int32_t angle = 0;
        do {
            g_pos_scratch = *thing_pos(t);
            math_rotate_offset(&g_pos_scratch, angle & 0xffff, 0, 0x200);
            Thing *c = thing_create(&g_pos_scratch, 5, 9);
            if (c) {
                c->mana = (int16_t)share != 0 ? (int32_t)(int16_t)budget % (int32_t)(int16_t)share : 0;
                c->mana_owner = t->owner;
                uint16_t a = (uint16_t)((angle + 0x400) & 0x7ff);
                c->target_yaw = a;
                c->yaw = a;
                budget -= share;
            }
            n--;
            angle += step;
        } while (n > 0);
    }
    thing_mark_delete(t);
}

// ---- mana ---------------------------------------------------------------------------------------------

// effect_mana_ball_update_25980 (state 0x29, type 0x27).
//  1. A claim (damage slot 1, written by the Possession spell) changes the owner.
//  2. A pull (damage slot 4, written by the magnet 28270) adds 4 units of velocity towards the puller.
//  3. Flag 0x40 = being collected: it homes on its target (+0x92, must be a balloon: class 3 type 3)
//     at 0x10 per tick, and once within 0x10 sits on it and matches its height.
//  4. Otherwise, while awake (timer_a != 0): ballistic flight with the velocity in home.x / home.y
//     (clamped to +-0x40), gravity 0x10, quarter bounce; on the ground it merges with a touching ball
//     and rolls downhill with friction 250 / 256; finally the sprite follows mana and owner.
void effect_mana_ball_update(Thing *t) {
    DamageSlot &claim = t->damage_slots[1];
    if (claim.attacker != 0) {
        if (!same_side(claim.attacker, t->mana_owner)) {          // round 12: an ally's ball stays his
            t->mana_owner = claim.attacker;
            sound_request((int16_t)claim.attacker, -1, 4);
            t->flags &= ~0x40u;
        }
        claim.attacker = 0;
        claim.amount = 0;
    }
    DamageSlot &pull = t->damage_slots[4];
    if (pull.attacker != 0) {
        t->yaw = (uint16_t)pos_angle_to(thing_pos(t), thing_pos(thing_ref(pull.attacker)));
        g_pos_scratch.x = 0;
        g_pos_scratch.y = 0;
        g_pos_scratch.z = 0;
        math_rotate_offset(&g_pos_scratch, t->yaw, 0, 4);
        t->home.x = (uint16_t)(t->home.x + g_pos_scratch.x);
        t->home.y = (uint16_t)(t->home.y + g_pos_scratch.y);
        pull.attacker = 0;
    }
    if (t->flags & 0x40) {
        Thing *target = thing_ref(t->target);
        if (!(target->cls == 3 && target->type == 3)) {
            t->flags &= ~0x40u;
            return;
        }
        t->z_vel = 0x80;
        t->yaw = (uint16_t)pos_angle_to(thing_pos(t), thing_pos(target));
        int dist = pos_dist_xy(thing_pos(t), thing_pos(target));
        if (dist > 0x400) {
            t->flags &= ~0x40u;
            return;
        }
        g_pos_scratch = *thing_pos(t);
        if (dist < 0x10) {
            g_pos_scratch.x = target->x;
            g_pos_scratch.y = target->y;
            if (g_pos_scratch.z < target->z)                    g_pos_scratch.z = (int16_t)(g_pos_scratch.z + 0x20);
            else if ((int32_t)g_pos_scratch.z > (int32_t)target->z + 0x200) g_pos_scratch.z = (int16_t)(g_pos_scratch.z - 0x20);
        } else {
            math_rotate_offset(&g_pos_scratch, t->yaw, 0, 0x10);
        }
        int16_t ground = (int16_t)terrain_height_at(&g_pos_scratch);
        if (ground > g_pos_scratch.z) g_pos_scratch.z = ground;
        thing_move_to(t, &g_pos_scratch);
        return;
    }
    if (t->timer_a == 0) return;
    clamp_velocity(t, 0x40);
    g_pos_scratch = *thing_pos(t);
    g_pos_scratch.x = t->x + vel_x(t);
    g_pos_scratch.y = t->y + vel_y(t);
    g_pos_scratch.z = (int16_t)(g_pos_scratch.z + t->z_vel);
    t->z_vel = (int16_t)(t->z_vel - 0x10);
    if (t->z_vel < -0x80) t->z_vel = -0x80;
    int16_t ground = (int16_t)terrain_height_at(&g_pos_scratch);
    if (ground > g_pos_scratch.z) {
        t->z_vel = (int16_t)-((int32_t)t->z_vel / 4);
        if (t->z_vel <= 0x10) t->z_vel = 0;
        g_pos_scratch.z = ground;
    }
    thing_move_to(t, &g_pos_scratch);
    if (ground == g_pos_scratch.z) {
        Thing *other = thing_find_collision_other_owner(t);
        if (other) mana_ball_merge(t, other);
        roll_on_slope(t);
    }
    mana_ball_update_sprite(t);
}

// effect_type40_s42_update_25f10 (state 0x2a, type 0x28): a mana hoard marker. It sits on the ground
// and owns mana balls through their +0x90; when a player claims it (damage slot 1) while nobody owns
// it, every thing whose mana owner it is passes to that player, and it is deleted.
void effect_mana_hoard_update(Thing *t) {
    t->z = (int16_t)terrain_height_at(thing_pos(t));
    DamageSlot &claim = t->damage_slots[1];
    if (claim.attacker == 0) return;
    Thing *claimer = thing_ref(claim.attacker);
    if (claimer->cls == 3 && t->mana_owner == 0) {
        uint16_t self = thing_index(t), to = thing_index(claimer);
        for (int i = 1; i < thing_pool_slots(); i++) {
            Thing *p = thing_at(i);
            if (p->mana_owner == self) p->mana_owner = to;
        }
    }
    claim.attacker = 0;
    thing_mark_delete(t);
}

}  // namespace

// thing_drop_mana_ball_25fe0
void thing_drop_mana_ball(Thing *t) {
    if (t->mana <= 0) return;
    // (mana / 2000 clamped to 1.. is computed and dropped: the ball count of an earlier version)
    rng_next(t);
    Thing *c = thing_create(thing_pos(t), 10, 0x27);
    if (c) {
        c->mana = t->mana;
        c->mana_owner = t->mana_owner;
        uint32_t r = rng_next(c) % 0x71u;
        c->yaw = (uint16_t)((t->yaw + r - 0x38) & 0x7ff);
        r = rng_next(c) % 0x30u;
        c->home.x = 0;
        c->home.y = 0;
        c->speed_cur = (int16_t)(r + 0x10);
        int32_t above = (int16_t)t->z - (int16_t)terrain_height_at(thing_pos(t));
        c->z_vel = (int16_t)((0x400 - above) / 8);
        math_rotate_offset(&c->home, c->yaw, 0, (int16_t)c->speed_cur);
    }
    t->mana_owner = 0;
}

// mana_ball_merge_26120
void mana_ball_merge(Thing *a, Thing *b) {
    a->mana += b->mana;                                // every path adds first
    if (a->mana_owner == 0) {
        if (b->mana_owner != 0) a->mana_owner = b->mana_owner;
    } else if (b->mana_owner != 0) {
        const Thing *oa = thing_ref(a->mana_owner), *ob = thing_ref(b->mana_owner);
        if (oa->cls == 10 && ob->cls == 10) {
            // two hoards: a keeps its owner
        } else if (oa->cls == 10) {
            a->mana_owner = thing_index(ob);
        } else if (ob->cls == 10) {
            a->mana_owner = thing_index(oa);
        } else if (oa != ob) {
            a->mana_owner = thing_index(oa->mana_total > ob->mana_total ? oa : ob);
        }
    }
    thing_free(b);
}

namespace {

// ---- castle building ----------------------------------------------------------------------------------

// effect_type41_s43_update_26b50 (state 0x2b, type 0x29): levels the footprint rectangle of a castle
// to the average height of its surroundings in 10 steps (the whole rectangle moves by the same
// amount), then waits 10 ticks and hands the castle back (cast_ticks = 2, home.z = the new base).
// cast_ticks (+0x30) = current base height byte, damage (+0x2c) = target height byte, aux = steps
// left (> 0) or the wait counter (-10..-1). Built-on cells are parked with flag 8 for the wait.
void effect_castle_level_ground_update(Thing *t) {
    const int cx = thing_cell_x(t), cy = thing_cell_y(t);
    uint32_t w, h;
    footprint_dims(thing_footprint(t), &w, &h);
    const int x0 = world_wrap_cell(cx - (int)(w >> 1)), y0 = world_wrap_cell(cy - (int)(h >> 1));
    if (!(t->flags & 2)) {
        t->flags |= 2;
        t->aux = 10;
        t->cast_ticks = (int16_t)((int16_t)t->z >> 5);
        t->damage = (uint16_t)terrain_height_avg(world_wrap_cell(x0 - 1), world_wrap_cell(y0 - 1), (int)(h + 2), (int)(w + 2));
        if (t->damage > 0xdc) t->damage = 0xdc;
        if ((int32_t)t->cast_ticks == (int32_t)t->damage) t->aux = 0;
        return;
    }
    Thing *castle = thing_ref(t->caster);
    if (castle->duration != 0 || t->aux == 0) {
        castle->cast_ticks = 2;
        castle->home.z = (int16_t)((uint16_t)t->cast_ticks << 5);
        terrain_smooth_castle_border(cx, cy, (int)((h >> 1) & 0xffff), (int)((w >> 1) & 0xffff), 3);
        thing_mark_delete(t);
        return;
    }
    int32_t delta = ((int32_t)t->damage - (int32_t)t->cast_ticks) / (int32_t)t->aux;
    t->cast_ticks = (int16_t)(t->cast_ticks + delta);
    auto for_rect = [&](auto &&fn) {
        for (uint32_t row = 0; row < h; row++)
            for (uint32_t col = 0; col < w; col++) fn(cell_xy(x0 + (int)col, y0 + (int)row));
    };
    if (t->aux == 1) {
        for_rect([&](unsigned c) {
            uint8_t f = g_map_flags[c];
            if (f & 0x80) g_map_flags[c] = (uint8_t)((f & 0x77) | 8);
            g_map_height[c] = (uint8_t)(g_map_height[c] + delta);
        });
        t->aux = -10;
    } else if (t->aux == -1) {
        for_rect([&](unsigned c) {
            uint8_t f = g_map_flags[c];
            if (f & 8) g_map_flags[c] = (uint8_t)((f | 0x80) & 0xf7);
        });
        t->aux++;
    } else if (t->aux > 0) {
        for_rect([&](unsigned c) { g_map_height[c] = (uint8_t)(g_map_height[c] + delta); });
        t->aux--;
    } else {
        t->aux++;
    }
}

// effect_castle_raise_terrain_s44_26f10 (state 0x2c, type 0x2a): grows the castle of size
// castle_size out of the ground in 18 steps. Every step re-derives, for each footprint cell, the
// difference between its target height (the footprints of sizes 1..castle_size laid over each other,
// see level_features.cpp for the instruction bytes) and its present height, and adds difference /
// steps_left; textures are painted on every 7th step and on the last one. Paused while the castle
// (+0x2a) has its hit timer running. After the last step it waits 1 tick (25 when spell_flags is
// set), restores the built-on flags it parked in bit 8 and hands the castle back (cast_ticks = 2).
//
// The sub-footprints are centred with their two dimensions swapped (rows = table byte +4, x offset
// from byte +5), as in the original; the shipped footprints are square.
void effect_castle_raise_terrain_update(Thing *t) {
    if (!(t->flags & 2)) {
        t->aux = 0x13;
        t->flags |= 2;
    }
    const int cx = thing_cell_x(t), cy = thing_cell_y(t);
    uint32_t W, H;
    footprint_dims(thing_footprint(t), &W, &H);
    const int x0 = world_wrap_cell(cx - (int)(W >> 1)), y0 = world_wrap_cell(cy - (int)(H >> 1));
    if (t->aux <= 0) {
        t->aux++;
        if (t->aux != 0) return;
        for (uint32_t row = 0; row < H; row++)
            for (uint32_t col = 0; col < W; col++) {
                unsigned c = cell_xy(x0 + (int)col, y0 + (int)row);
                uint8_t f = g_map_flags[c];
                if (f & 8) g_map_flags[c] = (uint8_t)((f | 0x80) & 0xf7);
            }
        // 1995 (CD 0x28cc4): step 5, so the castle then levels its ground (effect 0x29) first
        thing_ref(t->caster)->cast_ticks = engine1995() ? 5 : 2;
        thing_mark_delete(t);
        return;
    }
    t->aux--;
    if (t->aux == 0) {
        t->aux = t->spell_flags != 0 ? (int16_t)-0x19 : (int16_t)-1;
        return;
    }
    if (thing_ref(t->caster)->duration != 0) return;

    int32_t base = (int16_t)t->z >> 5;
    // DAT_000adf68 (the renderer's work buffer) holds the i16 differences in the original.
    static std::vector<int16_t> delta;
    const int32_t cells = (int32_t)(W * H);
    delta.assign((size_t)cells, 0);
    const uint8_t half_w = (uint8_t)(W >> 1), half_h = (uint8_t)(H >> 1);
    const bool paint = t->aux % 7 == 0 || t->aux == 1;
    for (int size = 1; size <= (int)(int8_t)t->castle_size; size++) {
        const CastleFootprint *fp = castle_footprint((unsigned)size);
        uint32_t a, b;
        footprint_dims(fp, &a, &b);
        const uint32_t half_a = a >> 1, half_b = b >> 1;
        const int xs = world_wrap_cell(cx - (int)half_b), ys = world_wrap_cell(cy - (int)half_a);
        const int32_t col_ofs = (int32_t)half_w - (int32_t)half_b;
        int32_t row_ofs = ((int32_t)half_h - (int32_t)half_a) * (int32_t)W;
        int32_t idx = col_ofs + row_ofs;
        int x = xs, y = ys;                            // wrapped by cell_xy (the original's uint8_t)
        uint32_t rows = a, i = 0;
        while (rows != 0 && i < fp->map_len) {
            int8_t c = (int8_t)fp->map[i++];
            if (c == 0) {
                y++;
                x = xs;
                row_ofs += (int32_t)W;
                rows--;
                idx = col_ofs + row_ofs;
            } else if (c < 0) {
                x = world_wrap_cell(x - c);
                idx -= c;
            } else {
                for (int n = c; n != 0 && i < fp->map_len; n--) {
                    unsigned cell = cell_xy(x, y);
                    if (t->flags & 0x10000) cell_kill_things(cell, (unsigned)(int)(int16_t)t->owner);
                    uint8_t ins = fp->map[i++];
                    // (a sub-footprint that does not fit the top one would write outside the buffer)
                    int16_t *d = (idx >= 0 && idx < cells) ? &delta[(size_t)idx] : nullptr;
                    if (ins >= 0xf) {
                        unsigned low = ins % 16u;
                        if (low != 0 && d) *d = (int16_t)(base + (int32_t)(((low - 1) & 0xff) << 2) - g_map_height[cell]);
                    } else if (ins > 6) {
                        if (d) *d = (int16_t)(base - g_map_height[cell]);
                    }
                    if (paint) {
                        unsigned nib = ins >> 4;
                        int kind;
                        unsigned dl = 0;               // DL at the call: the last idiv remainder
                        if (nib == 0) {
                            dl = ins % 7u;
                            kind = dl == 0 ? -1 : (int)dl - 1;
                        } else if (nib <= 2) {
                            kind = (int)nib + 7;
                        } else if (nib == 3) {
                            dl = (ins % 16u) % 3u;
                            kind = (int)(((ins % 16u) / 3u + 0xa) & 0xff);
                        } else {
                            kind = (int)((nib + 0xb) & 0xff);
                        }
                        if (kind >= 0) terrain_paint_cell(cell, (unsigned)kind, dl, ins);   // CL = the instruction byte
                    }
                    x++;
                    idx++;
                }
            }
        }
    }
    int32_t idx = 0;
    for (uint32_t row = 0; row < H; row++) {
        for (uint32_t col = 0; col < W; col++, idx++) {
            unsigned cell = cell_xy(x0 + (int)col, y0 + (int)row);
            int16_t d = delta[(size_t)idx];
            if (d != 0) {
                if (g_map_height[cell] == 0) {
                    g_map_flags[cell] = (uint8_t)((g_map_flags[cell] & 0xf8) | 1);
                    terrain_retexture_rect_force(cell, cell);
                }
                g_map_height[cell] = (uint8_t)(g_map_height[cell] + (int32_t)d / (int32_t)t->aux);
                if (t->aux == 1) {
                    uint8_t f = g_map_flags[cell];
                    if (f & 0x80) g_map_flags[cell] = (uint8_t)((f & 0x77) | 8);
                }
            }
            if (t->aux == 2) g_map_flags[cell] &= 0xf7;
        }
    }
}

// effect_type43_s45_update_27d20 (state 0x2d, type 0x2b): where a castle seed lands. When it touches
// its owner's castle it files an upgrade request with it (damage slot 5: amount 10, attacker = the
// owner); otherwise the owner's Castle spell is reset. One tick only.
void effect_castle_seed_update(Thing *t) {
    t->aux++;
    bool dead = life_expired(t);
    if (!dead) {
        thing_anim_advance(t);
        if (!(t->flags & 2)) {
            t->flags |= 2;
            // (a sign-extended 16-bit height is never above 0xe600: dead test, kept)
            if ((int32_t)(int16_t)terrain_height_at(thing_pos(t)) > 0xe600) castle_spell_reset_charge(t, 0);
            Thing *castle = thing_ref(player_block(thing_ref((unsigned)(int)(int16_t)t->owner))->castle);
            if (thing_collide(t, castle)) {
                castle->damage_slots[5].attacker = t->owner;
                castle->damage_slots[5].amount = 10;
            } else {
                castle_spell_reset_charge(t, 0);
            }
        }
    }
    thing_mark_delete(t);
}

// effect_type44_s46_update_27e90_disabled (state 0x2e, type 0x2c; the Table A record is disabled): a
// boulder that rolls downhill (velocity in home.x / home.y, clamped to +-0x80, no friction) and
// damages what it touches.
void effect_boulder_update(Thing *t) {
    int16_t slope[2];
    terrain_slope_vector(thing_pos(t), slope);
    g_pos_scratch.x = (uint16_t)slope[0];
    g_pos_scratch.y = (uint16_t)slope[1];
    t->home.x = (uint16_t)(t->home.x + g_pos_scratch.x);
    t->home.y = (uint16_t)(t->home.y + g_pos_scratch.y);
    clamp_velocity(t, 0x80);
    g_pos_scratch = *thing_pos(t);
    g_pos_scratch.x = g_pos_scratch.x + vel_x(t);
    g_pos_scratch.y = g_pos_scratch.y + vel_y(t);
    g_pos_scratch.z = (int16_t)terrain_height_at(&g_pos_scratch);
    thing_move_to(t, &g_pos_scratch);
    thing_area_damage(t, 0, t->damage);
}

// ---- spell area effects -----------------------------------------------------------------------------

// effect_type54_s56_update_27ff0 (state 0x38 = type 0x34, the Crab egg; the type numbers in the table
// names of 27ff0..284c0 are two too high): an egg on the ground; when aux (600) runs out it hatches
// (state 0x39).
void effect_egg_update(Thing *t) {
    t->z = (int16_t)terrain_height_at(thing_pos(t));
    if (life_expired(t)) {
        thing_mark_delete(t);
        return;
    }
    int16_t a = t->aux;
    t->aux = (int16_t)(a - 1);
    if (a == 0) {
        thing_set_state(t, 0x39);
        t->max_health = 0x1388;
    }
}

// effect_type55_s57_update_28050 (state 0x39, the hatching Crab egg): creates a creature of type 5
// (a crab, counted in the level's creature total) inside a big explosion owned by it.
void effect_hatch_update(Thing *t) {
    t->z = (int16_t)terrain_height_at(thing_pos(t));
    g_pos_scratch = *thing_pos(t);
    Thing *c = thing_create(&g_pos_scratch, 5, 5);
    if (c) {
        g_state->creature_count++;
        Thing *e = thing_create(&g_pos_scratch, 10, 1);
        if (e) e->owner = c->owner;
    }
    thing_mark_delete(t);
}

// effect_type56_s58_update_280d0 (state 0x3a = type 0x35): a pillar of fire. For 15 ticks it lights
// fires (type 6, no smoke, no area damage of their own, sprite + 7) on the centre quad and ring 1,
// each one 0x80 higher than the tick before; the first tick's fires last 14 ticks, the others 1.
// damage / max_health is dealt to everything in a 0x200 x 0x800 box every tick.
// Hidden Worlds (HIDDEN.EXE 0x29780): the handler is effect_meteor_update's body with the ring
// modulus 7 (life 6 from effect_create_type53: rings 0, 2, 4, 6, 1, 3, 5).
void effect_fire_pillar_update(Thing *t) {
    if (world_hidden()) { shock_ring_update(t, 7); return; }
    bool dead = life_expired(t);
    if (!dead) {
        thing_set_extents(t, 0x200, 0x800);
        int32_t per_tick = t->max_health != 0 ? (int32_t)t->damage / t->max_health : 0;
        thing_area_damage(t, 0, (unsigned)per_tick & 0xffff);
        SpiralSearch s;
        int dx, dy;
        if (spiral_search_begin(&s, 0, 1)) {
            rng_next(t);                               // one draw that is not used
            while (spiral_search_next(&s, &dx, &dy) == 1) {
                Pos p = scatter_pos(t, dx, dy, 0x70);
                Thing *c = thing_create(&p, 10, 6);
                if (!c) continue;
                c->owner = t->owner;
                c->damage = t->damage;
                c->health = t->aux == 0 ? 0xe : 1;
                c->flags |= 0x10080u;
                c->sprite = (uint16_t)(c->sprite + 7);
                c->aux = 7;
                c->z_vel = (int16_t)(((int32_t)t->aux << 8) / 2);
            }
        }
        t->aux++;
        if (t->aux > 0xe) dead = true;
    }
    if (dead) thing_mark_delete(t);
}

// effect_type57_s59_update_28270 (state 0x3b = type 0x36): a mana magnet. Every mana ball within
// 0xe00 (squared distance below 0xc40000) gets a pull request (damage slot 4: amount 100, attacker =
// the magnet) which effect_mana_ball_update turns into velocity.
void effect_mana_magnet_update(Thing *t) {
    if (life_expired(t)) {
        thing_mark_delete(t);
        return;
    }
    uint16_t self = thing_index(t);
    int guard = 0;
    for (uint32_t i = g_cfg->mana_ball_list; i != 0 && i < (uint32_t)thing_pool_slots() && guard < thing_pool_slots(); guard++) {
        Thing *p = thing_at(i);
        if (p->type == 0x27 && pos_dist_sq_xy(thing_pos(t), thing_pos(p)) < 0xc40000) {
            p->damage_slots[4].amount = 0x64;
            p->damage_slots[4].attacker = self;
        }
        i = p->next;
    }
}

// effect_type58_s60_update_28320 (state 0x3c = type 0x37): the delayed blast. Counts aux (0x20) down with
// sound 0x2b, then within 0xa00 of it kills every scenery thing and creature of another owner
// outright, deals its damage to the players, flashes the caster's palette (effect 3) and is gone.
// (jump table 0x282f8 on class - 2: 2 / 5 kill, 3 damage, 9 / 10 only zero the blast's own health.)
void effect_blast_update(Thing *t) {
    if (t->aux != 0) {
        t->aux--;
        sound_request(snd_index(t), -1, 0x2b);
        return;
    }
    if (life_expired(t)) {
        thing_mark_delete(t);
        return;
    }
    for (int i = 1; i < thing_pool_slots(); i++) {
        Thing *p = thing_at(i);
        if (p->cls == 0 || same_side(p->owner, t->owner)) continue;
        switch (p->cls) {
        case 2: case 5:
            if ((uint32_t)pos_dist_xy(thing_pos(t), thing_pos(p)) < 0xa00) p->health = -1;
            break;
        case 3:
            if ((uint32_t)pos_dist_xy(thing_pos(t), thing_pos(p)) < 0xa00) thing_add_pending_damage(t, p, 0, t->damage);
            break;
        case 9: case 10:
            if ((uint32_t)pos_dist_xy(thing_pos(t), thing_pos(p)) < 0xa00) t->health = 0;
            break;
        default:
            break;
        }
    }
    sound_request(snd_index(t), -1, 0x2c);
    sound_request((int16_t)t->owner, -1, 0x2c);
    player_set_palette_effect(thing_ref((unsigned)(int)(int16_t)t->owner), 3);
    thing_mark_delete(t);
}

}  // namespace

void effects_register_handlers() {
    g_hook_thing_drop_mana_ball = thing_drop_mana_ball;
    // Table A, class 10 (state in the comment)
    thing_register_update(0x238b0, effect_explosion_update);            // 0
    thing_register_update(0x23a80, effect_big_explosion_update);        // 1
    thing_register_update(0x23c00, effect_lifetime_update);             // 2
    thing_register_update(0x23d40, effect_lifetime_update);             // 3
    thing_register_update(0x242d0, effect_nop_update);                  // 4
    thing_register_update(0x23d60, effect_splash_update);               // 5
    thing_register_update(0x23c20, effect_fire_update);                 // 6
    thing_register_update(0x240b0, effect_type12_update);               // 0xc
    thing_register_update(0x24100, effect_white_smoke_update);          // 0xd
    thing_register_update(0x241f0, effect_black_smoke_update);          // 0xe
    thing_register_update(0x242e0, effect_earthquake_update);           // 0xf
    thing_register_update(0x243b0, effect_lava_blob_update);            // 0x10
    thing_register_update(0x24630, effect_meteor_update);               // 0x11
    thing_register_update(0x24810, effect_eruption_update);             // 0x12
    thing_register_update(0x24a90, effect_volcano_smoke_update);        // 0x13
    thing_register_update(0x24c10, effect_nop_update);                  // 0x14, 0x15, 0x16
    thing_register_update(0x24c20, effect_lightning_update);            // 0x17
    thing_register_update(0x24ca0, effect_nop_update);                  // 0x18
    thing_register_update(0x24cb0, effect_steal_mana_update);           // 0x19
    thing_register_update(0x24d10, effect_type26_update);               // 0x1a
    thing_register_update(0x25630, effect_type33_update);               // 0x23
    thing_register_update(0x253b0, effect_teleport_update);             // 0x24
    thing_register_update(0x25550, effect_orbiter_update);              // 0x25
    thing_register_update(0x257e0, effect_skeleton_army_update);        // 0x26
    thing_register_update(0x25670, effect_storm_update);                // 0x28
    thing_register_update(0x25980, effect_mana_ball_update);            // 0x29
    thing_register_update(0x25f10, effect_mana_hoard_update);           // 0x2a
    thing_register_update(0x26b50, effect_castle_level_ground_update);  // 0x2b
    thing_register_update(0x26f10, effect_castle_raise_terrain_update); // 0x2c
    thing_register_update(0x27d20, effect_castle_seed_update);          // 0x2d
    thing_register_update(0x27e90, effect_boulder_update);              // 0x2e (record disabled)
    thing_register_update(0x27ff0, effect_egg_update);                  // 0x38
    thing_register_update(0x28050, effect_hatch_update);                // 0x39
    thing_register_update(0x280d0, effect_fire_pillar_update);          // 0x3a
    thing_register_update(0x28270, effect_mana_magnet_update);          // 0x3b
    thing_register_update(0x28320, effect_blast_update);                // 0x3c
    thing_register_update(0x284c0, effect_nop_update);                  // 0x3d
}
