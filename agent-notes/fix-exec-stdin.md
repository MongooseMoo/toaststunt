# Native exec stdin transport

## Goal

Merge lisdude's latest origin/master into mongoose, fix native exec stdin on a branch from master, integrate that fix into mongoose, push mongoose, and stage a server build. Leave the production process running until Q deploys.

## State

The mongoose worktree now contains normal merges of mongoosemoo/mongoose and origin/master. Its five local audit-document commits and untracked logs were preserved. The fix branch starts at origin/master.

The running server executable is the October 27 release under mongoose/releases/04f67f7/moo, with matching source in toaststunt-04f67f7. That source, unlike the stale toaststunt checkout on the host, uses posix_spawn. Both it and master make stdin nonblocking and call write_all, which treats EAGAIN as E_EXEC.

## Decisions and why

Smallest change: retain a write offset on the existing exec owner and register its stdin with network_register_fd's existing writable callback. Retry EINTR, defer EAGAIN/EWOULDBLOCK, close stdin at completion, and preserve native E_EXEC on permanent write errors. Close and unregister stdin during task cleanup. About 70–100 native lines plus focused tests. No wrapper or permission changes.

Keep the standalone fix based on master. Integrate through a normal merge into mongoose, preserving mongoose's existing posix_spawn and signal handling.

## Surprises

The server's original toaststunt checkout is stale and is not the source of its running executable. The local mongoose checkout had divergent audit-only commits; both histories were merged rather than discarded.

## Next step

Live /api/ops/eval reproduced the reported failure using the installed z3_json: the original map returns status sat and model x=3; prepending 65,536 spaces returns {E_EXEC, "Exec error", 0} through #1584:bf_exec, #1300:exec and #1300:exec_json. Raw request uses variables.x={type:int,min:0,max:10}, constraints=[[=,x,3]], timeout=100, matching #1300:test_z3_json. No room objects changed.

Native callback implementation and regressions are written. Integrate, push, read CI, and stage a server build without changing the live executable link or restarting the service. Validate the new regression file against a disposable server and stage the padded Z3 control against the staged binary.
