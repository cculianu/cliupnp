#include "argparse.hpp"
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
    const char *name = PACKAGE_NAME, *version = PACKAGE_VERSION;
    argparse::ArgumentParser parser(name, version);
    parser.add_argument("port")
        .help("One or more ports to open up on the router")
        .nargs(argparse::nargs_pattern::at_least_one)
        .scan<'u', uint16_t>();
    parser.add_argument("-d", "--debug")
        .default_value(false)
        .implicit_value(true)
        .help("Enable extra debug logging");


    UpnpMgr::PortVec ports;
    try {
        parser.parse_args(argc, argv);
        // Grab port positional arg(s)
        ports = parser.get<UpnpMgr::PortVec>("port");
        // Interpret -d option
        Log::logLevel = int(parser.get<bool>("-d") ? Log::Level::Debug : Log::Level::Info);
    } catch (const std::exception &e) {
        // Rewrite some of the obscure errors that the ArgParser sends
        (Error() << e.what()).useStdOut = false;
        return 1;
    }

    psem = std::make_unique<AsyncSignalSafe::Sem>();
    Log::fatalCallback = signalSem;
    Defer d([]{
        Log::fatalCallback = {};
        psem.reset();
    });

    // Parse ports
    UpnpMgr upnp(name);

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
