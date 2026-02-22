/* network_wasm.cc -- WASM virtual connection layer for the network subsystem.
 * BSD sockets are unavailable in browser WASM.
 * Phase 3: Virtual connections allow JS to create connections, inject input,
 * and capture output through exported C functions.
 */

#include "options.h"
#include "config.h"
#include "network.h"
#include "server.h"
#include "structures.h"
#include "streams.h"
#include "log.h"
#include "list.h"
#include "storage.h"
#include "utils.h"
#include <emscripten.h>
#include <string.h>
#include <stdio.h>

/* External variables (outbound_network_enabled, bind_ipv4, bind_ipv6,
   default_certificate_path, default_key_path) are defined in server.cc */

/**** Virtual connection handle ****/

#define MAX_WASM_CONNECTIONS 16
#define INITIAL_OUTPUT_CAPACITY 4096

typedef struct wasm_handle {
    int id;                     /* connection identifier (slot index) */
    server_handle shandle;      /* back-pointer to server layer */
    char *output_buffer;        /* accumulated output text */
    int output_len;
    int output_capacity;
    bool connected;
    bool input_suspended;
    char *name;
} wasm_handle;

static wasm_handle *connections[MAX_WASM_CONNECTIONS];
static int next_conn_id = 0;

static struct proto proto;

/**** Helper: find connection by id ****/
static wasm_handle *
find_wasm_handle(int id)
{
    if (id < 0 || id >= MAX_WASM_CONNECTIONS)
        return nullptr;
    return connections[id];
}

/**** Helper: append text to output buffer ****/
static void
wasm_output_append(wasm_handle *h, const char *data, int len)
{
    if (!h || !h->connected)
        return;
    /* Grow buffer if needed */
    while (h->output_len + len + 1 > h->output_capacity) {
        h->output_capacity *= 2;
        h->output_buffer = (char *)realloc(h->output_buffer, h->output_capacity);
    }
    memcpy(h->output_buffer + h->output_len, data, len);
    h->output_len += len;
    h->output_buffer[h->output_len] = '\0';
}

/**** Exported functions callable from JavaScript ****/

extern "C" {

EMSCRIPTEN_KEEPALIVE
int wasm_new_connection(void)
{
    /* Find a free slot */
    int slot = -1;
    for (int i = 0; i < MAX_WASM_CONNECTIONS; i++) {
        int idx = (next_conn_id + i) % MAX_WASM_CONNECTIONS;
        if (connections[idx] == nullptr) {
            slot = idx;
            break;
        }
    }
    if (slot < 0) {
        errlog("wasm_new_connection: no free slots\n");
        return -1;
    }

    wasm_handle *h = (wasm_handle *)calloc(1, sizeof(wasm_handle));
    h->id = slot;
    h->connected = true;
    h->input_suspended = false;
    h->output_capacity = INITIAL_OUTPUT_CAPACITY;
    h->output_buffer = (char *)calloc(1, h->output_capacity);
    h->output_len = 0;

    char namebuf[64];
    snprintf(namebuf, sizeof(namebuf), "wasm-connection-%d", slot);
    h->name = strdup(namebuf);

    connections[slot] = h;
    next_conn_id = (slot + 1) % MAX_WASM_CONNECTIONS;

    /* Create the network_handle wrapping our wasm_handle */
    network_handle nh;
    nh.ptr = (void *)h;

    /* Call server_new_connection with null listener (defaults to #0) */
    server_handle sh = server_new_connection(null_server_listener, nh, false);
    h->shandle = sh;

    oklog("WASM: New virtual connection %d created\n", slot);
    return slot;
}

EMSCRIPTEN_KEEPALIVE
void wasm_inject_input(int conn_id, const char *line)
{
    wasm_handle *h = find_wasm_handle(conn_id);
    if (!h || !h->connected) {
        errlog("wasm_inject_input: invalid connection %d\n", conn_id);
        return;
    }
    if (h->input_suspended) {
        /* Input is suspended by the server; queue it anyway since
         * server_receive_line just enqueues a task */
    }
    server_receive_line(h->shandle, line, false);
}

EMSCRIPTEN_KEEPALIVE
const char *wasm_get_output(int conn_id)
{
    wasm_handle *h = find_wasm_handle(conn_id);
    if (!h)
        return "";
    /* Return the current buffer content. Caller must read before next call. */
    /* We don't clear here - use a separate clear or it gets cleared on next call */
    return h->output_buffer ? h->output_buffer : "";
}

EMSCRIPTEN_KEEPALIVE
void wasm_clear_output(int conn_id)
{
    wasm_handle *h = find_wasm_handle(conn_id);
    if (!h || !h->output_buffer)
        return;
    h->output_len = 0;
    h->output_buffer[0] = '\0';
}

EMSCRIPTEN_KEEPALIVE
void wasm_close_connection(int conn_id)
{
    wasm_handle *h = find_wasm_handle(conn_id);
    if (!h)
        return;

    oklog("WASM: Closing virtual connection %d\n", conn_id);

    if (h->connected) {
        h->connected = false;
        server_close(h->shandle);
    }

    connections[conn_id] = nullptr;
    if (h->output_buffer)
        free(h->output_buffer);
    if (h->name)
        free(h->name);
    free(h);
}

} /* extern "C" */

