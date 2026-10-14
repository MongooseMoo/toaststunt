# Recycled object slots referenced by WAIFs

An invalid object number can still be the immutable owner or current class of
an allocated WAIF. Assigning a fresh object to that number would attach the old
reference to an unrelated object lifetime. Native allocation therefore reserves
these slots until both maintained reference counts are zero.

The internal eligibility check uses the existing owner and class indexes,
without scanning objects or changing WAIFs. It includes pending finalizers and
resurrected instances. Invalidating a class releases that class reference;
actual native freeing releases its owner reference. Checkpoint loading rebuilds
the indexes before runtime allocation.

Runtime behavior:

- `next_recycled_object()` skips invalid slots referenced by either index.
- `recreate()` rejects such an explicit destination with `E_INVARG`, after its
  existing target, parent and permission checks, before quota debit or
  `initialize`. The refusal applies only before allocation. An initializer may
  legitimately allocate WAIFs owned by the new receiver, including after a
  suspension.
- `renumber()` skips referenced destination holes and keeps its existing
  hierarchy and ownership remapping for the selected destination.
- Ordinary and anonymous `create()` skip referenced appended slots after a
  high-water trim, reserving null holes. A later trim can remove those holes
  again, so every future append checks the maintained indexes again.

No public builtin, protection policy, WAIF ownership rewrite, finalizer hook or
VM tick accounting changes. Eligible holes retain their existing order. This
changes runtime destination selection; it does not change checkpoint format.

Append checks the existing signed-int object-table growth bound before
incrementing the high-water mark or converting the capacity argument. If no
slot within that bound can be appended, `create()` returns `E_QUOTA` and refunds
only quota it actually debited. Emergency wizard creation reports the failure
instead of using an invalid object. Explicit `recreate()` validates a target
already below the high-water mark and does not enter the append failure path.
The capacity refusal is reviewed at source level; tests do not attempt to
allocate a billion-object table.

Run the real lifecycle regressions in an isolated temporary Test.db server:

```
python3 test/run_map_order_regressions.py build/moo --test test_recycled_waif_slots.rb
python3 test/run_recycled_waif_regressions.py build/moo --prepare-with /path/to/old/moo
```

The reload test saves an invalid owner reference with the old runtime, verifies
refusal after two candidate-runtime loads, then releases the actual WAIF and
proves the same numeric slot becomes eligible again. The ordinary suite also
covers class-only references, pending and resurrected instances, error order,
initializer continuation, renumbered hierarchy and a trimmed high-water mark.
