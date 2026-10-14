#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

// Skater safety: guards against the respawn loop seen under CrossOver (the local skater
// destroyed, then rebuilt at the map's start point without a ground check every 533 ms,
// forever). Three parts, each on unless switched off with `safety <part> off` (kept in the
// local profile); the decisions themselves are in skater_safety_model.h.
//   boost  Forward / Up Boost and the trainer's push speed: no body is left faster than
//          100 m/s, and a boost is spread over 4 physics steps instead of one impulse
//          (non-finite boosts were already refused by velocity_boost_delta).
//   check  After every physics step of the local skater, a body velocity that is not finite or
//          over 200 m/s is zeroed; that, or a root position that stays not finite or out of
//          bounds, sends the skater to its last good grounded position, snapped to the ground.
//          It stands back while flying (noclip), during the game's teleports and for a second
//          after any skater creation (a spawn, a bail's respawn), where the game moves the skater.
//   loop   A rebuild of the local skater after it was lost, and every creation of a skater
//          component created more than 3 times in 5 s, is given the game's ground check; while
//          a loop lasts, the skater is sent to its last good grounded position (or onto the
//          ground under where it appeared) when it shows up.
//   rate   Offline, a game simulating physics below 60 Hz is raised to 60 Hz, the rate the SDK's
//          tuning was measured at (Engine/Game/Settings/simulation_time.h). Under CrossOver the
//          local skater was seen stepping at 30 Hz when its walking skeleton blew up to NaN. The
//          game's own rate comes back in a multiplayer session or with `safety rate off`.
// Teleports follow the host's noclip setting in a multiplayer session, like the trainer's.
// Nothing here changes what is sent to other players.
namespace dingosdk::skater_safety {
bool start(std::uintptr_t image_base) noexcept;
// Client thread, every tick. `level` names the map being played (the trainer's map name).
void on_client_tick(std::uintptr_t client, const std::string &level) noexcept;
// Skater component creation (skater_observer.cpp): the ground check to give the game.
std::uint8_t creation_ground_check(std::uintptr_t component, std::uint8_t requested) noexcept;
// The local skater the physics step check looks at; all zero while it is off or unknown.
struct PhysicsTarget {
    std::uintptr_t client{}, entity{}, core{};
};
PhysicsTarget physics_target() noexcept;
// From the physics step check (client_noclip.cpp), every step of that skater.
struct PhysicsReport {
    float fastest{};      // m/s, of the finite body velocities
    bool finite{true};    // every body velocity was finite
    bool zeroed{};        // the step's velocities were zeroed
    float seconds{};      // the core's physics step length
    float board_height{}; // the first board body's height
    int nonfinite_bodies{};
};
void report_physics(const PhysicsReport &report) noexcept;
// Raw physics bodies (0x130 bytes each) for the log: the first board body and the first skeleton
// body, once healthy and once broken per map. Only a body's velocity (+0x70) and flags (+0x60) are
// mapped; these show where the rest (its position) lives.
struct BodyDump {
    bool broken{};
    std::array<std::uint32_t, 76> board{}, rig{};
};
bool body_dump_wanted(bool broken) noexcept;
void report_body_dump(const BodyDump &dump) noexcept;
bool boost_limits() noexcept;
// The console verb: safety [status] | safety boost|check|loop|rate on|off.
std::string command(const std::vector<std::string> &words);
} // namespace dingosdk::skater_safety
