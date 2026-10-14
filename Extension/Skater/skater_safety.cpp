#include "skater_safety.h"
#include "client_source_spawn_internal.h"
#include "no_bail.h"
#include "skater_safety_model.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Build/20260929/local_placements.h"
#include "Engine/Game/Build/20260929/no_bail.h"
#include "Engine/Game/Multiplayer/session_tools.h"
#include "Engine/Game/Settings/simulation_time.h"
#include "Extension/HallOfMeat/hall_of_meat_skater.h"
#include "Extension/Profile/local_profile_runtime.h"
#include "Extension/Settings/named_settings.h"
#include "Extension/Trainer/trainer_landing.h"
#include <charconv>
#include <format>
#include <utility>

namespace dingosdk::skater_safety {
namespace {
namespace detail = client_source::detail;
using model::Vec3;
constexpr const char *boost_key = "Safety.BoostLimits", *check_key = "Safety.SkaterCheck", *loop_key = "Safety.LoopBreaker",
                     *rate_key = "Safety.SimRate60";
// The simulation rate the SDK's own tuning was measured at (Engine/Game/Settings/simulation_time.h).
constexpr const char *sim_rate = "SimulationTime.ForceSimRate", *max_sim_fps = "SimulationTime.MaxSimFps";
constexpr std::uint32_t wanted_rate = 60;
constexpr std::uint64_t rescue_gap_ms = 2000, settle_after_ms = 1000, broken_for_ms = 300, stuck_teleport_ms = 3000;
constexpr int max_rescues = 10; // per map: past that the game is not fought any longer
// Repeated lines (a loop repeats every 533 ms): the first few per map, then one in this many.
constexpr int log_first = 5, log_every = 20;

struct Safety {
    std::atomic<std::uintptr_t> base{};
    std::atomic<bool> started{};
    std::atomic<bool> boost{true}, check{true}, loop{true}, rate{true};
    bool options_loaded{};
    // The simulation rate: what the game had when it was raised to `wanted_rate` (client tick only).
    struct SimRate {
        bool rate_raised{}, max_raised{};
        std::string found_rate, found_max, last_seen;
        float found_step{};
        int lines{};
        std::uint64_t next_look{};
    } sim;
    // Raw physics bodies for the log, one healthy and one broken per map (body_dump_wanted).
    std::atomic<bool> healthy_dumped{}, broken_dumped{};
    SRWLOCK dump_lock = SRWLOCK_INIT;
    std::optional<BodyDump> dump;
    // Written by the client tick, read on every physics step.
    SRWLOCK target_lock = SRWLOCK_INIT;
    PhysicsTarget target;
    std::uint64_t target_until{};
    std::atomic<bool> looping{}; // a respawn loop is under way: the check runs through creations
    // From the physics step check.
    std::atomic<float> fastest{}, seconds{}, board_height{};
    std::atomic<int> nonfinite_bodies{};
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
    int rescues{}, lost_lines{}, broken_lines{};
    // The game's teleport manager (the one that respawns the skater): its state, 0 while idle.
    std::uint32_t teleport_state{};
    std::uint64_t teleport_busy_since{};
    bool teleport_dumped{}, teleport_wait_logged{};
    int teleport_lines{};
};
Safety &safety() {
    static auto *value = new Safety;
    return *value;
}
const char *on_off(bool on) { return on ? "on" : "off"; }
bool log_due(int &count) { return ++count <= log_first || count % log_every == 0; }

void load_options(Safety &s) {
    if (s.options_loaded || !profile_runtime::local_profile_store_ready()) return;
    s.options_loaded = true;
    s.boost.store(profile_runtime::local_preference(boost_key).value_or(true));
    s.check.store(profile_runtime::local_preference(check_key).value_or(true));
    s.loop.store(profile_runtime::local_preference(loop_key).value_or(true));
    s.rate.store(profile_runtime::local_preference(rate_key).value_or(true));
    if (!s.boost.load() || !s.check.load() || !s.loop.load() || !s.rate.load())
        logging::log(logging::Level::info, logging::Channel::skater,
                     "Skater safety from your profile: boost limits {}, physics check {}, loop breaker {}, 60 Hz physics {}.",
                     on_off(s.boost.load()), on_off(s.check.load()), on_off(s.loop.load()), on_off(s.rate.load()));
}

std::optional<float> number(const std::string &text) {
    float value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || !std::isfinite(value)) return std::nullopt;
    return value;
}
bool failed(const std::string &result) { return result.starts_with("error"); }

// The game's simulation rate: logged when it changes, and raised to `wanted_rate` offline while
// `rate` is on (the steps first, as Hall of Meat's slow motion does, then the live skater's own
// step, which the game sets only when it builds a skater). Given back in a session or when off.
void update_sim_rate(Safety &s, std::uint64_t now) {
    if (now < s.sim.next_look) return;
    s.sim.next_look = now + 1000;
    const auto rate_text = named_setting_value(sim_rate), max_text = named_setting_value(max_sim_fps);
    if (!rate_text || !max_text) return;
    const float step = hall_of_meat::step_length();
    const auto seen = std::format("{}|{}|{:.5f}", *rate_text, *max_text, step);
    if (seen != s.sim.last_seen) {
        s.sim.last_seen = seen;
        if (log_due(s.sim.lines)) logging::log(logging::Level::info, logging::Channel::skater,
                     "Skater safety: the game's simulation is SimulationTime.ForceSimRate {}, MaxSimFps {}; the local skater steps {}.",
                     *rate_text, *max_text,
                     step > 0 ? std::format("{:.5f} s ({} Hz)", step, simulation_time::rate_of(step)) : std::string("(no skater)"));
    }
    const auto ours = std::to_string(wanted_rate);
    const bool wanted = s.rate.load() && !multiplayer_session_active();
    if ((s.sim.rate_raised || s.sim.max_raised) && !wanted) {
        // Only what still holds ours goes back: a value changed from elsewhere meanwhile stays.
        if (s.sim.rate_raised && named_setting_value(sim_rate) == ours) (void)change_named_setting(sim_rate, s.sim.found_rate, false);
        if (s.sim.max_raised && named_setting_value(max_sim_fps) == ours) (void)change_named_setting(max_sim_fps, s.sim.found_max, false);
        if (s.sim.found_step > 0) (void)hall_of_meat::set_step_length(s.sim.found_step);
        s.sim.rate_raised = s.sim.max_raised = false;
        logging::log(logging::Level::info, logging::Channel::skater, "Skater safety: the game's own simulation rate is back (ForceSimRate {}, "
                     "MaxSimFps {}){}.", s.sim.found_rate, s.sim.found_max, multiplayer_session_active() ? " for the multiplayer session" : "");
        return;
    }
    if (!wanted || s.sim.rate_raised || s.sim.max_raised) return;
    // Either setting below 60 holds the simulation below 60: only that one is raised.
    const auto rate = number(*rate_text), max = number(*max_text);
    const bool rate_low = rate && *rate < static_cast<float>(wanted_rate);
    const bool max_low = max && *max > 0 && *max < static_cast<float>(wanted_rate);
    if (!rate_low && !max_low) {
        if (step > simulation_time::step_seconds(wanted_rate) * 1.5f) {
            // The skater steps slowly although both settings allow 60: not ours to force.
            logging::log(logging::Level::warning, logging::Channel::skater,
                         "Skater safety: the local skater steps at {} Hz although ForceSimRate is {} and MaxSimFps {}; left as it is.",
                         simulation_time::rate_of(step), *rate_text, *max_text);
            s.sim.next_look = now + 60000;
        }
        return;
    }
    s.sim.found_rate = *rate_text;
    s.sim.found_max = *max_text;
    s.sim.found_step = step;
    s.sim.rate_raised = rate_low && !failed(change_named_setting(sim_rate, ours, false));
    s.sim.max_raised = max_low && !failed(change_named_setting(max_sim_fps, ours, false));
    if ((rate_low && !s.sim.rate_raised) || (max_low && !s.sim.max_raised)) {
        logging::log(logging::Level::warning, logging::Channel::skater,
                     "Skater safety: could not raise the simulation to {} Hz (ForceSimRate {}, MaxSimFps {}).", wanted_rate, *rate_text, *max_text);
        s.sim.next_look = now + 60000;
        if (!s.sim.rate_raised && !s.sim.max_raised) return;
    }
    const bool skater = hall_of_meat::set_step_length(simulation_time::step_seconds(wanted_rate));
    logging::log(logging::Level::warning, logging::Channel::skater,
                 "Skater safety: the game held physics below {} Hz (ForceSimRate {}, MaxSimFps {}; skater step {}); raised to {} Hz{}. "
                 "`safety rate off` gives the game its own rate back.",
                 wanted_rate, *rate_text, *max_text, step > 0 ? std::format("{:.5f} s", step) : std::string("not read yet"), wanted_rate,
                 skater ? ", and the local skater steps at it now" : "; skaters built from now on step at it");
}

void log_body_dump(const BodyDump &dump) {
    const auto lines = [&](const char *which, const std::array<std::uint32_t, 76> &words) {
        for (std::size_t row = 0; row < words.size(); row += 16) {
            std::string line;
            for (std::size_t i = row; i < std::min(row + 16, words.size()); ++i) line += std::format("{:08x} ", words[i]);
            logging::log(logging::Level::info, logging::Channel::skater, "Skater safety: {} {} body +0x{:03x}: {}",
                         dump.broken ? "broken" : "healthy", which, row * 4, line);
        }
    };
    lines("board", dump.board);
    lines("skeleton", dump.rig);
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

// The game's teleport manager (Engine/Game/Build/20260929/local_placements.h): its state, or
// empty when it cannot be read. Its first uint32 is 0 while idle.
std::optional<std::uint32_t> teleport_manager_state(std::uintptr_t base, std::uintptr_t *manager_out = nullptr) {
    std::uintptr_t manager{};
    std::uint32_t state{};
    if (!memory::peek(base + addr::local_placements::teleport_manager, manager) || !manager || !memory::peek(manager, state))
        return std::nullopt;
    if (manager_out) *manager_out = manager;
    return state;
}

// Logs what the teleport manager does: the game's respawns go through it, and a manager that
// never goes idle again is what keeps a respawn loop alive.
void watch_teleport_manager(Safety &s, std::uintptr_t base, std::uint64_t now) {
    std::uintptr_t manager{};
    const auto state = teleport_manager_state(base, &manager).value_or(0);
    if (state != s.teleport_state) {
        if (log_due(s.teleport_lines)) {
            if (state && !s.teleport_state)
                logging::log(logging::Level::info, logging::Channel::skater, "Skater safety: the game's teleport manager is busy (state {}).", state);
            else if (!state)
                logging::log(logging::Level::info, logging::Channel::skater,
                             "Skater safety: the game's teleport manager is idle again after {} ms (was state {}).", now - s.teleport_busy_since,
                             s.teleport_state);
            else
                logging::log(logging::Level::info, logging::Channel::skater, "Skater safety: the game's teleport manager went from state {} to {}.",
                             s.teleport_state, state);
        }
        if (state && !s.teleport_state) s.teleport_busy_since = now;
        if (!state) s.teleport_dumped = s.teleport_wait_logged = false;
        s.teleport_state = state;
    }
    // Busy for seconds: what the manager holds is the evidence for why it does not finish.
    if (state && !s.teleport_dumped && now - s.teleport_busy_since >= stuck_teleport_ms) {
        s.teleport_dumped = true;
        std::array<std::uint32_t, 64> words{};
        if (!memory::peek_bytes(manager, words.data(), sizeof(words))) return;
        logging::log(logging::Level::warning, logging::Channel::skater,
                     "Skater safety: the game's teleport manager has been busy (state {}) for {} ms. Its first 256 bytes follow.", state,
                     now - s.teleport_busy_since);
        for (std::size_t row = 0; row < words.size(); row += 16) {
            std::string line;
            for (std::size_t i = row; i < row + 16; ++i) line += std::format("{:08x} ", words[i]);
            logging::log(logging::Level::warning, logging::Channel::skater, "Skater safety: teleport manager +0x{:03x}: {}", row * 4, line);
        }
    }
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
    // A busy teleport manager would only hold a new teleport until it is dropped.
    if (s.teleport_state) {
        if (!s.teleport_wait_logged)
            logging::log(logging::Level::warning, logging::Channel::skater,
                         "Skater safety: {}; the game's teleport manager is busy (state {}), so a teleport would never be sent.", why,
                         s.teleport_state);
        s.teleport_wait_logged = true;
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

// The moment a bound skater's transform stops being valid: what was wrong with it.
void log_broken(Safety &s, const char *reason, const detail::SkaterRead &found, std::uint8_t teleport_flag) {
    const auto &m = found.matrix;
    const auto matrix = found.matrix_read
        ? std::format("right ({:.3g}, {:.3g}, {:.3g}) up ({:.3g}, {:.3g}, {:.3g}) forward ({:.3g}, {:.3g}, {:.3g}) position ({:.3g}, {:.3g}, {:.3g})",
                      m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10], m[12], m[13], m[14])
        : std::string("not readable");
    logging::log(logging::Level::warning, logging::Channel::skater,
                 "Skater safety: the bound skater's transform is not valid ({}): {}. Bodies: {} of 32 velocities not finite, fastest "
                 "{:.1f} m/s, step {:.4g} s, board height {:.4g}; teleport flag {}, teleport manager state {} ({} so far on this map).",
                 reason, matrix, s.nonfinite_bodies.load(), s.fastest.load(), s.seconds.load(), s.board_height.load(), teleport_flag,
                 s.teleport_state, s.broken_lines);
}

void reset_map(Safety &s, const std::string &level) {
    s.level = level;
    s.player = s.component = 0;
    s.good = {};
    s.present = false;
    s.healthy_since = 0;
    s.broken_since = 0;
    s.next_rescue = 0;
    s.rescues = s.lost_lines = s.broken_lines = s.teleport_lines = 0;
    s.healthy_dumped.store(false, std::memory_order_release);
    s.broken_dumped.store(false, std::memory_order_release);
    AcquireSRWLockExclusive(&s.loop_lock);
    s.loops.reset();
    s.rebuild_component = 0;
    ReleaseSRWLockExclusive(&s.loop_lock);
    s.looping.store(false, std::memory_order_release);
}

void tick(Safety &s, std::uintptr_t client, const std::string &level) {
    load_options(s);
    const auto now = GetTickCount64();
    const auto base = s.base.load(std::memory_order_acquire);
    // The local skater: the player-bound entity, its root transform and its physics core.
    overlay::DebugModel skater;
    detail::SkaterRead found;
    const char *broken_reason = nullptr;
    try {
        (void)detail::debug_skater(base, client, skater, &found);
    } catch (const detail::SourceGuard &guard) {
        skater.skater_position_valid = false; // `found.entity` says whether the entity was there
        broken_reason = guard.message;
    } catch (...) {
        skater.skater_position_valid = false;
        broken_reason = "an unexpected error";
    }
    const auto bound = found.entity;
    std::uintptr_t player{}, component{}, core{};
    std::uint8_t teleport_flag{};
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
            teleport_flag = reader.value<std::uint8_t>(bound, 0x7e0);
            if (teleport_flag) s.teleport_until = now + 1000;
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
    if (level_active) watch_teleport_manager(s, base, now);
    update_sim_rate(s, now);
    AcquireSRWLockExclusive(&s.dump_lock);
    auto dump = std::exchange(s.dump, std::nullopt);
    ReleaseSRWLockExclusive(&s.dump_lock);
    if (dump) log_body_dump(*dump);

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
        if (log_due(s.lost_lines))
            logging::log(logging::Level::info, logging::Channel::skater,
                         "Skater safety: local skater lost ({}) at ({:.1f}, {:.1f}, {:.1f}): physics state {}, fastest body {:.1f} m/s, "
                         "ground below {} ({} so far on this map).",
                         bound ? "its transform is no longer valid" : "its entity is gone", s.last_position[0], s.last_position[1],
                         s.last_position[2], s.last_state, s.last_fastest,
                         level_active ? ground_text(ground_near(s.last_position)) : std::string("not looked for (level not active)"),
                         s.lost_lines);
        if (bound && log_due(s.broken_lines)) log_broken(s, broken_reason ? broken_reason : "position out of bounds", found, teleport_flag);
        if (!bound && !teleporting && level_active && s.loop.load() && s.component) {
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
                         new_nonfinite ? std::format("{} body velocities not finite", s.nonfinite_bodies.load())
                                       : std::format("a body at {:.0f} m/s", fastest),
                         s.good.trusted ? "" : ", and there is no good grounded position on this map yet");
        const bool broken = s.broken_since && now - s.broken_since >= broken_for_ms;
        if ((new_zeroed || broken) && !teleporting && !respawning && s.good.trusted)
            send_to_ground(s, *s.good.trusted, true,
                           new_zeroed ? "its physics went bad" : "the skater's transform is not valid", now);
    }

    bool ended{}, looping{};
    std::uint64_t forced{};
    const bool stayed = s.healthy_since && now - s.healthy_since >= model::stable_ms;
    AcquireSRWLockExclusive(&s.loop_lock);
    ended = s.loops.settle(now, s.healthy_since);
    looping = s.loops.any_looping();
    if (stayed) s.rebuild_component = 0;
    forced = s.forced;
    ReleaseSRWLockExclusive(&s.loop_lock);
    s.looping.store(looping, std::memory_order_release);
    if (ended)
        logging::log(stayed ? logging::Level::success : logging::Level::warning, logging::Channel::skater, "Loop breaker: {} ({} skater "
                     "creations given the ground check so far).", stayed ? "the respawn loop is over, the skater has stayed for 3 s"
                     : "the game stopped re-creating the skater, but it has not come back valid", forced);
    // The skater showed up while the game is still looping: put it somewhere that holds.
    if (s.loop.load() && looping && present && !teleporting && level_active)
        send_to_ground(s, s.good.trusted ? *s.good.trusted : skater.skater_position, s.good.trusted.has_value(), "respawn loop", now);
}

std::string status_text(const Safety &s) {
    const auto position = s.good.trusted ? std::format("({:.1f}, {:.1f}, {:.1f})", (*s.good.trusted)[0], (*s.good.trusted)[1],
                                                       (*s.good.trusted)[2])
                                         : std::string("none yet");
    return std::format("Skater safety: boost limits {} ({:.0f} m/s, {} physics steps), physics check {} ({:.0f} m/s), loop breaker {} "
                       "(more than {} creations in {} s), 60 Hz physics {}{}. Last good grounded position on this map: {}. Teleports used "
                       "here: {} of {}. Teleport manager state: {}.",
                       on_off(s.boost.load()), model::max_boost_speed, model::boost_steps, on_off(s.check.load()),
                       model::absurd_speed, on_off(s.loop.load()), model::loop_creates, model::loop_window_ms / 1000, on_off(s.rate.load()),
                       (s.sim.rate_raised || s.sim.max_raised) ? std::format(" (raised from ForceSimRate {}, MaxSimFps {})", s.sim.found_rate, s.sim.found_max) : std::string(), position, s.rescues, max_rescues,
                       s.teleport_state);
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
    if (started) {
        s.looping.store(true, std::memory_order_release);
        logging::log(logging::Level::warning, logging::Channel::skater,
                     "Loop breaker: skater component 0x{:x} was created more than {} times in {} s without a skater that stays "
                     "(the respawn loop); its creations get the game's ground check until one does.",
                     component, model::loop_creates, model::loop_window_ms / 1000);
    }
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
    // Through a respawn loop the check keeps running: the game re-creates the skater every 533 ms.
    if (!s.looping.load(std::memory_order_acquire) && now < s.last_creation.load(std::memory_order_acquire) + settle_after_ms) return {};
    return now < until ? target : PhysicsTarget{};
}

void report_physics(const PhysicsReport &report) noexcept {
    auto &s = safety();
    s.fastest.store(report.fastest, std::memory_order_relaxed);
    s.seconds.store(report.seconds, std::memory_order_relaxed);
    s.board_height.store(report.board_height, std::memory_order_relaxed);
    s.nonfinite_bodies.store(report.nonfinite_bodies, std::memory_order_relaxed);
    if (!report.zeroed) return;
    if (!report.finite) s.zeroed_nonfinite.fetch_add(1, std::memory_order_release);
    s.zeroed.fetch_add(1, std::memory_order_release);
}

bool boost_limits() noexcept {
    auto &s = safety();
    return s.started.load(std::memory_order_acquire) && s.boost.load(std::memory_order_acquire);
}

bool body_dump_wanted(bool broken) noexcept {
    auto &s = safety();
    return s.started.load(std::memory_order_acquire) && !(broken ? s.broken_dumped : s.healthy_dumped).load(std::memory_order_acquire);
}

void report_body_dump(const BodyDump &dump) noexcept {
    auto &s = safety();
    if ((dump.broken ? s.broken_dumped : s.healthy_dumped).exchange(true, std::memory_order_acq_rel)) return;
    AcquireSRWLockExclusive(&s.dump_lock);
    s.dump = dump;
    ReleaseSRWLockExclusive(&s.dump_lock);
}

std::string command(const std::vector<std::string> &words) {
    auto &s = safety();
    const auto verb = words.empty() ? std::string("status") : words[0];
    if (verb == "status") return status_text(s);
    std::atomic<bool> *option = verb == "boost" ? &s.boost
                               : verb == "check" ? &s.check
                               : verb == "loop"  ? &s.loop
                               : verb == "rate"  ? &s.rate
                                                 : nullptr;
    const char *key = verb == "boost" ? boost_key : verb == "check" ? check_key : verb == "loop" ? loop_key : rate_key;
    if (!option || words.size() < 2 || (words[1] != "on" && words[1] != "off"))
        return "error: usage: safety [status] | safety boost|check|loop|rate on|off";
    const bool on = words[1] == "on";
    option->store(on, std::memory_order_release);
    profile_runtime::set_local_preference(key, on);
    logging::log(logging::Level::info, logging::Channel::skater, "Skater safety: {} switched {} (kept in your profile).", verb, on_off(on));
    return status_text(s);
}
} // namespace dingosdk::skater_safety
