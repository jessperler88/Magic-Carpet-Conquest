// Exact-layout game structures of carpet.exe (retail), transcribed from ghidra/names/carpet_types.txt
// and docs/ENGINE.md. Every offset is asserted below so translated code that was written against
// "state + 0x7463 + i*0xa4" style addressing keeps working on the typed structs.
//
// Port rule for pointer fields: the original stores 32-bit pointers (Thing*, list heads, descriptor
// pointers). On x64 those cannot keep their size, so every such field becomes a 32-bit *index*
// (thing index, descriptor index, byte offset into the GameState) at the same offset. They are
// marked "idx" in the comments. All multi-byte fields are little-endian, unaligned (pack 1).
#pragma once
#include <cstdint>
#include <cstddef>

#pragma pack(push, 1)

// Per-player command packet, 8 at GameState+0x7413.
struct CmdPacket {
    uint8_t cmd;        // 0x0
    uint8_t arg;        // 0x1
    uint8_t pad2;       // 0x2
    int8_t  steer_x;    // 0x3 mouse steer -127..127
    int8_t  steer_y;    // 0x4
    uint8_t bits;       // 0x5
    uint8_t pad6[4];    // 0x6..0x9
};
static_assert(sizeof(CmdPacket) == 0xa);

// Level-file THING_INIT record (9 x u16).
struct ThingInit {
    uint16_t cls, model, x, y, dis_id, swi_sz, swi_id, parent, child;
};
static_assert(sizeof(ThingInit) == 0x12);

// GEN_MAP header of a level file (12 int32). Names from levels/gam00088.dat (text save).
struct GenMap {
    int32_t unk0;    // 0x00
    int32_t seed;    // 0x04 -> g_rng16 / g_state->rng
    int32_t off;     // 0x08 -> fractal (terrain_fractal_fill arg 1)
    int32_t raise;   // 0x0c -> fractal arg 2
    int32_t gnarl;   // 0x10 -> fractal arg 3
    int32_t river;   // 0x14 -> terrain_carve_rivers count
    int32_t sourc;   // 0x18 -> terrain_carve_rivers min height
    int32_t snlin;   // 0x1c not used by terrain_build_303f0; Hidden Worlds: terrain_mark_snow height
    int32_t snflt;   // 0x20 -> terrain_classify_flat threshold
    int32_t bhlin;   // 0x24 -> terrain_mark_lowland / mark_interior arg 1
    int32_t bhflt;   // 0x28 -> terrain_mark_lowland / mark_interior arg 2
    int32_t rkste;   // 0x2c -> terrain_mark_steep
};
static_assert(sizeof(GenMap) == 0x30);

// A whole decompressed level file (0x979c bytes). Loaded verbatim at GameState+0x2f503.
struct LevelData {
    GenMap    gen;                 // 0x0000
    uint8_t   zeros[0x442 - 0x30]; // 0x0030
    ThingInit things[1999];        // 0x0442 .. 0x90d0 (the spawn loops stop at GameState+0x385d3)
    uint8_t   player_block[8][0xd8]; // 0x90d0 per-player level data (GameState+0x385d3 + p*0xd8; castle position at +4)
    uint16_t  win_percent;         // 0x9790 footer[0] (GameState+0x38c93)
    uint16_t  player_count;        // 0x9792 footer[1] (GameState+0x38c95)
    uint8_t   castle_level[8];     // 0x9794 footer[2..5] per player (GameState+0x38c97)
};
static_assert(sizeof(LevelData) == 0x979c);
static_assert(offsetof(LevelData, things) == 0x442);
static_assert(offsetof(LevelData, player_block) == 0x90d0);
static_assert(offsetof(LevelData, win_percent) == 0x9790);

// ---- World coordinates (Phase 4 round 11, docs/analysis/phase4_code_world.md section 7) -----------------
// The original keeps a position as u16 world units (cell = high byte), so its world is exactly 256 cells
// wide and every position store wraps at 65536. The port stores x / y as int32 (`WCoord`) so a world can be
// N cells wide (N a power of two, 256 .. 4096): every store wraps into 0 .. N*256-1 through
// g_world_unit_mask (world.h sets it at level start). With the original's 256 cells the mask is 0xffff and a
// WCoord behaves exactly like the original's u16 field: same value after every store, same promotion to int
// on every read. The original byte layout is kept as the "image" types below (PosImage, ThingImage,
// GameStateImage) for movies, save states, the per-tick references and the network checksum
// (mc_image.h converts).
inline uint32_t g_world_unit_mask = 0xffffu;   // world units - 1 (N * 256 - 1); world.h owns it

