#include "threadinterrupt.h"
#include "util.h"

#include <miniupnpc/miniupnpc.h>
#include <miniupnpc/miniwget.h>
#include <miniupnpc/upnpcommands.h>
#include <miniupnpc/upnperrors.h>

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
        if (devlist) freeUPNPDevlist(devlist);
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

    struct UPNPUrls urls;
    struct IGDdatas data;
    int r;

    r = UPNP_GetValidIGD(devlist, &urls, &data, lanaddr, sizeof(lanaddr));
    if (r == 1) {
        if (true) {
            char externalIPAddress[40];
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
        }

        const std::string strDesc = "cliupnp";

        std::set<uint16_t> mappedPorts;

        do {
            if (g_upnp_interrupt)
                break;
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
        } while (g_upnp_interrupt.sleep_for(std::chrono::minutes(20)));

        for (const auto prt : mappedPorts) {
            const std::string port = strprintf("%u", prt);
            Debug() << "Unmapping " << port << " ...";
            r = UPNP_DeletePortMapping(urls.controlURL, data.first.servicetype, port.c_str(), "TCP", 0);
            LogPrintf("UPNP_DeletePortMapping() for %s returned: %d\n", port, r);
        }
        FreeUPNPUrls(&urls);
    } else {
        Error("No valid UPnP IGDs found (r=%d)", r);
        if (r != 0) FreeUPNPUrls(&urls);
    }
}

void StartMapPort(const PortVec &ports) {
    if (!g_upnp_thread.joinable()) {
        assert(!g_upnp_interrupt);
        g_upnp_thread = std::thread([=]{
            TraceThread("upnp", ThreadMapPort, std::move(ports));
        });
    }
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

extern "C" void SigHandler(int sig) {
    AsyncSignalSafe::writeStdErr(AsyncSignalSafe::SBuf("Got signal: ", sig, ", exiting ..."));
    InterruptMapPort();
}

} // namespace

int main(int argc, char *argv[])
{
    Log::fatalCallback = InterruptMapPort;
    if (argc <= 1) {
        Error() << "Please pass 1 or more port(s) to map via UPnP";
        return 1;
    }
    PortVec ports;
    for (int i = 1; i < argc; ++i) {
        const int p = std::stoi(argv[i]);
        if (p <= 0 || p > std::numeric_limits<uint16_t>::max()) {
            Error() << "Invalid port: " << argv[i];
            return 1;
        }
        ports.push_back(p);
    }
    std::signal(SIGINT, SigHandler);
    std::signal(SIGTERM, SigHandler);
    StartMapPort(ports);
    while (g_upnp_interrupt.sleep_for(std::chrono::minutes{60})) {}
    StopMapPort();
    return 0;
}
