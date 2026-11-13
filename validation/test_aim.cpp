// test_aim.cpp — compile the model-written aim TU and check projection, target selection, smoothing.
//
// Reference values are computed in this file from the documented UE4 row-vector convention
// (clip = world × M, px = (1 + cx/cw)·W/2, py = (1 − cy/cw)·H/2) instead of being read out of aim.cpp,
// so a wrong matrix convention or a flipped Y in the artifact shows up as a mismatch.
#include <cstdio>
#include <cmath>
#include <vector>

#include "../cheat/valorant/aim.cpp"

static int failures = 0;
static void check(bool cond, const char* what) {
    if (!cond) { ++failures; std::printf("  FAIL %s\n", what); }
}
static bool near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }

// independent projection with the row-vector convention
static bool ref_project(const FVector& w, const float M[4][4], float W, float H, float& px, float& py) {
    const float cx = w.X * M[0][0] + w.Y * M[1][0] + w.Z * M[2][0] + M[3][0];
    const float cy = w.X * M[0][1] + w.Y * M[1][1] + w.Z * M[2][1] + M[3][1];
    const float cw = w.X * M[0][3] + w.Y * M[1][3] + w.Z * M[2][3] + M[3][3];
    if (cw < 0.001f) return false;
    px = (1.0f + cx / cw) * 0.5f * W;
    py = (1.0f - cy / cw) * 0.5f * H;
    return true;
}

int main() {
    const float W = 1920.0f, H = 1080.0f;

    FViewState view{};
    for (int i = 0; i < 4; ++i) view.ViewProjection.M[i][i] = 1.0f;
    view.ScreenWidth = W;
    view.ScreenHeight = H;

    // 1. projection itself: world (0,0,0) maps to the exact centre
    {
        auto p = WorldToScreen(FVector{0.f, 0.f, 0.f}, view);
        check(p.has_value(), "centre projects");
        if (p) check(near(p->X, 960.f) && near(p->Y, 540.f), "centre is (960,540)");
        // and an off-axis point matches the reference formula
        FVector w{0.2f, 0.2f, 0.f};
        float rx = 0, ry = 0;
        const bool ok = ref_project(w, view.ViewProjection.M, W, H, rx, ry);
        auto q = WorldToScreen(w, view);
        check(ok && q.has_value() && near(q->X, rx) && near(q->Y, ry), "off-axis matches reference");
    }

    // 2. target selection: nearest live enemy inside the 240 px cone wins, ally/dead/behind ignored
    {
        std::vector<FEntityState> ents;
        ents.push_back({FVector{0.00f, 0.00f, 0.f}, true,  true});   // dead centre  (dist 0)
        ents.push_back({FVector{0.20f, 0.20f, 0.f}, true,  true});   // ~220 px away (inside cone)
        ents.push_back({FVector{0.01f, 0.01f, 0.f}, true,  false});  // ally even closer — must be ignored
        ents.push_back({FVector{0.02f, 0.02f, 0.f}, false, true});   // dead enemy — ignored
        ents.push_back({FVector{0.60f, 0.60f, 0.f}, true,  true});   // ~880 px away (outside cone)

        FAimState st{};
        FAimConfig cfg{};
        auto r = ComputeAimDelta(view, ents, cfg, st);
        check(r.TargetFound, "target found");
        // nearest enemy is the exact centre → raw delta (0,0), smoothing keeps it 0
        check(near(r.Delta.X, 0.f) && near(r.Delta.Y, 0.f), "nearest enemy selected (centre)");

        // remove the centre enemy: the ~220 px one must be chosen, smoothed by SmoothFactor 0.25
        ents.erase(ents.begin());
        FAimState st2{};
        auto r2 = ComputeAimDelta(view, ents, cfg, st2);
        float rx = 0, ry = 0;
        ref_project(FVector{0.20f, 0.20f, 0.f}, view.ViewProjection.M, W, H, rx, ry);
        const float rawX = rx - W * 0.5f, rawY = ry - H * 0.5f;
        check(r2.TargetFound, "second target found");
        check(near(r2.Delta.X, rawX * 0.25f, 1e-2f) && near(r2.Delta.Y, rawY * 0.25f, 1e-2f),
              "delta is 0.25 × raw offset");
    }

    // 3. smoothing is a pure single step, and the factor is clamped to [0.01, 1]
    {
        const FVector2D out = SmoothDelta(FVector2D{0.f, 0.f}, FVector2D{100.f, -40.f}, 0.25f);
        check(near(out.X, 25.f) && near(out.Y, -10.f), "SmoothDelta 0.25");

        const FVector2D snap = SmoothDelta(FVector2D{10.f, 10.f}, FVector2D{110.f, -30.f}, 1.0f);
        check(near(snap.X, 110.f) && near(snap.Y, -30.f), "factor 1 snaps");

        const FVector2D floor_ = SmoothDelta(FVector2D{0.f, 0.f}, FVector2D{100.f, 0.f}, 0.0f);
        check(near(floor_.X, 1.f), "factor clamped up to 0.01");
    }

    // 4. a point behind the near plane yields no projection and cannot be selected
    {
        FViewState v2{};
        for (int i = 0; i < 4; ++i) v2.ViewProjection.M[i][i] = 1.0f;
        v2.ViewProjection.M[0][3] = -0.5f;   // w = 1 - 0.5·x → negative for x > 2
        v2.ScreenWidth = W; v2.ScreenHeight = H;
        auto p = WorldToScreen(FVector{4.f, 0.f, 0.f}, v2);
        check(!p.has_value(), "behind-plane point rejected");
    }

    if (failures) { std::printf("FAIL aim (%d)\n", failures); return 1; }
    std::printf("PASS aim — projection, cone selection and smoothing match the reference math\n");
    return 0;
}
