#include "infra/command_queue.h"

#include <cassert>
#include <cstring>
#include <string>
#include <vector>

namespace {

bool oversized_failure(void*, char* out, uint32_t capacity, uint32_t* out_size) {
    std::memset(out, 'x', capacity);
    *out_size = capacity + 17;
    return false;
}

bool oversized_success(void*, char* out, uint32_t capacity, uint32_t* out_size) {
    constexpr char prefix[] = "{\"ok\":true";
    std::memcpy(out, prefix, sizeof(prefix) - 1);
    *out_size = capacity + 23;
    return true;
}

bool normal_result(void*, char* out, uint32_t, uint32_t* out_size) {
    constexpr char body[] = "{\"ok\":true}";
    std::memcpy(out, body, sizeof(body) - 1);
    *out_size = sizeof(body) - 1;
    return true;
}

}  // namespace

int main() {
    using namespace qol::infra;
    const std::vector<char> full(kMaxResultBytes, 'x');
    char result[kMaxResultBytes]{};
    uint32_t result_size = 0;

    command_queue_reset_for_test();
    const CommandTicket failed_handler_ticket =
        post(Command{"oversized-failure", nullptr, nullptr, false, &oversized_failure});
    assert(failed_handler_ticket != 0 && drain() == 1);
    assert(ticket_state(failed_handler_ticket) == CommandState::kDone);
    assert(ticket_result_overflowed(failed_handler_ticket));
    assert(read_result(failed_handler_ticket, result, sizeof(result), &result_size));
    assert(result_size == kMaxResultBytes);
    assert(std::memcmp(result, full.data(), full.size()) == 0);

    command_queue_reset_for_test();
    const CommandTicket eb_or_snapshot_ticket =
        post(Command{"oversized-success", nullptr, nullptr, false, &oversized_success});
    assert(eb_or_snapshot_ticket != 0 && drain() == 1);
    assert(ticket_state(eb_or_snapshot_ticket) == CommandState::kDone);
    assert(ticket_result_overflowed(eb_or_snapshot_ticket));
    assert(read_result(eb_or_snapshot_ticket, result, sizeof(result), &result_size));
    assert(result_size == kMaxResultBytes);
    assert(std::memcmp(result, "{\"ok\":true", 10) == 0);

    command_queue_reset_for_test();
    const CommandTicket normal_ticket =
        post(Command{"normal", nullptr, nullptr, false, &normal_result});
    assert(normal_ticket != 0 && drain() == 1);
    assert(ticket_state(normal_ticket) == CommandState::kDone);
    assert(!ticket_result_overflowed(normal_ticket));
    return 0;
}
