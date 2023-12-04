#include "upnpmgr.h"
#include "util.h"

#include <miniupnpc/miniupnpc.h>
#include <miniupnpc/upnpcommands.h>
#include <miniupnpc/upnperrors.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
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

    // Manages the upnp context, does RAII auto-cleanup, etc.
    struct UpnpCtx {
        struct UPNPDev *devlist = nullptr;
        UPNPUrls urls = {};
        IGDdatas data = {};
        char externalIPAddress[80] = {0};
        char lanaddr[64];

        void cleanup() noexcept {
            FreeUPNPUrls(&urls);
            if (devlist) { freeUPNPDevlist(devlist); devlist = nullptr; }
            std::memset(externalIPAddress, 0, sizeof(externalIPAddress));
            std::memset(lanaddr, 0, sizeof(lanaddr));
        }
        bool setup() {
            cleanup();
            int r{}, i{}, error [[maybe_unused]] {};
            /* Discover */
            constexpr int delay_msec = 2000;
#ifndef UPNPDISCOVER_SUCCESS
            /* miniupnpc 1.5 */
            devlist = upnpDiscover(delay_msec, nullptr, nullptr, 0);
#elif MINIUPNPC_API_VERSION < 14
            /* miniupnpc 1.6 */
            devlist = upnpDiscover(delay_msec, nullptr, nullptr, 0, 0, &error);
#else
            /* miniupnpc 1.9.20150730 */
            devlist = upnpDiscover(delay_msec, nullptr, nullptr, 0, 0, 2, &error);
#endif
            for (UPNPDev *d = devlist; d; d = d->pNext)
                Debug("Found UPNP Dev %d: %s", i++, d->descURL);

            /* Get valid IGD */
            r = UPNP_GetValidIGD(devlist, &urls, &data, lanaddr, sizeof(lanaddr));
            if (r != 1) {
                Error("No valid UPnP IGDs found (r=%d)", r);
                return false;
            }
            Log("UPnP: Local IP = %s", lanaddr);

            /* Probe external IP */
            r = UPNP_GetExternalIPAddress(urls.controlURL, data.first.servicetype, externalIPAddress);
            if (r != UPNPCOMMAND_SUCCESS) {
                Log("UPnP: GetExternalIPAddress() returned %d", r);
            } else {
                if (externalIPAddress[0]) {
                    Log("UPnP: External IP = %s", externalIPAddress);
                } else {
                    Log("UPnP: GetExternalIPAddress failed.");
                }
            }
            return true;
        }
        ~UpnpCtx() noexcept { cleanup(); }
    } ctx;

    if (!ctx.setup()) return; // failure, exit thread with errorFlag set

    std::set<uint16_t> mappedPorts;

    Defer cleanup([&mappedPorts, &ctx]{
        if (!ctx.urls.controlURL) return;
        for (const auto prt : mappedPorts) {
            const std::string port = strprintf("%u", prt);
            Debug() << "Unmapping " << port << " ...";
            const int res = UPNP_DeletePortMapping(ctx.urls.controlURL, ctx.data.first.servicetype, port.c_str(), "TCP", 0);
            Log("UPNP_DeletePortMapping() for %s: %s", port, res == 0 ? "success"
                                                                      : strprintf("returned %d", res));
        }
    });

    errorFlag = false; // ok, we are not in an early error return anymore

    uint64_t iters{};
    std::chrono::milliseconds wait_time;
    do {
        if (interrupt) break;
        // Redo context setup if we couldn't map anything -- we may have gotten a new IP address or other
        // shenanigans...
        bool ok = true;
        if (iters++ && mappedPorts.empty()) {
            Debug() << "Redoing UPNP context ...";
            ok = ctx.setup();
        }
        if (ok) {
            for (const auto prt : ports) {
                const std::string port = strprintf("%u", prt);
                Debug() << "Mapping " << port << " ...";
                int r;
    #ifndef UPNPDISCOVER_SUCCESS
                /* miniupnpc 1.5 */
                r = UPNP_AddPortMapping(ctx.urls.controlURL, ctx.data.first.servicetype,
                                        port.c_str(), port.c_str(), ctx.lanaddr,
                                        name.c_str(), "TCP", 0);
    #else
                /* miniupnpc 1.6 */
                r = UPNP_AddPortMapping(ctx.urls.controlURL, ctx.data.first.servicetype,
                                        port.c_str(), port.c_str(), ctx.lanaddr,
                                        name.c_str(), "TCP", 0, "0");
    #endif

                if (r != UPNPCOMMAND_SUCCESS) {
                    Error("AddPortMapping(%s, %s, %s) failed with code %d (%s)", port, port, ctx.lanaddr, r, strupnperror(r));
                    mappedPorts.erase(prt);
                } else {
                    Log("UPnP Port Mapping of port %s successful.", port);
                    mappedPorts.insert(prt);
                }
            }
        }
        wait_time = !ok || mappedPorts.empty() ? std::chrono::minutes{1} : std::chrono::minutes{20};
    } while (!interrupt.wait(wait_time));
}