struct WCoord {
    int32_t v;
    WCoord() = default;
    WCoord(int32_t a) : v((int32_t)((uint32_t)a & g_world_unit_mask)) {}
    operator int32_t() const { return v; }
    WCoord &operator=(int32_t a) { v = (int32_t)((uint32_t)a & g_world_unit_mask); return *this; }
    WCoord &operator+=(int32_t a) { return *this = v + a; }
    WCoord &operator-=(int32_t a) { return *this = v - a; }
    WCoord &operator&=(int32_t a) { return *this = v & a; }
    WCoord &operator|=(int32_t a) { return *this = v | a; }
    WCoord &operator++() { return *this = v + 1; }
    WCoord &operator--() { return *this = v - 1; }
    int32_t operator++(int) { const int32_t o = v; *this = v + 1; return o; }
    int32_t operator--(int) { const int32_t o = v; *this = v - 1; return o; }
};
static_assert(sizeof(WCoord) == 4);

// World position as the original passes it around ({u16 x, u16 y, i16 z}; cell = high byte).
// Thing+0x48 has this layout, so `thing_pos(t)` can be handed to anything that takes a Pos*.
// `Pos` is the port's in-memory form (int32 x / y), `PosImage` the original's 6 bytes.
template <class C> struct PosT {
    C       x;
    C       y;
    int16_t z;
};
using Pos      = PosT<WCoord>;
using PosImage = PosT<uint16_t>;
static_assert(sizeof(Pos) == 10);
static_assert(sizeof(PosImage) == 6);

// Pending damage by type, 6 slots at Thing+0x5a.
struct DamageSlot {
    int32_t  amount;
    uint16_t attacker;  // thing index
};
static_assert(sizeof(DamageSlot) == 6);

// Movement / AI descriptor shared by creatures and projectiles (Thing.desc), table at 0x96a10.
struct MoveDesc {
    uint16_t id;            // 0x00
    uint16_t turn_min;      // 0x02 max yaw step per update (not a minimum; name kept)
    uint16_t unk4;          // 0x04
    uint16_t turn_max;      // 0x06 max pitch step per update
    uint16_t unk8;          // 0x08
    int16_t  clear_lo;      // 0x0a MAXIMUM height above ground (creature_check_terrain_102b0; name kept)
    int16_t  clear_hi;      // 0x0c MINIMUM height above ground
    int16_t  z_step;        // 0x0e
    uint16_t unk10;         // 0x10 max pitch down / max walkable slope
    uint16_t unk12;         // 0x12 max pitch up
    uint32_t terrain_mask;  // 0x14 allowed cell types
    uint16_t unk18;         // 0x18
    uint16_t think_period;  // 0x1a
    uint16_t sight_radius;  // 0x1c
    uint16_t fov;           // 0x1e
};
static_assert(sizeof(MoveDesc) == 0x20);

// 14-byte sprite descriptor, table at 0x97678 (285 entries).
struct SpriteDesc {
    uint16_t base_sprite;   // 0x0 index into the sprite cache (0x211 sprites)
    uint16_t unk2;          // 0x2
    uint16_t unk4;          // 0x4
    int16_t  half_xy;       // 0x6 -> Thing.ext_x / ext_y
    int16_t  half_z;        // 0x8 -> Thing.ext_h (world size)
    uint8_t  shade_group;   // 0xa indexes the pixel-mode tables; non-zero = no shadow
    uint8_t  load_priority; // 0xb
    uint8_t  draw_type;     // 0xc -> Thing.draw_type via DAT_00094344
    uint8_t  unkd;          // 0xd
};
static_assert(sizeof(SpriteDesc) == 0xe);

