# Line coverage

`coverage_start()` and `coverage_stop()` take no arguments and require wizard
task permissions. Start returns zero and raises `E_INVARG` if the current task
already belongs to a recording. Stop raises `E_INVARG` without a recording.

Start records the current task, its subsequent forks (including descendants),
and suspended tasks it successfully resumes with `resume()`. Suspension, reads,
and asynchronous builtins preserve the recording through the existing task ID.
Unrelated tasks are excluded. Resuming a task belonging to a different active
recording raises `E_INVARG` before changing that task.

A task may record any number of times, one recording after another: once stop
has returned, start may be called again, in the same verb or in any verb the
task calls or returns to. Starting, stopping, suspending, forking, killing
tasks and recompiling verbs in any order never stops the server; the only
errors are the `E_INVARG` cases above.

Stop ends the entire recording, detaches its tasks, and returns a map:

```moo
["#123:calculate calc" -> {1, 2, 5}]
```

Keys contain `#`, the defining object's decimal number, `:`, and the full verb
name string from `verb_info()`, including aliases. Calls through children or
aliases accumulate under the defining verb. Values are sorted, unique line
numbers in `verb_code()` numbering. Anonymous receivers call verbs defined on
their permanent ancestors and are reported under those defining objects.
Temporary `eval()` programs have no defining verb and are omitted.

Recompilation drops the replaced program's hits. The result includes an empty
list at `!changed:` followed by that verb's normal key. Hits from the replacement
program, if executed, appear under its normal key. An already-running old
activation cannot add stale lines after replacement. Structural lines can be
reported when executable instructions map to them; this is line coverage, not
branch coverage or a list of all executable lines.

Any recorded wizard task can stop the shared recording. If the task that started
it finishes, aborts, or is killed before stopping, its entire recording is
discarded. Other child termination releases that child's association while
retaining its hits. Recordings are transient and do not survive a server restart.
Stop returns only work executed before the call; pending children are detached,
not awaited or killed.

The disabled opcode path checks one pointer and does not change MOO tick
accounting. Recording sets bits for program/vector/PC; only stop maps PCs to
lines, with `find_line_numbers()`: the traceback mapper's lookup given all of a
vector's recorded PCs at once. It decompiles each recorded vector once and
leaves out a PC that belongs to no line, where `find_line_number()` would panic.
Stop's conversion uses CPU time: one decompile for each recorded verb.
