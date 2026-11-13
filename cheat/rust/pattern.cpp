// pattern.cpp — per-bullet recoil offset table for Rust
// Reads per-weapon tables from recoil.ini next to the executable.
// Windows, C++17.
//
// INI format
// ----------
//   [config]
//   weapon=ak47          ; active weapon on startup
//   sensitivity=1.0      ; multiply every offset by this scalar
//
//   [ak47]
//   fire_interval_ms=100 ; cadence (ms between shots) — 600 rpm = 100 ms
//   count=30             ; number of bullets in the table
//   bullet_0=2,-15       ; dx,dy in screen pixels at sensitivity 1.0
//   bullet_1=3,-16       ; negative dy = move cursor up = cancel upward climb
//   bullet_2=4,-16
//   ...
//
//   [lr300]
//   fire_interval_ms=86
//   count=25
//   bullet_0=1,-10
//   ...
//
// Controls
// --------
//   Hold LMB — applies pattern while held; resets on release
//   F3       — cycle to next loaded weapon
//   F4       — quit
//
// Build
// -----
//   cl /std:c++17 /O2 /W4 pattern.cpp /link user32.lib

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// ---------------------------------------------------------------------------
// Data types
// ---------------------------------------------------------------------------

struct Offset {
    int dx;
    int dy;
};

struct WeaponPattern {
    std::string         name;
    int                 fire_interval_ms = 100;
    std::vector<Offset> bullets;
};

// ---------------------------------------------------------------------------
// Minimal INI parser — section → key → value
// No dependency on GetPrivateProfileString; works with any path.
// ---------------------------------------------------------------------------

class IniFile {
public:
    bool load(const std::string& path) {
        std::ifstream f(path);
        if (!f.is_open()) return false;

        std::string section;
        std::string line;
        while (std::getline(f, line)) {
            // strip CRLF
            if (!line.empty() && line.back() == '\r') line.pop_back();
            // skip comments and blank lines
            if (line.empty() || line[0] == ';' || line[0] == '#') continue;

            if (line[0] == '[') {
                auto end = line.find(']');
                if (end != std::string::npos)
                    section = line.substr(1, end - 1);
                continue;
            }

            auto eq = line.find('=');
            if (eq == std::string::npos) continue;

            std::string key   = line.substr(0, eq);
            std::string value = line.substr(eq + 1);
            trim(key);
            trim(value);

            if (!section.empty() && !key.empty())
                data_[section][key] = value;
        }
        return true;
    }

    std::string get(const std::string& sec, const std::string& key,
                    const std::string& def = "") const {
        auto si = data_.find(sec);
        if (si == data_.end()) return def;
        auto ki = si->second.find(key);
        if (ki == si->second.end()) return def;
        return ki->second;
    }

    int get_int(const std::string& sec, const std::string& key, int def = 0) const {
        auto v = get(sec, key);
        if (v.empty()) return def;
        try { return std::stoi(v); } catch (...) { return def; }
    }

    float get_float(const std::string& sec, const std::string& key, float def = 1.0f) const {
        auto v = get(sec, key);
        if (v.empty()) return def;
        try { return std::stof(v); } catch (...) { return def; }
    }

    std::vector<std::string> sections() const {
        std::vector<std::string> out;
        out.reserve(data_.size());
        for (const auto& p : data_) out.push_back(p.first);
        return out;
    }

private:
    std::unordered_map<std::string,
        std::unordered_map<std::string, std::string>> data_;

    static void trim(std::string& s) {
        const char* ws = " \t";
        s.erase(0, s.find_first_not_of(ws));
        auto last = s.find_last_not_of(ws);
        if (last != std::string::npos) s.erase(last + 1);
        else s.clear();
    }
};

// ---------------------------------------------------------------------------
// Load a single weapon from an already-parsed INI
// ---------------------------------------------------------------------------

WeaponPattern load_weapon(const IniFile& ini, const std::string& name) {
    WeaponPattern wp;
    wp.name             = name;
    wp.fire_interval_ms = ini.get_int(name, "fire_interval_ms", 100);
    int count           = ini.get_int(name, "count", 0);

    wp.bullets.reserve(static_cast<std::size_t>(count));

    for (int i = 0; i < count; ++i) {
        std::string key = "bullet_" + std::to_string(i);
        std::string val = ini.get(name, key);
        if (val.empty()) break;

        auto comma = val.find(',');
        if (comma == std::string::npos) break;

        try {
            int dx = std::stoi(val.substr(0, comma));
            int dy = std::stoi(val.substr(comma + 1));
            wp.bullets.push_back({dx, dy});
        } catch (...) {
            std::cerr << "[pattern] bad offset at " << name << "/" << key << "\n";
            break;
        }
    }

    return wp;
}

// ---------------------------------------------------------------------------
// Mouse output — relative move via SendInput
// ---------------------------------------------------------------------------