// The entity record (0xa4 bytes in the original). 1000 slots at GameState+0x7463; slot 0 is the scratch
// sentinel. `Thing` is the port's in-memory form (int32 x / y / home: 0xac bytes), `ThingImage` the
// original's 0xa4-byte record; the offsets in the comments are the original's.
template <class C> struct ThingT {
    uint32_t   next;            // 0x00 idx: next in per-tick class list (heads in Config)
    uint32_t   rng;             // 0x04 per-thing LCG seed x*0x24a1+0x24df
    int32_t    max_health;      // 0x08
    int32_t    health;          // 0x0c effects: lifetime; <0 dead
    uint32_t   flags;           // 0x10
    uint16_t   cell_next;       // 0x14 next thing index in the same cell
    uint16_t   cell_prev;       // 0x16
    uint16_t   owner;           // 0x18 owner thing index (self when independent)
    int16_t    aux;             // 0x1a z velocity / timer / castle level / spell aim
    uint16_t   prop_flags;      // 0x1c bit0 damageable, bit1 castle/wizard; accepted damage-type mask
    uint16_t   yaw;             // 0x1e 0..0x7ff
    uint16_t   pitch;           // 0x20
    uint16_t   target_yaw;      // 0x22
    uint16_t   target_pitch;    // 0x24
    uint16_t   killer;          // 0x26
    uint16_t   last_attacker;   // 0x28
    uint16_t   caster;          // 0x2a
    uint16_t   damage;          // 0x2c
    int16_t    z_vel;           // 0x2e
    int16_t    cast_ticks;      // 0x30
    int16_t    duration;        // 0x32
    uint16_t   parent;          // 0x34 leader / segment head
    uint16_t   child;           // 0x36 next segment
    uint16_t   speed;           // 0x38
    uint8_t    timer_a;         // 0x3a awake gate (creatures)
    uint8_t    timer_b;         // 0x3b
    uint8_t    spell_flags;     // 0x3c
    uint8_t    burst;           // 0x3d
    uint8_t    unk3e;           // 0x3e
    uint8_t    tick;            // 0x3f ++ after each handler call
    uint8_t    cls;             // 0x40 class
    uint8_t    type;            // 0x41 level-file Model; Table B index
    uint8_t    filter_cls;      // 0x42 collision filter (0xff any)
    uint8_t    filter_type;     // 0x43
    uint8_t    impact_cls;      // 0x44 (10 at alloc; projectiles: impact effect class)
    uint8_t    impact_type;     // 0x45
    uint8_t    state;           // 0x46 Table A index
    uint8_t    castle_size;     // 0x47
    C          x;               // 0x48 world units, cell = x>>8
    C          y;               // 0x4a
    int16_t    z;               // 0x4c
    int16_t    ext_z0;          // 0x4e bbox half extents
    int16_t    ext_x;           // 0x50
    int16_t    ext_y;           // 0x52
    int16_t    ext_h;           // 0x54
    uint16_t   sprite;          // 0x56 SpriteDesc index
    uint8_t    frame;           // 0x58
    uint8_t    draw_type;       // 0x59
    DamageSlot damage_slots[6]; // 0x5a..0x7d
    int16_t    speed_cur;       // 0x7e
    int16_t    speed_base;      // 0x80
    uint16_t   turn_rate;       // 0x82
    int32_t    mana_cost;       // 0x84
    int32_t    mana_total;      // 0x88
    int32_t    mana;            // 0x8c
    uint16_t   mana_owner;      // 0x90
    uint16_t   target;          // 0x92
    uint16_t   unk94;           // 0x94
    PosT<C>    home;            // 0x96 home / origin / destination position (castle, projectile launch, teleport target)
    uint32_t   desc;            // 0x9c idx: MoveDesc index (original: pointer, DAT_00096a10 default)
    uint32_t   player;          // 0xa0 idx: owner player sub-block offset into GameState (PlayerRec+0x44f), 0 = dummy
};
using Thing      = ThingT<WCoord>;
using ThingImage = ThingT<uint16_t>;
static_assert(sizeof(ThingImage) == 0xa4);
static_assert(offsetof(ThingImage, state) == 0x46);
static_assert(offsetof(ThingImage, x) == 0x48);
static_assert(offsetof(ThingImage, damage_slots) == 0x5a);
static_assert(offsetof(ThingImage, speed_cur) == 0x7e);
static_assert(offsetof(ThingImage, home) == 0x96);
static_assert(offsetof(ThingImage, desc) == 0x9c);
static_assert(sizeof(Thing) == 0xac);
static_assert(offsetof(Thing, x) == 0x48 && offsetof(Thing, z) == 0x50 && offsetof(Thing, home) == 0x9a);

// One HUD message slot (0x44 bytes). PlayerRec holds one per *sending* player: the text player q
// produced is shown from players[local].messages[q].
struct PlayerMsg {
    char     text[0x40];      // 0x00
    uint16_t ticks;           // 0x40 display countdown (100 = notice, 32000 = chat line being typed)
    uint16_t arg;             // 0x42
};
static_assert(sizeof(PlayerMsg) == 0x44);

// Camera log entry (14 bytes), written by player_log_position_3e080 and read by render_frame_1fab0
// as the camera of render_view_2f6e0 (x, y, yaw, z + 0x80, pitch, roll, zoom).
struct PosLogEntry {
    uint16_t x;               // 0x0
    uint16_t y;               // 0x2
    int16_t  z;               // 0x4 Thing z (the renderer adds 0x80)
    uint16_t yaw;             // 0x6
    int16_t  pitch;           // 0x8 P.pitch_acc / 2 + knock-back shake
    int16_t  roll;            // 0xa P.yaw_rate (banks into the turn)
    int16_t  zoom;            // 0xc PlayerRec.zoom (0x80)
};
static_assert(sizeof(PosLogEntry) == 0xe);

// 8-byte threat table entry of the AI (P+0x1cc + 8 * player).
struct PlayerThreat {
    uint16_t threat;          // 0x0 0x601f for an AI wizard at spawn, 0x9fdf toward a (re)spawned flyer
    uint16_t grudge;          // 0x2
    uint8_t  pad4[4];         // 0x4
};
static_assert(sizeof(PlayerThreat) == 8);

