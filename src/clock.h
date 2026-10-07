#ifndef CLOCK_H
#define CLOCK_H

#include <cmath>
#include <mutex>

extern "C" {
#include <libavutil/time.h>
}

class Clock
{
public:
    Clock() = default;

    void init()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pts_          = std::nan("");
        pts_drift_    = std::nan("");
        last_updated_ = 0.0;
    }

    void set(double pts)
    {
        const double now = av_gettime_relative() / 1000000.0;
        setAt(pts, now);
    }

    void setAt(double pts, double time)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pts_          = pts;
        last_updated_ = time;
        pts_drift_    = pts - time * speed_;
    }

    double get() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const double now = av_gettime_relative() / 1000000.0;
        return pts_drift_ + now * speed_;
    }

    void setSpeed(double speed)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (speed <= 0.0)
            return;
        const double now  = av_gettime_relative() / 1000000.0;
        const double curr = pts_drift_ + now * speed_;
        speed_            = speed;
        pts_drift_        = curr - now * speed_;
    }

    double speed() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return speed_;
    }

    double pts() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return pts_;
    }

private:
    mutable std::mutex mutex_;

    double pts_          = std::nan("");
    double pts_drift_    = 0.0;
    double last_updated_ = 0.0;
    double speed_        = 1.0;
};

#endif
