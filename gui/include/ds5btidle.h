#pragma once

#include <chrono>

struct Ds5BtIdlePolicy
{
    bool enabled = true;
    int desktopMinutes = 5;
    int streamMinutes = 15;
    bool keepWhileCharging = false;

    static constexpr bool validMinutes(int minutes)
    {
        return minutes == 0 || minutes == 1 || minutes == 5 || minutes == 10
            || minutes == 15 || minutes == 30 || minutes == 60;
    }
};

class Ds5BtIdleTimer
{
public:
    using Clock = std::chrono::steady_clock;

    void observe(Clock::time_point now, bool active, bool charging)
    {
        if(!hasReport || now - lastReport > freshness || active)
            lastActivity = now;
        lastReport = now;
        hasReport = true;
        if(charging)
            lastCharging = now;
        isCharging = charging;
    }

    bool expired(Clock::time_point now, const Ds5BtIdlePolicy &policy, bool streaming) const
    {
        const int minutes = streaming ? policy.streamMinutes : policy.desktopMinutes;
        if(!policy.enabled || minutes <= 0 || !hasReport || now - lastReport > freshness
            || (policy.keepWhileCharging && isCharging))
            return false;
        const auto since = policy.keepWhileCharging && lastCharging > lastActivity ? lastCharging : lastActivity;
        return now - since >= std::chrono::minutes(minutes);
    }

    void restart(Clock::time_point now) { lastActivity = now; }

private:
    static constexpr auto freshness = std::chrono::seconds(1);
    Clock::time_point lastReport{}, lastActivity{}, lastCharging{};
    bool hasReport = false;
    bool isCharging = false;
};
