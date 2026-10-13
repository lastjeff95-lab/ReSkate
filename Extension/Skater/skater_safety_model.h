#pragma once
// The decisions behind skater safety (skater_safety.h). They have no game in them, so
// Test/skater_safety_tests.cpp runs them on their own; skater_safety.cpp and client_noclip.cpp
// feed them velocities, positions and times.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace dingosdk::skater_safety::model {
using Vec3 = std::array<float, 3>;

// skate. steps its physics at a fixed 60 Hz (Engine/Game/Settings/simulation_time.h), so a
// boost is spread over physics steps, not frames: the same push at any frame rate.
constexpr int boost_steps = 4;
// No body of the skater is left faster than this by a boost or the trainer's push speed (m/s).
constexpr float max_boost_speed = 100.0f;
// After a physics step, a body faster than this, or one whose velocity is not finite, is
// treated as broken physics: nothing the game does on its own comes near it (m/s).
constexpr float absurd_speed = 200.0f;
// A root position further out than this on any axis is treated as broken (debug_skater's own
// bound is 1,000,000).
constexpr float absurd_position = 100000.0f;

inline bool finite(const Vec3 &v) noexcept { return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]); }
inline float dot(const Vec3 &a, const Vec3 &b) noexcept { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

inline bool sane_position(const Vec3 &p) noexcept {
    return finite(p) && std::abs(p[0]) <= absurd_position && std::abs(p[1]) <= absurd_position &&
           std::abs(p[2]) <= absurd_position;
}

// The largest share k in [0, 1] of `delta` that can be added to every body velocity without any
// body ending faster than `cap`. A body that is already at or over `cap`, or a velocity that is
// not finite, allows nothing.
inline float boost_share(std::span<const Vec3> bodies, const Vec3 &delta, float cap) noexcept {
    if (!finite(delta) || !(cap > 0)) return 0;
    const float dd = dot(delta, delta);
    if (dd <= 0) return 1;
    float share = 1;
    for (const auto &v : bodies) {
        if (!finite(v)) return 0;
        const float c = dot(v, v) - cap * cap;
        if (c >= 0) return 0;
        // |v + k delta|^2 = cap^2: dd k^2 + 2 vd k + c = 0 has one positive root while c < 0.
        const float vd = dot(v, delta);
        share = std::min(share, (-vd + std::sqrt(vd * vd - dd * c)) / dd);
    }
    return std::clamp(share, 0.0f, 1.0f);
}

struct BodyCheck {
    bool finite{true};
    float fastest{}; // m/s, of the finite velocities
};
inline BodyCheck check_bodies(std::span<const Vec3> bodies) noexcept {
    BodyCheck result;
    for (const auto &v : bodies) {
        if (!finite(v)) {
            result.finite = false;
            continue;
        }
        result.fastest = std::max(result.fastest, std::sqrt(dot(v, v)));
    }
    return result;
}
inline bool sane(const BodyCheck &check) noexcept { return check.finite && check.fastest <= absurd_speed; }

// The last good grounded position: a place the skater stood on the ground and then stayed
// healthy for `settle_ms` after (in the air is fine). The position a blow-up started from is
// never the one returned to, because the blow-up drops the position still settling.
constexpr std::uint64_t settle_ms = 1000, sample_ms = 250;
struct GoodPosition {
    std::optional<Vec3> trusted, pending;
    std::uint64_t pending_at{}, next_sample{};
    void observe(std::uint64_t now, const Vec3 &position, bool grounded, bool healthy) noexcept {
        if (!healthy) {
            pending.reset();
            return;
        }
        if (pending && now - pending_at >= settle_ms) {
            trusted = pending;
            pending.reset();
        }
        if (grounded && !pending && now >= next_sample && sane_position(position)) {
            pending = position;
            pending_at = now;
            next_sample = now + sample_ms;
        }
    }
};

// The respawn loop: one skater component created more than `loop_creates` times inside
// `loop_window_ms` (the game retries every 32 physics steps, 533 ms, forever). Normal play
// creates a component three times at a spawn and once per bail, seconds apart.
constexpr int loop_creates = 3;
constexpr std::uint64_t loop_window_ms = 5000;
// The loop is over once a skater has stayed healthy this long, or the game stopped creating.
constexpr std::uint64_t stable_ms = 3000, quiet_ms = 10000;
struct LoopWatch {
    std::uintptr_t component{};
    std::array<std::uint64_t, loop_creates + 1> times{}; // the latest creations, a ring
    std::size_t count{}, next{};
    bool looping{};
    std::uint64_t since{}, last{};
};
class LoopDetector {
public:
    // A creation of `component` at `now`. True when it is the one that shows a loop.
    bool creation(std::uintptr_t component, std::uint64_t now) noexcept {
        auto &w = watch_for(component, now);
        w.times[w.next] = now;
        w.next = (w.next + 1) % w.times.size();
        w.count = std::min(w.count + 1, w.times.size());
        w.last = now;
        if (w.looping || w.count < w.times.size()) return false;
        const auto oldest = w.times[w.next]; // the ring's next slot holds the oldest creation
        if (now - oldest > loop_window_ms) return false;
        w.looping = true;
        w.since = now;
        return true;
    }
    bool looping(std::uintptr_t component) const noexcept {
        for (const auto &w : watches_)
            if (w.component == component && w.looping) return true;
        return false;
    }
    bool any_looping() const noexcept {
        return std::any_of(watches_.begin(), watches_.end(), [](const LoopWatch &w) { return w.looping; });
    }
    // The local skater has been healthy since `healthy_since` (0: it is not). Ends every loop once
    // that has lasted `stable_ms`, and every loop the game stopped feeding. True when one ended.
    bool settle(std::uint64_t now, std::uint64_t healthy_since) noexcept {
        bool ended{};
        for (auto &w : watches_) {
            if (!w.looping) continue;
            const bool stayed = healthy_since && now - healthy_since >= stable_ms;
            if (stayed || now - w.last >= quiet_ms) {
                w = LoopWatch{.component = w.component};
                ended = true;
            }
        }
        return ended;
    }
    void reset() noexcept { watches_ = {}; }

private:
    LoopWatch &watch_for(std::uintptr_t component, std::uint64_t now) noexcept {
        LoopWatch *oldest = &watches_[0];
        for (auto &w : watches_) {
            if (w.component == component) return w;
            if (w.last < oldest->last) oldest = &w;
        }
        // A component not seen yet takes the slot used longest ago (AI and remote skaters
        // have components of their own).
        *oldest = LoopWatch{.component = component, .last = now};
        return *oldest;
    }
    std::array<LoopWatch, 4> watches_{};
};
} // namespace dingosdk::skater_safety::model
