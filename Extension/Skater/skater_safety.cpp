#include "skater_safety.h"
#include "client_source_spawn_internal.h"
#include "no_bail.h"
#include "skater_safety_model.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Build/20260929/no_bail.h"
#include "Engine/Game/Multiplayer/session_tools.h"
#include "Extension/Profile/local_profile_runtime.h"
#include "Extension/Trainer/trainer_landing.h"
#include <format>

namespace dingosdk::skater_safety {
namespace {
namespace detail = client_source::detail;
using model::Vec3;
constexpr const char *boost_key = "Safety.BoostLimits", *check_key = "Safety.SkaterCheck", *loop_key = "Safety.LoopBreaker";
constexpr std::uint64_t rescue_gap_ms = 2000, settle_after_ms = 1000, broken_for_ms = 300;
constexpr int max_rescues = 10; // per map: past that the game is not fought any longer

struct Safety {
    std::atomic<std::uintptr_t> base{};
    std::atomic<bool> started{};
    std::atomic<bool> boost{true}, check{true}, loop{true};
    bool options_loaded{};
    // Written by the client tick, read on every physics step.
    SRWLOCK target_lock = SRWLOCK_INIT;
    PhysicsTarget target;
    std::uint64_t target_until{};
    // From the physics step check.
    std::atomic<float> fastest{};
    std::atomic<std::uint64_t> zeroed{}, zeroed_nonfinite{};
    // The latest skater creation of any kind (a spawn, a bail's respawn): the check stands back
    // for `settle_after_ms` after one, while the game places the skater.
    std::atomic<std::uint64_t> last_creation{};
    // Loop breaker: the creation hook and the client tick.
    SRWLOCK loop_lock = SRWLOCK_INIT;
    model::LoopDetector loops;
    std::uintptr_t rebuild_component{}; // the lost local skater's component, until it stays again
    std::uint64_t forced{};
    // Client tick only.
    std::string level;
    std::uintptr_t player{}, component{};
    model::GoodPosition good;
    bool present{};
    Vec3 last_position{};
    std::uint32_t last_state{};
    float last_fastest{};
    std::uint64_t healthy_since{}, seen_zeroed{}, seen_nonfinite{}, next_rescue{};
    std::uint64_t teleport_until{}; // the game's own teleport, and a second after it
    std::uint64_t broken_since{};   // a bound skater whose position is not valid, since then
    int rescues{};
};
Safety &safety() {
    static auto *value = new Safety;
    return *value;
}
const char *on_off(bool on) { return on ? "on" : "off"; }

void load_options(Safety &s) {
    if (s.options_loaded || !profile_runtime::local_profile_store_ready()) return;
    s.options_loaded = true;
    s.boost.store(profile_runtime::local_preference(boost_key).value_or(true));
    s.check.store(profile_runtime::local_preference(check_key).value_or(true));
    s.loop.store(profile_runtime::local_preference(loop_key).value_or(true));
    if (!s.boost.load() || !s.check.load() || !s.loop.load())
        logging::log(logging::Level::info, logging::Channel::skater,
                     "Skater safety from your profile: boost limits {}, physics check {}, loop breaker {}.",
                     on_off(s.boost.load()), on_off(s.check.load()), on_off(s.loop.load()));
}

// The surface under a point: first the floor just below it (a start point under a roof or a
// bridge keeps its own floor), then the topmost surface there (trainer_landing.h's ray).
std::optional<float> ground_near(const Vec3 &p) {
    if (const auto hit = local_ground_height(p[0], p[2], p[1] + 2.0f, p[1] - 100.0f)) return hit;
    const auto first = trainer::landing::first_ray(p[1]);
    if (const auto hit = local_ground_height(p[0], p[2], first.top, first.bottom)) return hit;
    if (const auto retry = trainer::landing::retry_ray(first)) return local_ground_height(p[0], p[2], retry->top, retry->bottom);
    return std::nullopt;
}
std::string ground_text(const std::optional<float> &ground) {
    return ground ? std::format("{:.1f}", *ground) : std::string("not found");
}

// Sends the local skater onto the ground at `where`. `trusted`: a last good grounded position,
// whose own height may stand in when no ground is found (collision still streaming in).
void send_to_ground(Safety &s, const Vec3 &where, bool trusted, const char *why, std::uint64_t now) {
    if (now < s.next_rescue || s.rescues >= max_rescues) return;
    s.next_rescue = now + rescue_gap_ms;
    if (multiplayer_session_active() && !session_noclip_allowed()) {
        logging::log(logging::Level::warning, logging::Channel::skater,
                     "Skater safety: {}; the host has turned off teleporting, so the skater is not moved.", why);
        return;
    }
    const auto ground = ground_near(where);
    if (!ground && !trusted) {
        logging::log(logging::Level::warning, logging::Channel::skater,
                     "Skater safety: {}; no ground found under ({:.1f}, {:.1f}, {:.1f}) yet, so the skater is not moved.", why,
                     where[0], where[1], where[2]);
        return;
    }
    const Vec3 to{where[0], (ground ? *ground : where[1]) + trainer::landing::stand_height, where[2]};
    if (!teleport_local_skater(to)) {
        logging::log(logging::Level::warning, logging::Channel::skater, "Skater safety: {}; the game's teleport is unavailable.", why);
        return;
    }
    ++s.rescues;
    logging::log(logging::Level::warning, logging::Channel::skater,
                 "Skater safety: {}; sending the skater to {} ({:.1f}, {:.1f}, {:.1f}), ground {} ({} of {} on this map).", why,
                 trusted ? "its last good grounded position" : "the ground under where it appeared", to[0], to[1], to[2],
                 ground_text(ground), s.rescues, max_rescues);
}

void reset_map(Safety &s, const std::string &level) {
    s.level = level;
    s.player = s.component = 0;
    s.good = {};
    s.present = false;
    s.healthy_since = 0;
    s.broken_since = 0;
    s.next_rescue = 0;
    s.rescues = 0;
    AcquireSRWLockExclusive(&s.loop_lock);
    s.loops.reset();
    s.rebuild_component = 0;
    ReleaseSRWLockExclusive(&s.loop_lock);
}

void tick(Safety &s, std::uintptr_t client, const std::string &level) {
    load_options(s);
    const auto now = GetTickCount64();
    const auto base = s.base.load(std::memory_order_acquire);
    // The local skater: the player-bound entity, its root transform and its physics core.
    overlay::DebugModel skater;
    std::uintptr_t bound{}, player{}, component{}, core{};
    try {
        (void)detail::debug_skater(base, client, skater, &bound);
    } catch (...) {
        skater.skater_position_valid = false; // `bound` says whether the entity was there
    }
    if (bound) {
        try {
            detail::SourceReader reader;
            player = reader.pointer(bound, 0xf8);
            component = reader.pointer(bound, 0x628);
            detail::source_require(reader.pointer(component) == base + addr::engine::skater_component_vtable, "Skater component changed.");
            core = reader.pointer(component, 0x70);
            detail::source_require(reader.pointer(core) == base + addr::no_bail::bail_core_vtable, "Skater physics is unavailable.");
            // A teleport under way (fast travel, the trainer, a waypoint): the skater is moved on
            // purpose, and is neither lost nor put back while it lasts.
            if (reader.value<std::uint8_t>(bound, 0x7e0)) s.teleport_until = now + 1000;
            reader.verify();
        } catch (...) {
            component = core = 0;
        }
    }
    const bool teleporting = now < s.teleport_until;
    const bool respawning = now < s.last_creation.load(std::memory_order_acquire) + settle_after_ms;
    // Rays and teleports only into an active level, never one being loaded or torn down.
    std::uint32_t client_state{};
    const bool level_active = memory::peek(client + 0xc4, client_state) && (client_state == 13 || client_state == 21);
    // A new map (or a new local player, which a level load makes): nothing from before applies.
    if (level != s.level || (player && s.player && player != s.player)) reset_map(s, level);
    if (player) s.player = player;

    const float fastest = s.fastest.load(std::memory_order_relaxed);
    const auto zeroed = s.zeroed.load(std::memory_order_acquire), nonfinite = s.zeroed_nonfinite.load(std::memory_order_acquire);
    const bool new_zeroed = zeroed != s.seen_zeroed, new_nonfinite = nonfinite != s.seen_nonfinite;
    s.seen_zeroed = zeroed;
    s.seen_nonfinite = nonfinite;
    const bool present = bound && skater.skater_position_valid && model::sane_position(skater.skater_position);
    const bool healthy = present && !new_zeroed && fastest <= model::absurd_speed;
    bool grounded{};
    if (present) {
        watch_physics_state(client, bound);
        const auto watch = watched_physics_state();
        s.last_state = watch.valid ? watch.state : 0;
        // Riding the ground (the 100s) at a speed the game keeps on its own.
        grounded = watch.valid && watch.state >= 100 && watch.state < 200 && fastest < 30.0f;
    }
    s.good.observe(now, skater.skater_position, grounded, healthy);
    if (healthy) {
        if (!s.healthy_since) s.healthy_since = now;
    } else {
        s.healthy_since = 0;
    }

    AcquireSRWLockExclusive(&s.target_lock);
    s.target = s.check.load() && bound && core ? PhysicsTarget{client, bound, core} : PhysicsTarget{};
    s.target_until = now + 500;
    ReleaseSRWLockExclusive(&s.target_lock);

    if (present && component && component != s.component) {
        s.component = component;
        logging::log(logging::Level::info, logging::Channel::skater,
                     "Skater safety: watching the local skater (entity 0x{:x}, component 0x{:x}) at ({:.1f}, {:.1f}, {:.1f}).", bound,
                     component, skater.skater_position[0], skater.skater_position[1], skater.skater_position[2]);
    }
    // What the skater was doing when it went: the evidence for why the game rebuilds it.
    if (s.present && !present) {
        logging::log(logging::Level::info, logging::Channel::skater,
                     "Skater safety: local skater lost ({}) at ({:.1f}, {:.1f}, {:.1f}): physics state {}, fastest body {:.1f} m/s, ground below {}.",
                     bound ? "its position is no longer valid" : "its entity is gone", s.last_position[0], s.last_position[1],
                     s.last_position[2], s.last_state, s.last_fastest,
                     level_active ? ground_text(ground_near(s.last_position)) : std::string("not looked for (level not active)"));
        if (!bound && !teleporting && s.loop.load() && s.component) {
            AcquireSRWLockExclusive(&s.loop_lock);
            const bool fresh = s.rebuild_component != s.component;
            s.rebuild_component = s.component;
            ReleaseSRWLockExclusive(&s.loop_lock);
            if (fresh)
                logging::write(logging::Level::info, logging::Channel::skater, "Loop breaker: the skater's rebuild gets the game's ground check.");
        }
    }
    if (present) {
        s.last_position = skater.skater_position;
        s.last_fastest = fastest;
    }
    s.present = present;

    if (bound && !present && level_active) {
        if (!s.broken_since) s.broken_since = now;
    } else {
        s.broken_since = 0;
    }
    if (s.check.load() && level_active) {
        if (new_zeroed)
            logging::log(logging::Level::warning, logging::Channel::skater,
                         "Skater safety: the skater's physics went bad ({}); its velocities were zeroed{}.",
                         new_nonfinite ? "a velocity that is not finite" : std::format("a body at {:.0f} m/s", fastest),
                         s.good.trusted ? "" : ", and there is no good grounded position on this map yet");
        const bool broken = s.broken_since && now - s.broken_since >= broken_for_ms;
        if ((new_zeroed || broken) && !teleporting && !respawning && s.good.trusted)
            send_to_ground(s, *s.good.trusted, true,
                           new_zeroed ? "its physics went bad" : "the skater's position is not finite or out of bounds", now);
    }

    bool ended{}, looping{};
    std::uint64_t forced{};
    AcquireSRWLockExclusive(&s.loop_lock);
    ended = s.loops.settle(now, s.healthy_since);
    looping = s.loops.any_looping();
    if (s.healthy_since && now - s.healthy_since >= model::stable_ms) s.rebuild_component = 0;
    forced = s.forced;
    ReleaseSRWLockExclusive(&s.loop_lock);
    if (ended)
        logging::log(logging::Level::success, logging::Channel::skater,
                     "Loop breaker: the respawn loop is over ({} skater creations given the ground check so far).", forced);
    // The skater showed up while the game is still looping: put it somewhere that holds.
    if (s.loop.load() && looping && present && !teleporting && level_active)
        send_to_ground(s, s.good.trusted ? *s.good.trusted : skater.skater_position, s.good.trusted.has_value(), "respawn loop", now);
}

std::string status_text(const Safety &s) {
    const auto position = s.good.trusted ? std::format("({:.1f}, {:.1f}, {:.1f})", (*s.good.trusted)[0], (*s.good.trusted)[1],
                                                       (*s.good.trusted)[2])
                                         : std::string("none yet");
    return std::format("Skater safety: boost limits {} ({:.0f} m/s, {} physics steps), physics check {} ({:.0f} m/s), loop breaker {} "
                       "(more than {} creations in {} s). Last good grounded position on this map: {}. Teleports used here: {} of {}.",
                       on_off(s.boost.load()), model::max_boost_speed, model::boost_steps, on_off(s.check.load()),
                       model::absurd_speed, on_off(s.loop.load()), model::loop_creates, model::loop_window_ms / 1000, position,
                       s.rescues, max_rescues);
}
} // namespace

bool start(std::uintptr_t image_base) noexcept {
    auto &s = safety();
    s.base.store(image_base, std::memory_order_release);
    s.started.store(true, std::memory_order_release);
    logging::write(logging::Level::info, logging::Channel::skater,
                   "Skater safety ready: boost limits, physics check and loop breaker (switch with `safety` in the console).");
    return true;
}

void on_client_tick(std::uintptr_t client, const std::string &level) noexcept {
    auto &s = safety();
    if (!s.started.load(std::memory_order_acquire)) return;
    try {
        tick(s, client, level);
    } catch (...) {}
}

std::uint8_t creation_ground_check(std::uintptr_t component, std::uint8_t requested) noexcept {
    auto &s = safety();
    if (!s.started.load(std::memory_order_acquire)) return requested;
    const auto now = GetTickCount64();
    s.last_creation.store(now, std::memory_order_release);
    if (!s.loop.load(std::memory_order_acquire)) return requested;
    AcquireSRWLockExclusive(&s.loop_lock);
    const bool started = s.loops.creation(component, now);
    const bool looping = s.loops.looping(component);
    const bool rebuild = component && component == s.rebuild_component;
    const bool force = requested == 0 && (looping || rebuild);
    const auto forced = force ? ++s.forced : s.forced;
    ReleaseSRWLockExclusive(&s.loop_lock);
    if (started)
        logging::log(logging::Level::warning, logging::Channel::skater,
                     "Loop breaker: skater component 0x{:x} was created more than {} times in {} s without a skater that stays "
                     "(the respawn loop); its creations get the game's ground check until one does.",
                     component, model::loop_creates, model::loop_window_ms / 1000);
    if (force && (forced <= 3 || forced % 10 == 0))
        logging::log(logging::Level::info, logging::Channel::skater, "Loop breaker: skater creation given the ground check ({}; {} so far).",
                     looping ? "respawn loop" : "rebuild after the skater was lost", forced);
    return force ? std::uint8_t{1} : requested;
}

PhysicsTarget physics_target() noexcept {
    auto &s = safety();
    if (!s.started.load(std::memory_order_acquire) || !s.check.load(std::memory_order_acquire)) return {};
    AcquireSRWLockShared(&s.target_lock);
    const auto target = s.target;
    const auto until = s.target_until;
    ReleaseSRWLockShared(&s.target_lock);
    const auto now = GetTickCount64();
    if (now < s.last_creation.load(std::memory_order_acquire) + settle_after_ms) return {};
    return now < until ? target : PhysicsTarget{};
}

void report_physics(float fastest, bool zeroed, bool all_finite) noexcept {
    auto &s = safety();
    s.fastest.store(fastest, std::memory_order_relaxed);
    if (!zeroed) return;
    if (!all_finite) s.zeroed_nonfinite.fetch_add(1, std::memory_order_release);
    s.zeroed.fetch_add(1, std::memory_order_release);
}

bool boost_limits() noexcept {
    auto &s = safety();
    return s.started.load(std::memory_order_acquire) && s.boost.load(std::memory_order_acquire);
}

std::string command(const std::vector<std::string> &words) {
    auto &s = safety();
    const auto verb = words.empty() ? std::string("status") : words[0];
    if (verb == "status") return status_text(s);
    std::atomic<bool> *option = verb == "boost" ? &s.boost : verb == "check" ? &s.check : verb == "loop" ? &s.loop : nullptr;
    const char *key = verb == "boost" ? boost_key : verb == "check" ? check_key : loop_key;
    if (!option || words.size() < 2 || (words[1] != "on" && words[1] != "off"))
        return "error: usage: safety [status] | safety boost|check|loop on|off";
    const bool on = words[1] == "on";
    option->store(on, std::memory_order_release);
    profile_runtime::set_local_preference(key, on);
    logging::log(logging::Level::info, logging::Channel::skater, "Skater safety: {} switched {} (kept in your profile).", verb, on_off(on));
    return status_text(s);
}
} // namespace dingosdk::skater_safety
