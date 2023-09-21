#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>

/**
 * A helper class for interruptible sleeps. Calling operator() will interrupt
 * any current sleep, and after that point operator bool() will return true
 * until reset.
 */
class ThreadInterrupt {
    std::condition_variable cond;
    std::mutex mut;
    std::atomic<bool> flag;
public:
    ThreadInterrupt();
    explicit operator bool() const;
    void operator()();
    void reset();
    bool sleep_for(std::chrono::milliseconds rel_time);
    bool sleep_for(std::chrono::seconds rel_time);
    bool sleep_for(std::chrono::minutes rel_time);
};
