#include "threadinterrupt.h"
#include "util.h"

#include <miniupnpc/miniupnpc.h>
#include <miniupnpc/upnpcommands.h>
#include <miniupnpc/upnperrors.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <functional>
#include <limits>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

namespace {
using PortVec = std::vector<uint16_t>;

ThreadInterrupt g_upnp_interrupt;
std::thread g_upnp_thread;

void ThreadMapPort(PortVec ports) {
    Defer d([]{
        g_upnp_interrupt();
    });
    if (ports.empty()) {
        Error() << "Pass a vector of ports!";
        return;
    }
    Log() << "UPNP thread started, will manage " << ports.size() << " port mapping(s), probing for IGDs ...";
    const char *multicastif = nullptr;
    const char *minissdpdpath = nullptr;
    struct UPNPDev *devlist = nullptr;
    Defer d2([&devlist]{
        if (!devlist) return;
        freeUPNPDevlist(devlist);
        devlist = nullptr;
    });
    char lanaddr[64];
    constexpr const int delay_msec = 2000;

#ifndef UPNPDISCOVER_SUCCESS
    /* miniupnpc 1.5 */
    devlist = upnpDiscover(delay_msec, multicastif, minissdpdpath, 0);
#elif MINIUPNPC_API_VERSION < 14
    /* miniupnpc 1.6 */
    int error = 0;
    devlist = upnpDiscover(delay_msec, multicastif, minissdpdpath, 0, 0, &error);
#else
    /* miniupnpc 1.9.20150730 */
    int error = 0;
    devlist = upnpDiscover(delay_msec, multicastif, minissdpdpath, 0, 0, 2, &error);
#endif

    int i = 0;
    for (UPNPDev *d = devlist; d; d = d->pNext) {
        Debug("Found UPNP Dev %d: %s", i++, d->descURL);
    }

    UPNPUrls urls = {};
    IGDdatas data = {};
    int r;

    r = UPNP_GetValidIGD(devlist, &urls, &data, lanaddr, sizeof(lanaddr));
    Defer freeUrls([r, &urls]{ if (r != 0) FreeUPNPUrls(&urls); });

    if (r != 1) {
        Error("No valid UPnP IGDs found (r=%d)", r);
        return;
    }

    char externalIPAddress[80] = {0};
    r = UPNP_GetExternalIPAddress(urls.controlURL, data.first.servicetype, externalIPAddress);
    if (r != UPNPCOMMAND_SUCCESS) {
        LogPrintf("UPnP: GetExternalIPAddress() returned %d\n", r);
    } else {
        if (externalIPAddress[0]) {
            LogPrintf("UPnP: ExternalIPAddress = %s\n", externalIPAddress);
        } else {
            LogPrintf("UPnP: GetExternalIPAddress failed.\n");
        }
    }

    const std::string strDesc = "cliupnp";
    std::set<uint16_t> mappedPorts;

    Defer cleanup([&mappedPorts, &urls, &data]{
        for (const auto prt : mappedPorts) {
            const std::string port = strprintf("%u", prt);
            Debug() << "Unmapping " << port << " ...";
            const int res = UPNP_DeletePortMapping(urls.controlURL, data.first.servicetype, port.c_str(), "TCP", 0);
            LogPrintf("UPNP_DeletePortMapping() for %s: %s\n", port, res == 0 ? "success"
                                                                              : strprintf("returned %d", res));
        }
    });

    do {
        if (g_upnp_interrupt) break;
        for (const auto prt : ports) {
            const std::string port = strprintf("%u", prt);
            Debug() << "Mapping " << port << " ...";
#ifndef UPNPDISCOVER_SUCCESS
            /* miniupnpc 1.5 */
            r = UPNP_AddPortMapping(urls.controlURL, data.first.servicetype,
                                    port.c_str(), port.c_str(), lanaddr,
                                    strDesc.c_str(), "TCP", 0);
#else
            /* miniupnpc 1.6 */
            r = UPNP_AddPortMapping(urls.controlURL, data.first.servicetype,
                                    port.c_str(), port.c_str(), lanaddr,
                                    strDesc.c_str(), "TCP", 0, "0");
#endif

            if (r != UPNPCOMMAND_SUCCESS) {
                LogPrintf("AddPortMapping(%s, %s, %s) failed with code %d (%s)\n",
                          port, port, lanaddr, r, strupnperror(r));
                mappedPorts.erase(prt);
            } else {
                LogPrintf("UPnP Port Mapping of port %s successful.\n", port);
                mappedPorts.insert(prt);
            }
        }
    } while (!g_upnp_interrupt.wait(std::chrono::minutes{20}));
}

void InterruptMapPort() {
    g_upnp_interrupt();
}

void StopMapPort() {
    if (g_upnp_thread.joinable()) {
        InterruptMapPort();
        g_upnp_thread.join();
    }
    g_upnp_interrupt.reset();
}

void StartMapPort(PortVec ports) {
    StopMapPort();
    g_upnp_thread = std::thread([pv = std::move(ports)]() mutable {
        TraceThread("upnp", ThreadMapPort, std::move(pv));
    });
}

std::unique_ptr<AsyncSignalSafe::Sem> psem;

extern "C" void SigHandler(int sig) {
    assert(bool(psem));
    AsyncSignalSafe::writeStdErr(AsyncSignalSafe::SBuf(" --- Got signal: ", sig, ", exiting ---"));
    // tell InterrupterThread below to wake up
    if (auto err = psem->release()) {
        AsyncSignalSafe::writeStdErr(*err);
    }
}

void InterrupterThread() {
    assert(bool(psem));
    if (auto err = psem->acquire()) {
        Error() << *err;
    } else {
        // got woken up by SigHandler above, or my main() below on app exit
        InterruptMapPort();
        Debug() << "Signaled interrupt";
    }
}

} // namespace

int main(int argc, char *argv[])
{
    psem = std::make_unique<AsyncSignalSafe::Sem>();
    Log::fatalCallback = InterruptMapPort;
    if (argc <= 1) {
        (Error() << "Please pass 1 or more port(s) to map via UPnP").useStdOut = false;
        return 1;
    }
    PortVec ports;
    ports.reserve(std::max(argc - 1, 0));
    for (int i = 1; i < argc; ++i) {
        int p;
        try {
            p = std::stoi(argv[i]);
        } catch (const std::logic_error &e) {
            (Debug() << "args[" << i << "] = \"" << argv[i] << "\", got exception: " << e.what()).useStdOut = false;
            p = -1;
        }
        if (p <= 0 || p > std::numeric_limits<uint16_t>::max()) {
            (Error() << "Invalid port: " << argv[i]).useStdOut = false;
            return 1;
        }
        ports.push_back(p);
    }
    StartMapPort(std::move(ports));
    auto t = std::thread(TraceThread<void()>, "interrupter", InterrupterThread);
    const auto sigint_orig = std::signal(SIGINT, SigHandler);
    const auto sigterm_orig = std::signal(SIGTERM, SigHandler);
    Defer d([&t, &sigint_orig, &sigterm_orig]{
        std::signal(SIGINT, sigint_orig);
        std::signal(SIGTERM, sigterm_orig);
        psem->release(); // wake up sleeping InterrupterThread to get it to exit
        if (t.joinable()) t.join();
        psem.reset();
    });
    g_upnp_interrupt.wait();
    StopMapPort();
    return 0;
}
