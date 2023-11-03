#pragma once

#include "threadinterrupt.h"

#include <cstdint>
#include <thread>
#include <string>
#include <string_view>
#include <vector>

class UpnpMgr
{
public:
    UpnpMgr(std::string_view name = "UpnpMgr");
    ~UpnpMgr();

    using PortVec = std::vector<uint16_t>;

    void start(PortVec ports);
    void stop();

private:
    const std::string name;
    ThreadInterrupt interrupt;
    PortVec ports;
    std::thread thread;

    void run();
};