// The "P" sub-block of a player record (PlayerRec+0x44f, 0x3b2 bytes): everything the player's
// things reach through Thing.player. Fields established by the player port (docs/analysis/
// port_player.md); the AI fields are from docs/ENGINE.md region A and are not verified here.
struct PlayerBlock {
    uint32_t input_bits;        // 0x000 CmdPacket.bits: 1 faster, 2 slower, 4 strafe left, 8 strafe right, 0x10 / 0x20 fire left / right
    int16_t  steer_dx;          // 0x004 (2 * steer_x - yaw_rate) / 4, added to yaw_rate by the flyer
    int16_t  steer_dy;          // 0x006 (2 * steer_y - pitch_acc) / 4
    uint8_t  pad008[4];         // 0x008
    int16_t  target_speed;      // 0x00c -0x50..0x50; Thing.speed_cur follows it by 0x10 per tick
    uint16_t accelerating;      // 0x00e 1 while a speed key changes target_speed
    int16_t  strafe_speed;      // 0x010 sideways speed -0x50..0x50 (applied at yaw + 0x200), decays by 4
    uint8_t  pad012[4];         // 0x012
    int16_t  knock_speed;       // 0x016 knock-back (damage / 10, max 0x50), decays by 4
    uint16_t knock_yaw;         // 0x018
    uint16_t knock_pitch;       // 0x01a
    uint16_t move_pitch;        // 0x01c pitch actually flown (height-limited pitch_acc)
    uint16_t kills_of_player[8];// 0x01e times this player killed player n's wizard
    int16_t  combat_music;      // 0x02e ticks of combat music left (100 when hit)
    int16_t  player_no;         // 0x030
    uint16_t castle;            // 0x032 castle Thing index (0 = none)
    uint16_t balloons[3];       // 0x034
    uint8_t  pad03a[0x54 - 0x3a];
    uint16_t guards[34];        // 0x054 castle guard Thing indices
    uint8_t  pad098[0x122 - 0x98];
    int32_t  balloon_total;     // 0x122 (castle_manage_balloons_and_guards_419a0; not verified here)
    int32_t  balloon_mana;      // 0x126
    uint8_t  pad12a[0x134 - 0x12a];
    int32_t  mana_in_transit;   // 0x134 mana of this player's wizard effects (rebuilt by mana_totals_update)
    uint8_t  pad138[2];         // 0x138
    uint16_t tether_target;     // 0x13a rubber-band target Thing (0 = none)
    uint16_t tether_timer;      // 0x13c 200 at attach, released at 1000
    int32_t  tether_length;     // 0x13e 0x400..0xc00
    int32_t  mana;              // 0x142 the wizard's own mana share (1000); Thing.mana_total = this + owned mana
    uint8_t  aim_charge;        // 0x146 counts up to 200 (copied into projectiles)
    int16_t  yaw_rate;          // 0x147 steering accumulator: yaw += yaw_rate / 8 per tick
    int16_t  pitch_acc;         // 0x149 pitch accumulator: Thing.pitch = pitch_acc & 0x7ff
    int16_t  invuln_timer;      // 0x14b 100 at spawn, 2 inside the own castle: pending damage is discarded
    uint8_t  unk14d[8];         // 0x14d filled with 0x10 at spawn
    int16_t  health_regen;      // 0x155 max_health / 2000 (/ 250 in the castle; AI: / 500, / 200)
    int32_t  shots;             // 0x157
    int32_t  hits;              // 0x15b
    int32_t  countdown15f;      // 0x15f 2000 at spawn, -1 per tick
    uint8_t  pad163[4];         // 0x163
    int32_t  kills;             // 0x167
    int32_t  pct_spells;        // 0x16b
    int32_t  pct_accuracy;      // 0x16f
    int32_t  pct_mana;          // 0x173
    int32_t  pct_overall;       // 0x177
    uint32_t start_tick;        // 0x17b timer tick (DAT_0012eab4) at first spawn
    int32_t  regen_pause;       // 0x17f 0x10 when hit: no health regeneration while it counts down
    uint8_t  pad183[4];         // 0x183
    uint8_t  castle_hit_flash;  // 0x187
    uint8_t  hit_flash;         // 0x188
    uint8_t  damage_flash;      // 0x189
    uint8_t  pad18a[2];         // 0x18a
    int32_t  fire_dist;         // 0x18c nearest fire effect (0x800 = none; sound below 0x600)
    int32_t  ridge_dist;        // 0x190 nearest ridge node
    int16_t  ai_burst;          // 0x194
    int16_t  ai_conserve;       // 0x196
    int16_t  push_x;            // 0x198 displacement added to the position on the next move
    int16_t  push_y;            // 0x19a
    int16_t  push_z;            // 0x19c
    uint8_t  pad19e;            // 0x19e
    uint8_t  ai_mode;           // 0x19f
    uint16_t castle_level;      // 0x1a0 16-bit store from the castle's aux (castle.cpp, player_flyer2_s5_update_41500)
    uint8_t  pad1a2[0x1cc - 0x1a2];
    PlayerThreat threat[7];     // 0x1cc entries 0..6; entry 7 overlaps ai_aggression, see below
    uint16_t threat7;           // 0x204
    uint16_t grudge7;           // 0x206
    uint8_t  pad208[2];         // 0x208
    int16_t  ai_aggression;     // 0x20a level player block +4
    int16_t  ai_accuracy;       // 0x20c level player block +0xc
    int16_t  ai_reaction;       // 0x20e level player block +8 (also the respawn delay)
    int16_t  timer210;          // 0x210
    uint8_t  pad212[2];         // 0x212
    int32_t  spell_slot[24];    // 0x214 spell Thing index per book slot (0 = empty); between
                                //       players_init_records and the spawn: spell id, -1 = empty
    uint16_t ai_want_spell[24]; // 0x274
    uint16_t spell_thing[24];   // 0x2a4 spell id -> spell Thing index (player_rebuild_spell_index_40240)
    uint16_t spell_cooldown[24];// 0x2d4
    uint8_t  hotkey_slot[24];   // 0x304 quick-select key -> book slot (0xff = unassigned)
    uint8_t  ai_allowed[24];    // 0x31c
    uint8_t  pad334[24];        // 0x334
    uint8_t  spell_flash[24];   // 0x34c by spell id: 0x20 when selected through a quick-select key
    uint8_t  pad364[24];        // 0x364
    uint8_t  spell_found[24];   // 0x37c by spell id; survives players_init_records (campaign progress)
    uint8_t  spell_sealed[24];  // 0x394
    int16_t  slot_left;         // 0x3ac book slot of the left-hand spell (0xff = none)
    uint8_t  pad3ae[2];         // 0x3ae
    int16_t  slot_right;        // 0x3b0
};
static_assert(sizeof(PlayerBlock) == 0x801 - 0x44f);
static_assert(offsetof(PlayerBlock, knock_speed) == 0x16);
static_assert(offsetof(PlayerBlock, player_no) == 0x30);
static_assert(offsetof(PlayerBlock, guards) == 0x54);
static_assert(offsetof(PlayerBlock, mana_in_transit) == 0x134);
static_assert(offsetof(PlayerBlock, tether_target) == 0x13a);
static_assert(offsetof(PlayerBlock, mana) == 0x142);
static_assert(offsetof(PlayerBlock, yaw_rate) == 0x147);
static_assert(offsetof(PlayerBlock, invuln_timer) == 0x14b);
static_assert(offsetof(PlayerBlock, health_regen) == 0x155);
static_assert(offsetof(PlayerBlock, countdown15f) == 0x15f);
static_assert(offsetof(PlayerBlock, kills) == 0x167);
static_assert(offsetof(PlayerBlock, start_tick) == 0x17b);
static_assert(offsetof(PlayerBlock, regen_pause) == 0x17f);
static_assert(offsetof(PlayerBlock, hit_flash) == 0x188);
static_assert(offsetof(PlayerBlock, fire_dist) == 0x18c);
static_assert(offsetof(PlayerBlock, push_x) == 0x198);
static_assert(offsetof(PlayerBlock, ai_mode) == 0x19f);
static_assert(offsetof(PlayerBlock, threat) == 0x1cc);
static_assert(offsetof(PlayerBlock, ai_aggression) == 0x20a);
static_assert(offsetof(PlayerBlock, spell_slot) == 0x214);
static_assert(offsetof(PlayerBlock, spell_thing) == 0x2a4);
static_assert(offsetof(PlayerBlock, hotkey_slot) == 0x304);
static_assert(offsetof(PlayerBlock, ai_allowed) == 0x31c);
static_assert(offsetof(PlayerBlock, spell_flash) == 0x34c);
static_assert(offsetof(PlayerBlock, spell_found) == 0x37c);
static_assert(offsetof(PlayerBlock, spell_sealed) == 0x394);
static_assert(offsetof(PlayerBlock, slot_left) == 0x3ac);
static_assert(offsetof(PlayerBlock, slot_right) == 0x3b0);

