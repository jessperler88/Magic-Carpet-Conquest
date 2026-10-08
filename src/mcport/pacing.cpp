// Frame pacing helpers (pacing.h). Port-only, render-time only: reads the game state, never writes it.
#include "pacing.h"
#include "thing.h"
#include "player.h"

uint32_t pace_alpha(uint64_t now_us, uint64_t last_us, uint64_t period_us) {
    if (period_us == 0 || now_us <= last_us) return 0;
    const uint64_t d = now_us - last_us;
    if (d >= period_us) return 0x10000;
    return (uint32_t)((d << 16) / period_us);
}

int pace_lerp(int a, int b, uint32_t alpha) {
    if (alpha == 0) return a;
    if (alpha >= 0x10000) return b;
    return a + (int)(((int64_t)(b - a) * alpha) >> 16);
}

int pace_lerp16(int a, int b, uint32_t alpha) {
    if (alpha == 0) return (int16_t)a;
    if (alpha >= 0x10000) return (int16_t)b;
    const int d = (int16_t)(uint16_t)(b - a);             // shortest way round the 0x10000 torus
    return (int16_t)(uint16_t)(a + (int)(((int64_t)d * alpha) >> 16));
}

static int wrap_diff_angle(int a, int b) {
    int d = (b - a) & 0x7ff;
    return d >= 0x400 ? d - 0x800 : d;
}

int pace_lerp_angle(int a, int b, uint32_t alpha) {
    if (alpha == 0) return a & 0x7ff;
    if (alpha >= 0x10000) return b & 0x7ff;
    return (a + (int)(((int64_t)wrap_diff_angle(a, b) * alpha) >> 16)) & 0x7ff;
}

int pace_lerp_roll(int a, int b, uint32_t alpha) {
    if (alpha == 0) return a;
    if (alpha >= 0x10000) return b;
    // From b's side so values stay in b's representation (the renderer masks with 0x7ff).
    return b - (int)(((int64_t)wrap_diff_angle(a, b) * (0x10000 - alpha)) >> 16);
}

// Round 11: a world wider than 256 cells - the camera keeps the whole coordinate (world.h).
static int pace_lerp_w(int a, int b, uint32_t alpha) {
    if (alpha == 0) return world_wrap(a);
    if (alpha >= 0x10000) return world_wrap(b);
    const int d = world_signed(b - a);
    return world_wrap(a + (int)(((int64_t)d * alpha) >> 16));
}

static int abs16(int d) { d = world_wide() ? world_signed(d) : (int16_t)(uint16_t)d; return d < 0 ? -d : d; }

bool pace_camera_jump(const Camera &a, const Camera &b) {
    return abs16(b.cam_x - a.cam_x) > PACE_JUMP_XY || abs16(b.cam_y - a.cam_y) > PACE_JUMP_XY ||
           (b.cam_z > a.cam_z ? b.cam_z - a.cam_z : a.cam_z - b.cam_z) > PACE_JUMP_Z;
}

Camera pace_lerp_camera(const Camera &a, const Camera &b, uint32_t alpha, bool masked_xy) {
    if (alpha >= 0x10000 || pace_camera_jump(a, b)) return b;
    if (alpha == 0) return a;
    Camera c;
    if (world_wide()) {
        c.cam_x = pace_lerp_w(a.cam_x, b.cam_x, alpha);
        c.cam_y = pace_lerp_w(a.cam_y, b.cam_y, alpha);
    } else {
        c.cam_x = pace_lerp16(a.cam_x, b.cam_x, alpha);
        c.cam_y = pace_lerp16(a.cam_y, b.cam_y, alpha);
        if (masked_xy) { c.cam_x &= 0xffff; c.cam_y &= 0xffff; }
    }
    c.yaw = pace_lerp_angle(a.yaw, b.yaw, alpha);        // the renderer masks yaw with 0x7ff
    c.cam_z = pace_lerp(a.cam_z, b.cam_z, alpha);
    c.pitch = pace_lerp(a.pitch, b.pitch, alpha);
    c.roll = pace_lerp_roll(a.roll, b.roll, alpha);
    c.zoom = pace_lerp(a.zoom, b.zoom, alpha);
    return c;
}

