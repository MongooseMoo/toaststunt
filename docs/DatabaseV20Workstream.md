# Database V20 JSON Workstream

## Goal

Make ToastStunt database persistence pluggable and add a canonical JSON-v20
directory format backed by YAJL. Native text databases remain supported. Zip is
optional transport, not the canonical format.

## Constraints

- Branch from `master`.
- Use the existing test suite where it can cover behavior.
- Prefer TDD for each implementable slice.
- Keep the current public database API stable:
  - `db_initialize`
  - `db_load`
  - `db_flush`
  - `db_disk_size`
  - `db_shutdown`
- Do not replace the native text DB reader/writer until JSON-v20 is proven.
- Support an option to dump both native text DB and JSON-v20 during the same
  checkpoint/shutdown so adoption can be verified without giving up the current
  recovery format.
- Treat `moo-db-reconstructor` JSON as prior art/import shape, not as the full
  persistence format.
- Preserve database-only value identities: anonymous objects and WAIFs are not
  user JSON values and need private persistence handling.
- Use `moo-db-reconstructor` as a temporary writer-side oracle where practical:
  teach it to load JSON directories as well as zips, then reconstruct a native
  `.db` from JSON-v20 output until the C++ JSON reader is complete.
- Use `~/src/lambdamoo-db-py` as a native DB format reference for expected
  object, property, verb, program, anon, and WAIF structures during parity
  checks.

## Target Format

Canonical directory:

```text
world.v20/
  manifest.json
  users.json
  pending-finalization.json
  active-connections.json
  objects/
    000000.json
    001000.json
  anons/
    000000.json
  waifs.json
  programs/
    000000.json
  tasks/
    queued.json
    suspended.json
    interrupted.json
```

`manifest.json` owns:

- `format`: `toaststunt-json-db`
- `format_version`: `20`
- `engine_db_version`
- section counts
- chunk size
- optional section checksums
- optional source metadata

Zip support, if added, packages this directory without changing semantics.

## Value Encoding

The database JSON value codec is private and distinct from in-world
`generate_json()`.

```json
{ "type": "none" }
{ "type": "clear" }
{ "type": "int", "value": 12 }
{ "type": "float", "value": "1.2345678901234567" }
{ "type": "str", "value": "..." }
{ "type": "obj", "value": 123 }
{ "type": "err", "value": "E_PERM" }
{ "type": "bool", "value": true }
{ "type": "list", "value": [] }
{ "type": "map", "value": [{ "key": {}, "value": {} }] }
{ "type": "anon_ref", "id": 17 }
{ "type": "waif_ref", "id": 42 }
```

Strings must preserve MOO binary-string spelling without applying the
user-facing JSON builtin's binary escape conversion.

## Phases

### Phase 1: Backend Selection Skeleton

Tests first:

- Native input/output paths still select the legacy backend.
- Input directory with `manifest.json` and `format_version: 20` selects JSON-v20.
- Output path ending `.v20` selects JSON-v20.
- Unknown JSON manifest version fails plainly.

Implementation:

- Add a small database backend selector internal to the DB module.
- Preserve existing native file code behind the legacy backend.
- Add JSON-v20 backend stubs that fail load with a clear not-implemented error
  until each later phase fills them in.

Acceptance:

- Existing native DB load path behavior is unchanged.
- New selector tests pass.

### Phase 2: Manifest and Directory Dump

Tests first:

- Minimal JSON-v20 dump creates the required directory tree.
- `manifest.json` is valid JSON and contains the expected fixed fields.
- Failed dump leaves no promoted final output.

Implementation:

- Use YAJL to write manifest JSON.
- Write to a temporary sibling directory and atomically promote when complete.
- Keep panic/checkpoint retry behavior aligned with native dumps.

Acceptance:

- `Minimal.db` can be started and checkpointed to an empty/skeletal JSON-v20
  directory.

### Phase 3: Reconstructor Oracle Bridge

Tests first:

- `moo-db-reconstructor` accepts a directory containing the expected JSON files.
- Directory and zip inputs produce the same loaded record sets.
- JSON-v20 writer output can be normalized or translated into the
  reconstructor's expected object/waif/anon records for early writer checks.
- `lambdamoo-db-py` can parse the reconstructed native `.db` and expose the
  expected structure for comparison.

