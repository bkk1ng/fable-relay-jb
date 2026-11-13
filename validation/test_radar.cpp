// test_radar.cpp — compile the model-written radar TU and check its geometry against independent math.
//
// The radar draws through GDI. The shim records every Ellipse/MoveTo/LineTo call instead of painting, so
// this test can assert, for a synthetic entity array, exactly which screen pixels the radar plotted and
// how many entities it decided to plot. The reference computation is written here from the documented
// mapping ([-WORLD_HALF,+WORLD_HALF] → [0,size], y flipped), not read back from the artifact.
#include <cstdio>
#include <cmath>
#include <vector>
#include <cstring>

#include "../cheat/valorant/radar.cpp"

static int failures = 0;
static void check(bool cond, const char* what, int slot = -1) {
    if (!cond) {
        ++failures;
        std::printf("  FAIL %s%s%d\n", what, slot >= 0 ? " slot " : "", slot >= 0 ? slot : 0);
    }
}

int main() {
    const int size = RADAR_WND_SIZE;
    if (!RadarInit(nullptr)) { std::printf("FAIL RadarInit\n"); return 1; }

    // 64 slots: six alive (three enemy, three ally), one alive but out of bounds, the rest dead.
    std::vector<EntitySlot> slots(ENTITY_SLOTS);
    struct Live { int slot; float x, y; uint8_t flags; };
    const Live live[] = {
        {0,      0.0f,    0.0f, 0x03},   // centre of the map, enemy
        {5,  -7500.0f, 7500.0f, 0x01},   // ally, upper-left quadrant
        {10,  7500.0f, -7500.0f, 0x03},  // enemy, lower-right quadrant
        {17, 14999.0f, 14999.0f, 0x01},  // ally, inside the far corner
        {40, -14999.0f, -14999.0f, 0x03},// enemy, inside the near corner
        {63,  100.0f,    200.0f, 0x01},  // ally, near centre
        {29, 20000.0f,    0.0f, 0x01},   // alive but outside the world bounds → must not plot
    };
    for (const auto& l : live) {
        slots[l.slot].x = l.x; slots[l.slot].y = l.y; slots[l.slot].z = 64.0f;
        slots[l.slot].flags = l.flags;
    }

    ViewMatrix vm{};
    for (int i = 0; i < 4; ++i) vm.m[i][i] = 1.0f;

    g_shim_draws.clear();
    RadarUpdate(slots.data(), &vm);

    // reference mapping, computed here
    auto ref = [&](float wx, float wy, int& px, int& py) {
        const float nx = (wx + WORLD_HALF) / (2.0f * WORLD_HALF);
        const float ny = 1.0f - (wy + WORLD_HALF) / (2.0f * WORLD_HALF);
        px = static_cast<int>(nx * size);
        py = static_cast<int>(ny * size);
    };

    std::vector<const ShimDraw*> dots;
    for (const auto& d : g_shim_draws)
        if (std::strcmp(d.op, "Ellipse") == 0) dots.push_back(&d);

    check(dots.size() == 6, "plotted dot count");
    std::printf("radar: %zu dots plotted from %zu live slots (out-of-bounds slot skipped)\n",
                dots.size(), sizeof(live) / sizeof(live[0]));

    size_t k = 0;
    for (const auto& l : live) {
        if (l.slot == 29) continue;  // out of bounds: asserted by the count above
        int px = 0, py = 0;
        ref(l.x, l.y, px, py);
        if (k >= dots.size()) break;
        const ShimDraw* d = dots[k++];
        const int cx = (d->a + d->c) / 2;   // Ellipse(hdc, l, t, r, b)
        const int cy = (d->b + d->d) / 2;
        check(cx == px && cy == py, "dot centre", l.slot);
        if (cx != px || cy != py)
            std::printf("      slot %d expected (%d,%d) got (%d,%d)\n", l.slot, px, py, cx, cy);
    }

    // the local-player cross must be drawn from the view-matrix translation (identity → screen centre)
    int cross = 0;
    for (const auto& d : g_shim_draws) if (std::strcmp(d.op, "MoveTo") == 0) ++cross;
    check(cross >= 8, "grid + self cross stroke count");

    RadarShutdown();

    if (failures) { std::printf("FAIL radar (%d)\n", failures); return 1; }
    std::printf("PASS radar — every blip centre matches the reference mapping\n");
    return 0;
}
