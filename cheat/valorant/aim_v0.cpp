// aim.cpp
// Aim assist system for Valorant-style practice client
// Windows · C++17
//
// INTEGRATION NOTES
//   1. Each frame, fill a std::vector<AimEntity> with live enemy data
//      (positions in world-space, head/body offsets, alive flag).
//   2. Call AimAssist::Update() with that list plus the local player's
//      current view angles and screen-space crosshair centre.
//   3. If Update() returns a target, read .delta_angles and add them
//      to your camera pitch/yaw before sending to the server.
//   4. WorldToScreen() must be wired to your actual view/projection
//      matrices — replace the stub at the bottom of this file.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

// ---------------------------------------------------------------------------
// Math primitives
// ---------------------------------------------------------------------------

struct Vec2 {
    float x = 0.f, y = 0.f;

    Vec2 operator+(const Vec2& o) const noexcept { return {x + o.x, y + o.y}; }
    Vec2 operator-(const Vec2& o) const noexcept { return {x - o.x, y - o.y}; }
    Vec2 operator*(float s)       const noexcept { return {x * s,   y * s};   }

    float length_sq() const noexcept { return x * x + y * y; }
    float length()    const noexcept { return std::sqrtf(length_sq()); }
};

struct Vec3 {
    float x = 0.f, y = 0.f, z = 0.f;

    Vec3 operator+(const Vec3& o) const noexcept { return {x+o.x, y+o.y, z+o.z}; }
    Vec3 operator-(const Vec3& o) const noexcept { return {x-o.x, y-o.y, z-o.z}; }
    Vec3 operator*(float s)       const noexcept { return {x*s,   y*s,   z*s};   }

    float length() const noexcept { return std::sqrtf(x*x + y*y + z*z); }
};

// Euler camera angles, degrees.  pitch: -90 (down) .. +90 (up), yaw: ±180.
struct Angles {
    float pitch = 0.f;
    float yaw   = 0.f;
};

static constexpr float kPi  = 3.14159265358979323846f;
static constexpr float kDeg = 180.f / kPi;
static constexpr float kRad = kPi / 180.f;

// Normalise an angle to [-180, 180].
inline float NormaliseAngle(float a) noexcept {
    while (a >  180.f) a -= 360.f;
    while (a < -180.f) a += 360.f;
    return a;
}

// Angular delta from current to target, shortest path.
inline Angles AngleDelta(const Angles& current, const Angles& target) noexcept {
    return {
        NormaliseAngle(target.pitch - current.pitch),
        NormaliseAngle(target.yaw   - current.yaw)
    };
}

// Convert a world-space direction vector to pitch/yaw angles.
inline Angles VecToAngles(const Vec3& dir) noexcept {
    float len_xy = std::sqrtf(dir.x * dir.x + dir.y * dir.y);
    return {
        std::atan2f(dir.z, len_xy) * kDeg,   // pitch
        std::atan2f(dir.y, dir.x)  * kDeg    // yaw
    };
}

// Convert pitch/yaw to a unit forward vector.
inline Vec3 AnglesToVec(const Angles& a) noexcept {
    float sp = std::sinf(a.pitch * kRad), cp = std::cosf(a.pitch * kRad);
    float sy = std::sinf(a.yaw   * kRad), cy = std::cosf(a.yaw   * kRad);
    return {cp * cy, cp * sy, -sp};
}

// ---------------------------------------------------------------------------
// Entity description
// ---------------------------------------------------------------------------

struct AimEntity {
    int      id          = -1;
    Vec3     feet        = {};   // origin / feet position
    Vec3     head        = {};   // head bone position
    float    health      = 100.f;
    bool     alive       = true;
    bool     visible     = false; // has LOS (set by your engine/server query)
    int      team        = 1;    // 0 = local team, 1 = enemy
};

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

struct AimConfig {
    // Field of view half-angle (degrees) within which targets are considered.
    float fov_deg         = 8.f;

    // Smoothing: fraction of remaining angle closed per frame.
    // 1.0 = instant snap, 0.05 = very slow pull.
    float smooth          = 0.12f;

    // Bone to aim at: true = head, false = centre-mass.
    bool  aim_head        = true;

    // Require line-of-sight before assisting.
    bool  require_visible = false;

    // Maximum distance (world units) to assist against.
    float max_distance    = 5000.f;

    // Activation: only assist when this returns true.
    // Replace with your input query (e.g. right-mouse held).
    bool (*is_active)() = nullptr;

    // Screen dimensions for FOV cone projection.
    int screen_w = 1920;
    int screen_h = 1080;
};

// ---------------------------------------------------------------------------
// World-to-screen stub
// ---------------------------------------------------------------------------
// Replace this with your actual matrix-based projection.
// Returns false if the point is behind the camera.

struct ViewState {
    Angles view_angles = {};
    Vec3   origin      = {};
    float  fov_vdeg    = 73.7f;   // vertical FOV in degrees
};

