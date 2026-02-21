/******************************************************************************
  Copyright 2011 Todd Sundsted. All rights reserved.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are met:

   1. Redistributions of source code must retain the above copyright notice,
      this list of conditions and the following disclaimer.

   2. Redistributions in binary form must reproduce the above copyright notice,
      this list of conditions and the following disclaimer in the documentation
      and/or other materials provided with the distribution.

  THIS SOFTWARE IS PROVIDED BY TODD SUNDSTED ``AS IS'' AND ANY EXPRESS OR
  IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
  MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO
  EVENT SHALL TODD SUNDSTED OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA,
  OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
  LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
  NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
  EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

  The views and conclusions contained in the software and documentation are
  those of the authors and should not be interpreted as representing official
  policies, either expressed or implied, of Todd Sundsted.
 *****************************************************************************/

#include <errno.h>
#include <sys/stat.h>

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include "platform.h"
#include <windows.h>
#include <io.h>
#include <process.h>
/* Windows doesn't use signals for child process notification */
#define SIGCHLD 0
/* sig_atomic_t is not in MinGW's headers without signal.h */
typedef int sig_atomic_t;
/* S_ISREG macro - check if file is regular file */
#ifndef S_ISREG
#define S_ISREG(m) (((m) & _S_IFMT) == _S_IFREG)
#endif
#else
#include <signal.h>
#include <unistd.h>
#endif

#include "network.h"

#include "exec.h"
#include "functions.h"
#include "list.h"
#include "log.h"
#include "storage.h"
#include "structures.h"
#include "streams.h"
#include "tasks.h"
#include "utils.h"

typedef enum {
    TWS_CONTINUE,       /* The task is running and has not yet
                 * stopped or been killed.
                 */
    TWS_STOP,           /* The task has stopped.  This status
                 * is final.
                 */
    TWS_KILL            /* The task has been killed.  This
                 * status is final.
                 */
} task_waiting_status;

typedef struct task_waiting_on_exec {
    const char *cmd;
    const char *display_cmd;   /* Original path for queued_tasks() display (without Windows extension) */
    const char **args;
    const char *in;
    const char **env;
    int len;
    pid_t pid;
    task_waiting_status status;
    int code;
    int fin;
    int fout;
    int ferr;
    Stream *sout;
    Stream *serr;
    vm the_vm;
#ifdef _WIN32
    HANDLE hProcess;        /* Process handle for WaitForSingleObject */
    HANDLE hStdoutRead;     /* Pipe handles for reading stdout/stderr */
    HANDLE hStderrRead;
    HANDLE hReaderThread;   /* Handle to the reader thread */
#endif
} task_waiting_on_exec;

static task_waiting_on_exec *process_table[EXEC_MAX_PROCESSES];

volatile static sig_atomic_t sigchild_interrupt = 0;

#ifdef _WIN32
/* Windows: Use critical section instead of signal blocking */
static CRITICAL_SECTION exec_cs;
static int exec_cs_initialized = 0;
#define BLOCK_SIGCHLD do { if (exec_cs_initialized) EnterCriticalSection(&exec_cs); } while(0)
#define UNBLOCK_SIGCHLD do { if (exec_cs_initialized) LeaveCriticalSection(&exec_cs); } while(0)
#else
static sigset_t block_sigchld;
#define BLOCK_SIGCHLD sigprocmask(SIG_BLOCK, &block_sigchld, NULL)
#define UNBLOCK_SIGCHLD sigprocmask(SIG_UNBLOCK, &block_sigchld, NULL)
#endif

static Stream *logmsg = new_stream(30);

static task_waiting_on_exec *
malloc_task_waiting_on_exec()
{
    task_waiting_on_exec *tw =
        (task_waiting_on_exec *)mymalloc(sizeof(task_waiting_on_exec), M_TASK);
    tw->cmd = nullptr;
    tw->display_cmd = nullptr;
    tw->args = nullptr;
    tw->in = nullptr;
    tw->env = nullptr;
    tw->status = TWS_CONTINUE;
    tw->code = 0;
    tw->sout = new_stream(1000);
    tw->serr = new_stream(1000);
    return tw;
}

