#include "util.h"

#include "tinyformat.h"

#include <cerrno>
#include <cstdarg>
#include <cstring>
#include <mutex>
#include <thread>

#if WINDOWS
#  define WIN32_LEAN_AND_MEAN 1
#  include <windows.h>
#  include <psapi.h>
#  include <io.h>              // for _write(), _read(), _pipe(), _close()
#  include <fcntl.h>           // for O_BINARY, O_TEXT
#  include <errno.h>           // for errno
#elif UNIX
#  include <unistd.h>          // for write(), read(), pipe(), close(), isatty()
#  include <stdio.h>           // for fileno()
#endif

InternalError::~InternalError() {} // for vtable

Log::Log() {}

Log::Log(Color c)
{
    setColor(c);
}

/* static */ std::atomic<int> Log::logLevel = static_cast<int>(
#ifdef NDEBUG
    Level::Info
#else
    Level::Debug
#endif
);

/* static */ std::function<void()> Log::fatalCallback;

static const auto g_main_thread_id = std::this_thread::get_id();

static bool isMainThread(std::thread::id *id_out = nullptr) {
    const auto tid = std::this_thread::get_id();
    if (id_out) *id_out = tid;
    return tid == g_main_thread_id;
}

std::atomic<bool> g_shutdown_requested = false;

Log::~Log()
{
    if (doprt) {
        std::string thrdStr;
        if (!isMainThread()) {
            thrdStr = "<" + ThreadGetName() + "> ";
        }
        const std::string theString = thrdStr + (isaTTY(useStdOut) ? colorize(s.str(), color) : s.str());

        // just print to console for now..
        static std::mutex mut;
        {
            std::unique_lock g(mut);
            auto & os = (useStdOut ? std::cout : std::cerr);
            os << theString;
            if (autoNewLine) os << std::endl;
            os << std::flush;
        }
        // Fatal flags the app to quit
        if (level == static_cast<int>(Level::Fatal) && fatalCallback) {
            fatalCallback();
        }
    }
}

/* static */
bool Log::isaTTY(const bool stdOut) {
#if WINDOWS
    return false; // console control chars don't reliably work on windows. disable color always
#else
    const int fd = fileno(stdOut ? stdout : stderr);
    return isatty(fd);
#endif
}

/* static */
std::string Log::colorString(Color c) {
    const char *suffix = "[0m"; // normal
    switch(c) {
    case Black: suffix = "[30m"; break;
    case Red: suffix = "[31m"; break;
    case Green: suffix = "[32m"; break;
    case Yellow: suffix = "[33m"; break;
    case Blue: suffix = "[34m"; break;
    case Magenta: suffix = "[35m"; break;
    case Cyan: suffix = "[36m"; break;
    case White: suffix = "[37m"; break;
    case BrightBlack: suffix = "[30;1m"; break;
    case BrightRed: suffix = "[31;1m"; break;
    case BrightGreen: suffix = "[32;1m"; break;
    case BrightYellow: suffix = "[33;1m"; break;
    case BrightBlue: suffix = "[34;1m"; break;
    case BrightMagenta: suffix = "[35;1m"; break;
    case BrightCyan: suffix = "[36;1m"; break;
    case BrightWhite: suffix = "[37;1m"; break;

    default:
        // will just use normal
        break;
    }
    static const char prefix[2] = { 033, 0 }; // esc 033 in octal
    return strprintf("%s%s", prefix, suffix);
}

std::string Log::colorize(const std::string &str, Color c) {
    std::string colorStr = useColor && c != Normal ? colorString(c) : "";
    std::string normalStr = useColor && c != Normal ? colorString(Normal) : "";
    return colorStr + str + normalStr;
}

template <> Log & Log::operator<<(const Color &c) { setColor(c); return *this; }
template <> Log & Log::operator<<(const std::string &t) { s << t.c_str(); return *this; }

Debug::~Debug()
{
    level = static_cast<int>(Level::Debug);
    doprt = isEnabled();
    if (!doprt) return;
    if (!colorOverridden) color = Cyan;
    s.str("(Debug) " + s.str());
}

bool Debug::forceEnable = false;

bool Debug::isEnabled() {
    return forceEnable || logLevel.load() >= static_cast<int>(Level::Debug);
}

Trace::~Trace()
{
    level = static_cast<int>(Level::Debug);
    doprt = isEnabled();
    if (!doprt) return;
    if (!colorOverridden) color = Green;
    s.str("(Trace) " + s.str());
}

bool Trace::forceEnable = false;

