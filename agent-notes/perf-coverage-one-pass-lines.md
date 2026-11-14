# perf/coverage-one-pass-lines

## Goal

Q, 2026-11-13 (Mountain), about `coverage_stop()` decompiling a verb once per recorded
pc: "why on earth does it do that? can we fix that?", then "Make it so!". Decompile each
recorded verb once; keep one lookup for tracebacks and coverage.

## State

Done on this branch.

- Before: `coverage_stop()` called the traceback lookup once per recorded pc, and each
  call decompiled the whole program. Measured on a copy of the Mongoose database
  (`mongoose.db.new.2026-11-12T0055`, 28,656 programs): 6,898,956 lookups in 367 s,
  about 53 microseconds each.
- `src/decompile.cc`: the lookup's state was one hot byte, one hot node and one position.
  It is now a set: hot bytes are one pc or a bitmap of pcs (`is_hot_byte`), and marks are
  a map from node to positions (`mark_hot`, `is_marked`). The `HOT*` macros keep their
  meaning. The tree walk (`add_hot_lines`, was `find_hot_node`) appends the line of every
  marked node instead of stopping at the first.
- `find_line_number()` (tracebacks) is the same code with one pc; its results, its cache
  and its panic for a pc with no node are as before.
- `find_line_numbers()` (new, replaces `find_line_number_or_zero()`) takes a vector's
  bitmap. `coverage_stop()` calls it once per recorded vector.
- A node freed while decompiling gives up its mark first (`drop_node`), and its caller
  passes the mark on. Before, the freed node kept the mark and the next node allocated at
  that address inherited it.
- Tests: `test_a_loop_that_never_runs_does_not_report_its_body`,
  `test_a_long_verb_maps_every_line_in_one_pass`. Suite: 28 tests, pass on Release and
  AddressSanitizer builds in WSL Debian.

## Check against the deployed code (3f4cb16), whole database

Two throwaway builtins (local branches, never pushed) wrote the line of every byte offset
of every verb, one offset per lookup, from each code base.

- 4,784,686 offsets are placed on a line by both. 4,768,911 agree.
- 15,775 differ, all explained by the freed-node fault above:
  - 15,516 are the `OP_IMM` that initializes a for-list loop (checked by opcode). Old:
    a line in the loop body (so a loop that never ran reported its body). New: the `for`
    line.
  - 259 are instructions inside a for-list body (checked: the old line is a for-list line
    in all 259, the new line is after it). Old: the enclosing `for` line. New: the line
    the instruction is on, for example an `except` line for its codes and label.
- No offset is placed by one and not the other. No offset gets more than one line.
- New code, one lookup per vector with every offset set, against the union of its
  per-offset answers: equal for all 29,133 vectors.
- Time for those 29,133 one-pass lookups: 0.92 s in total (367 s per offset before).

None of the differing instructions can raise an error, so tracebacks are not expected to
change.

## Decisions and why

- One lookup, not a second coverage-only one: tracebacks call the set code with a set of
  one pc.
- The mark map is released after every lookup, and `is_marked` returns at once when
  nothing is hot, so plain decompiling (`verb_code()`, checkpoints) does no map work.

## Surprises

- The freed-node fault was already in the single-pc code (upstream too); nobody saw it
  because tracebacks never ask about those instructions.
- An AddressSanitizer run started while two other builds were loading the machine exited
  1 with its output filtered away by my own command; the rerun passed. Cause not seen.

## Next step

PR into `mongoose`, deploy.
