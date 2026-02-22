/* exec_wasm.cc -- WASM stub for exec subsystem.
 * fork/exec/pipe/signals are unavailable in WASM.
 * exec() returns E_PERM; deal_with_child_exit() is a no-op.
 */

#include "exec.h"
#include "functions.h"
#include "list.h"
#include "utils.h"

/* exec_subdir is defined in server.cc */

static package
bf_exec(Var arglist, Byte next, void *vdata, Objid progr)
{
    free_var(arglist);
    return make_raise_pack(E_PERM, "exec() is not available in WASM builds", var_ref(zero));
}

void
deal_with_child_exit(void)
{
    /* no-op in WASM */
}

pid_t
exec_complete(pid_t pid, int code)
{
    return 0;
}

void
register_exec(void)
{
    register_function("exec", 1, 3, bf_exec, TYPE_LIST, TYPE_STR, TYPE_LIST);
}