/**** Standard network interface implementations ****/

int
network_initialize(int argc, char **argv, Var *desc)
{
    proto.pocket_size = 0;
    proto.believe_eof = 1;
    proto.eol_out_string = "\r\n";

    desc->type = TYPE_INT;
    desc->v.num = 0;

    /* Initialize connection slots */
    for (int i = 0; i < MAX_WASM_CONNECTIONS; i++)
        connections[i] = nullptr;

    return 1; /* success */
}

enum error
network_make_listener(server_listener sl, Var desc,
                      network_listener *nl,
                      const char **name, const char **ip_address,
                      uint16_t *port, bool use_ipv6, const char *interface
                      USE_TLS_BOOL_DEF TLS_CERT_PATH_DEF)
{
    *name = str_dup("wasm-virtual");
    *ip_address = str_dup("0.0.0.0");
    *port = 0;
    nl->ptr = nullptr;
    return E_NONE;
}

int
network_listen(network_listener nl)
{
    return 1; /* success */
}

enum accept_error
network_accept_connection(int listener_fd,
                          int *read_fd, int *write_fd,
                          const char **name, const char **ip_addr,
                          uint16_t *port, sa_family_t *protocol
                          USE_TLS_BOOL_DEF SSL_CONTEXT_2_DEF TLS_CERT_PATH_DEF)
{
    return PA_OTHER;
}

void
network_close_connection(int read_fd, int write_fd)
{
    /* no-op */
}

void
close_listener(int fd)
{
    /* no-op */
}

enum error
make_listener(Var desc, int *fd,
              const char **name, const char **ip_address,
              uint16_t *port, const bool use_ipv6, const char *interface)
{
    *fd = -1;
    *name = str_dup("wasm-virtual");
    *ip_address = str_dup("0.0.0.0");
    *port = 0;
    return E_NONE;
}

#ifdef OUTBOUND_NETWORK
package
open_connection(Var arglist,
                int *read_fd, int *write_fd,
                const char **name, const char **ip_addr,
                uint16_t *port, sa_family_t *protocol, bool use_ipv6
                USE_TLS_BOOL_DEF SSL_CONTEXT_2_DEF)
{
    return make_raise_pack(E_PERM, "Networking not available in WASM", var_ref(zero));
}

package
network_open_connection(Var arglist, server_listener sl, bool use_ipv6 USE_TLS_BOOL_DEF)
{
    return make_raise_pack(E_PERM, "Networking not available in WASM", var_ref(zero));
}
#endif

int
network_send_line(network_handle nh, const char *line, int flush_ok, bool send_newline)
{
    wasm_handle *h = (wasm_handle *)nh.ptr;
    if (h && h->connected) {
        int len = strlen(line);
        wasm_output_append(h, line, len);
        if (send_newline) {
            wasm_output_append(h, "\n", 1);
        }
        /* Also emit to stdout via EM_ASM so JS print() callback captures it */
        if (send_newline)
            EM_ASM({ Module['print'](UTF8ToString($0)); }, line);
        else
            EM_ASM({ Module['printRaw'] ? Module['printRaw'](UTF8ToString($0)) : Module['print'](UTF8ToString($0)); }, line);
    }
    return 1;
}

int
network_send_bytes(network_handle nh, const char *buffer, int buflen, int flush_ok)
{
    wasm_handle *h = (wasm_handle *)nh.ptr;
    if (h && h->connected) {
        wasm_output_append(h, buffer, buflen);
    }
    return 1;
}

int
network_buffered_output_length(network_handle nh)
{
    wasm_handle *h = (wasm_handle *)nh.ptr;
    if (h)
        return h->output_len;
    return 0;
}

void
network_suspend_input(network_handle nh)
{
    wasm_handle *h = (wasm_handle *)nh.ptr;
    if (h)
        h->input_suspended = true;
}

