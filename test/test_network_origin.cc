// Native counter/entropy failure contracts; no server or database required.
#include <cstdio>
#include <cstring>
#include "network_origin.h"

#define CHECK(condition) do { if (!(condition)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); return 1; } } while (0)

static int reads;
static ssize_t partial_read(int, void *buffer, size_t size)
{
    if (++reads == 1) { errno = EINTR; return -1; }
    size_t count = size > 3 ? 3 : size;
    std::memset(buffer, 0x5a, count);
    return count;
}
static ssize_t eof_read(int, void *, size_t) { return 0; }
static ssize_t failed_read(int, void *, size_t) { errno = EIO; return -1; }

int main()
{
    Num physical_head = MAXINT - 1;
    auto state = new_connection_binding(physical_head);
    CHECK(state.physical == MAXINT && state.epoch == 1 && state.input_head == 0);
    auto unavailable = new_connection_binding(physical_head);
    CHECK(physical_head == MAXINT && unavailable.permanent_none && unavailable.retired);
    CHECK(unavailable.physical == 0 && !advance_network_input(unavailable));
    Num epoch = -1;
    CHECK(reserve_binding_epoch(unavailable, epoch) && epoch == 0);
    CHECK(reserve_binding_epoch(state, epoch) && epoch == 2 && state.epoch == 1);
    state.epoch = MAXINT;
    const auto before = state;
    CHECK(!reserve_binding_epoch(state, epoch));
    CHECK(std::memcmp(&before, &state, sizeof(state)) == 0);
    state.input_head = MAXINT - 1;
    CHECK(advance_network_input(state) && state.input_head == MAXINT);
    CHECK(!advance_network_input(state) && state.permanent_none && state.retired);
    CHECK(reserve_binding_epoch(state, epoch) && epoch == 0);
    CHECK(!advance_network_input(state) && state.input_head == MAXINT);
    state = new_connection_binding(physical_head = 0);
    state.retired = true;
    CHECK(!advance_network_input(state) && state.input_head == 1);
    CHECK(reserve_binding_epoch(state, epoch) && epoch == 2);
    unsigned char nonce[16]{};
    CHECK(read_connection_nonce(-1, nonce, partial_read));
    CHECK(reads > 5);
    for (auto byte : nonce) CHECK(byte == 0x5a);
    CHECK(!read_connection_nonce(-1, nonce, eof_read));
    CHECK(!read_connection_nonce(-1, nonce, failed_read));
    NetworkOrigin origin{};
    CHECK(origin.binding.physical == 0 && origin.ordinal == 0 && origin.kind == NetworkInputKind::None);
    return 0;
}
