#pragma once

#include "tinyformat.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#if defined(__clang__) || defined(__GNUC__)
#define EXPECT(expr, constant) __builtin_expect(expr, constant)
#else
#define EXPECT(expr, constant) (expr)
#endif

#if !defined(_MSC_VER) && (defined(__clang__) || defined(__GNUC__))
#define ATTR_PRINTF(fmt, arg) __attribute__((format(printf, fmt, arg)))
#else
#define ATTR_PRINTF(fmt, arg)
#define __PRETTY_FUNCTION__ __FUNCTION__
#endif

#define LIKELY(bool_expr)   EXPECT(int(bool(bool_expr)), 1)
#define UNLIKELY(bool_expr) EXPECT(int(bool(bool_expr)), 0)

/// No-op on every platform except Windows. On Windows it does the WSAStartup() ritual.
bool SetupNetworking();

/// A std::runtime_error subclass used to denote "InternalError"
struct InternalError : std::runtime_error {
    using std::runtime_error::runtime_error;
    ~InternalError() override;
};

/// Super class of Debug, Warning, Error classes.  Can be instantiated for regular log messages.
class Log
{
public:
    enum Color {
        Reset = 0,
        Normal = Reset, // no color/reset
        Black,
        Red, Green, Yellow, Blue, Magenta, Cyan, White,
        BrightBlack,
        BrightRed, BrightGreen, BrightYellow, BrightBlue, BrightMagenta, BrightCyan, BrightWhite,
        Color_N
    };

    /// Log levels
    enum class Level : int { Info = 0, Warning, Critical, Fatal, Debug, Trace };

    static std::atomic_int logLevel;  ///< app-global log level, defaults to Info on release builds, Debug on debug builds
    static std::atomic_bool logTimeStamps; ///< app-global; if true, we prepent timestamp info to the log lines

    static std::function<void()> fatalCallback; ///< if defined, called every time a Fatal() log line is printed

    bool doprt = true;
    bool useStdOut = true;  ///< whether to log to stdout or stderr
    bool autoNewLine = true;

    template<typename ...Args>
    explicit Log(const char *fmt, Args && ...args) : s(strprintf(fmt, std::forward<Args>(args)...)) {}
    explicit Log(Color);
    Log();
    virtual ~Log();

    template <class T> Log & operator<<(const T & t) { s << t; return *this;  }

    Log & setColor(Color c) { color = c; colorOverridden = true; return *this; }
    Color getColor() const { return color; }

    /// Used by the DebugM macros, etc.  Unpacks all of its args using operator<< for each arg.
    template <class ...Args>
    Log & operator()(Args&& ...args) {  ((*this) << ... << args); return *this; }

    /// returns true iff stdout is a tty, false otherwise. always returns false on windows
    static bool isaTTY(bool stdOut = true);

protected:
    static std::string colorString(Color c);
    std::string colorize(const std::string &, Color c);

    bool colorOverridden = false, useColor = true;
    int level = 0;
    Color color = Normal;
    std::ostringstream s;
};


// specialization to set the color.
template <> Log & Log::operator<<(const Color &);
// specialization for std::string
template <> Log & Log::operator<<(const std::string &t);

/** \brief Stream-like class to print a debug message to the app's logging facility
    Example:
   \code
        Debug() << "This is a debug message"; // would print a debug message to the logging facility
   \endcode
 */
class Debug : public Log
{
public:
    using Log::Log; // inherit c'tor
    virtual ~Debug() override;

    static bool isEnabled();
    static bool forceEnable; ///< defaults false -- set to true if there is no App and you want to ensure Debug() works
};

/// This is fast: It only evaluates args if Debug is enabled. Use this in performance-critical code.
/// Unfortunately, there is no way to do this exact thing with templates, so we opted for a C-style macro
/// to avoid evaluating the args in the !Debug::isEnabled() case.
#define DebugM(...)                \
do {                               \
        if (Debug::isEnabled())    \
        Debug()(__VA_ARGS__);      \
} while (0)

