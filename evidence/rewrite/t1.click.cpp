// click.cpp — click backend: tap/hold mouse injection driven by an on-target signal + hotkey
#include <windows.h>
#include <chrono>

namespace click {

enum class ClickMode   { Tap, Hold };
enum class MouseButton { Left, Right, Middle, Side1, Side2 };

struct ClickConfig {
    ClickMode   mode             = ClickMode::Tap;
    MouseButton button           = MouseButton::Left;
    int         min_interval_ms   = 50;  // minimum gap between tap presses
    int         tap_down_ms       = 8;   // button-down duration of one tap
};

class ClickBackend {
public:
    explicit ClickBackend(ClickConfig cfg = {}) : cfg_(cfg) {}

    void set_config(const ClickConfig &c) { cfg_ = c; }
    const ClickConfig &config() const { return cfg_; }

    // Per-frame update. on_target = crosshair currently over a valid entity.
    // hotkey_down = activation key currently held by the operator.
    void update(bool on_target, bool hotkey_down);

    // Release any held button and reset internal state (call on unload).
    void release();

private:
    void do_press();
    void do_release();

    ClickConfig cfg_;
    bool        held_           = false;
    long long   last_press_ms_  = 0;

    enum class TapState { Idle, Down };
    TapState tap_state_   = TapState::Idle;
    long long tap_down_ts_ = 0;
};

// ---- timing ---------------------------------------------------------------
static long long now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// ---- SendInput ------------------------------------------------------------
static void send_mouse(DWORD flags, DWORD mouse_data = 0) {
    INPUT inp{};
    inp.type           = INPUT_MOUSE;
    inp.mi.dwFlags     = flags;
    inp.mi.mouseData   = mouse_data;
    inp.mi.dwExtraInfo = GetMessageExtraInfo();
    ::SendInput(1, &inp, sizeof(inp));
}

void ClickBackend::do_press() {
    switch (cfg_.button) {
    case MouseButton::Left:   send_mouse(MOUSEEVENTF_LEFTDOWN);            break;
    case MouseButton::Right:  send_mouse(MOUSEEVENTF_RIGHTDOWN);           break;
    case MouseButton::Middle: send_mouse(MOUSEEVENTF_MIDDLEDOWN);          break;
    case MouseButton::Side1:  send_mouse(MOUSEEVENTF_XDOWN, XBUTTON1);     break;
    case MouseButton::Side2:  send_mouse(MOUSEEVENTF_XDOWN, XBUTTON2);     break;
    }
    held_ = true;
}

void ClickBackend::do_release() {
    switch (cfg_.button) {
    case MouseButton::Left:   send_mouse(MOUSEEVENTF_LEFTUP);               break;
    case MouseButton::Right:  send_mouse(MOUSEEVENTF_RIGHTUP);             break;
    case MouseButton::Middle: send_mouse(MOUSEEVENTF_MIDDLEUP);            break;
    case MouseButton::Side1:  send_mouse(MOUSEEVENTF_XUP, XBUTTON1);       break;
    case MouseButton::Side2:  send_mouse(MOUSEEVENTF_XUP, XBUTTON2);       break;
    }
    held_ = false;
}

void ClickBackend::update(bool on_target, bool hotkey_down) {
    bool active = on_target && hotkey_down;
    long long t = now_ms();

    if (cfg_.mode == ClickMode::Hold) {
        if (active && !held_)
            do_press();
        else if (!active && held_)
            do_release();
        return;
    }

    // Tap mode — press / wait / release state machine, throttled by min_interval_ms.
    switch (tap_state_) {
    case TapState::Idle:
        if (active && t - last_press_ms_ >= cfg_.min_interval_ms) {
            do_press();
            last_press_ms_ = t;
            tap_down_ts_   = t;
            tap_state_     = TapState::Down;
        }
        break;
    case TapState::Down:
        if (!active || t - tap_down_ts_ >= cfg_.tap_down_ms) {
            do_release();
            tap_state_ = TapState::Idle;
        }
        break;
    }
}

void ClickBackend::release() {
    if (held_)
        do_release();
    tap_state_ = TapState::Idle;
}

} // namespace click

// ---- standalone smoke test ------------------------------------------------
// Build:  cl /std:c++17 /DCLICK_SMOKE click.cpp /link user32.lib
#ifdef CLICK_SMOKE
#include <cstdio>
int main() {
    click::ClickConfig cfg{};
    cfg.mode            = click::ClickMode::Tap;
    cfg.button          = click::MouseButton::Left;
    cfg.min_interval_ms = 100;
    cfg.tap_down_ms     = 8;

    click::ClickBackend backend(cfg);
    for (int i = 0; i < 60; ++i) {
        backend.update(true, true);
        Sleep(5);
    }
    backend.update(false, false);
    backend.release();

    // hold-mode quick check
    click::ClickConfig h{};
    h.mode   = click::ClickMode::Hold;
    h.button  = click::MouseButton::Right;
    click::ClickBackend hold(h);
    hold.update(true, true);
    hold.update(false, true);
    hold.release();
    printf("click smoke: ok\n");
    return 0;
}
#endif // CLICK_SMOKE
