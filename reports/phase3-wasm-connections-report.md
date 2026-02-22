# Phase 3: Virtual Connection Layer - Implementation Report

## Result: GATE3_PASS

Gate test passed on iteration 2. Full pipeline verified: JS creates connection, server logs in player, output captured by JS callback.

## What Was Built

### 1. `src/network_wasm.cc` - Virtual Connection Layer

Rewrote the WASM network stub to implement a complete virtual connection system:

**Data structure** - `wasm_handle` struct with:
- Connection ID (slot in static array, max 16 connections)
- `server_handle` back-pointer to server layer
- Output buffer (dynamically growing, starts at 4KB)
- Connected/suspended state flags
- Connection name string

**Exported functions** (callable from JavaScript via `Module._wasm_*` or `Module.ccall`):
- `wasm_new_connection()` - Allocates a wasm_handle, wraps it as a network_handle, calls `server_new_connection(null_server_listener, nh, false)`. Returns connection ID.
- `wasm_inject_input(conn_id, line)` - Looks up handle, calls `server_receive_line(h->shandle, line, false)`.
- `wasm_get_output(conn_id)` - Returns accumulated output buffer content.
- `wasm_clear_output(conn_id)` - Clears the output buffer.
- `wasm_close_connection(conn_id)` - Calls `server_close`, frees resources.

**Network interface functions** updated from stubs to real implementations:
- `network_send_line()` - Appends text to wasm_handle output buffer AND calls `EM_ASM({ Module['print'](...) })` to deliver output to JS in real-time.
- `network_send_bytes()` - Appends raw bytes to output buffer.
- `network_buffered_output_length()` - Returns actual buffer length.
- `network_suspend_input()` / `network_resume_input()` - Set/clear input_suspended flag.
- `network_close()` - Marks handle as disconnected.
- `network_connection_name()` - Returns per-connection name (e.g., "wasm-connection-0").
- `network_shutdown()` - Cleans up all virtual connections.
- Refcount functions return 1 (always valid) instead of 0.

### 2. `CMakeLists.txt` - Export Configuration

Updated Emscripten linker flags:
```
EXPORTED_FUNCTIONS: _main, _wasm_new_connection, _wasm_inject_input,
                    _wasm_get_output, _wasm_clear_output, _wasm_close_connection
EXPORTED_RUNTIME_METHODS: callMain, FS, stringToNewUTF8, UTF8ToString, ccall, cwrap
```

### 3. `test/wasm/gate3_command.js` - Gate Test

Test flow:
1. Boots ToastStunt WASM module with Minimal.db
2. Detects "LISTEN:" in stderr (server log goes to printErr)
3. Creates virtual connection via `Module._wasm_new_connection()`
4. Waits for login (main loop iteration processes empty-string task -> `#0:do_login_command` returns #3)
5. Checks for "*** Connected ***" in print callback output
6. Reports GATE3_PASS

## Connection Lifecycle Observed

```
[gate3] Server is ready, scheduling connection creation...
[gate3] Creating virtual connection...
ACCEPT: #-2 on wasm-local [127.0.0.1], port 0 from wasm-connection-0 [127.0.0.1], port 0
WASM: New virtual connection 0 created
[gate3] Connection created, id=0
CONNECTED: Wizard (#3) on wasm-connection-0
[out] *** Connected ***
GATE3_PASS
```

The full data path:
1. `wasm_new_connection()` -> `server_new_connection(null_server_listener, nh, false)`
2. Server assigns player #-2, listener #0, queues empty-string task
3. `run_ready_tasks()` processes the task via `do_login_task("")`
4. `#0:do_login_command("")` returns #3 (Wizard)
5. `player_connected()` sends "*** Connected ***" via `network_send_line()`
6. `network_send_line()` calls `EM_ASM` to invoke JS `Module['print']`
7. JS callback detects "*** Connected ***" -> GATE3_PASS

## Key Design Decision: EM_ASM for Real-Time Output

Output is delivered to JavaScript two ways:
1. **Buffered**: Appended to `wasm_handle.output_buffer`, retrievable via `wasm_get_output()`
2. **Real-time**: `EM_ASM({ Module['print'](UTF8ToString($0)); })` in `network_send_line()` calls the JS print callback immediately

This dual approach means the gate test doesn't need to poll `wasm_get_output()` -- it receives output as soon as the server sends it, through the same callback mechanism Emscripten uses for `printf`.

## Regression Check

Gate 2 (boot test) still passes after changes.

## Iterations

- **Iteration 1**: Server booted but LISTEN detection failed. The gate test only checked `print` (stdout) for "LISTEN:", but server log goes to `printErr` (stderr). Timeout.
- **Iteration 2**: Fixed gate test to detect LISTEN in `printErr` handler, added fallback check in `.then()` for race condition where LISTEN arrives before Module is set. GATE3_PASS.

## Files Modified

- `src/network_wasm.cc` - Rewrote from stubs to virtual connection layer
- `CMakeLists.txt` - Added EXPORTED_FUNCTIONS and EXPORTED_RUNTIME_METHODS
- `test/wasm/gate3_command.js` - New gate test (created)
