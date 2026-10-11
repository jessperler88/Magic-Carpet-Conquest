// The generated world of a mode run (Phase 4 round 11 session 2, roadmap B4 + B5; port-only).
//
// A mode level with a generated world (mode_params_generated, mode.h) is built here instead of by terrain_build
// and the template level's Things: sim.cpp's level start calls g_hook_level_world (installed by mode_level.cpp),
// which sizes the world, generates the terrain and writes the placed features as THING_INIT records into
// g_state->level.things - from there the original's own level start spawns them (terrain_generate_features for
// the class-10 terrain features with DisId 0xffff, switch_activate(0) for everything else, switch_activate(id)
// for the hidden payloads of the trigger areas). Save states and movies carry the maps and the records, not how
// they were made, so nothing here is needed to load a generated world back.
//
// Pipeline (worldgen_build): world_set_size(N) -> torus diamond-square (terrain_fractal_world) -> symmetrise
// (the average over the symmetry group: no seams) -> sea level (terrain_normalise_world) -> spawn sites (orbit of
// one point under the group, best of a seeded candidate set) + their plateaus -> rivers (sources in orbits,
// plateaus protected) -> the original's class / texture / light passes (terrain_build_passes) -> features.
//
// Everything is integer arithmetic from ModeParams (seed, size, players, WorldParams) and the template level's
// GenMap thresholds: every peer and every replay builds the same world. The generator's LCG is its own; g_rng16 /
// g_state->rng are seeded from GenMap.seed as terrain_build seeds them.
#pragma once
#include <cstdint>
#include <vector>
#include "mc_types.h"
#include "mode.h"

enum : uint8_t {
    WORLD_SYM_AUTO = 0,         // 1 player: none, 2: point, 3-4: rot4, 5-8: dihedral
    WORLD_SYM_NONE = 1,         // spawns spread by distance only
    WORLD_SYM_POINT = 2,        // 180-degree rotation about cell (0, 0): 2 seats
    WORLD_SYM_MIRROR = 3,       // x -> -x: 2 seats
    WORLD_SYM_ROT4 = 4,         // 90-degree rotation: 4 seats
    WORLD_SYM_DIHEDRAL = 5,     // rotations and diagonal mirrors: 8 seats
    WORLD_SYM_COUNT
};
const char *worldgen_symmetry_name(int sym);    // "auto", "none", "point", "mirror", "rot4", "dihedral"
int worldgen_symmetry_parse(const char *s);     // -1 when unknown

// What the placer put where (cells), for tests, dumps and later rounds (territory, bots' waypoints).
enum : uint8_t {
    WFEAT_SPAWN = 1,            // a seat: plateau centre (grade = seat index)
    WFEAT_SPRING,               // mana spring (grade = band 1 safe, 2 contested, 3 rich)
    WFEAT_LAIR,                 // creature lair (grade 1..5)
    WFEAT_TRIGGER,              // trigger area (grade = payload WTRIG_*)
    WFEAT_PORTAL,               // one end of a teleport pair (grade = pair index)
    WFEAT_TREES,                // a woodland (grade = WOOD_PALM / WOOD_OAK; future lumber resource)
    WFEAT_TOWN,                 // a neutral town (effect 0x2d, the original's villager castle)
    WFEAT_CIRCLE,               // a stone circle (grade 1 = holds a trigger area, link = its DisId; 0 = empty)
    WFEAT_URN,                  // a spell urn, a level pickup (grade = tier 1..3, link = spell id)
    WFEAT_KRAKEN,               // a wild kraken in open water (grade 1)
};
enum : uint8_t { WOOD_PALM = 0, WOOD_OAK = 1 };
enum : uint8_t {               // trigger payloads (data-driven table in worldgen.cpp)
    WTRIG_MANA = 1, WTRIG_AMBUSH, WTRIG_EARTHQUAKE, WTRIG_VOLCANO, WTRIG_STORM, WTRIG_CRATERS, WTRIG_ELITE, WTRIG_URN,
    WTRIG_UNDEAD,               // an undead army rises around the circle (user, 2026-10-10)
    WTRIG_COUNT
};
struct WorldFeature {
    uint8_t  kind;
    uint8_t  grade;
    uint16_t x, y;              // cell
    uint16_t link;              // portal: the other end's index in the feature list; trigger: its DisId
};

struct WorldLayout {
    int size = 256;
    int symmetry = WORLD_SYM_NONE;   // the resolved group
    int seats = 1;                   // spawn sites placed (the group order, >= the players)
    std::vector<WorldFeature> features;
    int records = 0;                 // THING_INIT records written
    int records_dropped = 0;         // features that did not fit the 1999 records
    int extra_records = 0;           // scenery / wild creatures beyond them (thing.h level_extra_records_set)
};

// The resolved generator controls (defaults filled in, clamped).
struct WorldGenSettings {
    int size, players, symmetry;
    int roughness_pct, water_pct, continent_pct;
    int springs, wild_pct, lairs, triggers, portals;
};
WorldGenSettings worldgen_settings(const ModeParams &p);

// Build the world for LEVEL (the template's LevelData already in g_state->level: GenMap thresholds, player blocks):
// sizes the world, generates the maps, replaces level->things with the placed records. False when the size is
// invalid (nothing changed then). OUT (optional) receives the layout.
bool worldgen_build(const ModeParams &params, LevelData *level, WorldLayout *out = nullptr);

// The layout of the last worldgen_build in this process (tests / dumps / the debug overlay).
const WorldLayout &worldgen_last_layout();
// After the level start spawned the records (mode_level.cpp's level-start hook): the trees of the oak woods take
// the oak sprite (SpriteDesc MC_SPRITE_DESC_OAK: Hidden Worlds' broadleaf tree, world_art.h; its extents are the
// palm's, so the simulation does not depend on whether Hidden Worlds is installed). Returns the trees changed.
int worldgen_apply_woods();

// ---- round 12 (task A): the setup screen's minimap preview ----------------------------------------------------
// The height field, sea, spawn plateaus and rivers worldgen_build would make for `params` on the template `tmpl`
// (only its GenMap.off is read: the seeded lattice point), without building a level: everything runs on a private
// N x N scratch world - g_world, g_world_unit_mask and the five map pointers are switched to it and restored
// before returning, so no map, RNG (g_rng16 / g_state->rng / ModeRng / the AI's), Thing or GameState byte changes.
// The class / texture / light passes and the features after the rivers are not run (the preview's heights are the
// ones before terrain_build_passes; the seats are the same cells as worldgen_build's WFEAT_SPAWN features).
struct WorldPreview {
    int size = 0;                       // cells per side (N)
    int symmetry = WORLD_SYM_NONE;      // the resolved group
    int players = 1;                    // seats 0 .. players-1 get a start record
    std::vector<uint8_t> height;        // N * N, row-major (y * N + x); 0 = water (sea and rivers), land >= 1
    std::vector<WorldFeature> seats;    // WFEAT_SPAWN, grade = seat index (as worldgen_build's layout)
};
bool worldgen_preview(const ModeParams &params, const LevelData &tmpl, WorldPreview *out);

// sim.cpp: the level start asks this before terrain_build; true = the world was built (terrain_build skipped).
extern bool (*g_hook_level_world)();
// sim.cpp: every level start tells the renderer side whether a generated world is on (engine.h world_art_apply;
// null without a renderer). Render-only: the simulation never depends on it.
extern void (*g_hook_level_art)(bool generated_world);
