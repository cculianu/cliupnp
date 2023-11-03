#include "upnpmgr.h"
#include "util.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <csignal>
#include <limits>
#include <utility>
#include <vector>

namespace {
std::unique_ptr<AsyncSignalSafe::Sem> psem;
std::atomic_bool no_more_signals = false;

void signalSem() {
    assert(bool(psem));
    // tell InterrupterThread below to wake up
    if (auto err = psem->release()) {
        AsyncSignalSafe::writeStdErr(*err);
    }
}

void waitSem() {
    assert(bool(psem));
    if (auto err = psem->acquire()) {
        Error() << *err;
    } else {
        Debug() << "Read interrupt signal from semaphore";
    }
}

extern "C" void sigHandler(int sig) {
    if (bool val = false; no_more_signals.compare_exchange_strong(val, true)) {
        AsyncSignalSafe::writeStdErr(AsyncSignalSafe::SBuf(" --- Got signal: ", sig, ", exiting ---"));
        signalSem();
    }
}
} // namespace

int main(int argc, char *argv[])
{
    if (argc <= 1) {
        (Error() << "Please pass 1 or more port(s) to map via UPnP").useStdOut = false;
        return 1;
    }

    psem = std::make_unique<AsyncSignalSafe::Sem>();
    Log::fatalCallback = signalSem;
    Defer d([]{
        Log::fatalCallback = {};
        psem.reset();
    });

    // Parse ports
    UpnpMgr::PortVec ports;
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

    UpnpMgr upnp("cliupnp");

    // Install signal handlers and the defered cleanup
    std::vector<std::pair<int, decltype(std::signal(0, nullptr))>> sigs_saved;
    sigs_saved.emplace_back(SIGINT, std::signal(SIGINT, sigHandler));
    sigs_saved.emplace_back(SIGTERM, std::signal(SIGTERM, sigHandler));
#ifdef SIGHUP
    sigs_saved.emplace_back(SIGHUP, std::signal(SIGHUP, sigHandler));
#endif
#ifdef SIGQUIT
    sigs_saved.emplace_back(SIGQUIT, std::signal(SIGQUIT, sigHandler));
#endif
    Defer d2([&upnp, &sigs_saved]{
        no_more_signals = true;
        upnp.stop();
        for (const auto & [sig, orig_val]: sigs_saved)
            std::signal(sig, orig_val);
    });

    // Start the upnp thread
    upnp.start(std::move(ports));

    // Wait for signal handler, returning will call the cleanup Defer functions above in reverse order
    waitSem();

    return 0;
}
