# WAIF owner allocation counts

`waif_owner_count(OBJ owner) => INT`

Returns the number of allocated WAIF instances whose immutable native owner
is the requested object-number slot. The caller must be that owner or a wizard;
other callers receive `E_PERM`. Wizards may query invalid owner slots, including
`#-1`. A slot with no allocated WAIFs returns zero. Standard builtin argument
checks and the independent `protect_waif_owner_count` / `#0:bf_waif_owner_count`
mechanism apply.

The count includes live instances, instances queued for deferred recycling,
and instances retained or resurrected by `:recycle`. Additional references to
one instance do not increase the count. Only actual native freeing decreases
it. Loading a checkpoint reconstructs the count once per distinct instance,
including saved instances awaiting finalization. Invalidating a WAIF's class
does not remove its owner count. Renumbering or recycling an object does not
rewrite the immutable WAIF owner slot.

This query does not run recycling, collect garbage, expose references or scan
objects. Lookup is expected constant time; maintained counters add an owner
map update to allocation, loading and freeing. `waif_stats()` remains unchanged.
The result is a slot count, not evidence identifying an object incarnation.
Code disposing of a private owner must separately authenticate that original
owner lifetime immediately before disposal. A count above zero requires
retaining it, even after a scheduling boundary. With normal protection hooks
enabled, the database hook controls the returned value as for other builtins.

Run isolated lifecycle, protection, and existing WAIF regressions:

```sh
python3 test/run_map_order_regressions.py build/moo --test test_waif_owner_count.rb
python3 test/run_map_order_regressions.py build/moo --test test_waif.rb
```

Verify allocations, alias references, saved native builtin continuations and
two candidate-runtime reloads of an old-runtime checkpoint:

```sh
python3 test/run_waif_owner_count_reload.py build/moo --reload-from /path/to/old/moo
```

These commands use disposable `Test.db` processes on ephemeral localhost
ports, never production. Test fixture verbs use the existing native Ruby
harness's source installation API; they are not live-MOO authorship.

A Linux control runs the identical allocation/free workload on both binaries,
reports native MOO ticks and samples server-process CPU without writing results:

```sh
python3 test/benchmark_waif_owner_count.py /path/to/old/moo build/moo --pairs 6
```

The control covers one owner slot. CPU timer resolution, local contention and
the fixed workload limit its inference; it is not production acceptance.