/** \brief Stream-like class to print a trace message to the app's logging facility
    Example:
   \code
        Trace() << "This is a trace debug message"; // would print a trace message to the logging facility
   \endcode
 */
class Trace : public Log
{
public:
    using Log::Log; // inherit c'tor
    virtual ~Trace() override;

    static bool isEnabled();
    static bool forceEnable; ///< defaults false -- set to true if there is no App and you want Trace() to work.
};

/// This is fast: It only evaluates args if Trace is enabled. Use this in performance-critical code.
/// Unfortunately, there is no way to do this exact thing with templates, so we opted for a C-style macro
/// to avoid evaluating the args in the !Trace::isEnabled() case.
#define TraceM(...)                \
do {                               \
        if (Trace::isEnabled())    \
        Trace()(__VA_ARGS__);      \
} while (0)

/** \brief Stream-like class to print an error message to the app's logging facility
    Example:
   \code
        Error() << "This is an ERROR message!!"; // would print an error message to the logging facility
   \endcode
 */
class Error : public Log
{
public:
    using Log::Log; // inherit c'tor
    virtual ~Error() override;
};

/** \brief Stream-like class to print a warning message to the app's logging facility

 Example:
\code
     Warning() << "This is a warning message..."; // would print a warning message to the logging facility
\endcode
*/
class Warning : public Log
{
public:
    using Log::Log; // inherit c'tor
    virtual ~Warning() override;
};

/// Like Error(), except it will enqueue a qApp->exit(1) after logging the message
class Fatal : public Log
{
public:
    using Log::Log;
    virtual ~Fatal() override;
};

// Now add these macros for symmetry
#define LogM(...) (Log()(__VA_ARGS__))
#define WarningM(...) (Warning()(__VA_ARGS__))
#define ErrorM(...) (Error()(__VA_ARGS__))
#define FatalM(...) (Fatal()(__VA_ARGS__))

