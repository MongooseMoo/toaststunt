# Private connection close fact

Before invoking `user_disconnected` or `user_client_disconnected`, the runtime
can record `object._connection_closed = {object.incarnation_id, time()}`.
The object must have a nonempty string incarnation and an existing property
owned by a wizard, with neither public read nor public write permission.
Worlds without that schema retain their existing behavior.

This is a connection fact. It does not change the world's public presence
timestamps or its visibility policy, and does not execute additional MOO
instructions in the disconnect callback. A consumer checks the saved incarnation
against the current object before using the Unix-seconds timestamp. Recycling
removes the old object's properties; an explicit incarnation renewal also makes
the previous fact unusable. Restored checkpoint connections receive the normal
disconnect callback and a close fact when the runtime retires them on startup.

Run the existing isolated native wire harness:

```
python3 test/run_map_order_regressions.py build/moo --test test_connection_close_fact.rb
```

The native tests close their own harness connections and exercise client EOF,
server boot, immutable lifetime capture and permission filtering. In-game
consumers retain their domain-owned fixture tests and existing lifecycle APIs.