bool pace_slot_continues(const PaceSlot &pre, const PaceSlot &cur) {
    if (pre.cls == 0 || cur.cls == 0) return false;
    if (pre.cls != cur.cls || pre.type != cur.type || pre.owner != cur.owner || pre.gen != cur.gen) return false;
    return abs16(cur.x - pre.x) <= PACE_JUMP_XY && abs16(cur.y - pre.y) <= PACE_JUMP_XY &&
           (cur.z > pre.z ? cur.z - pre.z : pre.z - cur.z) <= PACE_JUMP_Z;
}

PaceSlot pace_read_slot(int idx) {
    const Thing *t = thing_at((unsigned)idx);
    PaceSlot s;
    s.x = (int16_t)t->x; s.y = (int16_t)t->y; s.z = t->z;
    s.cls = t->cls; s.type = t->type; s.owner = t->owner;
    s.gen = thing_slot_generation((unsigned)idx);
    return s;
}

void TickInterp::reset() {
    valid_ = false;
    have_pre_ = false;
    cam_snap_ = false;
    lerped_ = snapped_ = 0;
}

void TickInterp::before_tick(int local) {
    const int n = thing_pool_slots();
    pre_.resize((size_t)n);
    for (int i = 0; i < n; i++) pre_[(size_t)i] = pace_read_slot(i);
    cam_pre_ = player_camera(local);
    have_pre_ = true;
}

void TickInterp::after_tick(int local) {
    if (!have_pre_) { valid_ = false; return; }
    const int n = thing_pool_slots();
    if ((int)pre_.size() < n) pre_.resize((size_t)n, PaceSlot{0, 0, 0, 0, 0, 0});   // pool grew in the tick
    prev_.resize((size_t)n * 3);
    lerped_ = snapped_ = 0;
    for (int i = 0; i < n; i++) {
        const PaceSlot cur = pace_read_slot(i);
        const PaceSlot &pre = pre_[(size_t)i];
        const bool lerp = pace_slot_continues(pre, cur);
        const PaceSlot &from = lerp ? pre : cur;
        prev_[(size_t)i * 3 + 0] = from.x;
        prev_[(size_t)i * 3 + 1] = from.y;
        prev_[(size_t)i * 3 + 2] = from.z;
        if (cur.cls != 0) { if (lerp) lerped_++; else snapped_++; }
    }
    cam_cur_ = player_camera(local);
    cam_snap_ = pace_camera_jump(cam_pre_, cam_cur_);
    cam_prev_ = cam_snap_ ? cam_cur_ : cam_pre_;
    have_pre_ = false;
    valid_ = true;
}

void TickInterp::hold() {
    if (!valid_) return;
    const int n = (int)prev_.size() / 3;
    for (int i = 0; i < n; i++) {
        const PaceSlot cur = pace_read_slot(i);
        prev_[(size_t)i * 3 + 0] = cur.x;
        prev_[(size_t)i * 3 + 1] = cur.y;
        prev_[(size_t)i * 3 + 2] = cur.z;
    }
    cam_prev_ = cam_cur_;
}

void TickInterp::fill(RenderInterp &ri, uint32_t alpha, bool camera) const {
    ri = RenderInterp{};
    if (!valid_) return;
    ri.active = true;
    ri.alpha = alpha > 0x10000 ? 0x10000 : alpha;
    ri.prev_pos = prev_pos();
    ri.prev_count = (int)prev_.size() / 3;
    if (camera) {
        ri.have_camera = true;
        ri.camera = pace_lerp_camera(cam_prev_, cam_cur_, ri.alpha);
    }
}

uint64_t pace_cap_wait_us(uint64_t frame_start_us, uint64_t now_us, int fps_cap) {
    if (fps_cap <= 0) return 0;
    const uint64_t end = frame_start_us + 1000000ull / (uint64_t)fps_cap;
    return now_us >= end ? 0 : end - now_us;
}