#define FatalAssert(b,...)                                            \
do {                                                                  \
    if (!(b))                                                         \
        FatalM("ASSERTION FAILED: \"", #b, "\" - ", __VA_ARGS__);     \
} while (0)

/// Emulate bitcoin LogPrintf() (don't append NL)
template <typename ...Args>
void LogPrintf(const char *fmt, Args && ...args) {
    Log l(fmt, std::forward<Args>(args)...);
    l.autoNewLine = false;
}


/// A namespace for a bunch of functionality that can be used from an async POSIX signal handler.
///
/// We can't really use any functions in a signal handler besides the ones in this table --
/// https://pubs.opengroup.org/onlinepubs/9699919799/functions/V2_chap02.html#tag_15_04
namespace AsyncSignalSafe {

//! A very simple C string buffer. Uses the stack only, so it's async signal safe.
//! Use this in an the app signal handler to build a simple (non-allocating) C string for
//! writeStdErr() declared later in this file.
template <std::size_t N = 255>
struct SBuf {
    static_assert (N < std::size_t(std::numeric_limits<long>::max())); // ensure no signed overflow
    static constexpr std::size_t MaxLen = N;
    std::size_t len = 0;
    std::array<char, MaxLen + 1> strBuf;

    constexpr SBuf() noexcept { clear(); }

    /// Construct by formatting the args to this buffer.
    /// Usage: SBuf("A string: ", anum, " another string\n", anotherNum), etc.
    template <typename ... Args>
    SBuf(Args && ...args) noexcept : SBuf() {
        // fold expression: calls append() for each argument in the pack
        (append(std::forward<Args>(args)),...);
    }

    constexpr void clear() noexcept { len = 0; strBuf[0] = 0; }

    // append a string_view to the buffer
    SBuf & append(const std::string_view &sv) noexcept {
        auto *const s = sv.data();
        long slen = long(sv.length());
        if (slen <= 0) return *this;
        if (slen + len > MaxLen)
            slen = long(MaxLen) - len;
        if (slen <= 0) return *this;
        std::strncpy(strBuf.data() + len, s, slen);
        len += slen;
        strBuf[len] = 0;
        return *this;
    }
    // append a single character
    SBuf & append(char c) noexcept {
        if (len >= MaxLen)
            return *this;
        strBuf[len++] = c;
        strBuf[len] = 0;
        return *this;
    }
    // Append an integer converted to decimal string. If there is no room for the full decimal representation
    // of the integer, including possible minus sign, the decimal number will be truncated at the end.
    template <typename T, typename std::enable_if_t<std::is_integral_v<T>, int> = 0>
    SBuf & append(T n) noexcept {
        /* Note: ideally we'd just use C++17 std::to_chars here -- however on some compilers we target, the
         * implementation is missing from libstdc++!  So.. we must roll our own here... */
        static_assert(sizeof(T) <= 16, "This function assumes <= 128 bit ints for T");
        constexpr unsigned TmpMaxLen = 64; // should be enough even for 128 bit values
        char tmpBuf[TmpMaxLen];
        unsigned tmpLen = 0;
        bool neg = false;
        if (std::is_signed_v<T> && n < 0) { // special handling for negatives.. prepend minus, normalize to positive value
            neg = true;
            if (UNLIKELY(n == std::numeric_limits<T>::min())) { // special case for most negative `n`
                // add digit accounting for its negativeness, then divide n by 10 so that its absolute value
                // can fit in a positive T
                tmpBuf[tmpLen++] = '0' - n % 10;
                n /= 10;
            }
            n = -n; // when we get here, `-n` is guaranteed to fit in a positive T
        }
        do {
            tmpBuf[tmpLen++] = '0' + n % 10;
            n /= 10;
        } while (n); /* <-- no need to check if looping past end of tmpBuf; 64 chars is enough for at least 128 bit; see above static_assert */
        if (neg) tmpBuf[tmpLen++] = '-'; // append negative at end
        const long nBytes = std::max(std::min(long(MaxLen) - long(len), long(tmpLen)), 0L);
        const auto rbegin = std::make_reverse_iterator(tmpBuf + tmpLen),
                   rend   = std::make_reverse_iterator(tmpBuf + (long(tmpLen) - nBytes)); // handle truncation in cases where it doesn't fit
        std::copy(rbegin, rend, strBuf.begin() + len); // append in reverse to strBuf
        len += nBytes;
        strBuf[len] = 0; // terminating nul (there is always room for this char)
        return *this;
    }
    constexpr operator const char *() const noexcept { return strBuf.data(); }
    constexpr operator std::string_view() const noexcept { return {strBuf.data(), len}; }
    SBuf &operator=(const std::string_view &sv) noexcept { clear(); return append(sv); }
    SBuf &operator+=(const std::string_view &sv) noexcept { return append(sv); }
};

/// Writes directly to file descriptor 2 on platforms that have this concept (Windows, OSX, Unix, etc).
/// On other platforms is a no-op.  Use this with SBuf() to compose a string to output to stderr
/// immediately.  If writeNewLine is true, then the platform-specific "\r\n" or "\n" will be also written
/// in a second write call.
void writeStdErr(const std::string_view &, bool writeNewLine = true) noexcept;

/// A very rudimentary primitive for signaling a condition from a signal handler,
/// which is intended to get picked-up later by a monitoring thread.
///
/// This class is necessary because none of the C++ synchronization primitives are technically async signal
/// safe and thus cannot be used inside signal handlers.
///
/// Internally, this class uses a self-pipe technique on platforms that have pipe() (such as Windows & Unix).
/// On unknown platforms this behavior is emulated (in a technically async signal unsafe way) via use of C++
/// std::condition_variable. While this latter technique is not techincally safe -- it is only a fallback so
/// that we compile and run on such hypothetical unknown platforms. In practice this fallback technique won't
/// cause problems 99.9999999% of the time (what's more: it is not even used on any known platform).
struct Sem
{
    Sem() = default; ///< may throw InternalError if it could not allocate necessary resources
    /// Call this from a monitoring thread -- blocks until release() is called from e.g. a signal handler
    /// or another thread.  Will return a non-empty optional containing an error message on error.
    std::optional<SBuf<>> acquire() noexcept;
    /// Call this from a signal handler or from a thread that wants to wake up the monitoring thread.
    /// Will return a non-empty optional containing an error message on error.
    std::optional<SBuf<>> release() noexcept;

private:
#if WINDOWS || UNIX
         // async signal safe self-pipe
    struct Pipe { int fds[2]; Pipe(); /* <-- may throw */  ~Pipe(); };
    // copying not supported
    Sem(const Sem &) = delete;
    Sem &operator=(const Sem &) = delete;
#else
      // emulated fallback for unknown platforms
    struct Pipe { std::condition_variable cond; };
#endif
    Pipe p;
};
} // end namespace AsyncSignalSafe

/// Kind of like Go's "defer" statement. Call a lambda (for clean-up code) at scope end.
/// Note for performance, we don't use a std::function wrapper but instead wrap any passed-in lambda directly.
///
/// This is a tiny performance optimization as it avoids a std::function wrapper. You can, however, also use a
/// std::function, with this class -- just be sure it's valid (operator bool() == true), since we don't check for
/// validity on std::function before invoking.
template <typename VoidFuncT = std::function<void(void)>,
          std::enable_if_t<std::is_invocable_v<VoidFuncT>, int> = 0>
struct Defer
{
    using VoidFunc = VoidFuncT;
    Defer(VoidFunc && f) : func(std::move(f)) {}
    Defer(const VoidFunc & f) : func(f) {}
    /// move c'tor -- invalidate `o`, take its function.
    Defer(Defer && o) : func(std::move(o.func)), valid(o.valid) { o.valid = false; }
    /// move assign -- if we have a valid func, invoke it then take `o`'s func, invalidating `o`
    Defer &operator=(Defer && o) {
        if (this != &o) {
            if (valid) func();
            func = std::move(o.func);
            valid = o.valid;
            o.valid = false;
        }
        return *this;
    }

    // Disabled copy c'tor and copy-assign
    Defer(const Defer &) = delete;
    Defer &operator=(const Defer &) = delete;

    /// d'tor -- call wrapped func, if we are still valid.
    ~Defer() { if (valid) func(); }

    /// Mark this instance as a no-op. After a call to disable, this Defer instance  will no longer call its wrapped
    /// function upon descruction.  This operation cannot be reversed.
    void disable() { valid = false; }
protected:
    VoidFunc func;
    bool valid = true;
};

/// Like `Defer`, except you specify a function to be called at creation (immediately). Intended to be used for code
/// clarity so that it's obvious to readers of code what initialization code goes with what cleanup code.
///
/// E.g.:
///
///     RAII r1 {
///         [&]{ someUniqPtr = foo(); },  // called immediately
///         [&]{ someUniqPtr.reset(); }   // called at r1 destruction
///     };
///
/// Is equivalient to:
///
///     someUniqPtr = foo();
///     Defer d1( [&]{ someUniqPtr.reset(); } );
///
/// But the RAII version above is more explicit about what code goes with what cleanup.
struct RAII : public Defer<> {
    /// initFunc called immediately, cleanupFunc called in this instance's destructor
    RAII(const VoidFunc & initFunc, const VoidFunc &cleanupFunc) : Defer(cleanupFunc) { if (initFunc) initFunc(); valid = bool(cleanupFunc); }
    /// initFunc called immediately, cleanupFunc called in this instance's destructor
    RAII(const VoidFunc & initFunc, VoidFunc && cleanupFunc) : Defer(std::move(cleanupFunc)) { if (initFunc) initFunc(); valid = bool(cleanupFunc); }
};


// Thread internal name management (for Log printing, etc)
const std::string & ThreadGetName();
void ThreadSetName(std::string_view name);

template <typename Func, typename ...Args>
void TraceThread(std::string_view threadName, Func f, Args && ...args) {
    ThreadSetName(threadName);
    Debug() << "Thread start";
    f(std::forward<Args>(args)...);
    Debug() << "Thread exit";
}
