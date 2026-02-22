# Phase 1 Report: Compile ToastStunt to WASM with Emscripten

## Result: PASS

Gate test `test/wasm/gate1_compiles.sh` passes. `moo.js` (84,801 bytes) and `moo.wasm` (1,306,309 bytes) are produced by a clean build with zero warnings and zero errors.

## Total Compile-Fix Cycles: 7

## Files Created

| File | Purpose |
|------|---------|
| `src/exec_wasm.cc` | Stub exec subsystem. `bf_exec` returns E_PERM. `deal_with_child_exit` and `exec_complete` are no-ops. |
| `src/timers_wasm.cc` | Stub timer subsystem. All timer functions are no-ops. `virtual_timer_available` returns 0. |
| `src/network_wasm.cc` | Stub network subsystem. All ~30 functions from `network.h` implemented as stubs. |
| `src/background_wasm.cc` | Stub background thread subsystem. Always takes synchronous fallback path. |
| `test/wasm/gate1_compiles.sh` | Gate test verifying `moo.js` and `moo.wasm` exist with non-zero size. |

## Files Modified

| File | Changes |
|------|---------|
| `CMakeLists.txt` | Emscripten detection block, guarded `-march=native`, skip all `find_package` when EMSCRIPTEN, swap source files (exclude thpool/linenoise/exec/timers/network/net_mplex/background, include WASM stubs), Emscripten linker flags, `.js` output suffix. |
| `src/include/options.h` | Added `#ifdef WASM_BUILD` block: forces `UNFORKED_CHECKPOINTS`, disables `OUTBOUND_NETWORK`, `USE_TLS`, sets `DEFAULT_THREAD_MODE false`, `NO_NAME_LOOKUP 1`, `TOTAL_BACKGROUND_THREADS 0`. |
| `src/include/network.h` | Guarded `<netdb.h>` include -- uses `<sys/socket.h>` for Emscripten (provides `sa_family_t`). |
| `src/include/background.h` | Guarded `<mutex>`, `<condition_variable>`, `"thpool.h"` includes, `background_waiter` struct, and `pthread_*` externs with `#ifndef __EMSCRIPTEN__`. |
| `src/include/exec.h` | Added `#include <sys/types.h>` for `pid_t` portability. |
| `src/server.cc` | Extensive `#ifdef __EMSCRIPTEN__` guards: POSIX includes, signal handlers, `fork_server`, `getopt_long`, `fclose(stdout/stderr)`, `getpid`, `getrusage/sysinfo`, `linenoise`, `pcre_moo.h`, `parent_pid`/`in_child` usage in `panic_moo`. |
| `src/crypto.cc` | Guarded Nettle includes and entire native implementation. Added WASM stub `register_crypto` that registers all crypto builtins with E_PERM stubs. |

## Errors Encountered and Fixes

### Cycle 1: cmake configuration
**Error**: cmake executable not found on PATH; BISON not found.
**Fix**: Added mingw-winlibs bin, scoop shims, and WinGet links to PATH.

### Cycle 2: `nettle/hmac.h` file not found (crypto.cc)
**Error**: `#include <nettle/hmac.h>` fails because Nettle library is not available in Emscripten.
**Fix**: Guarded all Nettle includes in `crypto.cc` with `#ifndef __EMSCRIPTEN__`. Wrapped entire native implementation (DEF_HASH, DEF_HMAC, all bf_* functions, native `register_crypto`) in the guard. Added `#else` block with a stub `register_crypto` that registers all crypto builtins pointing to a single `bf_crypto_stub` function that returns E_PERM.

### Cycle 3: `pcre.h` file not found (server.cc via pcre_moo.h)
**Error**: `#include "pcre_moo.h"` in server.cc transitively includes `<pcre.h>`, which is not available.
**Fix**: Guarded `#include "pcre_moo.h"` in server.cc with `#ifndef __EMSCRIPTEN__`. Guarded the `pcre_shutdown()` call in the shutdown path.