// Per-player record (0x801 bytes), 8 at GameState+0x340b. Thing.player points at +0x44f ("P").
struct PlayerRec {
    uint16_t win_timer;       // 0x000 ticks above the win percentage (level won at 0x10)
    uint16_t status;          // 0x002 bit2 won, bit4, bit8 aborted
    uint8_t  quit;            // 0x004 nonzero = quit to DOS (1 quit, 2 CD check failed)
    uint8_t  flags;           // 0x005 bit8 debug overlay (word); command 4 xors it
    uint8_t  active;          // 0x006
    uint16_t index;           // 0x007 own player number (players_init_records_3bc10)
    uint8_t  is_computer;     // 0x009 1 = AI wizard (class 3 type 1), 0 = flyer driven by command packets
    uint16_t thing;           // 0x00a player's Thing index
    uint8_t  padc[2];         // 0x00c
    uint16_t view_entry;      // 0x00e log[] entry the renderer takes the camera from (0x1f)
    uint16_t log_count;       // 0x010 entries in log[] (0x20); the position is written to [log_count - 1]
    uint32_t tick;            // 0x012 level tick counter of the local player (incremented once per tick)
    uint8_t  pad16[2];        // 0x016
    uint32_t cheat;           // 0x018 survives players_clear_records; == 0xae89e gives 1,000,000 health / mana
    PlayerMsg messages[8];    // 0x01c HUD messages by sending player (see PlayerMsg)
    PosLogEntry log_template; // 0x23c copied into every log[] entry at level start; zoom (0x248) = 0x80
    PosLogEntry log[32];      // 0x24a camera log; only [log_count - 1] is ever written
    char     name[0x40];      // 0x40a
    uint8_t  input_mode;      // 0x44a 0 flight 2 spell select 3 text entry
    uint8_t  unk44b;          // 0x44b cleared for every player every tick
    uint8_t  pad44c[3];       // 0x44c
    union {
        uint8_t     p[0x801 - 0x44f];   // 0x44f "P" sub-block as raw bytes (thing_player_block)
        PlayerBlock blk;                // ... and typed
    };
};
static_assert(sizeof(PlayerRec) == 0x801);
static_assert(offsetof(PlayerRec, index) == 0x7);
static_assert(offsetof(PlayerRec, thing) == 0xa);
static_assert(offsetof(PlayerRec, view_entry) == 0xe);
static_assert(offsetof(PlayerRec, tick) == 0x12);
static_assert(offsetof(PlayerRec, cheat) == 0x18);
static_assert(offsetof(PlayerRec, messages) == 0x1c);
static_assert(offsetof(PlayerRec, log_template) == 0x23c);
static_assert(offsetof(PlayerRec, log) == 0x24a);
static_assert(offsetof(PlayerRec, name) == 0x40a);
static_assert(offsetof(PlayerRec, input_mode) == 0x44a);
static_assert(offsetof(PlayerRec, p) == 0x44f);
static_assert(offsetof(PlayerRec, blk) == 0x44f);