static bool WorldToScreen(const Vec3& world, const ViewState& vs,
                           int sw, int sh, Vec2& screen_out) noexcept {
    // --- stub: replace with your view/projection transform ---
    Vec3 delta = world - vs.origin;
    if (delta.length() < 0.001f) return false;

    // Build a simple camera basis from view angles.
    float yaw_r   = vs.view_angles.yaw   * kRad;
    float pitch_r = vs.view_angles.pitch * kRad;
    float sy = std::sinf(yaw_r),   cy = std::cosf(yaw_r);
    float sp = std::sinf(pitch_r), cp = std::cosf(pitch_r);

    // Local-space coordinates.
    float lx = delta.x * cy + delta.y * sy;        // forward
    float ly = delta.x * sy - delta.y * cy;        // right
    float lz = delta.z * cp + lx * sp;             // up
    lx        = lx * cp - delta.z * sp;            // recalc forward

    if (lx <= 0.f) return false; // behind camera

    float tan_half = std::tanf(vs.fov_vdeg * 0.5f * kRad);
    float scale    = (sh * 0.5f) / (tan_half * lx);

    screen_out.x = (sw * 0.5f) - ly * scale;
    screen_out.y = (sh * 0.5f) - lz * scale;
    return true;
}

// ---------------------------------------------------------------------------
// Result
// ---------------------------------------------------------------------------

struct AimResult {
    int    entity_id    = -1;
    Angles delta_angles = {};  // add this to current view angles
    float  fov_dist     = 0.f; // screen-space distance from crosshair (px)
};

// ---------------------------------------------------------------------------
// AimAssist
// ---------------------------------------------------------------------------

class AimAssist {
public:
    explicit AimAssist(AimConfig cfg) : cfg_(cfg) {}

    void SetConfig(const AimConfig& cfg) noexcept { cfg_ = cfg; }
    const AimConfig& GetConfig()   const noexcept { return cfg_; }

    // Call once per frame.
    // vs          – current view state (angles + origin)
    // entities    – all enemy entities this frame
    // Returns the aim adjustment to apply, or nullopt if assist is inactive.
    std::optional<AimResult> Update(const ViewState& vs,
                                    const std::vector<AimEntity>& entities) {
        if (cfg_.is_active && !cfg_.is_active()) return std::nullopt;

        Vec2 crosshair{static_cast<float>(cfg_.screen_w) * 0.5f,
                       static_cast<float>(cfg_.screen_h) * 0.5f};

        float    best_dist_sq = std::numeric_limits<float>::max();
        AimResult best{};
        bool      found       = false;

        for (const auto& ent : entities) {
            if (!ent.alive)              continue;
            if (ent.team == 0)           continue; // skip teammates
            if (cfg_.require_visible && !ent.visible) continue;

            float world_dist = (ent.feet - vs.origin).length();
            if (world_dist > cfg_.max_distance) continue;

            Vec3 aim_point = cfg_.aim_head ? ent.head : ent.feet;

            // --- screen-space FOV filter ---
            Vec2 screen_pos;
            if (!WorldToScreen(aim_point, vs, cfg_.screen_w, cfg_.screen_h,
                                screen_pos))
                continue;

            Vec2  offset    = screen_pos - crosshair;
            float dist_sq   = offset.length_sq();
            float fov_px    = (cfg_.screen_h * 0.5f)
                              * std::tanf(cfg_.fov_deg * kRad)
                              / std::tanf(vs.fov_vdeg * 0.5f * kRad);

            if (dist_sq > fov_px * fov_px) continue;

            if (dist_sq < best_dist_sq) {
                best_dist_sq = dist_sq;

                // Target angles toward the aim point.
                Vec3    dir    = aim_point - vs.origin;
                Angles  target = VecToAngles(dir);
                Angles  delta  = AngleDelta(vs.view_angles, target);

                // Apply smoothing — close only a fraction of the gap.
                delta.pitch *= cfg_.smooth;
                delta.yaw   *= cfg_.smooth;

                best = {ent.id, delta, std::sqrtf(dist_sq)};
                found = true;
            }
        }

        return found ? std::make_optional(best) : std::nullopt;
    }

private:
    AimConfig cfg_;
};

// ---------------------------------------------------------------------------
// Example usage (remove or gate behind a flag in production)
// ---------------------------------------------------------------------------
//
// int main() {
//     AimConfig cfg;
//     cfg.fov_deg         = 8.f;
//     cfg.smooth          = 0.15f;
//     cfg.aim_head        = true;
//     cfg.require_visible = false;
//     cfg.screen_w        = 1920;
//     cfg.screen_h        = 1080;
//     cfg.is_active       = []{ return GetAsyncKeyState(VK_RBUTTON) & 0x8000; };
//
//     AimAssist assist(cfg);
//
//     // --- game loop ---
//     while (running) {
//         ViewState vs = GetLocalPlayerView();      // your engine call
//         auto enemies = GetEnemyEntities();        // your engine call
//
//         if (auto result = assist.Update(vs, enemies)) {
//             vs.view_angles.pitch += result->delta_angles.pitch;
//             vs.view_angles.yaw   += result->delta_angles.yaw;
//             SetLocalPlayerView(vs);               // your engine call
//         }
//     }
// }
