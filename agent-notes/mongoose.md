# Mongoose privilege-escalation audit

## Goal

Help Q protect Mongoose by learning ToastStunt's permission model and auditing realistic paths from guest/ordinary-player access to programmer or wizard authority. Treat the C++ server, the MOO database/verbs, SQLite databases, and other sidecars as the eventual combined attack surface. This task begins with learning and source-backed hypotheses; Q has not authorized implementation changes.

## State

- 2026-10-23: Q confirmed the overall direction and supplied three local resources: `C:/Users/Q/code/moo-conformance-tests`, `C:/Users/Q/code/barn`, and `C:/Users/Q/src/toastcore`.
- Read those projects' top-level instructions and READMEs, plus Barn's `docs/mongoose-login.md`, `docs/mongoose-conformance.md`, `docs/reports/mongoose-phase0-barn.md`, and the object/task specs. These are context and secondary evidence, not proof of current Toast behavior.
- The conformance project is a YAML/pytest TCP harness with managed server lifecycle and disposable DB copies. Its documentation identifies ToastStunt as the behavioral oracle.
- Barn is a substantial Go implementation targeting ToastStunt compatibility on the real Mongoose workload, with database tooling and existing Mongoose fetch/local-probe workflows. It is relevant security-audit infrastructure, not a replacement for auditing the running system.
- ToastCore is available as a smaller core DB for initial local experiments. Mongoose also depends on its main Lambda-format DB, SQLite DBs, and sidecars; a ToastCore-only result does not establish Mongoose safety.
- No server has been launched, no production connection made, no DB fetched, and no source/test/config changes made.

## Decisions and why

- Audit the existing Mongoose/Toast system directly; do not frame Barn as the security fix. Q already has a Toast-oracle conformance strategy and real-workload compatibility work.
- Start by mapping Toast's permission enforcement from current C++ source before making behavioral claims or choosing exploit hypotheses.
- Reuse the existing managed conformance harness where it fits. Use private ports and disposable DB/SQLite copies for experiments; do not test destructively against live mongoose.world.
- Audit server and DB behavior together eventually: core-only coverage cannot expose Mongoose verb/sidecar-specific escalation paths.
- Keep verified source behavior, secondary-note claims, hypotheses, and experimental results distinct. Cite current file/line evidence for findings.
- No code, tests whose design depends on an open Q decision, fixes, or production changes until Q explicitly says "make it so." Reading and planning continue.

## Initial source-backed authority map

This is an incomplete map of the current `mongoose` worktree, not a set of vulnerabilities or a complete permission proof.

- Object privilege/state flags are bit positions in `db_object_flag`: user, programmer, wizard, read, write, fertile, and additional anonymous/lifecycle flags (`src/include/db.h:252-279`). `is_wizard`, `is_programmer`, and `is_user` test validity plus the corresponding flag (`src/db_objects.cc:1315-1331`).
- The shared object permission helper allows an operation when the running programmer is a wizard, owns the object, or the object has the requested flag (`src/db_objects.cc:1307-1313`; contract in `src/include/db.h:289-296`). Individual builtins add their own checks, so this helper is not the whole policy.
- `set_task_perms(oid)` changes `RUN_ACTIV.progr`, explicitly the top activation. A non-wizard can only set it to the current `progr`; a wizard can choose another object (`src/execute.cc:3695-3707`). `task_perms()` reads the current activation's `progr`; `caller_perms()` reads the previous activation's `progr`, or `#-1` at the root (`src/execute.cc:3710-3730`).
- `eval()` requires `is_programmer(progr)` before compiling (`src/verbs.cc:631-670`). `set_verb_code()` requires both a programmer flag and verb write permission (`src/verbs.cc:532-577`). Therefore becoming programmer is a meaningful escalation boundary even without wizard.
- `set_player_flag()` is wizard-only (`src/objects.cc:1011-1033`). `switch_player()` is also wizard-only and then validates the old connection and new player object (`src/tasks.cc:2954-3011`).
- The unlogged login task invokes `do_login_command` on `tq->handler`; if the connection is still unlogged and the result is an object with the user flag, the server assigns that object to `tq->player` and completes connection migration (`src/tasks.cc:877-958`). Authentication and any alternate login path in Mongoose's verbs remain unread.
- Protected builtin dispatch attempts `#0:bf_<name>` when `caller()` is not the system object; if that call does not proceed, a non-wizard gets `E_PERM`, while a wizard can fall through (`src/functions.cc:201-287`). The existing YAML describes this boundary in terms of `this`, so resolve the implementation of `caller()` before treating those descriptions as equivalent.
- Existing conformance coverage includes direct non-wizard denial for `switch_player`, direct builtin wizard/programmer property writes, and protected-builtin redirection. The `switch_player` file explicitly says full active-connection behavior is not covered (`moo-conformance-tests/src/moo_conformance/_tests/builtins/switch_player.yaml:97-111`). This is useful baseline coverage, not an escalation-test suite.
- The current `scripts/fetch-mongoose.ps1` downloads `mongoose.db.new`, creates/downloads a consistent backup of `sound.sqlite`, and downloads `executables/tz` (`scripts/fetch-mongoose.ps1:9-43`). It does not inventory or fetch the other SQLite DBs/sidecars Q described, so it is not by itself a complete Mongoose replica workflow.

## Surprises

- The local infrastructure is much more developed than the initial conversation revealed: managed lifecycle, fixture hashing/manifests, real-Mongoose probes, and existing permission-related tests already exist.
- `C:/Users/Q/code/barn` is on `master`, six commits behind `origin/master`, with many tracked-context/untracked investigation files. Do not treat old notes/spec implementation-status sections as current or modify that checkout casually.
- `C:/Users/Q/code/moo-conformance-tests` is on `tests/unknown-builtin-load`, not `master`, and also has untracked files. Record exact revisions before relying on test coverage or running comparisons.
- Existing documentation already contains potentially security-relevant distinctions (for example command dispatch versus programmed verb calls), but each must be verified against the current Toast source before use as an audit claim.

## Next step

1. Inventory the existing permission-related conformance tests and current Toast C++ permission enforcement; identify what is actually covered versus merely documented.
2. Write a compact source-backed permission/authority map in this note, including player/programmer/wizard state, activation permissions, object/property/verb checks, and login/eval entry points.
3. Propose a small first set of guest-to-programmer/wizard hypotheses ranked by reachable impact and evidence gaps. Do not implement fixes without Q's explicit go-ahead.
4. When an experiment becomes useful, select a private managed local fixture (ToastCore first where sufficient; disposable Mongoose plus required SQLite/sidecar copies where the question depends on Mongoose behavior) and record exact source/DB identities.
