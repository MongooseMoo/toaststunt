# String line primitives

`validate_string_lines(LIST lines)` returns integer `1` when every row is a
STR without a runtime LF or CR. An empty LIST succeeds. Rows are inspected in
order, checking STR type before LF/CR in that row. A non-STR raises `E_TYPE`;
LF/CR raises `E_INVARG`. Literal backslash escapes are ordinary text. The
input is neither changed nor serialized, so this check adds no aggregate
string allocation or serialization quota.
Scanning still takes native time proportional to the number of bytes inspected.

`file_writelines(handle, LIST lines)` returns integer `0` on success and writes
rows in order through the same operation as `file_writeline`. An empty LIST
performs no row operation, including no handle type, handle validity, mode or
FileIO wizard check. A nonempty LIST checks handle INT type and current row STR
type before the usual caller, handle and mode checks. It does not prevalidate
later rows. Native errors preserve earlier writes; callers retain responsibility
for closing handles, including with `try ... finally`.

Each row retains text/binary filtering, `fputs` semantics (decoded NUL truncates
the row), the added LF, flush-on-success in `f` mode, and ignored flush errors.
This is not `file_write`, whose byte-counting and NUL semantics differ.

Both functions participate in normal per-function builtin protection.
`protect_file_writelines` governs the bulk call independently from
`protect_file_writeline`, just as `protect_file_write` governs byte writes.
The bulk writer does not invoke a scalar builtin's optional softcode wrapper.
Nonempty bulk writes still require native FileIO wizard authority.
Builtin protection is checked before the native operation, including for an
empty LIST; the empty-loop guarantee describes the FileIO row checks.

Native regression tests run against an ephemeral localhost server and temporary
Test.db, never a production MOO:

```
python3 test/run_map_order_regressions.py build/moo --test test_string_lines.rb
```

The two write/flush fault controls require POSIX `/dev/full`. The suite creates
only temporary test-directory symlinks; it never writes to a live file directory.
