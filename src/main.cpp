#include "argparse.hpp"
#include "upnpmgr.h"
#include "util.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <csignal>
#include <cstdlib>
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
        Debug() << "Sem wake-up";
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
        return EXIT_FAILURE;
    }

    if ( ! SetupNetworking()) {
        (Error() << "Failed to start networking").useStdOut = false;
        return EXIT_FAILURE;
    }

    psem = std::make_unique<AsyncSignalSafe::Sem>();
    Defer d([origLogTs = Log::logTimeStamps.load(), origFatalCallback = Log::fatalCallback]{
        Log::logTimeStamps = origLogTs;
        Log::fatalCallback = origFatalCallback;
        psem.reset();
    });
    Log::logTimeStamps = true;
    Log::fatalCallback = signalSem;

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
    std::atomic_int exitCode = EXIT_SUCCESS; // can be accessed from cliupnp thread

    // Start the upnp thread.
    // We use a nested scope to ensure upnp.stop() runs before return. This guarantees the return code will be correct
    // even in the corner case the user hits CTRL-C but also upnp had an error.
    {
        Defer d2([&upnp, &sigs_saved]{
            no_more_signals = true;
            upnp.stop();
            for (const auto & [sig, orig_val]: sigs_saved)
                std::signal(sig, orig_val);
        });

        upnp.start(std::move(ports), /* errorCallback = */[&exitCode]{
            // this runs in cliupnp thread in case of error
            exitCode = EXIT_FAILURE;
            if (bool val = false; no_more_signals.compare_exchange_strong(val, true)) {
                Debug() << "Error encoutered, signaling main thread to exit program";
                psem->release(); // tell main thread to wake up
            }
        });

        // Wait for signal handler or error, returning will call the cleanup Defer functions above in reverse order
        waitSem();
    }

    return exitCode.load();
}
