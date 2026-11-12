# merge/upstream-2.8.0

## Goal
Daiverd, 2026-11-12: "open a pr to merge upstream toast code that we don't run yet". `mongoose` was 32 commits behind lisdude/master (ToastStunt 2.8.0 and the start of 2.8.1), about 25 of them crash and memory-safety fixes.

## State
A merge commit of upstream/master (31665c5) into mongoose (fd6f9aa). It builds as Release on Debian WSL. These pass through `test/run_map_order_regressions.py`: the five CI test files (test_connection_close_fact, test_switch_player, test_incarnation_validity, test_json, test_coverage) and test_map. The binary loads a 2026-11-12 19:49 UTC Mongoose checkpoint (287,645 objects, 28,548 verbs), listens, and writes a complete shutdown dump. This was a throwaway copy on a private port with `-O`.

## Decisions and why
- Merged rather than rebased, per the fork's convention.
- ChangeLog: took upstream's regrouped 2.8.0 text, which already covers most fixes mongoose had separately, and re-added the two Mongoose-only entries, marked "(Mongoose)": the verb_code() negative parentheses fix and the AST builtins.
- fileio.cc: upstream changed file_grep's third argument to TYPE_FLAG; kept mongoose's file_writelines registration beside it.
- test_json.rb and test_map.rb: both sides added tests at the same place; kept all of them.

## Surprises
- Running tests from a Windows checkout fails: git autocrlf gives test/Test.db CRLF line endings, and the server rejects it ("Bad number"). Run them from a clone made inside WSL.

## Next step
CI on this PR, then Q's review. Not run: the full Ruby suite, ASan/LeakCheck, any live deploy.
