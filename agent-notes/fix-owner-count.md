# fix/owner-count

## Goal

Q, 2026-11-22, on the Mongoose test suites' fixture cleanup: draft the server fix, "Up to PR only".
recycle() runs the SAFE_RECYCLE owner fixup (a walk over every object, verb and
property value) for any object that has ever been an owner. Keep a count of the
owner fields naming each object instead, and skip the walk when it is zero.

## State

Built and tested locally (WSL Debian, Release); PR open against `mongoose`. Not merged, not
deployed: Q merges, rebuilds and restarts.

- Before: `Object.is_owner` was set whenever an owner field was written and never cleared, so
  a test fixture that once owned anything paid the walk on recycle (75-105 ms on a copy of
  the live database, 6 ms otherwise).
- Now `Object.owned_count` counts owner fields of permanent objects that name the object.
  `db_owner_ref(holder, owner)` / `db_owner_unref(holder, owner)` are called wherever such a
  field is written or removed:
  - object owner: `dbpriv_set_object_owner` (create, `.owner =`, emergency wizard);
  - verbs: `db_add_verb`, `db_set_verb_owner`, `db_delete_verb`;
  - property values: `insert_prop2` (add_property on an object and every descendant),
    `remove_prop2` (delete_property), `relayout_properties` (chparent), `db_set_property_owner`
    (set_property_info; now takes the holder object);
  - `db_destroy_object` drops the destroyed object's own fields; `db_make_anonymous` moves an
    object's fields to the anonymous rule; `db_fixup_owners` zeroes the recycled object's count;
    `db_renumber_object` recounts the new number with one walk (renumber already walks).
  - load: `db_count_all_owners()` after `read_db_file()` resets and recounts everything.
- Anonymous holders are not counted: a recount cannot reach a parentless anonymous object, so
  a decrement from one could undercount. An owner named by an anonymous object's field gets the
  old sticky mark (`anon_owner`), as before. Owner numbers with no object keep their count in
  `unbound_owner_counts` and pass it to an object created at that number.
- A missed decrement only overcounts (the walk runs, as before); every write site increments.

## Tests

`test/tests/test_owner_count.rb`, also in CI: fixup exactness through every writer (object,
verb, property, inherited and `c` slots, chparent, renumber, anonymous holders) and a timing
check that an object that gave everything away recycles like one that never owned anything.
The timing test fails on the old binary (gave away 90 us = still an owner 91 us) and passes on
this one. 20 repo suites pass (list in the PR).

## Next step

Q reviews, merges, rebuilds the live server and restarts it.