static void
free_task_waiting_on_exec(task_waiting_on_exec * tw)
{
    int i;

    if (tw->cmd)
        free_str(tw->cmd);
    if (tw->display_cmd)
        free_str(tw->display_cmd);
    if (tw->args) {
        for (i = 0; tw->args[i]; i++)
            free_str(tw->args[i]);
        myfree(tw->args, M_ARRAY);
    }
    if (tw->env) {
        for (i = 0; tw->env[i]; i++)
            free_str(tw->env[i]);
        myfree(tw->env, M_ARRAY);
    }
    if (tw->in)
        free_str(tw->in);
#ifdef _WIN32
    /* Close Windows handles */
    if (tw->hStdoutRead)
        CloseHandle(tw->hStdoutRead);
    if (tw->hStderrRead)
        CloseHandle(tw->hStderrRead);
    if (tw->hProcess)
        CloseHandle(tw->hProcess);
    if (tw->hReaderThread)
        CloseHandle(tw->hReaderThread);
#else
    close(tw->fout);
    close(tw->ferr);
    network_unregister_fd(tw->fout);
    network_unregister_fd(tw->ferr);
#endif
    if (tw->sout)
        free_stream(tw->sout);
    if (tw->serr)
        free_stream(tw->serr);
    myfree(tw, M_TASK);
}

static task_enum_action
exec_waiter_enumerator(task_closure closure, void *data)
{
    task_enum_action action = TEA_CONTINUE;

    BLOCK_SIGCHLD;

    int i;
    for (i = 0; i < EXEC_MAX_PROCESSES; i++) {
        if (process_table[i]) {
            if (TWS_KILL != process_table[i]->status) {
                action = (*closure) (process_table[i]->the_vm,
                                     process_table[i]->display_cmd,
                                     data);
                if (TEA_KILL == action)
                    process_table[i]->status = TWS_KILL;
                if (TEA_CONTINUE != action)
                    break;
            }
        }
    }

    UNBLOCK_SIGCHLD;

    return action;
}

static int
write_all(int fd, const char *buffer, size_t length)
{
    ssize_t count;
    while (length) {
        if ((count = write(fd, buffer, length)) < 0)
            return -1;
        buffer += count;
        length -= count;
    }
    fsync(fd);
    return 1;
}

static void
stdout_readable(int fd, void *data)
{
    task_waiting_on_exec *tw = (task_waiting_on_exec *)data;
    char buffer[1000];
    int n;
    while ((n = read(fd, buffer, sizeof(buffer))) > 0) {
        stream_add_string(tw->sout, raw_bytes_to_binary(buffer, n));
    }
}

static void
stderr_readable(int fd, void *data)
{
    task_waiting_on_exec *tw = (task_waiting_on_exec *)data;
    char buffer[1000];
    int n;
    while ((n = read(fd, buffer, sizeof(buffer))) > 0) {
        stream_add_string(tw->serr, raw_bytes_to_binary(buffer, n));
    }
}

#ifdef _WIN32
/*
 * Normalize CRLF to LF in a buffer.
 * Returns the new length after removing CR characters that precede LF.
 */
static DWORD
normalize_crlf(char *buffer, DWORD len)
{
    DWORD j = 0;
    for (DWORD i = 0; i < len; i++) {
        /* Skip CR when followed by LF */
        if (buffer[i] == '\r' && i + 1 < len && buffer[i + 1] == '\n') {
            continue;
        }
        buffer[j++] = buffer[i];
    }
    return j;
}

/*
 * Windows process reader thread.
 * Reads stdout/stderr, waits for process exit, then signals completion.
 */
static DWORD WINAPI
exec_reader_thread(LPVOID lpParam)
{
    task_waiting_on_exec *tw = (task_waiting_on_exec *)lpParam;
    char buffer[1000];
    DWORD bytesRead;
    DWORD exitCode = 0;

    /* Read stdout until EOF */
    while (ReadFile(tw->hStdoutRead, buffer, sizeof(buffer), &bytesRead, NULL) && bytesRead > 0) {
        DWORD normalized_len = normalize_crlf(buffer, bytesRead);
        EnterCriticalSection(&exec_cs);
        stream_add_string(tw->sout, raw_bytes_to_binary(buffer, normalized_len));
        LeaveCriticalSection(&exec_cs);
    }

    /* Read stderr until EOF */
    while (ReadFile(tw->hStderrRead, buffer, sizeof(buffer), &bytesRead, NULL) && bytesRead > 0) {
        DWORD normalized_len = normalize_crlf(buffer, bytesRead);
        EnterCriticalSection(&exec_cs);
        stream_add_string(tw->serr, raw_bytes_to_binary(buffer, normalized_len));
        LeaveCriticalSection(&exec_cs);
    }

    /* Wait for process to exit */
    WaitForSingleObject(tw->hProcess, INFINITE);
    GetExitCodeProcess(tw->hProcess, &exitCode);

    /* Signal completion */
    EnterCriticalSection(&exec_cs);
    if (TWS_CONTINUE == tw->status) {
        tw->status = TWS_STOP;
        tw->code = (int)exitCode;
    }
    sigchild_interrupt = 1;
    LeaveCriticalSection(&exec_cs);

    return 0;
}