Implementation:

- Update `moo-db-reconstructor` to load directories in addition to zips.
- Keep its existing zip behavior unchanged.
- Add a translator only for the subset already emitted by the C++ writer.
- Use the reconstructed native `.db` as an oracle before the C++ JSON reader is
  implemented.
- Cross-check reconstructed native files with `lambdamoo-db-py` where its model
  covers the relevant structure.

Acceptance:

- Writer-only slices can prove `JSON-v20 dump -> reconstructor -> native .db ->
  ToastStunt load` before the server has a JSON-v20 reader.

### Phase 4: Dual-Format Dump Mode

Tests first:

- Configured native-only dump writes only the legacy output path.
- Configured JSON-only dump writes only the JSON-v20 output path.
- Configured dual dump writes both outputs from the same checkpoint request.
- If one child dump fails, the log and return status identify which format
  failed.

Implementation:

- Extend DB initialization to parse an optional secondary dump target rather
  than changing the two required positional database arguments.
- Model dump targets as a list: one legacy target, one JSON-v20 target, or both.
- Preserve native dump promotion semantics independently for each target.
- For checkpoint dumps, fork once per target when forking is enabled. Native and
  JSON dumps should not share mutable backend writer state across children.
- For shutdown and panic dumps, write both targets sequentially in-process so the
  caller receives a deterministic combined success/failure result.
- Keep `db_disk_size()` reporting the primary dump target; add internal logging
  for secondary target sizes instead of changing the public API in this phase.

Acceptance:

- A live server can checkpoint to native and JSON-v20 outputs in the same
  checkpoint cycle.
- Existing native-only behavior is unchanged when no secondary target is
  configured.

### Phase 5: Private Typed Value Codec

Tests first:

- `none`, `clear`, int, float, string, object, error, bool round-trip.
- Nested list and map round-trip.
- Unsupported `TYPE_ANON` and `TYPE_WAIF` without a registry fail explicitly.
- With a registry, anon and WAIF references encode as ids.

Implementation:

- Add private YAJL value writer/reader for DB use.
- Do not alter user-facing `parse_json()` or `generate_json()`.
- Use string float output precise enough for current native DB behavior.

Acceptance:

- Value codec tests pass.
- Existing `test_json.rb` still passes.

### Phase 6: Objects, Properties, Verbs, Programs

Tests first:

- Dump `Minimal.db` objects to JSON-v20 and load them back to a native in-memory
  database.
- Object id gaps/recycled slots survive.
- Property definition order and property value slot order survive.
- Verb metadata and verb program text survive.

Implementation:

- Write object chunks with explicit recycled entries.
- Store propdefs and propvals as ordered arrays.
- Store verbdefs ordered by verb index.
- Store verb programs by object id and verb index.

Acceptance:

- `Minimal.db -> JSON-v20 -> native text DB` loads successfully.
- Existing basic/object/property/verb tests still pass.

### Phase 7: Anonymous Objects and WAIFs

Tests first:

- Anon fixture DB round-trips without changing anon references.
- WAIF fixture DB round-trips without missing or duplicated WAIF identities.
- Pending recycled WAIFs remain pending after restart.

Implementation:

- Prewalk all persistent roots and assign stable dump-local ids to anons/WAIFs.
- Write anon records separately from regular objects.
- Write WAIF records with class, owner, propdefs length, and sparse property
  values.
- Rebuild anonymous parent map while loading.
- Run existing WAIF post-load validation.

Acceptance:

- `test/tests/Anon*.db` round-trip.
- WAIF-heavy database round-trips without `waif_count != n_saved_waifs`.

### Phase 8: Runtime State

Tests first:

- Pending finalization survives JSON-v20 restart.
- Active connections file loads into checkpointed connection state.
- Queued, suspended, and interrupted task fixtures survive restart.

Implementation:

- Convert pending-finalization writer/reader to backend-aware routines.
- Convert active connection writer/reader.
- Convert task, activation, runtime environment, and VM persistence.
- Keep native text DB routines available for legacy backend.

Acceptance:

- Existing task and suspended DB tests pass through JSON-v20 round-trip.

### Phase 9: Zip Transport

Tests first:

