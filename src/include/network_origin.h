/* Runtime-only network identity. Never serialize these fields or store Var/pointers here. */
#ifndef Network_Origin_H
#define Network_Origin_H 1

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <unistd.h>
#include "structures.h"

enum class NetworkInputKind { None, Inband, Oob, Binary };

struct ConnectionBinding {
    unsigned char boot[16];
    Num physical;
    Num epoch;
    Objid actor;
};

struct NetworkOrigin {
    ConnectionBinding binding;
    Num ordinal;
    NetworkInputKind kind;
    bool telnet_oob;
    Objid listener;
    uint16_t local_port;
    bool outbound;
};

struct ConnectionBindingState {
    Num physical;
    Num epoch;
    Num input_head;
    bool retired;
    bool permanent_none;
};

/* Internal reservation only; this pointer never appears in task metadata or MOO. */
struct ConnectionBindingReservation {
    void *handle;
    Objid old_actor;
    Num epoch;
};

static_assert(std::is_trivial<NetworkOrigin>::value, "origin must be POD");
static_assert(std::is_trivially_copyable<NetworkOrigin>::value, "origin must copy by value");

inline ConnectionBindingState new_connection_binding(Num &physical_head)
{
    ConnectionBindingState state{};
    if (physical_head == MAXINT) {
        state.permanent_none = state.retired = true;
    } else {
        state.physical = ++physical_head;
        state.epoch = 1;
    }
    return state;
}

inline bool reserve_binding_epoch(const ConnectionBindingState &state, Num &epoch)
{
    if (state.permanent_none) {
        epoch = 0;
        return true;
    }
    if (state.epoch == MAXINT)
        return false;
    epoch = state.epoch + 1;
    return true;
}

inline bool advance_network_input(ConnectionBindingState &state)
{
    if (state.permanent_none)
        return false;
    if (state.input_head == MAXINT) {
        state.permanent_none = state.retired = true;
        return false;
    }
    state.input_head++;
    return !state.retired;
}

/* The injectable read operation is a native test seam, never a MOO setter. */
inline bool read_connection_nonce(int fd, unsigned char (&nonce)[16],
        ssize_t (*reader)(int, void *, size_t) = ::read)
{
    size_t offset = 0;
    while (offset < sizeof(nonce)) {
        ssize_t count = reader(fd, nonce + offset, sizeof(nonce) - offset);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return false;
        offset += count;
    }
    return true;
}

#endif