### Cycle 4: `in_child` and `parent_pid` undeclared (server.cc)
**Error**: `panic_moo()` references `in_child` and `parent_pid`, which are guarded by `#ifndef __EMSCRIPTEN__` but `panic_moo` itself is not guarded.
**Fix**: Added `#ifdef __EMSCRIPTEN__` / `#else` branches in `panic_moo`:
- WASM path: prints "PANIC: %s" without the child indicator
- Guarded the `if (in_child) { kill(parent_pid, SIGINT); _exit(1); }` block with `#ifndef __EMSCRIPTEN__`

### Cycle 5: `pid_t` unknown type (exec.h)
**Error**: `exec.h` declares `exec_complete(pid_t, int)` but `pid_t` is not defined when `config.h` alone is included under Emscripten.
**Fix**: Added `#include <sys/types.h>` to `exec.h` (portable across all platforms).

### Cycle 6: Duplicate symbols at link time
**Error**: `exec_subdir`, `outbound_network_enabled`, `bind_ipv4`, `bind_ipv6` defined in both `server.cc` and the WASM stub files.
**Fix**: Removed the duplicate definitions from `exec_wasm.cc` and `network_wasm.cc`. These globals are already defined in `server.cc` unconditionally.

### Cycle 7: Clean build
No errors. All 63 source files compile and link. `moo.js` and `moo.wasm` produced.

## Gate Test Output

```
$ bash test/wasm/gate1_compiles.sh
moo.js
moo.wasm
GATE1_PASS
```

## Build Artifacts

```
-rw-r--r-- 1 Q 197121   84801 Feb 21 20:30 build-wasm/moo.js
-rw-r--r-- 1 Q 197121 1306309 Feb 21 20:30 build-wasm/moo.wasm
```

## Warnings

Zero warnings in the clean build.

## Native Build Impact

The native (non-Emscripten) CMake configuration still follows the original code path. All guards use `#ifdef __EMSCRIPTEN__`, `#ifdef WASM_BUILD`, or `if(EMSCRIPTEN)` in CMake. The native build was verified to reach the same failure point it had before (missing Nettle library -- a pre-existing condition on this Windows system, not caused by these changes).

## Emscripten Linker Flags

The WASM build uses these Emscripten-specific flags:
- `-sASYNCIFY` -- enables async/await for WASM (needed for future `emscripten_sleep` integration)
- `-sALLOW_MEMORY_GROWTH` -- allows WASM heap to grow beyond initial allocation
- `-sFORCE_FILESYSTEM` -- enables Emscripten's virtual filesystem (needed for DB file I/O)
- `-sINITIAL_MEMORY=268435456` -- 256 MB initial memory
- `-sMODULARIZE` -- wraps output in a module factory function
- `-sEXPORT_NAME=ToastStuntModule` -- the exported module name
- `-fexceptions` -- C++ exception support

## Phase 2 Considerations

1. **Network layer**: `network_wasm.cc` is entirely stubbed. Phase 2 should implement a virtual connection layer using JS `postMessage` for browser-based clients.
2. **Timers**: All timer callbacks are silently dropped. Phase 2 needs `setTimeout`/`setInterval` integration via Emscripten.
3. **Crypto**: All crypto builtins return E_PERM. If crypto is needed in WASM, Nettle can be compiled from source with Emscripten (it's pure C).
4. **Background threads**: Always synchronous. This is correct for single-threaded WASM but may need `ASYNCIFY` integration for long-running tasks.
5. **PCRE**: Not available. `pcre_moo.cc` compiles but PCRE functions will fail at runtime. Could use Emscripten port or the bundled `regexpr.c` fallback.
6. **SQLite**: Not linked. Could use Emscripten's `-sUSE_SQLITE3` port.

## Commit Hash

`60b50bd` -- `Phase 1: Add Emscripten/WASM build target with stub networking, timers, exec, and background`
