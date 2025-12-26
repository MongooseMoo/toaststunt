/*
 * Platform compatibility layer for Windows support
 * Handles differences between MSVC, MinGW, and Unix platforms
 */

#ifndef PLATFORM_H
#define PLATFORM_H

#ifdef _WIN32

/* Windows platform */
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <io.h>
#include <process.h>
#include <direct.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <time.h>

/* Detect compiler: MSVC vs MinGW */
#ifdef _MSC_VER
/* Microsoft Visual C++ */
#define PLATFORM_MSVC 1

/* MSVC doesn't have these POSIX types */
typedef int pid_t;
typedef unsigned short mode_t;
typedef int ssize_t;
typedef int clockid_t;

/* MSVC has native snprintf since VS2015, don't redefine */

/* String comparison functions */
#define strcasecmp _stricmp
#define strncasecmp _strnicmp

#else
/* MinGW - has more POSIX compatibility */
#define PLATFORM_MINGW 1
/* MinGW provides pid_t in sys/types.h */
/* MinGW may need snprintf mapping for older versions */
#ifndef snprintf
#define snprintf _snprintf
#endif
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#endif /* _MSC_VER */

/* Windows has select() in winsock2 */
#ifndef HAVE_SELECT
#define HAVE_SELECT 1
#endif

/* POSIX function mappings for Windows - common to both MSVC and MinGW */
#define getpid _getpid
#define access _access
#define unlink _unlink
#define dup _dup
#define dup2 _dup2
#define fileno _fileno
#define isatty _isatty
#define getcwd _getcwd
#define chdir _chdir
#define mkdir(path, mode) _mkdir(path)
#define rmdir _rmdir

/* File access mode constants */
#ifndef R_OK
#define R_OK 4
#endif
#ifndef W_OK
#define W_OK 2
#endif
#ifndef X_OK
#define X_OK 0  /* Windows doesn't have execute permission check */
#endif
#ifndef F_OK
#define F_OK 0
#endif

/* Provide a simple asprintf implementation */
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>

static inline int vasprintf(char **strp, const char *fmt, va_list ap)
{
    va_list ap_copy;
    va_copy(ap_copy, ap);
    int len = _vscprintf(fmt, ap_copy);
    va_end(ap_copy);
    if (len < 0) return -1;
    *strp = (char *)malloc(len + 1);
    if (!*strp) return -1;
    return vsprintf(*strp, fmt, ap);
}

static inline int asprintf(char **strp, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int ret = vasprintf(strp, fmt, ap);
    va_end(ap);
    return ret;
}

/* fsync replacement - flushes file buffers to disk */
#define fsync(fd) _commit(fd)

/* Socket operations - Windows uses different function names */
#define SOCKET_CLOSE(s) closesocket(s)
#define SOCKET_ERROR_CODE WSAGetLastError()

/* For fcntl O_NONBLOCK equivalent */
static inline int set_socket_nonblocking(SOCKET s)
{
    u_long mode = 1;
    return ioctlsocket(s, FIONBIO, &mode);
}

/* Random number generation - using bcrypt */
#include <bcrypt.h>
/* Note: Link against bcrypt.lib in CMakeLists.txt */

static inline int get_random_bytes(void *buf, size_t len)
{
    NTSTATUS status = BCryptGenRandom(NULL, (PUCHAR)buf, (ULONG)len, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return (status >= 0) ? 0 : -1;
}

/* gettimeofday replacement - use winsock2's timeval */
static inline int gettimeofday(struct timeval *tv, void *tz)
{
    (void)tz;
    FILETIME ft;
    ULARGE_INTEGER uli;
    GetSystemTimeAsFileTime(&ft);
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    /* Convert from 100-nanosecond intervals since 1601 to Unix epoch */
    uli.QuadPart -= 116444736000000000ULL;
    tv->tv_sec = (long)(uli.QuadPart / 10000000);
    tv->tv_usec = (long)((uli.QuadPart % 10000000) / 10);
    return 0;
}

/* clock_gettime replacement */
#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1
#define CLOCK_MONOTONIC_RAW 2  /* same as CLOCK_MONOTONIC on Windows */

/* timespec is already defined in MSVC's time.h - don't redefine */

static inline int clock_gettime(clockid_t clk_id, struct timespec *tp)
{
    (void)clk_id;
    LARGE_INTEGER freq, count;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&count);
    tp->tv_sec = (long)(count.QuadPart / freq.QuadPart);
    tp->tv_nsec = (long)(((count.QuadPart % freq.QuadPart) * 1000000000LL) / freq.QuadPart);
    return 0;
}

/* timeradd/timersub macros - not available on Windows */
#ifndef timeradd
#define timeradd(a, b, result) \
    do { \
        (result)->tv_sec = (a)->tv_sec + (b)->tv_sec; \
        (result)->tv_usec = (a)->tv_usec + (b)->tv_usec; \
        if ((result)->tv_usec >= 1000000) { \
            ++(result)->tv_sec; \
            (result)->tv_usec -= 1000000; \
        } \
    } while (0)
#endif

#ifndef timersub
#define timersub(a, b, result) \
    do { \
        (result)->tv_sec = (a)->tv_sec - (b)->tv_sec; \
        (result)->tv_usec = (a)->tv_usec - (b)->tv_usec; \
        if ((result)->tv_usec < 0) { \
            --(result)->tv_sec; \
            (result)->tv_usec += 1000000; \
        } \
    } while (0)
#endif

/* strsep - not available on Windows */
static inline char *strsep(char **stringp, const char *delim)
{
    char *s = *stringp;
    char *e;
    if (s == NULL) return NULL;
    e = strpbrk(s, delim);
    if (e) {
        *e++ = '\0';
    }
    *stringp = e;
    return s;
}

/* pipe - Windows uses _pipe with different semantics */
static inline int pipe(int pipefd[2])
{
    return _pipe(pipefd, 4096, _O_BINARY);
}

/* No fork on Windows - use UNFORKED_CHECKPOINTS */
#ifndef UNFORKED_CHECKPOINTS
#define UNFORKED_CHECKPOINTS
#endif

/* Signal replacements - Windows doesn't have Unix signals */
#ifndef SIGPIPE
#define SIGPIPE 0  /* Ignored - Windows handles this differently */
#endif
#ifndef SIGCHLD
#define SIGCHLD 0
#endif
#ifndef SIGUSR1
#define SIGUSR1 0
#endif
#ifndef SIGUSR2
#define SIGUSR2 0
#endif
#ifndef SIGHUP
#define SIGHUP 0
#endif
#ifndef SIGQUIT
#define SIGQUIT 0
#endif

/* Process control stubs for Windows */
#define WNOHANG 1
#define WEXITSTATUS(status) ((status) & 0xff)
#define WIFEXITED(status) (1)
#define WIFSIGNALED(status) (0)
#define WTERMSIG(status) (0)

static inline int kill(pid_t pid, int sig) { (void)pid; (void)sig; return -1; }

/* Note: getline is provided locally by files that need it (e.g., fileio.cc) */

#else /* Unix/POSIX */

#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <fcntl.h>
#include <dirent.h>

#define SOCKET_CLOSE(s) close(s)
#define SOCKET_ERROR_CODE errno

static inline int set_socket_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static inline int get_random_bytes(void *buf, size_t len)
{
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, len);
    close(fd);
    return (n == (ssize_t)len) ? 0 : -1;
}

#endif /* _WIN32 */

#endif /* PLATFORM_H */
