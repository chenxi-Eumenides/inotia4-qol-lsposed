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

bool max_result(void*, char* out, uint32_t capacity, uint32_t* out_size) {
    std::memset(out, 'x', capacity);
    *out_size = capacity;
    return true;
}

}  // namespace

int main() {
    using namespace qol::infra;
    char result[kMaxResultBytes]{};
    uint32_t result_size = 0;

    command_queue_reset_for_test();
    const CommandTicket failed_handler_ticket =
        post(Command{"oversized-failure", nullptr, nullptr, false, &oversized_failure});
    assert(failed_handler_ticket != 0 && drain() == 1);
    assert(ticket_state(failed_handler_ticket) == CommandState::kFailed);
    assert(ticket_result_overflowed(failed_handler_ticket));
    assert(read_result(failed_handler_ticket, result, sizeof(result), &result_size));
    constexpr char failed_summary[] =
        "{\"ok\":false,\"truncated\":true,\"reason\":\"result_limit_exceeded\"}";
    assert(result_size == sizeof(failed_summary) - 1);
    assert(std::memcmp(result, failed_summary, result_size) == 0);

    command_queue_reset_for_test();
    const CommandTicket eb_or_snapshot_ticket =
        post(Command{"oversized-success", nullptr, nullptr, false, &oversized_success});
    assert(eb_or_snapshot_ticket != 0 && drain() == 1);
    assert(ticket_state(eb_or_snapshot_ticket) == CommandState::kDone);
    assert(ticket_result_overflowed(eb_or_snapshot_ticket));
    assert(read_result(eb_or_snapshot_ticket, result, sizeof(result), &result_size));
    constexpr char success_summary[] =
        "{\"ok\":true,\"truncated\":true,\"reason\":\"result_limit_exceeded\"}";
    assert(result_size == sizeof(success_summary) - 1);
    assert(std::memcmp(result, success_summary, result_size) == 0);

    command_queue_reset_for_test();
    const CommandTicket normal_ticket =
        post(Command{"normal", nullptr, nullptr, false, &normal_result});
    assert(normal_ticket != 0 && drain() == 1);
    assert(ticket_state(normal_ticket) == CommandState::kDone);
    assert(!ticket_result_overflowed(normal_ticket));
    constexpr char normal_expected[] = "{\"ok\":true}";
    assert(read_result(normal_ticket, result, sizeof(result), &result_size));
    assert(result_size == sizeof(normal_expected) - 1);
    assert(std::memcmp(result, normal_expected, result_size) == 0);

    command_queue_reset_for_test();
    const CommandTicket overflow_summary_ticket =
        post(Command{"oversized-success", nullptr, nullptr, false, &oversized_success});
    assert(overflow_summary_ticket != 0 && drain() == 1);
    assert(ticket_result_overflowed(overflow_summary_ticket));
    assert(read_result(overflow_summary_ticket, result, sizeof(result), &result_size));
    assert(result_size == sizeof(success_summary) - 1);

    command_queue_reset_for_test();
    std::vector<CommandTicket> budget_tickets;
    budget_tickets.reserve(kMaxRetainedResultBytes / kMaxResultBytes + 1);
    for (uint32_t i = 0; i < kMaxRetainedResultBytes / kMaxResultBytes + 1; ++i) {
        const CommandTicket ticket =
            post(Command{"normal", nullptr, nullptr, false, &max_result});
        assert(ticket != 0);
        budget_tickets.push_back(ticket);
    }
    assert(drain() == budget_tickets.size());
    const CommandTicket evicted_ticket = budget_tickets.front();
    assert(ticket_state(evicted_ticket) == CommandState::kDone);
    assert(!ticket_result_overflowed(evicted_ticket));
    assert(!read_result(evicted_ticket, result, sizeof(result), &result_size));
    return 0;
}
