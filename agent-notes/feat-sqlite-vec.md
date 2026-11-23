# sqlite-vec in the server

## Goal

Q, 2026-11-22: "I would like sqlite-vec pr'd into toaststunt's mongoose fork mongoose
branch please", then "let's merge, push, and deploy it to the server as well, but not
restart the server yet", and the deploy explicitly includes PRs #20 and #21, already on
`mongoose`.

## State

- `CMakeLists.txt`: when SQLite is found, downloads the sqlite-vec v0.1.9 release
  amalgamation (sha256 `3acd67cb4aff080c7050926fd3cf8227905fe5b7ee3829d8ee5024ab1283cf61`,
  equal to GitHub's recorded digest for the asset) into the build directory and compiles
  `sqlite-vec.c` into `moo` with `SQLITE_CORE` and `SQLITE_VEC_STATIC`.
- `src/sqlite.cc` `register_sqlite()`: `sqlite3_auto_extension(sqlite3_vec_init)` after
  `sqlite3_config`, so every `sqlite_open()` connection has the `vec_*` functions and
  `vec0` tables. The server log names the sqlite-vec version at start.
- `test/tests/test_sqlite_vec.rb` (functions on a fresh connection; a vec0 nearest-neighbour
  query), added to CI. Passed 2/2 in WSL Debian (SQLite 3.40.1) on a Release build.

## Decisions and why

- Fetched at configure time by pinned URL and hash, not committed into the repo: the
  source exists once, upstream.
- `file(DOWNLOAD)` plus `cmake -E tar` keeps the CMake minimum at 3.12.

## Surprises

- A one-row `sqlite_query` result comes back flat in the test helper, and numeric
  columns come back as numbers.

## Next step

Merge the PR, `stage-release` the merge commit on the server, switch `~/mongoose/moo` to
it, no restart.
