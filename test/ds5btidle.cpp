// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
#include <ds5btidle.h>

#include <cstdio>

using namespace std::chrono_literals;

int main()
{
    const auto start = Ds5BtIdleTimer::Clock::time_point{} + 1h;
    const Ds5BtIdlePolicy policy{true, 1, 5, true};
    const auto expect = [](bool condition, const char *description) {
        if(!condition)
            std::fprintf(stderr, "%s\n", description);
        return condition;
    };

    Ds5BtIdleTimer timer;
    bool ok = expect(!timer.expired(start + 15min, policy, false), "No input report must not disconnect.");
    for(int second = 0; second <= 60; ++second)
        timer.observe(start + std::chrono::seconds(second), false, false);
    ok &= expect(timer.expired(start + 60s, policy, false), "Desktop timer must expire at its configured interval.");
    ok &= expect(!timer.expired(start + 60s, policy, true), "Stream timer must use a different interval.");
    ok &= expect(!timer.expired(start + 62s, policy, false), "Stale input reports must not disconnect.");
    timer.observe(start + 62s, false, false);
    ok &= expect(!timer.expired(start + 62s, policy, false), "Fresh input after a gap must start a new interval.");

    for(int second = 63; second <= 123; ++second)
        timer.observe(start + std::chrono::seconds(second), false, true);
    ok &= expect(!timer.expired(start + 123s, policy, false), "Charging protection must delay disconnect.");
    for(int second = 124; second <= 183; ++second)
        timer.observe(start + std::chrono::seconds(second), false, false);
    ok &= expect(timer.expired(start + 183s, policy, false), "A fresh idle interval must start after charging.");
    timer.observe(start + 183s + 1ms, true, false);
    ok &= expect(!timer.expired(start + 183s + 1ms, policy, false), "Controller activity must reset the interval.");

    for(int second = 184; second <= 245; ++second)
        timer.observe(start + std::chrono::seconds(second), false, false);
    ok &= expect(!timer.expired(start + 245s, {false, 1, 1, false}, false), "Disabling auto-disconnect must bypass the timer.");
    ok &= expect(!timer.expired(start + 245s, {true, 0, 1, false}, false), "Never must disable only the applicable timer.");
    timer.restart(start + 245s);
    ok &= expect(!timer.expired(start + 245s, policy, false), "A mode or policy change must reset the timer.");
    return ok ? 0 : 1;
}