// The game-state block (0x38d03 bytes in the original), pointed to by g_state. `GameState` is the port's
// in-memory form (Things as `Thing`, so everything from `things` on is 8 bytes per slot further than the
// offsets in the comments), `GameStateImage` the original's byte image (mc_image.h converts).
template <class T> struct GameStateT {
    uint8_t   pad0[4];               // 0x0000
    uint32_t  rng;                   // 0x0004
    int16_t   local_player;          // 0x0008
    int16_t   player_count;          // 0x000a
    uint8_t   padc[0x24 - 0xc];      // 0x000c zeroed per level; +0xc.. = per-type creature serial counters (constructors.cpp)
    uint16_t  volcano_thing;         // 0x0024 the erupting volcano (effect 0x12), 0 = none (effects.cpp)
    uint16_t  volcano_smoke;         // 0x0026 its smoke column (effect 0x13)
    int32_t   free_top;              // 0x0028 free-stack top (-1 empty)
    uint8_t   texture_needed[0x211]; // 0x002c
    uint8_t   pad23d[0x240 - 0x23d]; // 0x023d
    int32_t   level_music_track;     // 0x0240 track chosen at level start (music_start_level, sound.cpp)
    uint8_t   pad244;                // 0x0244
    uint32_t  title_flag_a;          // 0x0245
    uint32_t  title_flag_b;          // 0x0249
    uint32_t  title_flag_c;          // 0x024d
    int32_t   free_list[1000];       // 0x0251 idx: free thing indices (original: Thing*)
    int32_t   active_top;            // 0x11f1
    int32_t   active_list[1000];     // 0x11f5 idx: recyclable things (original: Thing*)
    uint8_t   opt_second_surface;    // 0x2195 '?'
    uint8_t   opt_shadows;           // 0x2196 'A'
    uint8_t   opt_textured_sky;      // 0x2197
    uint8_t   view_size;             // 0x2198 0x28 = full
    uint8_t   opt_hud_a;             // 0x2199
    uint8_t   opt_hud_b;             // 0x219a
    uint8_t   mode_3d;               // 0x219b 0 off 1 anaglyph 2 SIRDS
    uint8_t   opt_motion_blur;       // 0x219c
    uint8_t   opt_smooth;            // 0x219d 2x2 smoothing
    uint8_t   opt_interlaced;        // 0x219e VFX1 interlaced stereo
    uint8_t   pad219f;               // 0x219f
    uint16_t  texture_block_size;    // 0x21a0 0x10 -> block16.dat, 0x20 -> block32.dat
    uint8_t   pad21a2[0x21ad - 0x21a2];
    uint8_t   opt_allowed[11];       // 0x21ad
    uint8_t   pad21b8[0x23d9 - 0x21b8];
    uint8_t   start_pos[8][6];       // 0x23d9 per-player start position {u16 x, u16 y, i16 z}
    uint8_t   pad2409[0x340b - 0x2409]; // 0x2409 player record backup ...
    PlayerRec players[8];            // 0x340b
    CmdPacket commands[8];           // 0x7413
    T         things[1000];          // 0x7463 slot 0 = scratch sentinel; the pool loop walks 1..999
    LevelData level;                 // 0x2f503 the level file, loaded verbatim (things at 0x2f945)
    uint32_t  creature_count;        // 0x38c9f creatures in the level file (switch_activate_356e0)
    uint32_t  spells_present[24];    // 0x38ca3 count of class-12 records per spell
};
using GameState      = GameStateT<Thing>;
using GameStateImage = GameStateT<ThingImage>;
constexpr size_t MC_STATE_IMAGE_SIZE = 0x38d03;   // the original's GameState (movie/gam*.dat, save states)
static_assert(sizeof(GameStateImage) == MC_STATE_IMAGE_SIZE);
static_assert(offsetof(GameStateImage, volcano_thing) == 0x24);
static_assert(offsetof(GameStateImage, level_music_track) == 0x240);
static_assert(offsetof(GameStateImage, free_list) == 0x251);
static_assert(offsetof(GameStateImage, active_top) == 0x11f1);
static_assert(offsetof(GameStateImage, opt_second_surface) == 0x2195);
static_assert(offsetof(GameStateImage, texture_block_size) == 0x21a0);
static_assert(offsetof(GameStateImage, opt_allowed) == 0x21ad);
static_assert(offsetof(GameStateImage, players) == 0x340b);
static_assert(offsetof(GameStateImage, commands) == 0x7413);
static_assert(offsetof(GameStateImage, things) == 0x7463);
static_assert(offsetof(GameStateImage, level) == 0x2f503);
static_assert(offsetof(GameStateImage, level.things) == 0x2f945);
static_assert(offsetof(GameStateImage, level.win_percent) == 0x38c93);
static_assert(offsetof(GameStateImage, level.player_block) == 0x385d3);
static_assert(offsetof(GameStateImage, creature_count) == 0x38c9f);
static_assert(offsetof(GameStateImage, spells_present) == 0x38ca3);
// Everything in front of the Things is the same in both forms (Thing.player offsets stay valid).
static_assert(offsetof(GameState, things) == 0x7463);
static_assert(sizeof(GameState) == MC_STATE_IMAGE_SIZE + 1000 * (sizeof(Thing) - sizeof(ThingImage)));

