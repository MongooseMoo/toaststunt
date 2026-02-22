/* background_wasm.cc -- WASM stub for the background thread subsystem.
 * pthreads/thread pool are not available in basic WASM.
 * All background operations run synchronously on the main thread,
 * using the same fallback path as the native build when threading is disabled.
 */

#include "config.h"
#include "functions.h"
#include "bf_register.h"
#include "storage.h"
#include "tasks.h"
#include "utils.h"
#include "server.h"
#include "list.h"
#include "log.h"
#include "map.h"
#include "structures.h"

/* Since threaded functions can only return Vars, not packages, we instead
 * create and return an 'error map'. Which is just a map with the keys:
 * error, which is an error type, and message, which is the error string. */
void make_error_map(enum error error_type, const char *msg, Var *ret)
{
    static const Var error_key = str_dup_to_var("error");
    static const Var message_key = str_dup_to_var("message");

    Var err;
    err.type = TYPE_ERR;
    err.v.err = error_type;

    *ret = new_map();
    *ret = mapinsert(*ret, var_ref(error_key), err);
    *ret = mapinsert(*ret, var_ref(message_key), str_dup_to_var(msg));
}

/* In WASM, always run synchronously -- the same path the native build takes
 * when get_thread_mode() returns false. */
package
background_thread(void (*callback)(Var, Var*, void*), Var* data,
                  void *extra_data, void (*cleanup)(void*))
{
    Var r;
    callback(*data, &r, extra_data);
    free_var(*data);
    if (cleanup)
        cleanup(extra_data);
    return make_var_pack(r);
}

void background_shutdown()
{
    /* no-op -- no threads to wait for */
}

static task_enum_action
background_enumerator(task_closure closure, void *data)
{
    return TEA_CONTINUE;
}

static package
bf_threads(Var arglist, Byte next, void *vdata, Objid progr)
{
    free_var(arglist);

    if (!is_wizard(progr))
        return make_error_pack(E_PERM);

    /* No background threads in WASM -- return empty list */
    return make_var_pack(new_list(0));
}

static package
bf_thread_pool(Var arglist, Byte next, void *vdata, Objid progr)
{
    free_var(arglist);

    if (!is_wizard(progr))
        return make_error_pack(E_PERM);

    return make_raise_pack(E_PERM, "Thread pools not available in WASM", var_ref(zero));
}

void
register_background()
{
    register_task_queue(background_enumerator);
    /* Skip thpool_init -- no thread pool in WASM */
    register_function("threads", 0, 0, bf_threads);
    register_function("thread_pool", 2, 3, bf_thread_pool, TYPE_STR, TYPE_STR, TYPE_INT);
}