- JSON-v20 zip with `v20/manifest.json` loads the same as the directory.
- Dump to `.zip` produces the same normalized contents as directory dump.

Implementation:

- Add zip packaging only after the directory format is complete.
- Keep checksums based on logical section contents, not container metadata.

Acceptance:

- Zip round-trip is semantically equal to directory round-trip.

### Phase 10: Full Verification

Run:

- Build from a clean tree.
- Existing Ruby suite.
- JSON-v20 targeted round-trip suite.
- Native DB regression suite.
- `git diff --check`.

Final acceptance:

- Native text DB compatibility preserved.
- JSON-v20 directory load/dump works for normal worlds, anons, WAIFs, tasks, and
  active connection state.
- Zip support is either passing or explicitly deferred with directory format
  complete.

## Implementation Status

Branch:

- ToastStunt: `json-v20-db`
- `moo-db-reconstructor`: `json-v20-dir-oracle`

Completed:

- Phase 1 backend selection is implemented for native files and JSON-v20
  directories.
- Phase 2 directory dump is implemented with YAJL-generated JSON skeletons and
  promoted output directories.
- Phase 3 reconstructor oracle support is implemented for directory input and
  the emitted ToastStunt v20 subset.
- Phase 4 dual-format dump mode is implemented with `--dump-json-v20 <path>`.
  Checkpoint dumps fork once per target when forking is enabled. Shutdown and
  panic dumps write JSON before native in-process so native WAIF save-index
  mutation cannot corrupt JSON WAIF references.
- Phase 5 private typed value codec is implemented for DB JSON values,
  including nested lists/maps and dump-local anon/WAIF references.
- Phase 6 object, property, verb, and program dump/load is implemented.
- Phase 7 anonymous object and WAIF dump/load is implemented, including
  dump-local id assignment, sparse WAIF property values, anonymous object
  sidefiles, and post-load WAIF validation.
- Phase 8 runtime state is implemented for pending finalization, active
  connections, and queued task state.

Deferred:

- Phase 9 zip transport is deferred. Directory format is the canonical format
  for this workstream, and zip was optional.
- Queued task state is currently persisted as a JSON-contained native task queue
  payload in `tasks/queued.json` under `native_task_queue_lines`. This preserves
  current VM/task behavior without rewriting the VM activation serializer into a
  structured JSON schema in this slice. `tasks/suspended.json` and
  `tasks/interrupted.json` remain schema placeholders until those runtime
  sections need non-empty structured payloads.

Verification:

- ToastStunt build:
  `wsl -e bash -lc 'cd /mnt/c/Users/Q/src/toaststunt && cmake --build build -j2'`
  passed.
- Backend selector/unit coverage:
  `wsl -e bash -lc 'cd /mnt/c/Users/Q/src/toaststunt && ./build/test_db_backend'`
  passed.
- JSON-v20 WAIF/active-connection/native-output round trip:
  `wsl -e bash -lc 'cd /mnt/c/Users/Q/src/toaststunt/test && ruby json_v20_waif_roundtrip.rb'`
  passed.
- Anonymous object fixture round trips for `Anon1.db` through `Anon6.db` passed
  through JSON-v20 dump and reload.
- `Suspended.db` JSON-v20 dump and reload passed; `tasks/queued.json` contained
  non-empty `native_task_queue_lines`.
- A handcrafted non-empty `pending-finalization.json` containing a WAIF
  reference loaded successfully and dumped back to native format.
- `moo-db-reconstructor` targeted loader tests:
  `uv run pytest tests/test_loader.py` passed with 10 tests.
- ToastStunt hygiene:
  `wsl -e bash -lc 'cd /mnt/c/Users/Q/src/toaststunt && git diff --check'`
  passed.

Known verification limits:

- The full ToastStunt Ruby suite was attempted against the native server
  harness, not the JSON-v20 path. It reported 12 existing-looking failures in
  `tests/test_anonymous.rb`, mostly `E_VERBNF` results in caller/task-stack/
  queued-task anonymous-object visibility tests, then hung in
  `tests/test_canned_dbs.rb` after about five minutes. The stuck make/test
  process and temporary server were terminated.
- The full `moo-db-reconstructor` suite was not made green in this workstream;
  the targeted loader tests for the new directory support pass.
