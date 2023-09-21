#include "threadinterrupt.h"

ThreadInterrupt::ThreadInterrupt() : flag(false) {}

ThreadInterrupt::operator bool() const {
    return flag.load(std::memory_order_acquire);
}

void ThreadInterrupt::reset() {
    flag.store(false, std::memory_order_release);
}

void ThreadInterrupt::operator()() {
    {
        std::unique_lock l(mut);
        flag.store(true, std::memory_order_release);
    }
    cond.notify_all();
}

bool ThreadInterrupt::sleep_for(std::chrono::milliseconds rel_time) {
    std::unique_lock lock(mut);
    return !cond.wait_for(lock, rel_time, [this] {
        return flag.load(std::memory_order_acquire);
    });
}

bool ThreadInterrupt::sleep_for(std::chrono::seconds rel_time) {
    return sleep_for(std::chrono::duration_cast<std::chrono::milliseconds>(rel_time));
}

bool ThreadInterrupt::sleep_for(std::chrono::minutes rel_time) {
    return sleep_for(std::chrono::duration_cast<std::chrono::milliseconds>(rel_time));
}