// The config / flags block (0x8e7e bytes), pointed to by g_cfg.
struct Config {
    uint16_t  flags;              // 0x0000 2 record demo 4 play demo 8 skip game 0x10 local chosen 0x80 network 0x100 movie
    uint8_t   paused;             // 0x0002 bit0
    uint8_t   pad3;               // 0x0003
    uint32_t  tick;               // 0x0004 frame counter (LRU stamps, cadence tests)
    uint8_t   pentium;            // 0x0008
    uint32_t  demo_file;          // 0x0009 movie file handle (demo_record_playback_step_3c540)
    uint16_t  movie;              // 0x000d
    uint16_t  roll;               // 0x000f
    uint16_t  level;              // 0x0011
    uint16_t  saved_level;        // 0x0013
    uint8_t   pad15;              // 0x0015
    uint8_t   spell_slot;         // 0x0016
    uint8_t   fade_stage;         // 0x0017
    uint8_t   debug_bits;         // 0x0018
    uint32_t  password;           // 0x0019
    char      save_str_a[0x20];   // 0x001d
    char      save_str_b[0x20];   // 0x003d
    uint8_t   pad5d;              // 0x005d
    uint8_t   tick_bits[15];      // 0x005e [k-1] = (local player's tick / k) & 1, k = 1..15 (blink cadences)
    uint8_t   pad6d[0x75 - 0x6d]; // 0x006d
    char      session[0x20];      // 0x0075
    uint8_t   disk_activity;      // 0x0095 set to 5 on every sprite demand load (4bfb0); reader not found yet, the name is a guess
    uint8_t   substeps;           // 0x0096 0..2 -> 1/4/16 thing updates per tick
    uint8_t   language;           // 0x0097
    uint8_t   palette_effect;     // 0x0098
    uint32_t  frame_time;         // 0x0099 timer ticks of the last frame (frame-time display)
    uint32_t  net_time;           // 0x009d network time (game.cpp, port_game.md)
    uint8_t   credits_state[7];   // 0x00a1
    uint32_t  pool;               // 0x00a8 idx (original: u8* texture pool)
    uint32_t  pool_size;          // 0x00ac
    uint8_t   padb0[0xbc - 0xb0]; // 0x00b0
    uint32_t  total_mana;         // 0x00bc
    uint8_t   padc0[0xce - 0xc0]; // 0x00c0
    uint32_t  pit_reload;         // 0x00ce 0x2726 (119 Hz)
    uint32_t  pit_accum;          // 0x00d2
    uint8_t   padd6[0x8e1e - 0xd6];
    uint32_t  creature_lists[20]; // 0x8e1e idx: per creature type list heads (original: Thing*)
    uint32_t  player_list;        // 0x8e6e idx
    uint32_t  mana_ball_list;     // 0x8e72 idx
    uint32_t  wizard_list;        // 0x8e76 idx
    uint32_t  projectile_list;    // 0x8e7a idx
};
static_assert(sizeof(Config) == 0x8e7e);
static_assert(offsetof(Config, demo_file) == 0x9);
static_assert(offsetof(Config, tick_bits) == 0x5e);
static_assert(offsetof(Config, disk_activity) == 0x95);
static_assert(offsetof(Config, substeps) == 0x96);
static_assert(offsetof(Config, pit_reload) == 0xce);
static_assert(offsetof(Config, creature_lists) == 0x8e1e);
static_assert(offsetof(Config, projectile_list) == 0x8e7a);

