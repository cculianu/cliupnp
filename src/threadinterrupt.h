#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>

/**
 * A helper class for interruptible sleeps. Calling operator() will interrupt
 * any current sleep, and after that point operator bool() will return true
 * until reset.
 */
class ThreadInterrupt {
    mutable std::condition_variable cond;
    mutable std::mutex mut;
    std::atomic<bool> flag = false;
public:
    // If true, interrupt flag is set
    explicit operator bool() const;
    // Set the interrupt flag
    void operator()();
    // Unset the interrupt flag
    void reset();
    // Sleep until either the interrupt flag is set, or the specified time elapses. Use std::nullopt to sleep
    // indefinitely.
    // @return `true` if the interrupt flag was set, `false` otherwise.
    bool wait(std::optional<std::chrono::milliseconds> timeout = std::nullopt) const;
};
