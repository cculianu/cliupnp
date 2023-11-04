#include "threadinterrupt.h"

ThreadInterrupt::operator bool() const { return flag.load(std::memory_order_acquire); }

void ThreadInterrupt::reset() { flag.store(false, std::memory_order_release); }

void ThreadInterrupt::operator()() {
    {
        std::unique_lock l(mut);
        flag.store(true, std::memory_order_release);
    }
    cond.notify_all();
}

bool ThreadInterrupt::wait(std::optional<std::chrono::milliseconds> rel_time) const {
    const auto predicate = [this] { return flag.load(std::memory_order_acquire); };
    std::unique_lock lock(mut);
    if (predicate()) {
        return true;
    } else if (rel_time) {
        return cond.wait_for(lock, *rel_time, predicate);
    } else {
        cond.wait(lock, predicate);
        return predicate(); // should always be true here
    }
}
