// The original's byte layout of the port's game state (Phase 4 round 11).
//
// The port keeps Thing.x / .y / .home as int32 world coordinates (mc_types.h WCoord) so a world can be wider
// than 256 cells. Everything that treats the game state as the original's byte image - the original's movies
// and their gam*.dat snapshots, the per-tick reference dumps, save states, the network checksum, the
// original's unchecked reads past a record - goes through these converters, so with a 256-cell world the
// bytes are exactly the original's. A world wider than 256 cells needs more than the 16 bits an image holds:
// ThingHi carries the high halves (save states / movie snapshots write them in their own chunk).
#pragma once
#include <cstring>
#include "mc_types.h"

// High 16 bits of the four coordinates of one Thing (zero in a 256-cell world).
struct ThingHi {
    uint16_t x, y, home_x, home_y;
};
static_assert(sizeof(ThingHi) == 8);

inline void thing_to_image(const Thing &t, ThingImage *o) {
    const uint8_t *s = reinterpret_cast<const uint8_t *>(&t);
    uint8_t *d = reinterpret_cast<uint8_t *>(o);
    std::memcpy(d, s, offsetof(Thing, x));
    o->x = (uint16_t)(int32_t)t.x;
    o->y = (uint16_t)(int32_t)t.y;
    std::memcpy(d + offsetof(ThingImage, z), s + offsetof(Thing, z), offsetof(ThingImage, home) - offsetof(ThingImage, z));
    o->home.x = (uint16_t)(int32_t)t.home.x;
    o->home.y = (uint16_t)(int32_t)t.home.y;
    o->home.z = t.home.z;
    std::memcpy(d + offsetof(ThingImage, desc), s + offsetof(Thing, desc), sizeof(ThingImage) - offsetof(ThingImage, desc));
}

// `hi` (optional) restores the high halves of a wider world.
inline void thing_from_image(const ThingImage &in, Thing *t, const ThingHi *hi = nullptr) {
    const uint8_t *s = reinterpret_cast<const uint8_t *>(&in);
    uint8_t *d = reinterpret_cast<uint8_t *>(t);
    std::memcpy(d, s, offsetof(ThingImage, x));
    t->x = (int32_t)in.x | (hi ? (int32_t)hi->x << 16 : 0);
    t->y = (int32_t)in.y | (hi ? (int32_t)hi->y << 16 : 0);
    std::memcpy(d + offsetof(Thing, z), s + offsetof(ThingImage, z), offsetof(ThingImage, home) - offsetof(ThingImage, z));
    t->home.x = (int32_t)in.home.x | (hi ? (int32_t)hi->home_x << 16 : 0);
    t->home.y = (int32_t)in.home.y | (hi ? (int32_t)hi->home_y << 16 : 0);
    t->home.z = in.home.z;
    std::memcpy(d + offsetof(Thing, desc), s + offsetof(ThingImage, desc), sizeof(ThingImage) - offsetof(ThingImage, desc));
}

inline ThingHi thing_hi(const Thing &t) {
    return ThingHi{(uint16_t)((uint32_t)(int32_t)t.x >> 16), (uint16_t)((uint32_t)(int32_t)t.y >> 16),
                   (uint16_t)((uint32_t)(int32_t)t.home.x >> 16), (uint16_t)((uint32_t)(int32_t)t.home.y >> 16)};
}

inline void state_to_image(const GameState &s, GameStateImage *o) {
    std::memcpy(o, &s, offsetof(GameState, things));
    for (int i = 0; i < MC_THING_SLOTS; i++) thing_to_image(s.things[i], &o->things[i]);
    std::memcpy(reinterpret_cast<uint8_t *>(o) + offsetof(GameStateImage, level),
                reinterpret_cast<const uint8_t *>(&s) + offsetof(GameState, level),
                sizeof(GameStateImage) - offsetof(GameStateImage, level));
}

// `hi` (optional): MC_THING_SLOTS entries.
inline void state_from_image(const GameStateImage &in, GameState *s, const ThingHi *hi = nullptr) {
    std::memcpy(s, &in, offsetof(GameStateImage, things));
    for (int i = 0; i < MC_THING_SLOTS; i++) thing_from_image(in.things[i], &s->things[i], hi ? hi + i : nullptr);
    std::memcpy(reinterpret_cast<uint8_t *>(s) + offsetof(GameState, level),
                reinterpret_cast<const uint8_t *>(&in) + offsetof(GameStateImage, level),
                sizeof(GameStateImage) - offsetof(GameStateImage, level));
}

// n bytes at byte offset `off` of the original's image of `s` (the original's unchecked reads past a field:
// a P block's spell_slot[0xff] lands in the next player record, or in the Things after player 7). Bytes
// outside the image read as 0.
inline void state_image_read(const GameState &s, intptr_t off, void *out, size_t n) {
    uint8_t *o = static_cast<uint8_t *>(out);
    const intptr_t things = (intptr_t)offsetof(GameStateImage, things);
    const intptr_t level = (intptr_t)offsetof(GameStateImage, level);
    for (size_t k = 0; k < n; k++, off++) {
        if (off < 0 || off >= (intptr_t)MC_STATE_IMAGE_SIZE) { o[k] = 0; continue; }
        if (off < things) { o[k] = reinterpret_cast<const uint8_t *>(&s)[off]; continue; }
        if (off >= level) { o[k] = reinterpret_cast<const uint8_t *>(&s)[off - level + (intptr_t)offsetof(GameState, level)]; continue; }
        const intptr_t slot = (off - things) / (intptr_t)sizeof(ThingImage);
        ThingImage img;
        thing_to_image(s.things[slot], &img);
        o[k] = reinterpret_cast<const uint8_t *>(&img)[(off - things) % (intptr_t)sizeof(ThingImage)];
    }
}