/* Windows: Use CreateProcess instead of fork/exec */
static pid_t
fork_and_exec(const char *cmd, const char *const args[], const char *const env[],
              int *in, int *out, int *err, task_waiting_on_exec *tw)
{
    HANDLE hStdinRead = NULL, hStdinWrite = NULL;
    HANDLE hStdoutRead = NULL, hStdoutWrite = NULL;
    HANDLE hStderrRead = NULL, hStderrWrite = NULL;
    SECURITY_ATTRIBUTES sa;
    PROCESS_INFORMATION pi;
    STARTUPINFOA si;
    BOOL success = FALSE;

    /* Set up security attributes for inheritable handles */
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    /* Create pipes for stdin, stdout, stderr */
    if (!CreatePipe(&hStdinRead, &hStdinWrite, &sa, 0)) {
        log_perror("EXEC: Couldn't create stdin pipe");
        goto fail;
    }
    /* Make stdin write end non-inheritable */
    SetHandleInformation(hStdinWrite, HANDLE_FLAG_INHERIT, 0);

    if (!CreatePipe(&hStdoutRead, &hStdoutWrite, &sa, 0)) {
        log_perror("EXEC: Couldn't create stdout pipe");
        goto close_stdin;
    }
    /* Make stdout read end non-inheritable */
    SetHandleInformation(hStdoutRead, HANDLE_FLAG_INHERIT, 0);

    if (!CreatePipe(&hStderrRead, &hStderrWrite, &sa, 0)) {
        log_perror("EXEC: Couldn't create stderr pipe");
        goto close_stdout;
    }
    /* Make stderr read end non-inheritable */
    SetHandleInformation(hStderrRead, HANDLE_FLAG_INHERIT, 0);

    /* Build command line from args */
    /* Windows needs a single command line string, not an array */
    Stream *cmdline = new_stream(1024);

    /* Check if this is a batch file - needs to run through cmd.exe */
    size_t cmdlen = strlen(cmd);
    int is_batch = (cmdlen > 4 &&
                    (_stricmp(cmd + cmdlen - 4, ".bat") == 0 ||
                     _stricmp(cmd + cmdlen - 4, ".cmd") == 0));

    const char *actual_cmd = cmd;
    if (is_batch) {
        /* For batch files, we run: cmd.exe /c "batchfile arg1 arg2..."
         * The entire command+args must be in one quoted string for cmd.exe
         * Convert forward slashes to backslashes for cmd.exe */
        actual_cmd = "cmd.exe";
        stream_add_string(cmdline, "cmd.exe /c \"");
        /* Add path with forward slashes converted to backslashes */
        for (const char *p = cmd; *p; p++) {
            if (*p == '/')
                stream_add_char(cmdline, '\\');
            else
                stream_add_char(cmdline, *p);
        }
        /* Add the rest of the arguments inside the same quotes */
        for (int i = 1; args[i] != NULL; i++) {
            stream_add_char(cmdline, ' ');
            stream_add_string(cmdline, args[i]);
        }
        stream_add_char(cmdline, '\"');
    } else {
        for (int i = 0; args[i] != NULL; i++) {
            if (i > 0)
                stream_add_char(cmdline, ' ');
            /* Quote arguments that contain spaces */
            if (strchr(args[i], ' ') != NULL) {
                stream_add_char(cmdline, '"');
                stream_add_string(cmdline, args[i]);
                stream_add_char(cmdline, '"');
            } else {
                stream_add_string(cmdline, args[i]);
            }
        }
    }

    /* Build environment block (null-terminated strings, double-null at end) */
    Stream *envblock = new_stream(1024);
    for (int i = 0; env[i] != NULL; i++) {
        stream_add_string(envblock, env[i]);
        stream_add_char(envblock, '\0');
    }
    stream_add_char(envblock, '\0');  /* Double-null terminator */

    /* Set up STARTUPINFO with redirected handles */
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.hStdInput = hStdinRead;
    si.hStdOutput = hStdoutWrite;
    si.hStdError = hStderrWrite;
    si.dwFlags |= STARTF_USESTDHANDLES;

    ZeroMemory(&pi, sizeof(pi));

    /* CreateProcess */
    /* For batch files, pass NULL as application name and let cmd.exe handle it */
    success = CreateProcessA(
        is_batch ? NULL : cmd,        /* Application name (NULL for batch files) */
        stream_contents(cmdline),     /* Command line (mutable) */
        NULL,                         /* Process security attributes */
        NULL,                         /* Thread security attributes */
        TRUE,                         /* Inherit handles */
        0,                            /* Creation flags */
        stream_contents(envblock),    /* Environment block */
        NULL,                         /* Current directory */
        &si,                          /* Startup info */
        &pi                           /* Process information */
    );

    free_stream(cmdline);
    free_stream(envblock);

    if (!success) {
        log_perror("EXEC: CreateProcess failed");
        goto close_stderr;
    }

    /* Close thread handle (we don't need it) */
    CloseHandle(pi.hThread);

    /* Close the child's ends of the pipes */
    CloseHandle(hStdinRead);
    CloseHandle(hStdoutWrite);
    CloseHandle(hStderrWrite);

    /* Store handles in the task structure */
    tw->hProcess = pi.hProcess;
    tw->hStdoutRead = hStdoutRead;
    tw->hStderrRead = hStderrRead;

    /* Convert stdin write handle to file descriptor for write_all() */
    *in = _open_osfhandle((intptr_t)hStdinWrite, 0);

    /* These are not used on Windows - we read via handles in the thread */
    *out = -1;
    *err = -1;

    /* Start reader thread */
    tw->hReaderThread = CreateThread(NULL, 0, exec_reader_thread, tw, 0, NULL);
    if (tw->hReaderThread == NULL) {
        log_perror("EXEC: CreateThread failed");
        CloseHandle(pi.hProcess);
        goto fail;
    }

    return (pid_t)pi.dwProcessId;

close_stderr:
    CloseHandle(hStderrRead);
    CloseHandle(hStderrWrite);

close_stdout:
    CloseHandle(hStdoutRead);
    CloseHandle(hStdoutWrite);

close_stdin:
    CloseHandle(hStdinRead);
    CloseHandle(hStdinWrite);

fail:
    return 0;
}
#else
static pid_t
fork_and_exec(const char *cmd, const char *const args[], const char *const env[],
              int *in, int *out, int *err)
{
    pid_t pid;
    int pipeIn[2];
    int pipeOut[2];
    int pipeErr[2];

    if (pipe(pipeIn) < 0) {
        log_perror("EXEC: Couldn't create pipe - in");
        goto fail;
    }
    else if (pipe(pipeOut) < 0) {
        log_perror("EXEC: Couldn't create pipe - out");
        goto close_in;
    }
    else if (pipe(pipeErr) < 0) {
        log_perror("EXEC: Couldn't create pipe - err");
        goto close_out;
    }
    else if ((pid = fork()) < 0) {
        log_perror("EXEC: Couldn't fork");
        goto close_err;
    }
    else if (0 == pid) { /* child */
        int status;

        if ((status = dup2(pipeIn[0], STDIN_FILENO)) < 0) {
            perror("dup2");
            exit(status);
        }
        if ((status = dup2(pipeOut[1], STDOUT_FILENO)) < 0) {
            perror("dup2");
            exit(status);
        }
        if ((status = dup2(pipeErr[1], STDERR_FILENO)) < 0) {
            perror("dup2");
            exit(status);
        }

        close(pipeIn[1]);
        close(pipeOut[0]);
        close(pipeErr[0]);

        status = execve(cmd, (char *const *)args, (char *const *)env);
        perror("execve");
        exit(status);
    }

    close(pipeIn[0]);
    close(pipeOut[1]);
    close(pipeErr[1]);

    *in = pipeIn[1];
    *out = pipeOut[0];
    *err = pipeErr[0];

    return pid;

close_err:
    close(pipeErr[0]);
    close(pipeErr[1]);

close_out:
    close(pipeOut[0]);
    close(pipeOut[1]);

close_in:
    close(pipeIn[0]);
    close(pipeIn[1]);

fail:
    return 0;
}
#endif