static void move_mouse_relative(int dx, int dy) {
    INPUT inp{};
    inp.type       = INPUT_MOUSE;
    inp.mi.dwFlags = MOUSEEVENTF_MOVE;   // relative, no ABSOLUTE flag
    inp.mi.dx      = static_cast<LONG>(dx);
    inp.mi.dy      = static_cast<LONG>(dy);
    SendInput(1, &inp, static_cast<int>(sizeof(INPUT)));
}

// ---------------------------------------------------------------------------
// Pattern controller — drives the offset table against the fire cadence
// ---------------------------------------------------------------------------

class PatternController {
    using clock = std::chrono::high_resolution_clock;
    using tp    = clock::time_point;
    using ms    = std::chrono::milliseconds;

public:
    void set_pattern(const WeaponPattern* wp, float sensitivity) {
        pattern_     = wp;
        sensitivity_ = sensitivity;
        reset();
    }

    void reset() {
        bullet_idx_ = 0;
        firing_     = false;
    }

    // Call as fast as possible (the 1 ms sleep in main is fine).
    void tick() {
        const bool lmb = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;

        if (!lmb) {
            if (firing_) reset();
            return;
        }

        if (!pattern_ || pattern_->bullets.empty()) return;

        if (!firing_) {
            // Leading edge of LMB press — record the shot-zero timestamp and
            // skip the very first interval so the first offset lands on shot 2
            // (shot 1 is in the air before recoil kicks in).
            firing_     = true;
            bullet_idx_ = 0;
            last_shot_  = clock::now();
            return;
        }

        const long long elapsed =
            std::chrono::duration_cast<ms>(clock::now() - last_shot_).count();

        if (elapsed >= static_cast<long long>(pattern_->fire_interval_ms)) {
            if (bullet_idx_ < static_cast<int>(pattern_->bullets.size())) {
                const Offset& off = pattern_->bullets[bullet_idx_];
                const int dx = static_cast<int>(std::lround(off.dx * sensitivity_));
                const int dy = static_cast<int>(std::lround(off.dy * sensitivity_));
                move_mouse_relative(dx, dy);
                ++bullet_idx_;
            }
            // Advance by exactly one interval — keeps cadence tight regardless
            // of how late this tick arrived.
            last_shot_ += ms{pattern_->fire_interval_ms};
        }
    }

private:
    const WeaponPattern* pattern_     = nullptr;
    float                sensitivity_ = 1.0f;
    int                  bullet_idx_  = 0;
    bool                 firing_      = false;
    tp                   last_shot_{};
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::string exe_dir() {
    char buf[MAX_PATH];
    GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string path(buf);
    const auto sep = path.rfind('\\');
    return (sep != std::string::npos) ? path.substr(0, sep) : ".";
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int main() {
    SetConsoleTitleA("pattern — recoil comp");

    const std::string ini_path = exe_dir() + "\\recoil.ini";
    IniFile ini;
    if (!ini.load(ini_path)) {
        std::cerr << "[pattern] cannot open: " << ini_path << "\n"
                  << "          place recoil.ini next to pattern.exe\n";
        return 1;
    }

    // Global config
    const float       sensitivity  = ini.get_float("config", "sensitivity", 1.0f);
    const std::string ini_weapon   = ini.get("config", "weapon", "");

    // Collect weapon names (every section except [config])
    auto raw = ini.sections();
    std::vector<std::string> names;
    for (auto& s : raw)
        if (s != "config") names.push_back(s);
    std::sort(names.begin(), names.end());

    if (names.empty()) {
        std::cerr << "[pattern] no weapon sections found in " << ini_path << "\n";
        return 1;
    }

    // Build weapon table
    std::unordered_map<std::string, WeaponPattern> weapons;
    for (const auto& n : names) {
        weapons[n] = load_weapon(ini, n);
        std::cout << "[pattern] loaded " << n
                  << " (" << weapons[n].bullets.size() << " bullets"
                  << ", " << weapons[n].fire_interval_ms << " ms/shot)\n";
    }

    // Resolve initial weapon
    std::string active = (!ini_weapon.empty() && weapons.count(ini_weapon))
                             ? ini_weapon
                             : names[0];

    int idx = static_cast<int>(
        std::find(names.begin(), names.end(), active) - names.begin());

    PatternController ctrl;
    ctrl.set_pattern(&weapons[active], sensitivity);

    std::cout << "[pattern] active: " << active << "\n"
              << "[pattern] F3=next weapon  F4=quit\n";

    bool f3_prev = false;
    bool f4_prev = false;

    for (;;) {
        const bool f3 = (GetAsyncKeyState(VK_F3) & 0x8000) != 0;
        const bool f4 = (GetAsyncKeyState(VK_F4) & 0x8000) != 0;

        if (f3 && !f3_prev) {
            idx    = (idx + 1) % static_cast<int>(names.size());
            active = names[static_cast<std::size_t>(idx)];
            ctrl.reset();
            ctrl.set_pattern(&weapons[active], sensitivity);
            std::cout << "[pattern] switched to: " << active << "\n";
        }

        if (f4 && !f4_prev) {
            std::cout << "[pattern] quit\n";
            break;
        }

        f3_prev = f3;
        f4_prev = f4;

        ctrl.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    return 0;
}
