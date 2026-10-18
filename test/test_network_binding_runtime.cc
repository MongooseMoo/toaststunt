// Native lifecycle seam tests, including private counter injection. This test
// translation unit replaces server.cc in a disposable test executable; no
// production setter, build flag or MOO-visible testing API is introduced.
#define main native_server_main
#include "../src/server.cc"
#undef main
#include "eval_env.h"
#include "eval_vm.h"
#include "db_private.h"
#include "parser.h"
#include <chrono>
#include <sys/syscall.h>
#include <sys/wait.h>

static bool entropy_eof;
static bool login_returned_actor;
extern "C" ssize_t read(int fd, void *buffer, size_t size)
{
    if (entropy_eof && size == sizeof(native_boot_nonce)) return 0;
    return syscall(SYS_read, fd, buffer, size);
}

#define CHECK(condition) do { if (!(condition)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); return 1; } } while (0)

// Test executable only: positively prove the synchronous login verb executes
// and returns its user OBJ, without production hooks or saved MOO outcomes.
static package login_probe(Var args, Byte, void *, Objid)
{
    free_var(args);
    login_returned_actor = true;
    return make_var_pack(Var::new_obj(4));
}

static package invoke(const char *name, Var args)
{
    // Native fixture has no interpreter activation; the builtin's own authority
    // checks still run, while compiler argument checks were already supplied.
    return call_bi_func(number_func_by_name(name), args, 0, 3, nullptr);
}
static Var queue_snapshot(Objid actor)
{
    Var args = new_list(1);
    args.v.list[1] = Var::new_obj(actor);
    return invoke("queue_info", args).u.ret;
}
static Var current_binding(Objid actor)
{
    Var args = new_list(1);
    args.v.list[1] = Var::new_obj(actor);
    return bf_connection_binding(args, 1, nullptr, 3).u.ret;
}

static Program *compile(const char *line)
{
    Var code = new_list(1), errors;
    code.v.list[1] = str_dup_to_var(line);
    Program *program = parse_list_as_program(code, &errors);
    free_var(code); free_var(errors);
    return program;
}

static std::string vm_bytes(vm value)
{
    FILE *file = tmpfile();
    dbpriv_set_dbio_output(file);
    write_vm(value);
    fflush(file);
    rewind(file);
    std::string bytes;
    char buffer[4096];
    size_t count;
    while ((count = fread(buffer, 1, sizeof(buffer), file))) bytes.append(buffer, count);
    fclose(file);
    return bytes;
}

