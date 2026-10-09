// The generated world of a mode run (worldgen.h). Round 11 session 2 (B4 + B5); port-only.
#include "worldgen.h"
#include "terrain.h"
#include "world.h"
#include "mc_globals.h"
#include "mc_math.h"
#include "thing.h"
#include <algorithm>
#include <cstring>

bool (*g_hook_level_world)() = nullptr;
void (*g_hook_level_art)(bool) = nullptr;

namespace {

WorldLayout s_last;
std::vector<uint8_t> s_oak;     // 1 = a cell of an oak wood (worldgen_apply_woods), g_world.cells entries

// ---- the generator's LCG (its own: never g_rng16, never the mode's ModeRng) -------------------------------
struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed * 0x9e3779b97f4a7c15ull + 0x632be59bd9b4e019ull) {}
    uint32_t next() {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        return (uint32_t)(s >> 33);
    }
    int below(int n) { return n <= 1 ? 0 : (int)(next() % (uint32_t)n); }
    int range(int lo, int hi) { return lo + below(hi - lo + 1); }    // inclusive
};

uint32_t mix(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

// ---- symmetry ---------------------------------------------------------------------------------------------
// The group elements acting on cell coordinates (about cell (0, 0) of the torus). Element k of the dihedral
// group: rotations by k * 90 degrees (k < 4), then the four mirrors.
int group_order(int sym) {
    switch (sym) {
    case WORLD_SYM_POINT: case WORLD_SYM_MIRROR: return 2;
    case WORLD_SYM_ROT4: return 4;
    case WORLD_SYM_DIHEDRAL: return 8;
    default: return 1;
    }
}
void group_apply(int sym, int k, int x, int y, int *ox, int *oy) {
    if (sym == WORLD_SYM_MIRROR) {
        *ox = k ? -x : x; *oy = y;
    } else {
        if (sym == WORLD_SYM_POINT && k == 1) k = 2;
        switch (k) {
        case 0: *ox = x;  *oy = y;  break;
        case 1: *ox = -y; *oy = x;  break;
        case 2: *ox = -x; *oy = -y; break;
        case 3: *ox = y;  *oy = -x; break;
        case 4: *ox = y;  *oy = x;  break;
        case 5: *ox = -x; *oy = y;  break;
        case 6: *ox = -y; *oy = -x; break;
        default: *ox = x; *oy = -y; break;
        }
    }
    *ox = world_wrap_cell(*ox);
    *oy = world_wrap_cell(*oy);
}

struct Cell { int x, y; };

int orbit(int sym, int x, int y, Cell out[8]) {
    const int n = group_order(sym);
    for (int k = 0; k < n; k++) group_apply(sym, k, x, y, &out[k].x, &out[k].y);
    return n;
}

// torus distance in cells (Euclidean, integer)
int cell_dist(int ax, int ay, int bx, int by) {
    const int n = g_world.size;
    int dx = (int)((uint32_t)(bx - ax + n / 2) & g_world.cell_mask) - n / 2;
    int dy = (int)((uint32_t)(by - ay + n / 2) & g_world.cell_mask) - n / 2;
    return (int)world_isqrt((int64_t)dx * dx + (int64_t)dy * dy);
}
int cheb_dist(int ax, int ay, int bx, int by) {
    const int n = g_world.size;
    int dx = (int)((uint32_t)(bx - ax + n / 2) & g_world.cell_mask) - n / 2;
    int dy = (int)((uint32_t)(by - ay + n / 2) & g_world.cell_mask) - n / 2;
    return std::max(std::abs(dx), std::abs(dy));
}

// The average over the group is symmetric and seamless, but it only keeps the field's full relief where a cell's
// images are close together (near the group's fixed points: the centre and the corners would become one huge
// mountain or trench, the rest flat). So the average's deviation from the mean is divided by its expected standard
// deviation: with correlation rho(d) = max(0, 1 - d / L) between two images d cells apart (L ~ the decorrelation
// length of the fractal, whose levels above 128 cells are independent), Var(avg) / Var(f) = sum_ab rho(d_ab) / g^2.
// That ratio is smooth, so it is computed every 4 cells.
void symmetrise(std::vector<int32_t> &f, int sym) {
    const int g = group_order(sym);
    if (g == 1) return;
    int64_t total = 0;
    for (uint32_t i = 0; i < g_world.cells; i++) total += f[i];
    const int64_t mean = total / (int64_t)g_world.cells;
    const int L = std::min(256, g_world.size / 2);
    const int n = g_world.size, q = n / 4;
    std::vector<int32_t> sd((size_t)q * q);             // sqrt(ratio) in 1/256
    for (int y = 0; y < q; y++)
        for (int x = 0; x < q; x++) {
            Cell img[8];
            orbit(sym, x * 4 + 2, y * 4 + 2, img);
            int64_t acc = 0;                            // sum over ordered pairs, rho in 1/65536
            for (int a = 0; a < g; a++)
                for (int b = 0; b < g; b++) {
                    const int d = a == b ? 0 : cell_dist(img[a].x, img[a].y, img[b].x, img[b].y);
                    acc += d >= L ? 0 : (int64_t)(L - d) * 65536 / L;
                }
            sd[(size_t)y * q + x] = (int32_t)std::max<uint32_t>(world_isqrt(acc / (g * g)), 1);
        }
    std::vector<int32_t> out(f.size());
    for (uint32_t i = 0; i < g_world.cells; i++) {
        const int x = world_cell_x(i), y = world_cell_y(i);
        int64_t sum = 0, s = 0;                         // s over the orbit: the same for every image (symmetric)
        for (int k = 0; k < g; k++) {
            int ox, oy;
            group_apply(sym, k, x, y, &ox, &oy);
            sum += f[world_cell(ox, oy)];
            s += sd[(size_t)(oy / 4) * q + ox / 4];
        }
        const int64_t dev = sum / g - mean;
        out[i] = (int32_t)(mean + dev * 256 * g / std::max<int64_t>(s, 1));
    }
    f.swap(out);
}

// ---- placement -----------------------------------------------------------------------------------------
constexpr int kPlateauHalf = 12;        // the 24 x 24 spawn plateau
constexpr int kRamp = 12;               // blended ring around it
constexpr int kRecords = 1999;

struct Placer {
    const WorldGenSettings &s;
    LevelData *level;
    Rng rng;
    WorldLayout &out;
    std::vector<Cell> seats;
    int voronoi = 128;                  // half the smallest seat-to-seat distance (cells)
    int next_dis = 1;                   // DisIds of the trigger payloads (< 0x8000: switch_id sign-extends)
    int nrec = 0;

    Placer(const WorldGenSettings &s_, LevelData *lv, uint32_t seed, WorldLayout &o)
        : s(s_), level(lv), rng(seed), out(o) {}

    bool add(uint16_t cls, uint16_t model, int x, int y, uint16_t dis = 0, uint16_t swi_sz = 0, uint16_t swi_id = 0,
             uint16_t parent = 0, uint16_t child = 0) {
        if (nrec >= kRecords) { out.records_dropped++; return false; }
        ThingInit &r = level->things[nrec++];
        r.cls = cls; r.model = model;
        r.x = (uint16_t)world_wrap_cell(x); r.y = (uint16_t)world_wrap_cell(y);
        r.dis_id = dis; r.swi_sz = swi_sz;
        r.swi_id = swi_id; r.parent = parent; r.child = child;
        return true;
    }
    int free_records() const { return kRecords - nrec; }
    std::vector<ThingInit> extra;       // records beyond the level file's (thing.h level_extra_records_set)
    void add_extra(uint16_t cls, uint16_t model, int x, int y) {
        ThingInit r{};
        r.cls = cls; r.model = model;
        r.x = (uint16_t)world_wrap_cell(x); r.y = (uint16_t)world_wrap_cell(y);
        extra.push_back(r);
    }
    void feature(uint8_t kind, uint8_t grade, int x, int y, uint16_t link = 0) {
        out.features.push_back(WorldFeature{kind, grade, (uint16_t)world_wrap_cell(x), (uint16_t)world_wrap_cell(y), link});
    }

    // distance from the nearest seat in units of the seat's own region (0 = on it, 1 = the border between two)
    int band_pct(int x, int y) const {
        int best = 1 << 30;
        for (const Cell &c : seats) best = std::min(best, cell_dist(x, y, c.x, c.y));
        return best * 100 / std::max(voronoi, 1);
    }
    int cls_at(int x, int y) const { return g_map_flags[world_cell(x, y)] & 7; }
    bool land(int x, int y) const {
        const int c = cls_at(x, y);
        return c >= 2 && c <= 5;
    }
    // open water: sea level (height 0) on the cell and every cell R away (a kraken off the shore)
    bool deep_water(int x, int y, int r) const {
        for (int dy = -r; dy <= r; dy += r ? r : 1)
            for (int dx = -r; dx <= r; dx += r ? r : 1)
                if (g_map_height[world_cell(x + dx, y + dy)] != 0) return false;
        return true;
    }
    bool near_feature(int x, int y, int min_dist) const {
        for (const WorldFeature &f : out.features)
            if (cell_dist(x, y, f.x, f.y) < min_dist) return true;
        return false;
    }

    // A point whose whole orbit lies in the band [lo, hi] % of the seat region, on land, at least SPACING cells
    // from every placed feature and from each other. False after TRIES candidates.
    bool pick_orbit(int lo, int hi, int spacing, bool need_land, Cell orb[8], int *norb, int tries = 64) {
        const int g = group_order(out.symmetry);
        for (int t = 0; t < tries; t++) {
            const int x = rng.below(g_world.size), y = rng.below(g_world.size);
            const int n = orbit(out.symmetry, x, y, orb);
            bool ok = true;
            for (int i = 0; i < n && ok; i++) {
                const int b = band_pct(orb[i].x, orb[i].y);
                if (b < lo || b > hi) ok = false;
                else if (need_land && !land(orb[i].x, orb[i].y)) ok = false;
                else if (near_feature(orb[i].x, orb[i].y, spacing)) ok = false;
                for (int j = 0; j < i && ok; j++)
                    if (cell_dist(orb[i].x, orb[i].y, orb[j].x, orb[j].y) < spacing) ok = false;
            }
            if (ok && n == g) { *norb = n; return true; }
        }
        return false;
    }

    // a land cell within R cells of (x, y) (the centre itself when nothing else is found)
    Cell scatter(int x, int y, int r) {
        for (int t = 0; t < 8; t++) {
            const int cx = x + rng.range(-r, r), cy = y + rng.range(-r, r);
            if (land(cx, cy)) return Cell{world_wrap_cell(cx), world_wrap_cell(cy)};
        }
        return Cell{world_wrap_cell(x), world_wrap_cell(y)};
    }
};

// creature models (level.py MODEL_NAMES class 5) by lair grade; krakens (water) are not placed on land
struct Pack { uint8_t model; uint8_t lo, hi; };
const Pack kPacks[5][3] = {
    { {2, 6, 10}, {1, 4, 7},  {10, 4, 6} },     // 1: bees, vultures, emus
    { {5, 3, 5},  {3, 2, 3},  {4, 3, 5}  },     // 2: crabs, worms, archers
    { {7, 3, 5},  {9, 4, 6},  {8, 2, 4}  },     // 3: trolls, skeletons, griffons
    { {0, 1, 2},  {8, 3, 5},  {7, 5, 7}  },     // 4: dragons, griffons, trolls
    { {16, 1, 2}, {11, 1, 1}, {0, 2, 3}  },     // 5: wyverns, a genie, dragons
};

// A spell record's SwiId is its phase (level_spawn_thing_record: state += SwiId): 2 = a level pickup that leaves a
// copy for the other wizards (spells.h; the campaign's urns use 1 and 2). 0 would be "owned", never picked up.
constexpr uint16_t kUrnPhase = 2;

// trigger payload weights per band (safe / contested / rich): the data-driven table of B5 (round 20 adds MC2's)
struct PayloadWeight { uint8_t kind, w[3]; };
const PayloadWeight kPayloads[] = {
    {WTRIG_MANA, {6, 4, 3}}, {WTRIG_AMBUSH, {2, 4, 4}}, {WTRIG_EARTHQUAKE, {1, 2, 2}}, {WTRIG_VOLCANO, {0, 1, 2}},
    {WTRIG_STORM, {1, 2, 2}}, {WTRIG_CRATERS, {1, 1, 1}}, {WTRIG_ELITE, {0, 0, 1}}, {WTRIG_URN, {0, 1, 1}},
};

int band_of(int pct) { return pct < 55 ? 0 : (pct < 95 ? 1 : 2); }

void place_pack(Placer &P, int grade, int x, int y, uint16_t dis) {
    grade = std::clamp(grade, 1, 5);
    const Pack &k = kPacks[grade - 1][P.rng.below(3)];
    const int n = P.rng.range(k.lo, k.hi) * std::max(P.s.wild_pct, 50) / 100;
    for (int i = 0; i < std::max(n, 1); i++) {
        const Cell c = P.scatter(x, y, 3);
        P.add(5, k.model, c.x, c.y, dis, 0, dis);
    }
}

// The payload of trigger DIS at (x, y) (records spawned by switch_activate(dis) when a wizard flies over it).
void place_payload(Placer &P, int kind, int band, int x, int y, uint16_t dis) {
    switch (kind) {
    case WTRIG_MANA: {
        const int n = 8 + 4 * band + P.rng.below(5);
        for (int i = 0; i < n; i++) { const Cell c = P.scatter(x, y, 3); P.add(10, 0x27, c.x, c.y, dis, 0, dis); }
        break;
    }
    case WTRIG_AMBUSH: {
        const int grade = 1 + band * 2 + P.rng.below(2);
        for (int i = 0; i < 2; i++) place_pack(P, grade, x + P.rng.range(-6, 6), y + P.rng.range(-6, 6), dis);
        break;
    }
    case WTRIG_EARTHQUAKE: P.add(10, 15, x, y, dis, 0, dis); break;
    case WTRIG_VOLCANO: P.add(10, 9, x + P.rng.range(-6, 6), y + P.rng.range(-6, 6), dis, 0, dis); break;
    case WTRIG_STORM:
        for (int i = 0; i < 3 + band; i++) P.add(10, 23, x + P.rng.range(-5, 5), y + P.rng.range(-5, 5), dis, 0, dis);
        for (int i = 0; i < 2 + band; i++) P.add(10, 17, x + P.rng.range(-5, 5), y + P.rng.range(-5, 5), dis, 0, dis);
        break;
    case WTRIG_CRATERS:
        for (int i = 0; i < 3; i++) P.add(10, 11, x + P.rng.range(-8, 8), y + P.rng.range(-8, 8), dis, 0, dis);
        break;
    case WTRIG_ELITE: {
        static const uint8_t elite[3] = {16, 11, 0};    // a wyvern, a genie or a dragon (round 13 adds affixes)
        P.add(5, elite[P.rng.below(3)], x, y, dis, 0, dis);
        break;
    }
    case WTRIG_URN: {
        // a spell the template allows player 0 (+0x74) that is not a start spell (+0x10)
        int cand[24], n = 0;
        for (int i = 0; i < 24; i++)
            if (P.level->player_block[0][0x74 + i] == 1 && P.level->player_block[0][0x10 + i] == 0) cand[n++] = i;
        if (n) P.add(12, (uint16_t)cand[P.rng.below(n)], x, y, dis, 0, kUrnPhase);
        else   P.add(10, 0x27, x, y, dis, 0, dis);
        break;
    }
    default: break;
    }
}

int pick_payload(Placer &P, int band) {
    int total = 0;
    for (const PayloadWeight &w : kPayloads) total += w.w[band];
    int r = P.rng.below(total);
    for (const PayloadWeight &w : kPayloads) { if (r < w.w[band]) return w.kind; r -= w.w[band]; }
    return WTRIG_MANA;
}

// ---- seats -------------------------------------------------------------------------------------------------
int water_cells(int x, int y) {
    int n = 0;
    for (int dy = -kPlateauHalf; dy < kPlateauHalf; dy++)
        for (int dx = -kPlateauHalf; dx < kPlateauHalf; dx++)
            n += g_map_height[world_cell(x + dx, y + dy)] == 0;
    return n;
}

void choose_seats(Placer &P, int seats_wanted) {
    const int n = g_world.size, bucket = std::max(n / 32, 1);
    const int g = group_order(P.out.symmetry);
    if (g > 1) {
        int best = -(1 << 30);
        Cell best_orb[8] = {};
        for (int t = 0; t < 160; t++) {
            Cell orb[8];
            orbit(P.out.symmetry, P.rng.below(n), P.rng.below(n), orb);
            int m = 1 << 30;
            for (int i = 0; i < g; i++)
                for (int j = 0; j < i; j++) m = std::min(m, cell_dist(orb[i].x, orb[i].y, orb[j].x, orb[j].y));
            const int score = (m / bucket) * 1000 - water_cells(orb[0].x, orb[0].y);
            if (score > best) { best = score; std::copy(orb, orb + g, best_orb); }
        }
        P.seats.assign(best_orb, best_orb + g);
    } else {
        // no symmetry: farthest-point sampling over a seeded candidate set
        std::vector<Cell> cand(192);
        for (Cell &c : cand) c = Cell{P.rng.below(n), P.rng.below(n)};
        for (int s = 0; s < seats_wanted; s++) {
            int best = -(1 << 30), bi = 0;
            for (int i = 0; i < (int)cand.size(); i++) {
                int m = n;
                for (const Cell &c : P.seats) m = std::min(m, cell_dist(cand[i].x, cand[i].y, c.x, c.y));
                const int score = (m / bucket) * 1000 - water_cells(cand[i].x, cand[i].y);
                if (score > best) { best = score; bi = i; }
            }
            P.seats.push_back(cand[(size_t)bi]);
        }
    }
    int m = n;
    for (size_t i = 0; i < P.seats.size(); i++)
        for (size_t j = 0; j < i; j++)
            m = std::min(m, cell_dist(P.seats[i].x, P.seats[i].y, P.seats[j].x, P.seats[j].y));
    P.voronoi = std::max(P.seats.size() > 1 ? m / 2 : n / 2, 16);
}

// The plateau: the 24 x 24 block at one height (its average, kept on land), a 12-cell ring blended into the terrain.
void flatten_plateaus(const std::vector<Cell> &seats) {
    // the same height for every seat (fairness): the mean of the seats' averages
    int64_t sum = 0;
    for (const Cell &c : seats)
        for (int dy = -kPlateauHalf; dy < kPlateauHalf; dy++)
            for (int dx = -kPlateauHalf; dx < kPlateauHalf; dx++)
                sum += g_map_height[world_cell(c.x + dx, c.y + dy)];
    const int h = std::clamp((int)(sum / (int64_t)(seats.size() * 4 * kPlateauHalf * kPlateauHalf)), 0x28, 0x90);
    const int r = kPlateauHalf + kRamp;
    for (const Cell &c : seats)
        for (int dy = -r; dy < r; dy++)
            for (int dx = -r; dx < r; dx++) {
                uint8_t &v = g_map_height[world_cell(c.x + dx, c.y + dy)];
                const int d = std::max(std::max(dx < 0 ? -dx - 1 : dx, dy < 0 ? -dy - 1 : dy) - (kPlateauHalf - 1), 0);
                if (d == 0) v = (uint8_t)h;
                else v = (uint8_t)((v * d + h * (kRamp + 1 - d)) / (kRamp + 1));
            }
}

void place_rivers(Placer &P, const GenMap &gen) {
    terrain_rivers_begin_world();
    const int prot = kPlateauHalf + kRamp + 2;
    for (const Cell &c : P.seats)
        for (int dy = -prot; dy < prot; dy++)
            for (int dx = -prot; dx < prot; dx++) g_map_type[world_cell(c.x + dx, c.y + dy)] = 0;
    const int area = (g_world.size / 256) * (g_world.size / 256);
    const int g = group_order(P.out.symmetry);
    int count = std::max(gen.river, 0) * area / g;
    int tries = 1000 * area;
    while (count > 0 && tries > 0) {
        --tries;
        const int x = P.rng.below(g_world.size), y = P.rng.below(g_world.size);
        const uint32_t c = world_cell(x, y);
        if (g_map_height[c] <= (uint8_t)gen.sourc || g_map_flags[c] == 0 || g_map_type[c] == 0) continue;
        Cell orb[8];
        const int n = orbit(P.out.symmetry, x, y, orb);
        for (int i = 0; i < n; i++) terrain_trace_river_world(world_cell(orb[i].x, orb[i].y));
        --count;
    }
    terrain_rivers_end_world();
}

// ---- the features (after the class passes) ------------------------------------------------------------------
void place_features(Placer &P, int players) {
    const WorldGenSettings &s = P.s;
    const int area = (g_world.size / 256) * (g_world.size / 256);
    const int g = group_order(P.out.symmetry);
    const int seats = (int)P.seats.size();
    Cell orb[8];
    int n = 0;

    // seats: the start positions of the players (Flyer1..8 records), the other seats stay empty
    for (int i = 0; i < seats; i++) {
        P.feature(WFEAT_SPAWN, (uint8_t)i, P.seats[(size_t)i].x, P.seats[(size_t)i].y);
        if (i < players) P.add(3, (uint16_t)(4 + i), P.seats[(size_t)i].x, P.seats[(size_t)i].y);
    }
    // per-seat counts become orbit counts (an orbit gives every seat one); without symmetry, per player
    auto orbits_for = [&](int per_seat) { return g > 1 ? per_seat : per_seat * seats; };

    // portals: linked pairs, one end in the seat's region, the other at least a quarter of the world away
    for (int k = 0; k < orbits_for(s.portals); k++) {
        if (!P.pick_orbit(40, 80, 10, true, orb, &n)) continue;
        for (int i = 0; i < n; i++) {
            Cell far{};
            bool found = false;
            for (int t = 0; t < 64 && !found; t++) {
                far = Cell{P.rng.below(g_world.size), P.rng.below(g_world.size)};
                found = P.land(far.x, far.y) && P.band_pct(far.x, far.y) >= 60 &&
                        cell_dist(far.x, far.y, orb[i].x, orb[i].y) >= g_world.size / 4 && !P.near_feature(far.x, far.y, 10);
            }
            if (!found || P.free_records() < 2) continue;
            const uint16_t a = (uint16_t)P.out.features.size();
            P.feature(WFEAT_PORTAL, (uint8_t)k, orb[i].x, orb[i].y, (uint16_t)(a + 1));
            P.feature(WFEAT_PORTAL, (uint8_t)k, far.x, far.y, a);
            // the destination is 3 cells beside the other gate (arriving on it would bounce straight back)
            P.add(10, 0x22, orb[i].x, orb[i].y, 0, 0, 0, (uint16_t)world_wrap_cell(far.y + 3), (uint16_t)world_wrap_cell(far.x + 3));
            P.add(10, 0x22, far.x, far.y, 0, 0, 0, (uint16_t)world_wrap_cell(orb[i].y + 3), (uint16_t)world_wrap_cell(orb[i].x + 3));
        }
    }

    // a neutral town (the original's villager castle, effect 0x2d, built during generation) per seat
    if (P.pick_orbit(60, 95, 24, true, orb, &n))
        for (int i = 0; i < n; i++) {
            P.add(10, 0x2d, orb[i].x, orb[i].y, 0xffff, 0, 0xffff, (uint16_t)P.rng.range(3, 6));
            P.feature(WFEAT_TOWN, 0, orb[i].x, orb[i].y);
        }

    // mana springs: a blob of balls; bands safe / contested / rich
    for (int k = 0; k < orbits_for(s.springs); k++) {
        const int band = k % 3;
        const int lo = band == 0 ? 30 : (band == 1 ? 65 : 95), hi = band == 0 ? 55 : (band == 1 ? 95 : 160);
        if (!P.pick_orbit(lo, hi, 16, true, orb, &n)) continue;
        for (int i = 0; i < n; i++) {
            P.feature(WFEAT_SPRING, (uint8_t)(band + 1), orb[i].x, orb[i].y);
            const int balls = 8 + band * 6;
            for (int b = 0; b < balls; b++) { const Cell c = P.scatter(orb[i].x, orb[i].y, 3); P.add(10, 0x27, c.x, c.y); }
        }
    }

    // stone circles (user, 2026-10-08: the standing stones only in circles, a lone one very rarely): a ring of 7-11
    // standing stones, a dolmen in the centre of every third; 60 % of them hold a trigger area - a hidden one-shot
    // switch (type 0, "hidden inside": fires for a wizard flying into its box) in the centre with a payload group of
    // its own DisId. About `triggers` trigger areas per seat (5 circles per 3 triggers).
    const int circles = s.triggers ? (s.triggers * 5 + 2) / 3 : 2;
    const int corbits = orbits_for(circles), torbits = orbits_for(s.triggers);
    std::vector<uint8_t> holds((size_t)corbits, 0);        // exactly `triggers` of them (60 %), at random
    for (int k = 0; k < corbits; k++) holds[(size_t)k] = k < torbits;
    for (int k = corbits - 1; k > 0; k--) std::swap(holds[(size_t)k], holds[(size_t)P.rng.below(k + 1)]);
    for (int k = 0; k < corbits; k++) {
        if (!P.pick_orbit(35, 150, 16, true, orb, &n)) continue;
        const bool trig = holds[(size_t)k] != 0;
        const int band = band_of(P.band_pct(orb[0].x, orb[0].y));
        const int kind = trig ? pick_payload(P, band) : 0;
        const int stones = P.rng.range(7, 11), r = stones > 8 ? 4 : 3, phase = P.rng.below(0x800);
        const bool dolmen = P.rng.below(3) == 0;
        for (int i = 0; i < n; i++) {
            for (int j = 0; j < stones; j++) {
                const unsigned a = (unsigned)(phase + j * 0x800 / stones);
                const int sx = orb[i].x + (int)(((int64_t)mc_sin(a) * r + 0x8000) >> 16);
                const int sy = orb[i].y + (int)(((int64_t)mc_sin(a + 0x200) * r + 0x8000) >> 16);
                if (P.land(sx, sy)) P.add_extra(2, 1, sx, sy);
            }
            if (dolmen) P.add_extra(2, 2, orb[i].x, orb[i].y);
            uint16_t dis = 0;
            if (trig && P.next_dis < 0x7fff && P.free_records() >= 24) {
                dis = (uint16_t)P.next_dis++;
                P.feature(WFEAT_TRIGGER, (uint8_t)kind, orb[i].x, orb[i].y, dis);
                P.add(11, 0, orb[i].x, orb[i].y, 0, 2, dis);
                Rng saved = P.rng;      // every seat's payload is the same group, mirrored
                place_payload(P, kind, band, orb[i].x, orb[i].y, dis);
                if (i + 1 < n) P.rng = saved;
            }
            P.feature(WFEAT_CIRCLE, dis ? 1 : 0, orb[i].x, orb[i].y, dis);
        }
    }

    // spell urns (user, 2026-10-08): level pickups (class 12; every wizard can take one, a copy stays for the
    // others), graded by distance from the seats - per seat two near ones, one on contested ground, one far out.
    // The same spell for a whole orbit (fair). Not Smart Bomb (22): its phase-2 pickup is unbound in the 1996 engine.
    {
        static const uint8_t kTier1[] = {0x01, 0x04, 0x05, 0x0d, 0x0e, 0x0f, 0x13};     // heal shield sight steal rebound lightning magnet
        static const uint8_t kTier2[] = {0x06, 0x09, 0x0a, 0x0b, 0x0c, 0x11, 0x14};     // quake crater teleport rubber invisible skeleton fire wall
        static const uint8_t kTier3[] = {0x07, 0x08, 0x12};                             // meteor volcano thunderbolt
        struct Tier { const uint8_t *ids; int count, lo, hi, per_seat; };
        const Tier tiers[3] = {{kTier1, (int)sizeof kTier1, 25, 60, 2}, {kTier2, (int)sizeof kTier2, 60, 100, 1},
                               {kTier3, (int)sizeof kTier3, 100, 170, 1}};
        for (int t = 0; t < 3; t++)
            for (int k = 0; k < orbits_for(tiers[t].per_seat); k++) {
                if (!P.pick_orbit(tiers[t].lo, tiers[t].hi, 10, true, orb, &n, 256)) continue;
                const uint8_t id = tiers[t].ids[P.rng.below(tiers[t].count)];
                for (int i = 0; i < n && P.free_records() >= 24; i++) {
                    P.add(12, id, orb[i].x, orb[i].y, 0, 0, kUrnPhase);
                    P.feature(WFEAT_URN, (uint8_t)(t + 1), orb[i].x, orb[i].y, id);
                }
            }
    }

    // lairs: creature packs graded 1..5 by distance from the seats, Poisson-disc spaced
    for (int k = 0; k < orbits_for(s.lairs); k++) {
        if (!P.pick_orbit(35, 160, 20, true, orb, &n)) continue;
        const int grade = std::clamp(1 + (P.band_pct(orb[0].x, orb[0].y) - 35) * 5 / 120, 1, 5);
        const Rng saved = P.rng;
        for (int i = 0; i < n; i++) {
            if (P.free_records() < 16) break;
            P.rng = saved;
            P.feature(WFEAT_LAIR, (uint8_t)grade, orb[i].x, orb[i].y);
            place_pack(P, grade, orb[i].x, orb[i].y, 0);
        }
    }

    // Scenery and wild creatures go to the extra records (thing.h level_extra_records_set), not the level file's
    // 1999: per 256 x 256 cells (the campaign's multiplayer maps have ~80 trees, ~40 stones, ~90 free creatures)
    // the trees of 12 groves of ~10 (as 8 woods of 10-22 below), 40 lone trees (a stone or dolmen 1 in 40) and 40
    // wild creatures (x wild %), all scaled down together to fit a third of the pool. Stone circles are extra.
    int groves = 12 * area, singles = 40 * area, wild = 40 * area * s.wild_pct / 100;
    const int want = groves * 10 + singles + wild, cap = mode_generated_world_slots((uint32_t)g_world.size) / 3;
    if (want > cap) {
        groves = (int)((int64_t)groves * cap / want);
        singles = (int)((int64_t)singles * cap / want);
        wild = (int)((int64_t)wild * cap / want);
    }
    for (int k = 0; k < wild / g; k++) {
        if (!P.pick_orbit(45, 200, 4, true, orb, &n, 16)) continue;
        const int grade = std::clamp(1 + (P.band_pct(orb[0].x, orb[0].y) - 45) * 4 / 120, 1, 4);
        const Pack &pk = kPacks[grade - 1][P.rng.below(3)];
        for (int i = 0; i < n; i++) P.add_extra(5, pk.model, orb[i].x, orb[i].y);
    }
    // Woodlands (user, 2026-10-08: clusters of each tree type, future lumber): each wood is one type - palms on the
    // low ground near the coast, oaks (Hidden Worlds' broadleaf tree, worldgen_apply_woods) inland and higher up.
    // Fewer but bigger woods than session 2's groves (the same number of trees).
    // the type by the land's median height: palms below it, oaks above (about half of each, palms near the coast)
    int median = 0x40;
    {
        uint32_t hist[256] = {}, land = 0, acc = 0;
        for (uint32_t c = 0; c < g_world.cells; c++) if (g_map_height[c]) { hist[g_map_height[c]]++; land++; }
        for (median = 1; median < 255 && acc + hist[median] < land / 2; median++) acc += hist[median];
    }
    const int woods = std::max(groves * 2 / 3, g);
    for (int k = 0; k < woods / g; k++) {
        // alternate the wanted type; look for a site on its side of the median (else take the last site found)
        const uint8_t wanted = (k & 1) ? WOOD_OAK : WOOD_PALM;
        bool found = false, fits = false;
        for (int a = 0; a < 12 && !fits; a++) {
            if (!P.pick_orbit(25, 200, 12, true, orb, &n, 16)) continue;
            found = true;
            fits = (g_map_height[world_cell(orb[0].x, orb[0].y)] < median) == (wanted == WOOD_PALM);
        }
        if (!found) continue;
        const int trees = P.rng.range(10, 22), r = trees > 16 ? 6 : 5;
        const uint8_t type = g_map_height[world_cell(orb[0].x, orb[0].y)] < median ? WOOD_PALM : WOOD_OAK;
        const Rng saved = P.rng;
        for (int i = 0; i < n; i++) {
            P.rng = saved;
            P.feature(WFEAT_TREES, type, orb[i].x, orb[i].y);
            for (int t = 0; t < trees; t++) {
                const Cell c = P.scatter(orb[i].x, orb[i].y, r);
                P.add_extra(2, 0, c.x, c.y);
                if (type == WOOD_OAK) s_oak[world_cell(c.x, c.y)] = 1;
            }
        }
    }
    for (int k = 0; k < singles / g; k++) {
        // lone scenery: no spacing to other features (the feature list would grow by thousands), land only; trees
        // (an oak above the palm line), a standing stone or dolmen only 1 time in 40
        const int x = P.rng.below(g_world.size), y = P.rng.below(g_world.size);
        const int roll = P.rng.below(40);
        const uint16_t model = roll < 39 ? 0 : (P.rng.below(2) ? 1 : 2);   // tree, standing stone, dolmen
        n = orbit(P.out.symmetry, x, y, orb);
        if (P.band_pct(x, y) < 20) continue;
        for (int i = 0; i < n; i++) {
            if (!P.land(orb[i].x, orb[i].y)) continue;
            P.add_extra(2, model, orb[i].x, orb[i].y);
            if (model == 0 && g_map_height[world_cell(orb[i].x, orb[i].y)] >= median) s_oak[world_cell(orb[i].x, orb[i].y)] = 1;
        }
    }
    // Wild krakens (user, 2026-10-09): one per 10 wild creatures, in open water away from the seats (9000 health,
    // they drag wizards in). Placed last, so they change nothing else of a seed's world.
    for (int k = 0; k < std::max(wild / 10, wild ? g : 0) / g; k++) {
        bool ok = false;
        for (int a = 0; a < 24 && !ok; a++) {
            if (!P.pick_orbit(50, 250, 16, false, orb, &n, 32)) continue;
            ok = true;
            for (int i = 0; i < n && ok; i++) ok = P.deep_water(orb[i].x, orb[i].y, 3);
        }
        if (!ok) continue;
        for (int i = 0; i < n; i++) {
            P.add_extra(5, 6, orb[i].x, orb[i].y);
            P.feature(WFEAT_KRAKEN, 1, orb[i].x, orb[i].y);
        }
    }
}

} // namespace

