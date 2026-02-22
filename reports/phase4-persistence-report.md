# Phase 4: Database Persistence in WASM

## Result: GATE4_PASS

Commit: `5843590`

Gate test passed on iteration 2. The WASM server can checkpoint its database to MEMFS.

## What Was Built

### 1. `src/network_wasm.cc` - Added `wasm_checkpoint()` Export

New exported function callable from JavaScript:

```c
EMSCRIPTEN_KEEPALIVE
void wasm_checkpoint(void)
{
    oklog("WASM: Requesting checkpoint (will execute on next main loop iteration)\n");
    server_request_checkpoint();
}
```

This sets the `checkpoint_requested` flag in `server.cc`. The actual checkpoint I/O is then executed by the main loop on its next iteration, which is critical for ASYNCIFY correctness (see Design Decision below).

### 2. `src/server.cc` - Added `server_request_checkpoint()`

```c
#ifdef WASM_BUILD
extern "C" void
server_request_checkpoint(void)
{
    checkpoint_requested = CHKPT_FUNC;
}
#endif
```

This provides a non-static entry point for `network_wasm.cc` to trigger the same checkpoint mechanism that the MOO `dump_database()` builtin uses.

### 3. `CMakeLists.txt` - Export Configuration Updated

Added `_wasm_checkpoint` to `EXPORTED_FUNCTIONS`:
```
EXPORTED_FUNCTIONS: _main, _wasm_new_connection, _wasm_inject_input,
                    _wasm_get_output, _wasm_clear_output,
                    _wasm_close_connection, _wasm_checkpoint
```

### 4. `test/wasm/gate4_persistence.js` - Gate Test

Test flow:
1. Boots ToastStunt WASM module with Minimal.db
2. Detects "LISTEN:" in stderr (server ready)
3. Creates virtual connection, waits for login ("*** Connected ***")
4. Calls `Module._wasm_checkpoint()` to set the checkpoint flag
5. Watches stderr for "CHECKPOINTING on ... finished" (checkpoint completion)
6. Verifies `/Minimal.db.new` exists in MEMFS and is non-empty
7. Reads the file and confirms it contains valid LambdaMOO Database format
8. Prints GATE4_PASS

## Checkpoint Flow Observed

```
[gate4] Phase 1 PASSED: Login output verified ("*** Connected ***")
[gate4] Requesting checkpoint via wasm_checkpoint()...
WASM: Requesting checkpoint (will execute on next main loop iteration)
[gate4] Checkpoint requested. Waiting for main loop to execute it...
CHECKPOINTING on /Minimal.db.new.#1# ...
CHECKPOINTING: Writing values pending finalization ...
CHECKPOINTING: Writing forked and suspended tasks ...
CHECKPOINTING: Writing list of formerly active connections ...
CHECKPOINTING: Writing 4 objects ...
CHECKPOINTING: Done writing 4 objects ...
CHECKPOINTING: Writing 2 MOO verb programs ...
CHECKPOINTING: Done writing 2 verb programs ...
CHECKPOINTING on /Minimal.db.new.#1# finished
[gate4] Checkpoint completion detected in server log
[gate4] Verifying checkpoint output...
[gate4] Minimal.db related files: ["Minimal.db","Minimal.db.new"]
[gate4]   /Minimal.db size=401
[gate4]   /Minimal.db.new size=528
[gate4] Checkpoint file found: /Minimal.db.new (528 bytes)
[gate4] Has DB header: true
[gate4] References objects: true
GATE4_PASS
```

The saved database contains:
- LambdaMOO Database Format Version 17 header
- All 4 objects (#0 System Object, #1 Root Class, #2 The First Room, #3 Wizard)
- 2 verb programs (do_start_script, do_login_command)
- Active connection state (player #3 connected)
- 528 bytes total (larger than the 401-byte input because the format includes additional checkpoint metadata)

## Key Design Decision: Flag-Based Checkpoint (Not Direct Call)

### Iteration 1 (Failed): Direct `db_flush()` call

Initially `wasm_checkpoint()` called `db_flush(FLUSH_ALL_NOW)` directly. This caused a `RuntimeError: memory access out of bounds` crash during ASYNCIFY rewind.

**Root cause:** `db_flush` writes files, which involves `fflush`/`fsync`. In the Emscripten ASYNCIFY model, these operations may trigger async unwinding. When called directly from a JS `setTimeout` callback (outside the main loop's ASYNCIFY context), the rewind state became corrupted. The function returned 0 (the default before async completion), the log showed the checkpoint actually succeeded asynchronously, but then the rewind crashed.

### Iteration 2 (Passed): Set flag, let main loop execute

The fix was to split the operation:
1. `wasm_checkpoint()` just sets `checkpoint_requested = CHKPT_FUNC` (no I/O, instant, no ASYNCIFY needed)
2. The main loop (which has proper ASYNCIFY wrapping) detects the flag on its next iteration and executes `db_flush(FLUSH_ALL_NOW)` in the correct context

This matches exactly how the MOO `dump_database()` builtin works -- it sets the flag, doesn't do I/O itself.

## Checkpoint Internals

The checkpoint mechanism (in `src/db_file.cc`):
1. `db_flush(FLUSH_ALL_NOW)` calls `dump_database(DUMP_CHECKPOINT)`
2. With `UNFORKED_CHECKPOINTS` defined (required for WASM), the dump is synchronous -- no fork
3. Writes to temp file `/Minimal.db.new.#<generation>#`
4. On success, renames temp file to `/Minimal.db.new`
5. In MEMFS, both the write and rename are in-memory operations

## Regression Check

Gate 3 (command round-trip) still passes after changes.

## Files Modified

- `src/network_wasm.cc` - Added `wasm_checkpoint()` export, `#include "db.h"` removed (not needed for flag approach)
- `src/server.cc` - Added `server_request_checkpoint()` (WASM_BUILD only)
- `CMakeLists.txt` - Added `_wasm_checkpoint` to EXPORTED_FUNCTIONS
- `test/wasm/gate4_persistence.js` - New gate test (created)
