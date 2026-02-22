/* Minimal PCRE 8.45 configuration for Emscripten/WASM build.
   This is NOT the ToastStunt config.h -- it is specific to the bundled PCRE sources.
   Included by PCRE source files via -include directive. */

#ifndef PCRE_CONFIG_WASM_H
#define PCRE_CONFIG_WASM_H

/* We have standard C library functions */
#define HAVE_MEMMOVE 1
#define HAVE_STRERROR 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_STDINT_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_LIMITS_H 1

/* Enable 8-bit PCRE library (the one ToastStunt uses) */
#define SUPPORT_PCRE8 1

/* Enable UTF-8 support */
#define SUPPORT_UTF 1

/* Enable Unicode Character Properties */
#define SUPPORT_UCP 1

/* Link size: 2 bytes (default, supports patterns up to 64K) */
#ifndef LINK_SIZE
#define LINK_SIZE 2
#endif

/* Default match limit */
#ifndef MATCH_LIMIT
#define MATCH_LIMIT 10000000
#endif

/* Recursion match limit */
#ifndef MATCH_LIMIT_RECURSION
#define MATCH_LIMIT_RECURSION MATCH_LIMIT
#endif

/* Max named capture groups */
#ifndef MAX_NAME_COUNT
#define MAX_NAME_COUNT 10000
#endif

/* Max name size */
#ifndef MAX_NAME_SIZE
#define MAX_NAME_SIZE 32
#endif

/* Default newline is LF */
#ifndef NEWLINE
#define NEWLINE 10
#endif

/* Nested parentheses limit */
#ifndef PARENS_NEST_LIMIT
#define PARENS_NEST_LIMIT 250
#endif

/* Use heap instead of C stack for match recursion.
   Critical for WASM which has limited stack space. */
#define NO_RECURSE 1

/* We are linking statically */
#define PCRE_STATIC 1

/* POSIX malloc threshold */
#ifndef POSIX_MALLOC_THRESHOLD
#define POSIX_MALLOC_THRESHOLD 10
#endif

/* Package info */
#define PACKAGE "pcre"
#define PACKAGE_NAME "PCRE"
#define PACKAGE_VERSION "8.45"
#define PACKAGE_STRING "PCRE 8.45"
#define VERSION "8.45"

/* No JIT for WASM */
/* #undef SUPPORT_JIT */

/* No pthreads in WASM */
/* #undef HAVE_PTHREAD */

#endif /* PCRE_CONFIG_WASM_H */
