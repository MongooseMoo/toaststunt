# Runtime connection identity and original input

`connection_binding(OBJ actor)` returns `0`, or this exact list:

```
{{bootNonce, physicalSerial, bindingEpoch, actor}, lastInputOrdinal}
```

Only a verb whose programmer is the actor, or a wizard, may call it; other callers receive
`E_PERM`. The four-element inner list identifies the current live binding.
`bootNonce` is a fresh 32-character lowercase hexadecimal string from 128 bits
of OS entropy on every server process start, including TEST builds. The serial
identifies a physical socket. An eligible native login or player switch publishes
a fresh epoch. Both are positive integers. Reconnecting creates a new serial.
`lastInputOrdinal` is the same socket's most recently enqueued physical input
ordinal, initially zero. It is a watermark, outside binding equality.

`task_network_origin()` returns `0`, or this exact list for the current task:

```
{{bootNonce, physicalSerial, bindingEpoch, actor}, originalOrdinal,
 actualKind, genuineTelnetOob, listener, acceptedLocalPort, outbound}
```

`actualKind` is the string `"oob"`, `"inband"`, or `"binary"`, reflecting actual
dispatch after input-option handling. The two flags are integers `0` or `1`.
`listener` is an object; `acceptedLocalPort` is the physical socket's local port
captured at birth, before any PROXY-protocol rewrite. `outbound` distinguishes
server-created outbound connections. The ordinal is positive and increases per
physical socket across player switches. A genuine Telnet negotiation is marked
independently of dispatch: a typed `#$#` prefix has flag zero, and disabled OOB
dispatch can produce `"inband"` with flag one. Binary dispatch takes precedence.

The original binding and ordinal are captured when real network input enters
the queue. They remain unchanged across subsequent player switches, ordinary
verb calls, eval, suspension, read/read_http, and native background waits.
The input consumed by a read does not replace its waiting task's origin.
`force_input`, forked tasks, restored tasks, server notifications, nested native
error/lag callbacks, startup and emergency tasks have no original network input
and return zero. Changing task-local data cannot change native provenance.
These APIs return fresh MOO values; editing a returned list changes no metadata.

Consumers must compare the complete four-field identity and inspect all route
fields required by their protocol. An origin identifies the task's original
input, not its current authority: it can belong to a superseded binding. For
ordered report recovery, seed a missing report or a report from another binding
with the **current** enqueue head in the same nonyielding operation. For an
existing report from the same binding, retain its previous watermark and the
accepted original ordinal; advancing every report to the head can discard valid
queued work. The head never substitutes for the original ordinal or identity.

Object destruction and actual renumbering retire matching current receipts
without booting sockets or rewriting their players. A later real rebind can
create a new epoch. Serial or input-ordinal exhaustion permanently makes that
physical connection ineligible; existing input and login continue with zero
metadata. Eligible epoch exhaustion raises `E_QUOTA` before `switch_player`
changes either task queue; synchronous return-login instead leaves its negative
connection unbound. Native counters never wrap or reuse an identity. Failure to
obtain boot entropy is fatal at startup. Metadata is runtime-only: database
formats and existing serialized task data are unchanged.

A login verb that has already booted its connection may still return an actor,
as in existing login handling. That exact socket completes the existing rebind and
notifiers with no eligible receipt. A disconnect flag raised between reservation
and rebind also retires authority without interrupting existing cleanup; merely
clearing the flag cannot revive the retired receipt.
