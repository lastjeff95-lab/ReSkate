#include "Extension/Skater/skater_safety_model.h"
#include <cstdlib>
#include <iostream>
#include <limits>
#include <vector>

namespace {
using namespace dingosdk::skater_safety::model;
void check(bool condition, const char *message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
bool near(float a, float b, float within = .01f) { return std::abs(a - b) < within; }
constexpr float not_a_number = std::numeric_limits<float>::quiet_NaN();
constexpr float infinite = std::numeric_limits<float>::infinity();
} // namespace

int main() {
    // Boost share: the whole boost while every body stays under the cap.
    {
        const std::vector<Vec3> bodies(32, Vec3{10, 0, 0});
        check(near(boost_share(bodies, {15, 0, 0}, max_boost_speed), 1), "A small boost was limited");
        check(near(boost_share(bodies, {0, 0, 0}, max_boost_speed), 1), "No boost was not a full share");
    }
    // Exactly enough of it to bring the fastest body to the cap.
    {
        std::vector<Vec3> bodies(32, Vec3{10, 0, 0});
        bodies[5] = {60, 0, 0};
        const float share = boost_share(bodies, {80, 0, 0}, max_boost_speed);
        check(near(share, .5f), "The fastest body did not decide the share");
        check(near(60 + 80 * share, max_boost_speed), "The fastest body did not end at the cap");
    }
    // At an angle the cap still holds.
    {
        const std::vector<Vec3> bodies(4, Vec3{0, 0, 90});
        const Vec3 delta{60, 0, 0};
        const float share = boost_share(bodies, delta, max_boost_speed);
        const Vec3 end{delta[0] * share, 0, 90};
        check(share > 0 && share < 1 && near(std::sqrt(dot(end, end)), max_boost_speed), "A sideways boost broke the cap");
    }
    // Nothing for a body already at the cap, a broken velocity or a broken boost.
    {
        std::vector<Vec3> bodies(32, Vec3{10, 0, 0});
        bodies[0] = {0, -120, 0};
        check(boost_share(bodies, {15, 0, 0}, max_boost_speed) == 0, "A body over the cap was boosted");
        bodies[0] = {not_a_number, 0, 0};
        check(boost_share(bodies, {15, 0, 0}, max_boost_speed) == 0, "A non-finite body was boosted");
        bodies[0] = {10, 0, 0};
        check(boost_share(bodies, {infinite, 0, 0}, max_boost_speed) == 0, "A non-finite boost was applied");
        check(boost_share(bodies, {not_a_number, 0, 0}, max_boost_speed) == 0, "A NaN boost was applied");
    }
    // A boost spread over steps reaches what one push would have, and stays under the cap.
    {
        std::vector<Vec3> bodies(32, Vec3{5, 0, 0});
        const Vec3 step{20.0f / boost_steps, 0, 0};
        for (int i = 0; i < boost_steps; ++i) {
            const float share = boost_share(bodies, step, max_boost_speed);
            for (auto &v : bodies) v[0] += step[0] * share;
        }
        check(near(bodies[0][0], 25), "A spread boost did not add up to the whole boost");
        for (int i = 0; i < 100; ++i) {
            const float share = boost_share(bodies, {50, 0, 0}, max_boost_speed);
            for (auto &v : bodies) v[0] += 50 * share;
        }
        check(bodies[0][0] <= max_boost_speed + .01f, "Repeated boosts passed the cap");
    }

    // Body check: finite and plausible passes; NaN, infinity and absurd speeds fail.
    {
        std::vector<Vec3> bodies(32, Vec3{3, -9, 4});
        auto result = check_bodies(bodies);
        check(sane(result) && near(result.fastest, std::sqrt(9.f + 81 + 16)), "A normal skater failed the check");
        bodies[31] = {0, 0, absurd_speed + 1};
        result = check_bodies(bodies);
        check(!sane(result) && result.finite, "An absurd speed passed the check");
        bodies[31] = {0, not_a_number, 0};
        check(!sane(check_bodies(bodies)) && !check_bodies(bodies).finite, "A NaN velocity passed the check");
        bodies[31] = {infinite, 0, 0};
        check(!sane(check_bodies(bodies)), "An infinite velocity passed the check");
    }
    check(sane_position({627.1f, 199.9f, 1062.8f}), "San Vansterdam's start point is not sane");
    check(sane_position({842.1f, -55.1f, 1253.1f}), "Skate 2 Improved's start point is not sane");
    check(!sane_position({not_a_number, 0, 0}) && !sane_position({0, -absurd_position * 2, 0}), "A broken position passed");

    // Good position: only after it settled, and never the one a blow-up started from.
    {
        GoodPosition good;
        good.observe(0, {1, 2, 3}, true, true);
        check(!good.trusted, "A position was trusted before it settled");
        good.observe(500, {4, 5, 6}, false, true); // in the air: fine
        good.observe(1000, {4, 5, 6}, false, true);
        check(good.trusted && (*good.trusted)[0] == 1, "A settled grounded position was not trusted");
        good.observe(1250, {7, 8, 9}, true, true);
        good.observe(1600, {7, 8, 9}, true, false); // blew up while settling
        good.observe(5000, {7, 8, 9}, true, true);
        check((*good.trusted)[0] == 1, "The position a blow-up started from was trusted");
        good.observe(6000, {7, 8, 9}, true, true);
        check((*good.trusted)[0] == 7, "A later grounded position did not replace the trusted one");
        good.observe(6500, {not_a_number, 0, 0}, true, true);
        good.observe(8000, {not_a_number, 0, 0}, true, true);
        check(good.trusted && std::isfinite((*good.trusted)[0]), "A broken position was trusted");
    }

    // Loop detection: the logs' 533 ms retries are a loop; a spawn and bails are not.
    {
        LoopDetector loops;
        const std::uintptr_t skater = 0x2aaa6740;
        // A spawn (three creations within a second), then bails 25-30 s apart.
        for (const std::uint64_t t : {0ull, 300ull, 500ull, 30000ull, 55000ull, 84000ull})
            check(!loops.creation(skater, t), "Normal spawns and bails were taken for a loop");
        check(!loops.looping(skater), "A loop was reported without one");
        // The Skate 2 Improved log: three spawn creations, then retries every 533 ms.
        LoopDetector s2;
        const std::uint64_t base = 100000;
        check(!s2.creation(skater, base) && !s2.creation(skater, base + 150) && !s2.creation(skater, base + 290),
              "The spawn alone was a loop");
        check(s2.creation(skater, base + 1525), "The fourth creation in 5 s did not show the loop");
        check(s2.looping(skater) && s2.any_looping(), "The loop was not held");
        check(!s2.creation(skater, base + 2058), "A loop was reported twice");
        // Another component's creations do not end or start it.
        check(!s2.creation(0x36124fa0, base + 2100), "One creation of another component was a loop");
        check(s2.looping(skater) && !s2.looping(0x36124fa0), "Components were mixed up");
        // A skater healthy for 3 s ends it.
        check(!s2.settle(base + 3000, base + 2500), "The loop ended before the skater had stayed");
        check(s2.settle(base + 5600, base + 2500) && !s2.looping(skater), "A skater that stayed did not end the loop");
        // A loop the game stopped feeding ends on its own.
        LoopDetector quiet;
        for (std::uint64_t t = 0; t < 4u * 533u; t += 533u) (void)quiet.creation(skater, t);
        check(quiet.looping(skater), "533 ms retries were not a loop");
        check(!quiet.settle(5000, 0), "The loop ended while the game was still retrying");
        check(quiet.settle(1599 + quiet_ms, 0), "A loop the game stopped feeding did not end");
    }
    std::cout << "skater safety tests passed\n";
    return 0;
}
