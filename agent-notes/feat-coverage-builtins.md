# Task coverage builtins

## Goal

Implement issue 795 coverage_start() and coverage_stop() off upstream master and
merge into mongoose. Q authorized implementation with "Make it so!". No production
deployment or in-MOO framework changes are included.

## State

Native implementation, documentation and Ruby wire regressions are complete.
Verification uses the existing native PR CI workflow and disposable Test.db
runner, extended with test_coverage.rb. No local builds or tests were used.
PR #16 targets mongoose. Its initial add/add workflow
conflict prevented CI from starting. Mongoose was merged into the feature for
integration, retaining all existing checks and adding the coverage step. The
original implementation commit remains directly based on upstream master.
The first CI native build and existing checks passed. Coverage assertions exposed
two test-helper defects: using the full alias string instead of the returned verb
index when setting code, and relying on the shared Ruby parser for singleton
lists inside maps. define() now asserts successful verb creation and compiles by
index. Coverage assertions use native equal() with the raw toliteral() on failure,
following the existing AST suite. Added lifecycle and implicit-return regressions.
Native equal() assertions subsequently exposed the synthetic final OP_DONE being
reported as a line beyond verb_code(). The recorder now excludes that opcode
before creating an entry. It records after execution-limit checks so an opcode
rejected by a hard abort is not reported as executed. The implicit-return test
remains an exact regression. Corrected resumed-suspend expectations to include
the executing continuation on its original source line; the fork test now
compares its complete expected map instead of treating MOO's membership index as
a Boolean.

The native build, actionlint/zizmor, existing connection/player/incarnation/JSON
checks and the coverage suite passed in hosted PR CI run 42910220939. Native
assertions verify exact branch maps, alias/inherited definitions, sorted unique
hits, fork vectors and descendants, explicit resume, unrelated task exclusion,
recompilation (including suspended old activations), permissions/session errors,
owner completion/kill cleanup, implicit returns, independent recordings and
identical MOO ticks with recording off and on. The new suite completes below the
two-second requirement. No claim of measured CPU overhead is made.

PR #16 is the authoritative integration/merge record:
https://github.com/MongooseMoo/toaststunt/pull/16

## Decisions and why

The source spec is on the host at
~/agent-workspace/tasks/issue-795-cpu-for-bugs/coverage-builtin-spec.md.
It explicitly says its Toast source claims were not checked by its author.
The VM, task queues, traceback mapper, program lifetime, verb replacement,
registration and Ruby helpers were inspected directly before implementation.

The existing execute/task owners hold one transient session per recording and
task-ID associations. Forks and successful explicit resumes inherit it; ordinary
suspension preserves it. Stop detaches all members. Owner completion, kill and
abort discard the session; child completion retains its already-recorded hits.
Independent sessions cannot be combined by resume().

The opcode path takes one disabled pointer check. While enabled, recording stores
PC bits per program and bytecode vector. Only stop calls find_line_number().
Program replacement clears old bitmaps and marks obsolete programs, including
old suspended activations first observed after replacement. Program references
keep recorded bytecode valid until stop or owner cleanup.

Toast rejects list map keys. The spec permits verb-name keys, so the result uses
"#definer:full verb names" mapped to sorted unique line numbers. A replaced
program produces an empty list at "!changed:#definer:full verb names". Hits for
the replacement, if executed, use the normal key. docs/coverage.md documents the
precise API, lifetime, errors and limitations. Optional executable-line
enumeration and hit counts are omitted.

The existing mongoose CI workflow and disposable runner are brought onto this
master-based branch at the same repository paths. The workflow retains its
existing checks and adds the coverage suite. PR integration supplies the existing
mongoose-only tests; no second runner or test execution path is introduced.

## Surprises

The mongoose checkout contains unrelated dirty changes to its exec task note and
untracked logs. Preserve them. The primary checkout and numerous existing
worktrees also contain unrelated work; none may be cleaned up for this task.

## Next step

No additional implementation is planned. Delivery ends with the normal mongoose
merge and cleanup of this task's temporary branch/worktree. Production deployment
and the in-MOO consumer/framework remain separate work.
