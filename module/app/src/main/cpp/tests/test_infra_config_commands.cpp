// infra/config.h + infra/commands.h host 单测：
// config 覆盖默认值、读写、bool 0/1、变更事件（同值不重复广播）、容量与非法键、reset；
// commands 覆盖 post→state(pending)→drain→state(done)、配置事件到达、取消未开始票据、
// 未知 kind 失败、非法参数、槽位容量与 drain 后回收。

#include "contracts/commands.h"
#include "contracts/events.h"
#include "infra/command_queue.h"
#include "infra/commands.h"
#include "infra/config.h"
#include "infra/events.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) ++g_pass; \
    else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

using qol::contracts::kCommandSetConfigBool;
using qol::contracts::QolCommand;
using qol::contracts::QolCommandTicket;
using qol::infra::command_queue_reset_for_test;
using qol::infra::command_state_name;
using qol::infra::CommandState;
using qol::infra::commands_drain;
using qol::infra::commands_port;
using qol::infra::commands_reset_for_test;
using qol::infra::config_get_bool;
using qol::infra::config_get_int;
using qol::infra::config_reset_for_test;
using qol::infra::config_set_bool;
using qol::infra::config_set_int;
using qol::infra::events_reset_for_test;
using qol::infra::subscribe;

namespace {

int g_event_count = 0;
const char* g_last_key = nullptr;
int32_t g_last_value = 0;
uint32_t g_typed_calls = 0;
uint32_t g_typed_value = 0;

void on_config_changed(void* ctx, const void* payload) {
    (void)ctx;
    ++g_event_count;
    const auto* event = static_cast<const qol::contracts::ConfigChangedEvent*>(payload);
    g_last_key = event->key;
    g_last_value = event->int_value;
}

void reset_all() {
    commands_reset_for_test();
    command_queue_reset_for_test();
    config_reset_for_test();
    events_reset_for_test();
    g_event_count = 0;
    g_last_key = nullptr;
    g_last_value = 0;
    g_typed_calls = 0;
    g_typed_value = 0;
}

void subscribe_config_changed() {
    CHECK(subscribe(qol::contracts::kEventConfigChanged, on_config_changed, nullptr) != 0);
}

void test_config_defaults() {
    reset_all();
    CHECK(config_get_int("missing", 42) == 42);
    CHECK(config_get_int(nullptr, 7) == 7);
    CHECK(config_get_bool("missing", true) == true);
    CHECK(config_get_bool("missing", false) == false);
}

void test_config_set_get() {
    reset_all();
    CHECK(config_set_int("a", 5));
    CHECK(config_get_int("a", 0) == 5);
    CHECK(config_set_int("a", -3));
    CHECK(config_get_int("a", 0) == -3);

    CHECK(config_set_bool("b", true));
    CHECK(config_get_bool("b", false) == true);
    CHECK(config_get_int("b", 99) == 1); // bool 以 0/1 存
    CHECK(config_set_bool("b", false));
    CHECK(config_get_int("b", 99) == 0);

    CHECK(!config_set_int(nullptr, 1));
    CHECK(!config_set_int("", 1));

    char long_key[65];
    std::memset(long_key, 'k', sizeof(long_key) - 1);
    long_key[64] = '\0';
    CHECK(!config_set_int(long_key, 1)); // 64 字符超长

    char max_key[64];
    std::memset(max_key, 'm', sizeof(max_key) - 1);
    max_key[63] = '\0';
    CHECK(config_set_int(max_key, 9)); // 63 字符可存
    CHECK(config_get_int(max_key, 0) == 9);
}

void test_config_change_events() {
    reset_all();
    subscribe_config_changed();

    CHECK(config_set_int("k", 1));
    CHECK(g_event_count == 1);
    CHECK(std::strcmp(g_last_key, "k") == 0);
    CHECK(g_last_value == 1);

    CHECK(config_set_int("k", 1)); // 同值不重复广播
    CHECK(g_event_count == 1);

    CHECK(config_set_int("k", 2)); // 变更广播
    CHECK(g_event_count == 2);
    CHECK(g_last_value == 2);

    CHECK(config_set_bool("b", true));
    CHECK(g_event_count == 3);
    CHECK(std::strcmp(g_last_key, "b") == 0);
    CHECK(g_last_value == 1);
}

void test_config_capacity() {
    reset_all();
    static char key[16];
    for (int i = 0; i < 64; ++i) {
        std::snprintf(key, sizeof(key), "cap%d", i);
        CHECK(config_set_int(key, i));
    }
    CHECK(!config_set_int("cap64", 64)); // 容量满
    CHECK(config_get_int("cap64", -1) == -1);
    CHECK(config_set_int("cap0", 100)); // 已存在键仍可更新
    CHECK(config_get_int("cap0", 0) == 100);
    config_reset_for_test();
    CHECK(config_get_int("cap0", -1) == -1);
}

void test_commands_post_drain() {
    reset_all();
    const qol::contracts::QolCommands& port = commands_port();

    QolCommand cmd{kCommandSetConfigBool, "flag", 1};
    QolCommandTicket ticket = 0;
    CHECK(port.post(&ticket, &cmd).ok == 0);
    CHECK(ticket != 0);
    CHECK(port.state(ticket) == static_cast<int32_t>(CommandState::kPending));
    CHECK(config_get_bool("flag", false) == false); // 未 drain 不执行

    CHECK(commands_drain() == 1);
    CHECK(port.state(ticket) == static_cast<int32_t>(CommandState::kDone));
    CHECK(config_get_bool("flag", false) == true);
}

void test_commands_config_event() {
    reset_all();
    subscribe_config_changed();
    const qol::contracts::QolCommands& port = commands_port();

    QolCommand cmd{kCommandSetConfigBool, "auto", 0};
    QolCommandTicket ticket = 0;
    CHECK(port.post(&ticket, &cmd).ok == 0);
    CHECK(g_event_count == 0);
    CHECK(commands_drain() == 1);
    CHECK(g_event_count == 1);
    CHECK(std::strcmp(g_last_key, "auto") == 0);
    CHECK(g_last_value == 0);
}

void test_commands_cancel() {
    reset_all();
    const qol::contracts::QolCommands& port = commands_port();

    QolCommand cmd{kCommandSetConfigBool, "cancelme", 1};
    QolCommandTicket ticket = 0;
    CHECK(port.post(&ticket, &cmd).ok == 0);
    CHECK(port.cancel(ticket).ok == 0);
    CHECK(port.state(ticket) == static_cast<int32_t>(CommandState::kCancelled));
    CHECK(commands_drain() == 0); // 已取消命令不执行
    CHECK(config_get_bool("cancelme", false) == false);
    CHECK(port.cancel(ticket).ok != 0);  // 已取消不可再次取消
    CHECK(port.cancel(999999).ok != 0);  // 未知票据
}

void test_commands_unknown_kind() {
    reset_all();
    const qol::contracts::QolCommands& port = commands_port();

    QolCommand cmd{999, "x", 1};
    QolCommandTicket ticket = 0;
    CHECK(port.post(&ticket, &cmd).ok == 0);
    CHECK(commands_drain() == 1);
    CHECK(port.state(ticket) == static_cast<int32_t>(CommandState::kFailed));
    CHECK(config_get_bool("x", false) == false);
}

void test_commands_slot_capacity_and_reclaim() {
    reset_all();
    const qol::contracts::QolCommands& port = commands_port();

    QolCommand cmd{kCommandSetConfigBool, "k", 1};
    QolCommandTicket tickets[64];
    for (int i = 0; i < 64; ++i) {
        CHECK(port.post(&tickets[i], &cmd).ok == 0);
    }
    QolCommandTicket over = 0;
    CHECK(port.post(&over, &cmd).ok != 0); // 槽位满
    CHECK(over == 0);

    CHECK(commands_drain() == 64);
    // drain 回收已终态槽位后可再次投递。
    QolCommandTicket reused = 0;
    CHECK(port.post(&reused, &cmd).ok == 0);
    CHECK(reused != 0);
    CHECK(commands_drain() == 1);
}

void test_commands_invalid() {
    reset_all();
    const qol::contracts::QolCommands& port = commands_port();

    QolCommand cmd{kCommandSetConfigBool, "k", 1};
    CHECK(port.post(nullptr, &cmd).ok != 0);
    CHECK(port.post(nullptr, nullptr).ok != 0);

    QolCommand bad{kCommandSetConfigBool, nullptr, 1};
    QolCommandTicket ticket = 123;
    CHECK(port.post(&ticket, &bad).ok != 0);
    CHECK(ticket == 0);

    CHECK(port.state(0) == static_cast<int32_t>(CommandState::kFailed)); // 未知票据
}

bool typed_handler(void*, const uint8_t* payload, uint32_t size) {
    if (payload == nullptr || size != sizeof(uint32_t)) return false;
    std::memcpy(&g_typed_value, payload, sizeof(g_typed_value));
    ++g_typed_calls;
    return true;
}

void test_typed_commands() {
    reset_all();
    const qol::contracts::QolCommands& port = commands_port();
    constexpr uint32_t kHandlerId = 7;
    CHECK(port.register_typed_handler(kHandlerId, &typed_handler, nullptr).ok == 0);

    uint32_t payload = 0x12345678;
    QolCommandTicket ticket = 0;
    CHECK(port.post_typed(kHandlerId, &payload, sizeof(payload), &ticket).ok == 0);
    payload = 0;
    CHECK(g_typed_calls == 0);
    CHECK(commands_drain() == 1);
    CHECK(port.state(ticket) == static_cast<int32_t>(CommandState::kDone));
    CHECK(g_typed_calls == 1);
    CHECK(g_typed_value == 0x12345678);

    uint8_t oversized[qol::contracts::kTypedCommandPayloadCapacity + 1] = {};
    CHECK(port.post_typed(kHandlerId, oversized, sizeof(oversized), &ticket).ok != 0);
    CHECK(port.unregister_typed_handler(kHandlerId).ok == 0);
    CHECK(port.post_typed(kHandlerId, &payload, sizeof(payload), &ticket).ok != 0);
}

// command_state_name：4 态稳定英文名 + 越界 unknown。
void test_command_state_name() {
    CHECK(std::strcmp(command_state_name(static_cast<int32_t>(CommandState::kPending)),
                      "pending") == 0);
    CHECK(std::strcmp(command_state_name(static_cast<int32_t>(CommandState::kDone)), "done") == 0);
    CHECK(std::strcmp(command_state_name(static_cast<int32_t>(CommandState::kFailed)),
                      "failed") == 0);
    CHECK(std::strcmp(command_state_name(static_cast<int32_t>(CommandState::kCancelled)),
                      "cancelled") == 0);
    CHECK(std::strcmp(command_state_name(-1), "unknown") == 0);
    CHECK(std::strcmp(command_state_name(4), "unknown") == 0);
    CHECK(std::strcmp(command_state_name(999), "unknown") == 0);
}

}  // namespace

int main() {
    test_config_defaults();
    test_config_set_get();
    test_config_change_events();
    test_config_capacity();
    test_commands_post_drain();
    test_commands_config_event();
    test_commands_cancel();
    test_commands_unknown_kind();
    test_commands_slot_capacity_and_reclaim();
    test_command_state_name();
    test_commands_invalid();
    test_typed_commands();
    std::printf("infra_config_commands: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
