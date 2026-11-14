# fix/coverage-decompile-panics

## Goal

Q, 2026-11-13 (Mountain): fix `coverage_start()`/`coverage_stop()`, which panicked the
live Mongoose server twice, get the fix into `mongoose` and deploy it. Required: no call
sequence may panic; second start while recording and stop when idle raise E_INVARG;
return format unchanged; ticks with recording off unchanged. Full request on the host:
`~/agent-workspace/tasks/pbt-roadmap/coverage-builtin-fix-prompt.md`.

## State

Both panics are in the line-number lookup in `src/decompile.cc`, not in the recording
state. Journal on the host (`journalctl -u mongoose.service`, times UTC 2026-11-14):

- 05:03:30 `*** PANIC: Missing END_CATCH in DECOMPILE!` at `#6038:coverage_call` line 16.
- 05:29:43 `*** PANIC: Can't do job in FIND_LINE_NUMBER!` in a wizard eval, line 9.

`coverage_stop()` asked `find_line_number()` for every executed pc. That function was
written for the pcs a traceback names (where an error or a call can happen) and panics on
others:

- The pc of a catch expression's `EOP_CATCH`: `is_hot = (is_hot || ptr++ == hot_byte)`
  skipped the `ptr++` when `is_hot` was already set, so the next byte read was
  `OP_EXTENDED` instead of `EOP_END_CATCH`. Any recorded verb that ran `` `x ! ANY' ``.
- The pc of the `OP_PUSH_TEMP` that closes `x[i] = v` or `x[a..b] = v`: skipped without
  being marked, so no node was found. Any recorded verb with an indexed assignment.

Both reproduced on the unfixed build with the same messages (`test_catch_expressions_are_mapped`,
`test_indexed_and_ranged_assignments_are_mapped`), and the live sequence (start, stop,
start, verb call, stop, start, stop across verb frames) panicked the same way
(`test_stop_and_start_inside_a_verb_called_while_recording`).

Fixed:

- `decompile.cc`: the catch step always advances; the indexed-assignment tail marks the
  assignment for its closing `PUSH_TEMP` and reads the top of the expression stack
  (`top_expr_stack - 1`, it read one past it); `OP_MAP_INSERT` checks the pair just
  inserted (it checked the map's first pair).
- New `find_line_number_or_zero()`: same lookup, returns 0 instead of panicking when a
  pc belongs to no line. `coverage_stop()` uses it and leaves such a pc out.
  `find_line_number()` keeps its panic for tracebacks.
- `test/tests/test_coverage.rb`: 11 new tests (both panics, every statement kind with an
  exact line map, the live sequence, 2000 start/stop pairs in one task, second start
  inside a verb, a child stopping the recording, suspend and kill_task while recording,
  children outliving the stop, recompiling the running verb).

Release build in WSL Debian: 25 tests, 125 assertions, all pass.

## Decisions and why

- The recording state (sessions keyed by task id, shared by forks) was read and left
  alone: neither panic came from it.
- A pc with no line is dropped, not reported as an error: every line with instructions
  still gets its other pcs, and the exact-map test shows which structural lines appear.
- No change to the interpreter loop, so ticks are untouched.

## Surprises

- The request's suspects (state freed by stop and reused, forks outliving the stop,
  recompilation) were not the fault. The times in the request (05:10, 05:45) are 05:03
  and 05:29 in the journal.
- `coverage_stop()` decompiles the whole program once per recorded pc. Not changed here.
- To keep a test server's log after the runner deletes its directory, pass a wrapper
  script as the binary that execs the real `moo` with output appended to a file, and use
  the runner's `--name` to run one test.

## Next step

ASan build of the suite; a run on a copy of the Mongoose database; PR into `mongoose`;
deploy; notes on @issue 846 and @issue 795.
