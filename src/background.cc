#include "background.h"
#include "bf_register.h"
#ifdef _WIN32
#include "platform.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <unistd.h>                     // sleep()
#endif
#include "storage.h"                    // myfree, mymalloc
#include "tasks.h"                      // TEA
#include "utils.h"                      // var_dup
#include "server.h"                     // server options
#include "list.h"                       // listappend
#include "network.h"                    // network_fd shenanigans
#include "log.h"                        // errlog
#include "map.h"
#include <unordered_map>

#ifdef _WIN32
/*
 * Windows doesn't have socketpair(), so we create a self-connected
 * socket pair using localhost. This allows us to use select() on
 * the socket pair, which is needed because Windows select() doesn't
 * work with pipes.
 */
static int win32_socketpair(int domain, int type, int protocol, SOCKET sv[2])
{
    (void)domain;  /* always use AF_INET */
    (void)protocol; /* always use 0 */

    SOCKET listener = INVALID_SOCKET;
    SOCKET client = INVALID_SOCKET;
    SOCKET server = INVALID_SOCKET;
    struct sockaddr_in addr;
    int addrlen = sizeof(addr);

    sv[0] = sv[1] = INVALID_SOCKET;

    /* Create listening socket */
    listener = socket(AF_INET, type, 0);
    if (listener == INVALID_SOCKET)
        goto fail;

    /* Bind to loopback on ephemeral port */
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;

    if (bind(listener, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR)
        goto fail;

    if (listen(listener, 1) == SOCKET_ERROR)
        goto fail;

    /* Get the port we bound to */
    if (getsockname(listener, (struct sockaddr*)&addr, &addrlen) == SOCKET_ERROR)
        goto fail;

    /* Create client socket and connect */
    client = socket(AF_INET, type, 0);
    if (client == INVALID_SOCKET)
        goto fail;

    if (connect(client, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR)
        goto fail;

    /* Accept the connection */
    server = accept(listener, NULL, NULL);
    if (server == INVALID_SOCKET)
        goto fail;

    closesocket(listener);

    sv[0] = client;  /* read end (also write) */
    sv[1] = server;  /* write end (also read) */

    return 0;

fail:
    if (listener != INVALID_SOCKET) closesocket(listener);
    if (client != INVALID_SOCKET) closesocket(client);
    if (server != INVALID_SOCKET) closesocket(server);
    return -1;
}
#endif

/*
  A general-purpose extension for doing work in separate threads. The entrypoint (background_thread)
  will suspend the MOO task, create a thread, run the callback function on the thread, and then resume
  the MOO task with the return value from the callback thread. A sample function (background_test)
  is provided for demonstration purposes. Additionally, you can set $server_options.max_background_threads
  to limit the number of threads that the MOO can queue at any given moment.

  Your callback function should periodically (or oftenly) check the status of the background_waiter's active
  member, which will indicate whether or not the MOO task has been killed or not. If active is 0, the task is dead
  and your function should clean up and not bother worrying about returning anything.

  Demonstrates:
     - Suspending tasks
     - Spawning external threads
     - Resuming tasks with data from external threads
*/

static threadpool background_pool;
static std::unordered_map <uint16_t, background_waiter*> background_process_table;
static uint16_t next_background_handle = 1;

std::mutex shutdown_mutex;
std::condition_variable shutdown_condition;
uint16_t shutdown_complete = false;

/* Make sure creating a new thread won't exceed MAX_BACKGROUND_THREADS or $server_options.max_background_threads */
static bool can_create_thread()
{
    // Make sure we don't overrun the background thread limit.
    if (background_process_table.size() > server_int_option("max_background_threads", MAX_BACKGROUND_THREADS))
        return false;
    else
        return true;
}

/* Insert the background waiter into the process table. */
static void initialize_background_waiter(background_waiter *waiter)
{
    waiter->active = false;
    waiter->fd[0] = -1;
    waiter->fd[1] = -1;
    waiter->handle = next_background_handle;
    background_process_table[next_background_handle] = waiter;
    next_background_handle++;
    waiter->return_value = var_ref(none);
    waiter->data = var_ref(none);
    waiter->extra_data = nullptr;
}

/* Remove the background waiter from the process table, free any memory,
 * and reset the maximum handle if there are no threads running. */
static void deallocate_background_waiter(background_waiter *waiter)
{
    const uint16_t handle = waiter->handle;
    if (waiter->cleanup)
        waiter->cleanup(waiter->extra_data);
    if (waiter->fd[0] >= 0) {
        network_unregister_fd(waiter->fd[0]);
#ifdef _WIN32
        closesocket((SOCKET)waiter->fd[0]);
#else
        close(waiter->fd[0]);
#endif
    }
    if (waiter->fd[1] >= 0) {
#ifdef _WIN32
        closesocket((SOCKET)waiter->fd[1]);
#else
        close(waiter->fd[1]);
#endif
    }
    free_var(waiter->return_value);
    free_var(waiter->data);
    myfree(waiter, M_STRUCT);
    background_process_table.erase(handle);

    if (background_process_table.size() == 0)
        next_background_handle = 1;
}

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

static threadpool *thread_pool_by_name(const char* pool)
{
    if (!strcmp(pool, "MAIN"))
        return &background_pool;

    return nullptr;
}

/* @forked will use the enumerator to find relevant tasks in your external queue, so everything we've spawned
 * will need to return TEA_CONTINUE to get counted. The enumerator handles cases where you kill_task from inside the MOO. */
static task_enum_action
background_enumerator(task_closure closure, void *data)
{
    for (auto& it : background_process_table)
    {
        if (it.second->active)
        {
            char *thread_name = nullptr;
            asprintf(&thread_name, "waiting on thread %d", it.first);
            const task_enum_action tea = (*closure) (it.second->the_vm, thread_name, data);
            free(thread_name);

            if (tea == TEA_KILL) {
                // When the task gets killed, it's responsible for cleaning up after itself by checking active from time to time.
                it.second->active = false;
            }
            if (tea != TEA_CONTINUE)
                return tea;
        }
    }

    return TEA_CONTINUE;
}

/* The default thread callback function: Responsible for calling the function specified in the original
 * background function call and then passing it off to the network callback to resume the MOO task. */
static void run_callback(void *bw)
{
    background_waiter *w = (background_waiter*)bw;

    w->callback(w->data, &w->return_value, w->extra_data);

    if (!is_shutdown_triggered()) {
        // Write to our network pipe/socket to resume the MOO loop
#ifdef _WIN32
        send((SOCKET)w->fd[1], "1", 2, 0);
#else
        write(w->fd[1], "1", 2);
#endif
    } else if (w->active) {
        /* The server is shutting down. Sneak this into the task queue before it goes...
         * Note: We don't want to deallocate the background waiter at this point because
         * it's going to be necessary when the task queue gets saved. */
        task_queue_mutex.lock();
        resume_task(w->the_vm, var_ref(w->return_value));
        task_queue_mutex.unlock();
        {
            std::lock_guard<std::mutex> lock(shutdown_mutex);
            shutdown_complete++;
        }
        shutdown_condition.notify_one();
    }
}

/* The function called by the network when data has been read. This is the final stage and
 * is responsible for actually resuming the task and cleaning up the associated mess. */
static void network_callback(int fd, void *data)
{
    background_waiter *w = (background_waiter*)data;

    char buffer[2];
#ifdef _WIN32
    recv((SOCKET)w->fd[0], buffer, 2, 0);
#else
    read(w->fd[0], buffer, 2);
#endif

    /* Resume the MOO task if it hasn't already been killed. */
    if (w->active)
        resume_task(w->the_vm, var_ref(w->return_value));

    deallocate_background_waiter(w);
}

/* Creates the background_waiter struct and starts the worker thread. */
static enum error
background_suspender(vm the_vm, void *data)
{
    background_waiter *w = (background_waiter*)data;

    if (!check_user_task_limit(the_vm->activ_stack->progr)) {
        deallocate_background_waiter(w);
        return E_QUOTA;
    }

    w->the_vm = the_vm;
    w->active = true;

    // Register so we can write to the pipe and resume the main loop if the MOO is idle
    network_register_fd(w->fd[0], network_callback, nullptr, data);

    const int add_work_success = thpool_add_work(background_pool, run_callback, data);

    if (add_work_success < 0) {
        errlog("Error adding work to thread pool\n");
        deallocate_background_waiter(w);
        return E_QUOTA;
    }

    return E_NONE;
}

/* Create a new background thread, supplying a callback function, a Var of data, and a string of explanatory text for what the thread is.
 * If threading has been disabled for the current verb, this function will invoke the callback immediately. */
package
background_thread(void (*callback)(Var, Var*, void*), Var* data, void *extra_data, void (*cleanup)(void*))
{
    const bool threading_enabled = get_thread_mode();
    if (threading_enabled && !can_create_thread())
    {
        errlog("Can't create a new thread\n");
        free_var(*data);
        if (cleanup)
            cleanup(extra_data);
        return make_error_pack(E_QUOTA);
    }

    if (!threading_enabled)
    {
        Var r;
        callback(*data, &r, extra_data);
        free_var(*data);
        if (cleanup)
            cleanup(extra_data);
        return make_var_pack(r);
    } else {
        background_waiter *w = (background_waiter*)mymalloc(sizeof(background_waiter), M_STRUCT);
        initialize_background_waiter(w);
        w->callback = callback;
        w->cleanup = cleanup;
        w->data = *data;
        w->extra_data = extra_data;
#ifdef _WIN32
        /* Windows: Use socket pair instead of pipe for select() compatibility */
        SOCKET sv[2];
        if (win32_socketpair(AF_INET, SOCK_STREAM, 0, sv) == -1)
        {
            errlog("Failed to create socket pair for background thread\n");
            deallocate_background_waiter(w);
            return make_error_pack(E_QUOTA);
        }
        w->fd[0] = (int)sv[0];
        w->fd[1] = (int)sv[1];
#else
        if (pipe(w->fd) == -1)
        {
            log_perror("Failed to create pipe for background thread");
            deallocate_background_waiter(w);
            return make_error_pack(E_QUOTA);
        }
#endif

        return make_suspend_pack(background_suspender, (void*)w);
    }
}

/* Called when the server shuts down. This ensures that all threads have finished before dumping the database. */
void background_shutdown()
{
    int active = thpool_num_threads_working(background_pool);
    if (active) {
        oklog("SHUTDOWN: Waiting for %d thread%s ...\n", active, active > 1 ? "s" : "");
        thpool_destroy(background_pool);
    }

    {
        std::unique_lock<std::mutex> lock(shutdown_mutex);
        shutdown_condition.wait(lock, [&active]{ return shutdown_complete >= active; });
    }
}

static package
bf_threads(Var arglist, Byte next, void *vdata, Objid progr)
{
    free_var(arglist);

    if (!is_wizard(progr))
        return make_error_pack(E_PERM);

    int count = 0;
    Var r = new_list(background_process_table.size());
    for (auto& it : background_process_table)
        r.v.list[++count] = Var::new_int(it.first);

    return make_var_pack(r);
}

/* Allows the database to control the thread pools. It's entirely possible
 * that this function is intentionally obtuse to discourage casual usage.
 * bf_thread_pool(STR <function>, STR <pool> [, INT value])
 * Function is one of: INIT
 * Pool is one of: MAIN, DNS
 */
static package bf_thread_pool(Var arglist, Byte next, void *vdata, Objid progr)
{
    const int nargs = arglist.v.list[0].v.num;
    const char* func = arglist.v.list[1].v.str;
    const char* pool = arglist.v.list[2].v.str;
    const int value = (nargs > 2 ? arglist.v.list[3].v.num : 0);
    free_var(arglist);

    if (!is_wizard(progr))
        return make_error_pack(E_PERM);

    threadpool *the_pool = thread_pool_by_name(pool);
    if (the_pool == nullptr)
        return make_raise_pack(E_INVARG, "Invalid thread pool", str_dup_to_var(pool));

    if (!strcmp(func, "INIT")) {
        if (value < 0)
            return make_raise_pack(E_INVARG, "Invalid number of threads", Var::new_int(value));
        thpool_destroy(*the_pool);
        if (value <= 0)
            *the_pool = nullptr;
        else
            *the_pool = thpool_init(value);
        return make_var_pack(Var::new_int(1));
    } else {
        return make_raise_pack(E_INVARG, "Invalid function", str_dup_to_var(func));
    }

    return no_var_pack();
}

#ifdef BACKGROUND_TEST
/* The actual callback function for our background_test function. This function does all of the actual work
 * for the background_test. Receives a pointer to the relevant background_waiter struct. */
static void background_test_callback(Var args, Var *ret, void *extra_data)
{
    int wait = (args.v.list[0].v.num >= 2 ? args.v.list[2].v.num : 5);

    sleep(wait);

    ret->type = TYPE_STR;
    if (args.v.list[0].v.num == 0)
        ret->v.str = str_dup("Hello, world.");
    else
        ret->v.str = str_dup(args.v.list[1].v.str);
}

/* The background testing function. Accepts a string argument and a time argument. Its goal is simply
 * to spawn a helper thread, sleep, and then return the string back to you. */
static package
bf_background_test(Var arglist, Byte next, void *vdata, Objid progr)
{
    return background_thread(background_test_callback, &arglist);
}
#endif

void
register_background()
{
    register_task_queue(background_enumerator);
    background_pool = thpool_init(TOTAL_BACKGROUND_THREADS);
    register_function("threads", 0, 0, bf_threads);
    register_function("thread_pool", 2, 3, bf_thread_pool, TYPE_STR, TYPE_STR, TYPE_INT);
#ifdef BACKGROUND_TEST
    register_function("background_test", 0, 2, bf_background_test, TYPE_STR, TYPE_INT);
#endif
}