const char *worldgen_symmetry_name(int sym) {
    static const char *const k[] = {"auto", "none", "point", "mirror", "rot4", "dihedral"};
    return sym >= 0 && sym < WORLD_SYM_COUNT ? k[sym] : "?";
}
int worldgen_symmetry_parse(const char *s) {
    for (int i = 0; i < WORLD_SYM_COUNT; i++) if (!std::strcmp(s, worldgen_symmetry_name(i))) return i;
    return -1;
}

WorldGenSettings worldgen_settings(const ModeParams &p) {
    const WorldParams &w = p.world;
    WorldGenSettings s{};
    s.size = (int)p.world_size;
    uint32_t players = p.humans + p.bots;
    s.players = (int)std::clamp<uint32_t>(players, 1, 8);
    int sym = w.symmetry < WORLD_SYM_COUNT ? w.symmetry : WORLD_SYM_AUTO;
    if (sym == WORLD_SYM_AUTO)
        sym = s.players == 1 ? WORLD_SYM_NONE : s.players == 2 ? WORLD_SYM_POINT : s.players <= 4 ? WORLD_SYM_ROT4 : WORLD_SYM_DIHEDRAL;
    if (group_order(sym) > 1 && group_order(sym) < s.players) sym = s.players <= 4 ? WORLD_SYM_ROT4 : WORLD_SYM_DIHEDRAL;
    s.symmetry = sym;
    s.roughness_pct = w.roughness ? w.roughness : 100;
    s.water_pct = w.water ? std::min<int>(w.water, 90) : 30;
    s.continent_pct = w.continent ? w.continent : 50;
    s.springs = w.springs == 0xff ? 0 : (w.springs ? w.springs : 3);
    s.wild_pct = w.wild == 0xff ? 0 : (w.wild ? std::min<int>(w.wild, 250) : 100);
    s.lairs = w.lairs == 0xff ? 0 : (w.lairs ? w.lairs : 3);
    const int trig = w.features & 15, port = w.features >> 4;
    s.triggers = trig == 15 ? 0 : (trig ? trig : 3);
    s.portals = port == 15 ? 0 : (port ? port : 1);
    return s;
}