int main(int argc, char **argv)
{
    CHECK(argc == 3);
    char **database_args = argv + 1;
    int database_argc = 2;
    set_log_file(stderr);
    register_bi_functions();
    CHECK(db_initialize(&database_argc, &database_args));
    CHECK(db_load());
    load_server_options();
    init_random();
    unsigned char first_boot[16];
    memcpy(first_boot, native_boot_nonce, sizeof(first_boot));
    init_random();
    CHECK(memcmp(first_boot, native_boot_nonce, sizeof(first_boot)) != 0);
    pid_t entropy_child = fork();
    CHECK(entropy_child >= 0);
    if (!entropy_child) {
        entropy_eof = true;
        init_random();
        _exit(99);
    }
    int entropy_status;
    CHECK(waitpid(entropy_child, &entropy_status, 0) == entropy_child);
    CHECK(WIFEXITED(entropy_status) && WEXITSTATUS(entropy_status) == 1);
    setup_signals();

    shandle incoming{}, duplicate{};
    incoming.player = 3;
    incoming.disconnect_me = false;
    incoming.binding = {101, MAXINT, 7, false, false};
    incoming.tasks = new_task_queue(3, 0);
    all_shandles = &incoming;
    task_queue destination = new_task_queue(4, 0);
    new_input_task(destination, "pending destination input", 0, false);
    Var old_queue = queue_snapshot(3), dead_queue = queue_snapshot(4);
    Var args = new_list(2);
    args.v.list[1] = Var::new_obj(3);
    args.v.list[2] = Var::new_obj(4);
    package rejected = invoke("switch_player", args);
    CHECK(rejected.kind == package::BI_RAISE && rejected.u.raise.code.v.err == E_QUOTA);
    free_var(rejected.u.raise.code);
    free_str(rejected.u.raise.msg);
    free_var(rejected.u.raise.value);
    Var after_old = queue_snapshot(3), after_dead = queue_snapshot(4);
    CHECK(equality(old_queue, after_old, 1) && equality(dead_queue, after_dead, 1));
    CHECK(incoming.player == 3 && incoming.binding.epoch == MAXINT);
    free_var(old_queue); free_var(dead_queue); free_var(after_old); free_var(after_dead);

    incoming.binding.epoch = 2;
    ConnectionBindingReservation reserved{};
    shandle dead_old{};
    dead_old.player = 3;
    dead_old.disconnect_me = true;
    dead_old.next = &incoming;
    all_shandles = &dead_old;
    CHECK(reserve_connection_binding(3, reserved));
    CHECK(reserved.handle == &incoming); // live incoming wins over dead same-id duplicate
    duplicate.player = 4;
    duplicate.disconnect_me = true; // refcount-retained old redirect handle, first in list
    duplicate.binding = {102, 9, 900, false, false};
    duplicate.next = &incoming;
    all_shandles = &duplicate;
    CHECK(commit_connection_binding(reserved, 4) == &incoming);
    CHECK(incoming.player == 4 && incoming.binding.epoch == 3);
    CHECK(duplicate.binding.retired && duplicate.binding.epoch == 9);
    Var live = current_binding(4);
    CHECK(live.type == TYPE_LIST && live.v.list[1].v.list[2].v.num == 101);
    CHECK(live.v.list[2].v.num == 7); // the same selected handle supplies the enqueue head
    free_var(live);
    // A flag flip after reservation never panics or grants a fresh receipt.
    CHECK(reserve_connection_binding(4, reserved));
    incoming.disconnect_me = true;
    CHECK(commit_connection_binding(reserved, 3) == &incoming);
    CHECK(incoming.binding.retired);
    incoming.disconnect_me = false;
    live = current_binding(3);
    CHECK(live.type == TYPE_INT && live.v.num == 0);
    free_var(live);
    // An already-disconnecting exhausted handle keeps existing rebind semantics
    // with NONE; a retained dead duplicate cannot displace live incoming.
    incoming.binding.epoch = MAXINT;
    incoming.disconnect_me = true;
    CHECK(reserve_connection_binding(3, reserved) && reserved.epoch == 0);
    CHECK(commit_connection_binding(reserved, 4) == &incoming);
    CHECK(incoming.binding.retired && incoming.binding.epoch == MAXINT);
    incoming.disconnect_me = false;
    incoming.binding.epoch = 3;
    retire_connection_binding(4);
    live = current_binding(4);
    CHECK(live.type == TYPE_INT && live.v.num == 0);
    CHECK(incoming.binding.retired && duplicate.binding.retired);
    CHECK(reserve_connection_binding(4, reserved));
    CHECK(commit_connection_binding(reserved, 3) == &incoming);
    CHECK(!incoming.binding.retired && incoming.binding.epoch == 4);
    incoming.binding.input_head = MAXINT;
    CHECK(!advance_network_input(incoming.binding));
    CHECK(reserve_connection_binding(3, reserved) && reserved.epoch == 0);
    CHECK(commit_connection_binding(reserved, 4) == &incoming);
    live = current_binding(4);
    CHECK(live.type == TYPE_INT && live.v.num == 0 && incoming.binding.input_head == MAXINT);

    // Object destruction and renumber invalidate receipts without booting or
    // renaming the actual socket. Recreating an identical slot does not revive it.
    Objid hole = db_create_object(NOTHING), actor = db_create_object(NOTHING);
    incoming.player = actor;
    incoming.binding = {101, 1, 1, false, false};
    db_destroy_object(hole);
    CHECK(db_renumber_object(actor) == hole);
    CHECK(incoming.player == actor && incoming.binding.retired && !incoming.disconnect_me.load());
    CHECK(db_create_object(actor) == actor);
    live = current_binding(actor);
    CHECK(live.type == TYPE_INT && live.v.num == 0);
    incoming.binding.retired = false;
    db_destroy_object(actor);
    CHECK(incoming.binding.retired && !incoming.disconnect_me.load());

    // Exercise the actual synchronous OBJ-return login path at exhausted
    // epoch, with an already-populated destination queue. No notifier or
    // transfer may run, and the negative connection must remain unbound.
    register_function("native_login_probe", 0, 0, login_probe);
    Program *login = compile("return native_login_probe();");
    CHECK(login);
    db_verb_handle login_verb = db_find_defined_verb(Var::new_obj(0), "do_login_command", 0);
    CHECK(login_verb.ptr);
    db_set_verb_program(login_verb, login);
    incoming.player = -1000;
    incoming.next = nullptr;
    incoming.binding = {101, MAXINT, 0, false, false};
    incoming.tasks = new_task_queue(-1000, 0);
    all_shandles = &incoming;
    CHECK(last_input_task_id(-1000) == 0);
    CHECK(tasks_set_connection_option(destination, "hold-input", Var::new_int(1)));
    new_input_task(incoming.tasks, "return-login-attempt", 0, false);
    run_ready_tasks();
    CHECK(login_returned_actor); // callable verb synchronously returned user #4
    CHECK(last_input_task_id(-1000) != 0); // OBJ-return login actually dispatched
    CHECK(incoming.player == -1000 && incoming.binding.epoch == MAXINT);
    CHECK(current_task_id == -1 && current_network_origin.binding.physical == 0);
    Var pending_input = read_input_now(4);
    CHECK(pending_input.type == TYPE_STR && !strcmp(pending_input.v.str, "pending destination input"));
    free_var(pending_input);
    all_shandles = nullptr;

    NetworkOrigin authentic{};
    authentic.binding = { {}, 901, 2, 3 };
    authentic.ordinal = 7;
    authentic.kind = NetworkInputKind::Oob;
    authentic.telnet_oob = true;
    authentic.local_port = 7777;
    current_network_origin = authentic;
    Program *plain = compile("return task_network_origin();");
    CHECK(plain);
    Var result;
    CHECK(run_server_program_task(3, "native_probe", new_list(0), 3, "native_probe", plain,
                                  3, 1, 3, "", &result) == OUTCOME_DONE);
    CHECK(result.type == TYPE_INT && result.v.num == 0);
    CHECK(current_network_origin.binding.physical == 901);
    free_var(result); free_program(plain);

    Program *blocking = compile("suspend(100000); return 0;");
    CHECK(blocking);
    current_task_id = 4455;
    current_local = new_map();
    CHECK(do_server_program_task(Var::new_obj(3), "native_probe", new_list(0), Var::new_obj(3),
          "native_probe", blocking, 3, 1, 3, "", nullptr, 0) == OUTCOME_BLOCKED);
    free_program(blocking);
    free_var(current_local);
    vm pending = find_suspended_task(4455);
    CHECK(pending && pending->network_origin.ordinal == 7);
    std::string bytes = vm_bytes(pending);
    pending->network_origin.binding.physical = 999;
    pending->network_origin.ordinal = 999;
    CHECK(vm_bytes(pending) == bytes); // provenance adds no database-format bytes

    FILE *saved = tmpfile();
    CHECK(saved);
    CHECK(fwrite(bytes.data(), 1, bytes.size(), saved) == bytes.size());
    dbio_input_version = current_db_version;
    for (Num physical : {701, 702}) {
        rewind(saved);
        dbpriv_set_dbio_input(saved);
        vm restored = read_vm(4455);
        CHECK(restored && restored->network_origin.binding.physical == 0);
        // The same visible task ID may identify two distinct VMs; their own
        // runtime metadata, never an ID-indexed table, controls restoration.
        restored->network_origin = authentic;
        restored->network_origin.binding.physical = physical;
        current_task_id = 4455;
        current_local = var_ref(restored->local);
        CHECK(resume_from_previous_vm(restored, zero) == OUTCOME_DONE);
        CHECK(current_network_origin.binding.physical == physical);
        free_var(current_local);
    }
    fclose(saved);
    current_task_id = -1;
    current_network_origin = {};
    args = new_list(1);
    args.v.list[1] = Var::new_int(4455);
    package killed = invoke("kill_task", args);
    CHECK(killed.kind == package::BI_RETURN);
    free_var(killed.u.ret);
    CHECK(find_suspended_task(4455) == nullptr);

    // Fixed-cost receipt snapshots and monotone counter work, without parsing,
    // network latency or a claim about end-to-end MOO delivery.
    incoming.player = 3;
    incoming.binding = {101, 2, 0, false, false};
    incoming.next = nullptr;
    all_shandles = &incoming;
    auto start = std::chrono::steady_clock::now();
    uint64_t checksum = 0;
    for (int i = 0; i < 100000; i++) {
        CHECK(advance_network_input(incoming.binding));
        Var snapshot = current_binding(3);
        checksum += snapshot.v.list[2].v.num;
        free_var(snapshot);
    }
    double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    CHECK(checksum == (uint64_t)100000 * 100001 / 2);
    std::printf("native binding snapshot benchmark: 100000 iterations %.6f seconds; origin=%zu bytes\n", seconds, sizeof(NetworkOrigin));
    all_shandles = nullptr;
    current_network_origin = {};
    return 0;
}