static int
set_nonblocking(int fd)
{
#ifdef _WIN32
    /* Windows: Use ioctlsocket for sockets, or just succeed for pipes */
    /* Since we're using this with pipes from exec, just return success */
    /* Pipe I/O on Windows is handled differently */
    (void)fd;
    return 1;
#else
    int flags;

    if ((flags = fcntl(fd, F_GETFL, 0)) < 0
            || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        return 0;
    else
        return 1;
#endif
}

static enum error
exec_waiter_suspender(vm the_vm, void *data)
{
    task_waiting_on_exec *tw = (task_waiting_on_exec *)data;
    enum error error = E_QUOTA;

    BLOCK_SIGCHLD;

    int i;
    for (i = 0; i < EXEC_MAX_PROCESSES; i++) {
        if (process_table[i] == nullptr) {
            process_table[i] = tw;
            break;
        }
    }
    if (i == EXEC_MAX_PROCESSES) {
        error = E_QUOTA;
        goto free_task_waiting_on_exec;
    }

#ifdef _WIN32
    if ((tw->pid = fork_and_exec(tw->cmd, tw->args, tw->env, &tw->fin, &tw->fout, &tw->ferr, tw)) == 0) {
#else
    if ((tw->pid = fork_and_exec(tw->cmd, tw->args, tw->env, &tw->fin, &tw->fout, &tw->ferr)) == 0) {
#endif
        error = E_EXEC;
        goto clear_process_slot;
    }

    stream_printf(logmsg, "EXEC: %s (%d)", tw->cmd, tw->pid);
    for (int x = 0; tw->env[x]; ++x) {
        stream_printf(logmsg, " [%s]", tw->env[x]);
    }

    oklog("%s\n", reset_stream(logmsg));

    set_nonblocking(tw->fin);
#ifndef _WIN32
    set_nonblocking(tw->fout);
    set_nonblocking(tw->ferr);
#endif

    if (tw->in) {
        if (write_all(tw->fin, tw->in, tw->len) < 0) {
            error = E_EXEC;
            goto close_fin;
        }
    }

    close(tw->fin);

#ifndef _WIN32
    /* On Windows, the reader thread handles stdout/stderr */
    network_register_fd(tw->fout, stdout_readable, nullptr, tw);
    network_register_fd(tw->ferr, stderr_readable, nullptr, tw);
#endif

    tw->the_vm = the_vm;

    /* success */
    UNBLOCK_SIGCHLD;
    return E_NONE;

close_fin:
    close(tw->fin);

clear_process_slot:
    process_table[i] = nullptr;

free_task_waiting_on_exec:
    free_task_waiting_on_exec(tw);

    /* fail */
    UNBLOCK_SIGCHLD;
    return error;
}

static package
bf_exec(Var arglist, Byte next, void *vdata, Objid progr)
{
    package pack;

    const char *cmd = nullptr;
    const char **args = nullptr;
    const char **env = nullptr;
    task_waiting_on_exec *tw = nullptr;
    const char *in = nullptr;
    const char *display_cmd = nullptr;
    int len;

    /* The first argument must be a list of strings.  The first string
     * is the command (required).  The rest are command line arguments
     * to the command.
     */
    Var v;
    int i, c;
    FOR_EACH(v, arglist.v.list[1], i, c) {
        if (TYPE_STR != v.type) {
            pack = make_error_pack(E_INVARG);
            goto free_arglist;
        }
    }
    /* check for the empty list */
    if (1 == i) {
        pack = make_error_pack(E_INVARG);
        goto free_arglist;
    }

    /* check the path */
    cmd = arglist.v.list[1].v.list[1].v.str;
    if (0 == strlen(cmd)) {
        pack = make_raise_pack(E_INVARG, "Invalid path", var_ref(zero));
        goto free_arglist;
    }
#ifdef _WIN32
    /* Windows path checks: no absolute paths, no parent directory traversal */
    if (('/' == cmd[0]) || ('\\' == cmd[0])
            || (strlen(cmd) >= 2 && cmd[1] == ':')  /* Drive letter */
            || (strlen(cmd) > 1 && '.' == cmd[0] && '.' == cmd[1])) {
        pack = make_raise_pack(E_INVARG, "Invalid path", var_ref(zero));
        goto free_arglist;
    }
    if (strstr(cmd, "/.") || strstr(cmd, "./")
            || strstr(cmd, "\\.") || strstr(cmd, ".\\")) {
        pack = make_raise_pack(E_INVARG, "Invalid path", var_ref(zero));
        goto free_arglist;
    }
#else
    if (('/' == cmd[0])
            || (1 < strlen(cmd) && '.' == cmd[0] && '.' == cmd[1])) {
        pack = make_raise_pack(E_INVARG, "Invalid path", var_ref(zero));
        goto free_arglist;
    }
    if (strstr(cmd, "/.") || strstr(cmd, "./")) {
        pack = make_raise_pack(E_INVARG, "Invalid path", var_ref(zero));
        goto free_arglist;
    }
#endif

    /* Make sure any environment variables supplied are strings. */
    if (arglist.v.list[0].v.num >= 3) {
        // Use our own i and c here because i gets reused later.
        int env_i, env_c;
        FOR_EACH(v, arglist.v.list[3], env_i, env_c) {
            if (v.type != TYPE_STR) {
                pack = make_error_pack(E_INVARG);
                goto free_arglist;
            }
        }
    }

    /* prepend the exec subdirectory path */
    static Stream *s;
    if (!s)
        s = new_stream(strlen(exec_subdir) * 2);
    stream_add_string(s, exec_subdir);
    stream_add_string(s, cmd);
    cmd = str_dup(reset_stream(s));

    /* clean input */
    in = nullptr;
    len = 0;
    if (listlength(arglist) > 1) {
        if ((in = binary_to_raw_bytes(arglist.v.list[2].v.str, &len)) == nullptr) {
            pack = make_error_pack(E_INVARG);
            goto free_cmd;
        }
        in = str_dup(in);
    }

    /* If there's a length mismatch, it likely means the argument string
       contained ~00. Instead of truncating the string and potentially
       creating a difficult to debug situation, we raise E_INVARG. */
    if (in && memo_strlen(in) != len) {
        free_str(in);
        pack = make_error_pack(E_INVARG);
        goto free_cmd;
    }

    /* check perms */
    if (!is_wizard(progr)) {
        pack = make_error_pack(E_PERM);
        goto free_in;
    }

    /* stat the command */
    struct stat buf;
#ifdef _WIN32
    {
    /* Save the original command path for display in queued_tasks() before
     * we potentially modify it by adding a Windows executable extension */
    display_cmd = str_dup(cmd);

    /* Windows: Try PATHEXT extensions FIRST (.BAT, .CMD, .EXE, etc.)
     * because the base name might exist but not be executable on Windows */
    bool found = false;
    const char *pathext = getenv("PATHEXT");
    if (!pathext)
        pathext = ".COM;.EXE;.BAT;.CMD";  /* Default Windows PATHEXT */

    /* Parse PATHEXT and try each extension */
    char *pathext_copy = _strdup(pathext);
    char *ext = pathext_copy;
    char *ext_next;
    Stream *try_path = new_stream(strlen(cmd) + 10);

    while (ext && *ext && !found) {
        ext_next = strchr(ext, ';');
        if (ext_next)
            *ext_next++ = '\0';

        /* Skip empty extensions */
        if (*ext) {
            stream_add_string(try_path, cmd);
            stream_add_string(try_path, ext);
            const char *try_cmd = reset_stream(try_path);
            if (stat(try_cmd, &buf) == 0 && S_ISREG(buf.st_mode)) {
                /* Found it - update cmd to include the extension */
                free_str(cmd);
                cmd = str_dup(try_cmd);
                found = true;
            }
        }
        ext = ext_next;
    }
    free_stream(try_path);
    free(pathext_copy);

    /* If no extension found, try the exact name (for .exe files specified with extension) */
    if (!found && stat(cmd, &buf) == 0 && S_ISREG(buf.st_mode)) {
        found = true;
    }

    if (!found) {
        free_str(display_cmd);
        pack = make_raise_pack(E_INVARG, "Does not exist", var_ref(zero));
        goto free_in;
    }
    }
#else
    if (stat(cmd, &buf) != 0) {
        pack = make_raise_pack(E_INVARG, "Does not exist", var_ref(zero));
        goto free_in;
    }
    if (!S_ISREG(buf.st_mode)) {
        pack = make_raise_pack(E_INVARG, "Is not a file", var_ref(zero));
        goto free_in;
    }
#endif

    args = (const char **)mymalloc(sizeof(const char *) * i, M_ARRAY);
    FOR_EACH(v, arglist.v.list[1], i, c)
    args[i - 1] = str_dup(v.v.str);
    args[i - 1] = nullptr;


    /* setup the environment variables */
    // Add two to the args so we're guaranteed to always have env[0] for PATH and env[$] for null
    env = (const char **)mymalloc(sizeof(const char *) * ((listlength(arglist) >= 3 ? listlength(arglist.v.list[3]) : 0) + 2), M_ARRAY);
#ifdef _WIN32
    {
    /* On Windows, inherit system PATH or use a reasonable default */
    const char *syspath = getenv("PATH");
    if (syspath) {
        Stream *pathstream = new_stream(strlen(syspath) + 6);
        stream_add_string(pathstream, "PATH=");
        stream_add_string(pathstream, syspath);
        env[0] = str_dup(reset_stream(pathstream));
        free_stream(pathstream);
    } else {
        env[0] = str_dup("PATH=C:\\Windows\\System32;C:\\Windows");
    }
    }
#else
    env[0] = str_dup("PATH=/bin:/usr/bin");
#endif
    if (listlength(arglist) >= 3) {
        FOR_EACH(v, arglist.v.list[3], i, c)
        env[i] = str_dup(v.v.str);
        env[i] = nullptr;
    } else {
        env[1] = nullptr;
    }

    tw = malloc_task_waiting_on_exec();
    tw->cmd = cmd;
#ifdef _WIN32
    tw->display_cmd = display_cmd;
#else
    tw->display_cmd = str_dup(cmd);
#endif
    tw->args = args;
    tw->in = in;
    tw->len = len;
    tw->env = env;

    free_var(arglist);

    return make_suspend_pack(exec_waiter_suspender, tw);

free_in:
    if (in)
        free_str(in);

free_cmd:
    free_str(cmd);

free_arglist:
    free_var(arglist);

    /* fail */
    return pack;
}

/*
 * Called from child_completed_signal() in server.c.
 * SIGCHLD is already blocked.
 */
pid_t
exec_complete(pid_t pid, int code)
{
    task_waiting_on_exec *tw = nullptr;

    int i;
    for (i = 0; i < EXEC_MAX_PROCESSES; i++)
        if (process_table[i] && process_table[i]->pid == pid) {
            tw = process_table[i];
            break;
        }

    if (tw) {
        sigchild_interrupt = 1;

        if (TWS_CONTINUE == tw->status) {
            tw->status = TWS_STOP;
            tw->code = code;
        }

        return pid;
    }

    /* We wind up here if the child process was a checkpoint process,
     * or if an exec task was explicitly killed while the process
     * itself was still executing.
     */
    return 0;
}

/*
 * Called from main_loop() in server.c.
 */
void
deal_with_child_exit(void)
{
    if (!sigchild_interrupt)
        return;

    BLOCK_SIGCHLD;

    sigchild_interrupt = 0;

    task_waiting_on_exec *tw = nullptr;

    int i;
    for (i = 0; i < EXEC_MAX_PROCESSES; i++) {
        tw = process_table[i];
        if (tw && TWS_STOP == tw->status) {
            Var v;
            v = new_list(3);
            v.v.list[1].type = TYPE_INT;
            v.v.list[1].v.num = tw->code;
#ifndef _WIN32
            /* On Unix, drain any remaining output from the pipes */
            stdout_readable(tw->fout, tw);
#endif
            v.v.list[2].type = TYPE_STR;
            v.v.list[2].v.str = str_dup(reset_stream(tw->sout));
#ifndef _WIN32
            stderr_readable(tw->ferr, tw);
#endif
            v.v.list[3].type = TYPE_STR;
            v.v.list[3].v.str = str_dup(reset_stream(tw->serr));

            resume_task(tw->the_vm, v);
        }
        if (tw && TWS_CONTINUE != tw->status) {
            free_task_waiting_on_exec(tw);
            process_table[i] = nullptr;
        }
    }

    UNBLOCK_SIGCHLD;
}

void
register_exec(void)
{
#ifdef _WIN32
    /* Initialize critical section for process table protection */
    InitializeCriticalSection(&exec_cs);
    exec_cs_initialized = 1;
#else
    sigemptyset(&block_sigchld);
    sigaddset(&block_sigchld, SIGCHLD);
#endif

    register_task_queue(exec_waiter_enumerator);
    register_function("exec", 1, 3, bf_exec, TYPE_LIST, TYPE_STR, TYPE_LIST);
}