bool Trace::isEnabled() {
    return forceEnable || logLevel.load() >= static_cast<int>(Level::Trace);
}

Error::~Error()
{
    level = static_cast<int>(Level::Critical);
    if (!colorOverridden) color = BrightRed;
}

Warning::~Warning()
{
    level = static_cast<int>(Level::Warning);
    if (!colorOverridden) color = Yellow;
}

Fatal::~Fatal()
{
    level = static_cast<int>(Level::Fatal);
    s.str("FATAL: " + s.str());
    if (!colorOverridden) color = BrightRed;
}

namespace AsyncSignalSafe {
namespace {
#if WINDOWS
auto writeFD = ::_write; // Windows API docs say to use this function, since write() is deprecated
auto readFD  = ::_read;  // Windows API docs say to use this function, since read() is deprecated
auto closeFD = ::_close; // Windows API docs say to use this function, since close() is deprecated
inline constexpr std::array<char, 3> NL{"\r\n"};
#elif UNIX
auto writeFD = ::write;
auto readFD  = ::read;
auto closeFD = ::close;
inline constexpr std::array<char, 2> NL{"\n"};
#else
// no-op on unknown platform (this platform would use the cond variable and doesn't need read/close/pipe)
auto writeFD = [](int, const void *, size_t n) { return int(n); };
inline constexpr std::array<char, 1> NL{0};
#endif
}
void writeStdErr(const std::string_view &sv, bool wrnl) noexcept {
    constexpr int stderr_fd = 2; /* this is the case on all platforms */
    writeFD(stderr_fd, sv.data(), sv.length());
    if (wrnl && NL.size() > 1)
        writeFD(stderr_fd, NL.data(), NL.size()-1);
}
#if WINDOWS || UNIX
Sem::Pipe::Pipe() {
    const int res =
#    if WINDOWS
        ::_pipe(fds, 32 /* bufsize */, O_BINARY);
#    else
        ::pipe(fds);
#    endif
    if (res != 0)
        throw InternalError(strprintf("Failed to create a Cond::Pipe: (%d) %s", errno, std::strerror(errno)));
}
Sem::Pipe::~Pipe() { closeFD(fds[0]), closeFD(fds[1]); }
std::optional<SBuf<>> Sem::acquire() noexcept {
    std::optional<SBuf<>> ret;
    char c;
    if (const int res = readFD(p.fds[0], &c, 1); res != 1)
        ret.emplace("Sem::acquire: readFD returned ", res);
    return ret;
}
std::optional<SBuf<>> Sem::release() noexcept {
    std::optional<SBuf<>> ret;
    const char c = 0;
    if (const int res = writeFD(p.fds[1], &c, 1); res != 1)
        ret.emplace("Sem::release: writeFD returned ", res);
    return ret;
}
#else /* !WINDOWS && !UNIX */
// fallback to emulated -- use std C++ condition variable which is not technically
// guaranteed async signal safe, but for all pratical purposes it's safe enough as a fallback.
std::optional<SBuf<>> Sem::acquire() noexcept {
    std::mutex dummy; // hack, but works
    std::unique_lock l(dummy);
    p.cond.wait(l);
    return std::nullopt;
}
std::optional<SBuf<>> Sem::release() noexcept {
    p.cond.notify_one();
    return std::nullopt;
}
#endif // WINDOWS || UNIX
} // end namespace AsyncSignalSafe

static thread_local std::string g_thread_name;

const std::string & ThreadGetName() {
    if (!g_thread_name.empty()) {
        return g_thread_name;
    } // else ...
    static thread_local std::string fallback_name;
    if (fallback_name.empty()) {
        if (std::thread::id tid; isMainThread(&tid)) {
            fallback_name = "main";
        } else {
            std::ostringstream os;
            os << tid << std::flush;
            fallback_name = os.str();
        }
    }
    return fallback_name;
}

#if (defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__DragonFly__))
#include <pthread.h>
#include <pthread_np.h>
#endif

#if  __has_include(<sys/prctl.h>)
#include <sys/prctl.h> // For prctl, PR_SET_NAME, PR_GET_NAME
#endif

void ThreadSetName(std::string_view name) {
    g_thread_name = name;
#if defined(PR_SET_NAME)
    // Only the first 15 characters are used (16 - NUL terminator)
    ::prctl(PR_SET_NAME, g_thread_name.c_str(), 0, 0, 0);
#elif (defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__DragonFly__))
    pthread_set_name_np(pthread_self(), g_thread_name.c_str());
#elif defined(__MACH__) && defined(__APPLE__)
    pthread_setname_np(g_thread_name.c_str());
#endif
}