// Landscape renderer vertex grid record (44 bytes), 40 x 21 at the start of g_work.
struct VertexRec {
    int32_t  x_cam;        // 0x00
    int32_t  h_rel;        // 0x04 height - cam_z
    int32_t  h_mirror;     // 0x08
    int32_t  z_cam;        // 0x0c
    int32_t  sx;           // 0x10 screen x
    int32_t  sy;           // 0x14 screen y
    int32_t  sx_mirror;    // 0x18
    int32_t  sy_mirror;    // 0x1c
    int32_t  shade;        // 0x20 light<<16 scaled by fog (+ wave term)
    uint16_t first_thing;  // 0x24
    uint8_t  flags;        // 0x26 bit0 diagonal, bit1 culled, bit2 far side, bits3-6 offscreen, bit7 translucent
    uint8_t  flags2;       // 0x27 bits0-3 second surface offscreen, bit4 flat-shaded
    uint8_t  pad28;        // 0x28
    uint8_t  texture;      // 0x29
    uint8_t  uv_sel;       // 0x2a
    uint8_t  tex_prop;     // 0x2b
};
static_assert(sizeof(VertexRec) == 0x2c);

// Per view-quadrant step table, 4 at 0x93b20.
struct QuadStep {
    int8_t start_dx, start_dy;  // 0x0, 0x1
    int8_t tex_dx, tex_dy;      // 0x2, 0x3
    int8_t thing_dx, thing_dy;  // 0x4, 0x5
    int8_t row_dx, row_dy;      // 0x6, 0x7
    int8_t col_dx, col_dy;      // 0x8, 0x9
};
static_assert(sizeof(QuadStep) == 0xa);

// Rasteriser per-scanline edge record (20 bytes), table at 0x9b608 (one per screen row).
// +0 and +4 are the full 16.16 left/right edge x; the span fillers read only the high words:
// pixels [x_left, count) with `count` = integer part of x_right (exclusive end, not a pixel count).
struct SpanRec {
    uint16_t unk0;    // 0x00 x_left fraction
    int16_t  x_left;  // 0x02 x_left integer part
    uint16_t unk4;    // 0x04 x_right fraction
    uint16_t count;   // 0x06 x_right integer part (exclusive end)
    int32_t  u;       // 0x08 16.16
    int32_t  v;       // 0x0c 16.16
    int32_t  shade;   // 0x10 16.16
};
static_assert(sizeof(SpanRec) == 0x14);

#pragma pack(pop)

// Pool geometry helpers.
constexpr int MC_THING_SLOTS = 1000;          // slot 0 unused; the slots inside GameState (the original's pool)
// Phase 3: largest pool of the extended setting (PortSettings::thing_slots). Indices stay below 0x8000 so
// every 16-bit index field the original sign-extends (movsx of Thing.owner, P.spell_thing, the free
// count ...) keeps its value - see docs/analysis/port_pool.md.
constexpr int MC_THING_SLOTS_MAX = 0x8000;
constexpr int MC_MAP_SIZE    = 256;           // cells per side
constexpr int MC_MAP_CELLS   = 256 * 256;
constexpr int MC_CELL_UNITS  = 256;           // world units per cell
constexpr unsigned MC_ANGLE_MASK = 0x7ff;     // 2048 units per turn

// Cell index as the original computes it: (y_cell << 8) | x_cell.
inline uint16_t mc_cell(unsigned x_cell, unsigned y_cell) {
    return (uint16_t)(((y_cell & 0xff) << 8) | (x_cell & 0xff));
}
// Cell index from world coordinates (cell = coordinate >> 8).
inline uint16_t mc_cell_of(uint16_t x, uint16_t y) {
    return (uint16_t)((y & 0xff00) | (x >> 8));
}
