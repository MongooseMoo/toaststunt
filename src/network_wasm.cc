/* network_wasm.cc -- WASM stub for the network subsystem.
 * BSD sockets are unavailable in browser WASM.
 * All network functions are stubs that return no-op/error values.
 * Phase 2+ will implement a virtual connection layer via JS postMessage.
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

/* External variables (outbound_network_enabled, bind_ipv4, bind_ipv6,
   default_certificate_path, default_key_path) are defined in server.cc */

static struct proto proto;

int
network_initialize(int argc, char **argv, Var *desc)
{
    proto.pocket_size = 0;
    proto.believe_eof = 1;
    proto.eol_out_string = "\r\n";

    desc->type = TYPE_INT;
    desc->v.num = 0;
    return 1; /* success */
}

enum error
network_make_listener(server_listener sl, Var desc,
                      network_listener *nl,
                      const char **name, const char **ip_address,
                      uint16_t *port, bool use_ipv6, const char *interface
                      USE_TLS_BOOL_DEF TLS_CERT_PATH_DEF)
{
    *name = str_dup("wasm-stub");
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
    *name = str_dup("wasm-stub");
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
    return 1;
}

int
network_send_bytes(network_handle nh, const char *buffer, int buflen, int flush_ok)
{
    return 1;
}

int
network_buffered_output_length(network_handle nh)
{
    return 0;
}

void
network_suspend_input(network_handle nh)
{
    /* no-op */
}

void
network_resume_input(network_handle nh)
{
    /* no-op */
}

void
network_set_connection_binary(network_handle nh, bool binary)
{
    /* no-op */
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
    return "wasm";
}

int
lookup_network_connection_name(const network_handle nh, const char **name)
{
    *name = "wasm";
    return -1;
}

char *
full_network_connection_name(const network_handle nh, bool legacy)
{
    return str_dup("wasm");
}

const char *
network_ip_address(network_handle nh)
{
    return "0.0.0.0";
}

const char *
network_source_connection_name(const network_handle nh)
{
    return "wasm";
}

const char *
network_source_ip_address(const network_handle nh)
{
    return "0.0.0.0";
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
    /* no-op */
}

void
network_close_listener(network_listener nl)
{
    /* no-op */
}

void
network_shutdown(void)
{
    /* no-op */
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
    /* no-op */
}

void
decrement_nhandle_refcount(const network_handle nh)
{
    /* no-op */
}

uint32_t
get_nhandle_refcount(const network_handle nh)
{
    return 0;
}

uint32_t
nhandle_refcount(const network_handle nh)
{
    return 0;
}
