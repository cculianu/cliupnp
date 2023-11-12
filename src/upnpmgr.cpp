#include "upnpmgr.h"
#include "util.h"

#include <miniupnpc/miniupnpc.h>
#include <miniupnpc/upnpcommands.h>
#include <miniupnpc/upnperrors.h>

#include <algorithm>
#include <set>
#include <utility>

UpnpMgr::UpnpMgr(std::string_view name_) : name(name_) {}

UpnpMgr::~UpnpMgr() { stop(); }

void UpnpMgr::start(PortVec pv, std::function<void()> errorCallback_)
{
    stop();
    errorCallback = std::move(errorCallback_);

    // ensure pv is sorted and contains unique elements before assigning to `ports`
    std::sort(pv.begin(), pv.end());
    auto it = std::unique(pv.begin(), pv.end());
    if (it != pv.end()) {
        pv.erase(it, pv.end());
        pv.shrink_to_fit();
    }
    ports = std::move(pv);

    thread = std::thread([this]{
        TraceThread(name, [this]{
            run();
        });
    });
}

void UpnpMgr::stop()
{
    if (thread.joinable()) {
        interrupt();
        thread.join();
    }
    interrupt.reset();
    if (errorCallback) errorCallback = {}; // clear
}

void UpnpMgr::run()
{
    bool errorFlag = true;
    Defer d([this, &errorFlag]{
        interrupt();
        if (errorCallback) {
            if (errorFlag) errorCallback(); // signal error to obvserver (if any)
            errorCallback = {}; // clear std::function to release resources (if any)
        }
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
        Log("UPnP: GetExternalIPAddress() returned %d", r);
    } else {
        if (externalIPAddress[0]) {
            Log("UPnP: ExternalIPAddress = %s", externalIPAddress);
        } else {
            Log("UPnP: GetExternalIPAddress failed.");
        }
    }

    std::set<uint16_t> mappedPorts;

    Defer cleanup([&mappedPorts, &urls, &data]{
        for (const auto prt : mappedPorts) {
            const std::string port = strprintf("%u", prt);
            Debug() << "Unmapping " << port << " ...";
            const int res = UPNP_DeletePortMapping(urls.controlURL, data.first.servicetype, port.c_str(), "TCP", 0);
            Log("UPNP_DeletePortMapping() for %s: %s", port, res == 0 ? "success"
                                                                      : strprintf("returned %d", res));
        }
    });

    errorFlag = false; // ok, we are not in an early error return anymore

    do {
        if (interrupt) break;
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
                                    name.c_str(), "TCP", 0, "0");
#endif

            if (r != UPNPCOMMAND_SUCCESS) {
                Log("AddPortMapping(%s, %s, %s) failed with code %d (%s)",
                    port, port, lanaddr, r, strupnperror(r));
                mappedPorts.erase(prt);
            } else {
                Log("UPnP Port Mapping of port %s successful.", port);
                mappedPorts.insert(prt);
            }
        }
    } while (!interrupt.wait(std::chrono::minutes{20}));
}