bool worldgen_build(const ModeParams &params, LevelData *level, WorldLayout *out) {
    const WorldGenSettings s = worldgen_settings(params);
    if (!world_set_size(s.size)) return false;
    WorldLayout lay;
    lay.size = s.size;
    lay.symmetry = s.symmetry;
    // The terrain parameters are a fixed preset, not the template's: the multiplayer templates' class thresholds
    // are tuned to their own hand-made maps (level 50 has them all 0 = every land cell steep and no rivers, others
    // 7 = nearly all cliffs or 100 = none), so a generated world from them would depend on which template the seed
    // picked. The template keeps its seed (textures / light RNG) and supplies the player blocks (spells, AI).
    // Written back into level->gen, so the LevelData says what built the world.
    GenMap &gen = level->gen;
    gen.raise = 0;
    gen.gnarl = 16;             // second noise amplitude (x roughness %); the campaign uses 0..59
    gen.river = 20;             // per 256 x 256 cells; sources above height 40
    gen.sourc = 40;
    gen.snlin = 200;            // snow line: set from the heights below (the top 6 % of the land)
    gen.snflt = 45;             // terrain_classify_flat: cross range below = flat land
    gen.bhlin = 25;             // lowland / interior: 3x3 max below ...
    gen.bhflt = 13;             // ... with a range up to this
    gen.rkste = 12;             // cross range from this on = steep rock / cliff edge (a few % of the land)
    g_rng16 = (uint16_t)gen.seed;
    g_state->rng = (uint32_t)gen.seed;
    const uint32_t seed = mix(params.seed ^ 0x3017d00du);
    // The spells: every seat the same (a symmetric world is only fair with a symmetric start) - the multiplayer
    // standard of levels 50 / 52 / 53 ...: spells 0, 3 and the Castle (0x10) at the start, all 24 allowed. Some
    // templates start seats without the Castle because their map holds castle urns, which a generated world
    // replaces (round 11 session 3: on template 59 no computer wizard ever built a castle). Plus the speed-up (2)
    // (user, 2026-10-09: fireball, possession, speed-up and castle are every wizard's base kit, never in urns).
    for (int p = 0; p < 8; p++) {
        uint8_t *blk = level->player_block[p];
        for (int id = 0; id < 24; id++) {
            blk[0x10 + id] = (id == 0 || id == 2 || id == 3 || id == 0x10) ? 1 : 0;
            blk[0x74 + id] = 1;
        }
    }

    // height field
    {
        std::vector<int32_t> field(g_world.cells);
        TerrainWorldGen tg;
        tg.seed = seed;
        tg.start_x = (int)(gen.off & 0xff) * (s.size / 256);
        tg.start_y = (int)((gen.off >> 8) & 0xff) * (s.size / 256);
        tg.raise = (int16_t)(uint16_t)gen.raise;
        tg.gnarl = (int32_t)((int64_t)(uint16_t)gen.gnarl * s.roughness_pct / 100);
        tg.coarse_pct = s.continent_pct;
        terrain_fractal_world(field.data(), tg);
        symmetrise(field, s.symmetry);
        terrain_normalise_world(field.data(), s.water_pct);
    }
    std::memset(g_cell_things, 0, sizeof(uint16_t) * g_world.cells);

    std::memset(level->things, 0, sizeof level->things);
    Placer P(s, level, mix(seed + 1), lay);
    choose_seats(P, s.players);
    lay.seats = (int)P.seats.size();
    flatten_plateaus(P.seats);
    place_rivers(P, gen);
    // Snow caps (user, 2026-10-08): Hidden Worlds' snow pass on the highest land - the snow line is the height above
    // which the top 6 % of the land cells lie (flat enough ones turn to snow: cross range below snflt); Hidden Worlds'
    // classification comes with it (steep rock = class 1, snow = class 6). The snow tiles are drawn from the Hidden
    // Worlds textures when installed (world_art.h); the classes are the same either way.
    {
        uint32_t hist[256] = {}, land = 0;
        for (uint32_t i = 0; i < g_world.cells; i++) if (g_map_height[i]) { hist[g_map_height[i]]++; land++; }
        uint32_t acc = 0;
        int h = 255;
        while (h > 1 && acc + hist[h] <= land * 6 / 100) acc += hist[h--];
        gen.snlin = std::max(h, 0x90);
    }
    s_oak.assign(g_world.cells, 0);
    terrain_build_passes(gen, true);
    place_features(P, s.players);
    level_extra_records_set(P.extra.data(), P.extra.size());
    lay.records = P.nrec;
    lay.extra_records = (int)P.extra.size();
    s_last = lay;
    if (out) *out = lay;
    return true;
}

const WorldLayout &worldgen_last_layout() { return s_last; }

int worldgen_apply_woods() {
    if (s_oak.size() != g_world.cells) return 0;
    int n = 0;
    for (int i = 1; i < thing_pool_slots(); i++) {
        Thing *t = thing_at((unsigned)i);
        if (t->cls != 2 || t->type != 0 || !s_oak[world_cell_of(t->x, t->y)]) continue;
        thing_set_sprite(t, MC_SPRITE_DESC_OAK);
        n++;
    }
    return n;
}