void
network_resume_input(network_handle nh)
{
    wasm_handle *h = (wasm_handle *)nh.ptr;
    if (h)
        h->input_suspended = false;
}

void
network_set_connection_binary(network_handle nh, bool binary)
{
    /* no-op for WASM */
}

int
network_process_io(int timeout)
{
    if (timeout > 0)
        emscripten_sleep(timeout > 1000000 ? 1000 : timeout / 1000);
    else
        emscripten_sleep(10); /* minimal yield even with timeout=0 */
    return 0;
}

const char *
network_connection_name(network_handle nh)
{
    wasm_handle *h = (wasm_handle *)nh.ptr;
    if (h && h->name)
        return h->name;
    return "wasm";
}

int
lookup_network_connection_name(const network_handle nh, const char **name)
{
    wasm_handle *h = (wasm_handle *)nh.ptr;
    if (h && h->name) {
        *name = h->name;
        return 0;
    }
    *name = "wasm";
    return -1;
}

char *
full_network_connection_name(const network_handle nh, bool legacy)
{
    wasm_handle *h = (wasm_handle *)nh.ptr;
    if (h && h->name)
        return str_dup(h->name);
    return str_dup("wasm");
}

const char *
network_ip_address(network_handle nh)
{
    return "127.0.0.1";
}

const char *
network_source_connection_name(const network_handle nh)
{
    return "wasm-local";
}

const char *
network_source_ip_address(const network_handle nh)
{
    return "127.0.0.1";
}

uint16_t
network_port(const network_handle nh)
{
    return 0;
}

uint16_t
network_source_port(const network_handle nh)
{
    return 0;
}

const char *
network_protocol(const network_handle nh)
{
    return "wasm";
}

Var
network_connection_options(network_handle nh, Var list)
{
    return list;
}

int
network_connection_option(network_handle nh, const char *option, Var *value)
{
    return 0;
}

int
network_set_connection_option(network_handle nh, const char *option, Var value)
{
    return 0;
}

int
network_set_client_keep_alive(network_handle nh, Var map)
{
    return 0;
}

void
network_close(network_handle nh)
{
    wasm_handle *h = (wasm_handle *)nh.ptr;
    if (h) {
        h->connected = false;
        /* Don't free here - server may still reference the handle.
         * It will be cleaned up by wasm_close_connection or shutdown. */
    }
}

void
network_close_listener(network_listener nl)
{
    /* no-op */
}

void
network_shutdown(void)
{
    /* Clean up all virtual connections */
    for (int i = 0; i < MAX_WASM_CONNECTIONS; i++) {
        if (connections[i]) {
            wasm_handle *h = connections[i];
            connections[i] = nullptr;
            if (h->output_buffer)
                free(h->output_buffer);
            if (h->name)
                free(h->name);
            free(h);
        }
    }
}

int
network_parse_proxy_string(char *command, Stream *new_connection_name, struct sockaddr_storage *new_ai_addr)
{
    return 0;
}

#ifdef USE_TLS
int
network_handle_is_tls(network_handle nh)
{
    return 0;
}

int
nlistener_is_tls(const void *ptr)
{
    return 0;
}

Var
tls_connection_info(network_handle nh)
{
    return new_list(0);
}
#endif

void
network_register_fd(int fd, network_fd_callback readable,
                    network_fd_callback writable, void *data)
{
    /* no-op */
}

void
network_unregister_fd(int fd)
{
    /* no-op */
}

#ifndef HAVE_ACCEPT4
int
network_set_nonblocking(int fd)
{
    return 1;
}
#endif

int
rewrite_connection_name(const network_handle nh, const char *destination,
                        const char *destination_port, const char *source,
                        const char *source_port)
{
    return 0;
}

int
network_name_lookup_rewrite(const Objid obj, const char *name, const network_handle nh)
{
    return 0;
}

void
lock_connection_name_mutex(const network_handle nh)
{
    /* no-op */
}

void
unlock_connection_name_mutex(const network_handle nh)
{
    /* no-op */
}

void
increment_nhandle_refcount(const network_handle nh)
{
    /* no-op - wasm_handle has no refcount */
}

void
decrement_nhandle_refcount(const network_handle nh)
{
    /* no-op - wasm_handle has no refcount */
}

uint32_t
get_nhandle_refcount(const network_handle nh)
{
    return 1; /* Always 1 for virtual connections */
}

uint32_t
nhandle_refcount(const network_handle nh)
{
    return 1; /* Always 1 for virtual connections */
}
