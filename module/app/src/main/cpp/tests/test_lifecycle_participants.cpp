#include "contracts/host_sdk.h"
#include "game/adapter_ports/lifecycle_participant_facade.h"
#include "host/lifecycle_participants.h"
#include "plugins/extension_bag/lifecycle_registration.h"

#include <cstdio>
#include <cstring>

namespace {

int g_pass = 0;
int g_fail = 0;
int g_available[9]{};
int g_prepare_calls[9]{};
int g_main_menu_calls[9]{};
int g_enter_calls[9]{};
int g_state_calls[9]{};
int g_prepare_fail = -1;
int g_enter_match = -1;
int g_state_value[9]{};

#define CHECK(condition) do { \
    if (condition) ++g_pass; \
    else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); } \
} while (0)

int available_0() { return g_available[0]; }
int available_1() { return g_available[1]; }
int available_2() { return g_available[2]; }
int available_3() { return g_available[3]; }
int available_4() { return g_available[4]; }
int available_5() { return g_available[5]; }
int available_6() { return g_available[6]; }
int available_7() { return g_available[7]; }

int prepare_0() { ++g_prepare_calls[0]; return g_prepare_fail == 0 ? 0 : 1; }
int prepare_1() { ++g_prepare_calls[1]; return g_prepare_fail == 1 ? 0 : 1; }
int prepare_2() { ++g_prepare_calls[2]; return g_prepare_fail == 2 ? 0 : 1; }
int prepare_3() { ++g_prepare_calls[3]; return g_prepare_fail == 3 ? 0 : 1; }
int prepare_4() { ++g_prepare_calls[4]; return g_prepare_fail == 4 ? 0 : 1; }
int prepare_5() { ++g_prepare_calls[5]; return g_prepare_fail == 5 ? 0 : 1; }
int prepare_6() { ++g_prepare_calls[6]; return g_prepare_fail == 6 ? 0 : 1; }
int prepare_7() { ++g_prepare_calls[7]; return g_prepare_fail == 7 ? 0 : 1; }

int enter_0(uintptr_t) { ++g_enter_calls[0]; return g_enter_match == 0; }
int enter_1(uintptr_t) { ++g_enter_calls[1]; return g_enter_match == 1; }
int enter_2(uintptr_t) { ++g_enter_calls[2]; return g_enter_match == 2; }
int enter_3(uintptr_t) { ++g_enter_calls[3]; return g_enter_match == 3; }
int enter_4(uintptr_t) { ++g_enter_calls[4]; return g_enter_match == 4; }
int enter_5(uintptr_t) { ++g_enter_calls[5]; return g_enter_match == 5; }
int enter_6(uintptr_t) { ++g_enter_calls[6]; return g_enter_match == 6; }
int enter_7(uintptr_t) { ++g_enter_calls[7]; return g_enter_match == 7; }

int main_menu_0() { ++g_main_menu_calls[0]; return 1; }
int main_menu_1() { ++g_main_menu_calls[1]; return 1; }
int main_menu_2() { ++g_main_menu_calls[2]; return 1; }
int main_menu_3() { ++g_main_menu_calls[3]; return 1; }
int main_menu_4() { ++g_main_menu_calls[4]; return 1; }
int main_menu_5() { ++g_main_menu_calls[5]; return 1; }
int main_menu_6() { ++g_main_menu_calls[6]; return 1; }
int main_menu_7() { ++g_main_menu_calls[7]; return 1; }

int state_0() { ++g_state_calls[0]; return g_state_value[0]; }
int state_1() { ++g_state_calls[1]; return g_state_value[1]; }
int state_2() { ++g_state_calls[2]; return g_state_value[2]; }
int state_3() { ++g_state_calls[3]; return g_state_value[3]; }
int state_4() { ++g_state_calls[4]; return g_state_value[4]; }
int state_5() { ++g_state_calls[5]; return g_state_value[5]; }
int state_6() { ++g_state_calls[6]; return g_state_value[6]; }
int state_7() { ++g_state_calls[7]; return g_state_value[7]; }

using Probe = int (*)();
using Callback = int (*)();
using Enter = int (*)(uintptr_t);

const Probe kProbes[] = {available_0, available_1, available_2, available_3,
                         available_4, available_5, available_6, available_7};
const Callback kPrepare[] = {prepare_0, prepare_1, prepare_2, prepare_3,
                             prepare_4, prepare_5, prepare_6, prepare_7};
const Enter kEnter[] = {enter_0, enter_1, enter_2, enter_3,
                        enter_4, enter_5, enter_6, enter_7};
const Callback kMainMenu[] = {main_menu_0, main_menu_1, main_menu_2, main_menu_3,
                              main_menu_4, main_menu_5, main_menu_6, main_menu_7};
const Callback kState[] = {state_0, state_1, state_2, state_3,
                           state_4, state_5, state_6, state_7};

qol::contracts::QolLifecycleParticipant participant(uint32_t i) {
    return {sizeof(qol::contracts::QolLifecycleParticipant),
            qol::contracts::kLifecycleParticipantAbiVersion, kProbes[i], kPrepare[i],
            kEnter[i], kMainMenu[i], kState[i]};
}

}  // namespace

int main() {
    using namespace qol;

    CHECK(game::lifecycle_participant_prepare_save_slot_load());
    CHECK(!game::lifecycle_participant_is_inventory_enter(17));
    CHECK(game::lifecycle_participant_prepare_main_menu());
    CHECK(game::lifecycle_participant_inventory_state_id() == -1);

    contracts::QolLifecycleParticipant bad = participant(0);
    bad.struct_size = sizeof(bad) - 1;
    CHECK(host::host_lifecycle_participant_register(&bad) == 0);
    bad = participant(0);
    bad.abi_version++;
    CHECK(host::host_lifecycle_participant_register(&bad) == 0);
    bad = participant(0);
    bad.available = nullptr;
    CHECK(host::host_lifecycle_participant_register(&bad) == 0);

    contracts::QolLifecycleParticipant entries[8]{};
    entries[0] = participant(0);
    g_available[0] = 1;
    bool eb_participant_registered = false;
    CHECK(features::extension_bag::detail::register_lifecycle_participant_once(
        &host::host_lifecycle_participant_register, &entries[0], eb_participant_registered));
    CHECK(eb_participant_registered);
    g_available[0] = 0;
    CHECK(features::extension_bag::detail::register_lifecycle_participant_once(
        &host::host_lifecycle_participant_register, &entries[0], eb_participant_registered));
    g_available[0] = 1;
    CHECK(features::extension_bag::detail::register_lifecycle_participant_once(
        &host::host_lifecycle_participant_register, &entries[0], eb_participant_registered));
    CHECK(host::host_lifecycle_participant_register(&entries[0]) == 0);
    for (uint32_t i = 1; i < 8; ++i) {
        entries[i] = participant(i);
        g_available[i] = 1;
        CHECK(host::host_lifecycle_participant_register(&entries[i]) == 1);
    }
    CHECK(host::host_lifecycle_participant_register(&entries[1]) == 0);
    bad = participant(0);
    bad.available = available_0;
    bad.prepare_save_slot_load = nullptr;
    CHECK(host::host_lifecycle_participant_register(&bad) == 0);

    CHECK(game::lifecycle_participant_bind_host(host::host_lifecycle_dispatch()));
    CHECK(!game::lifecycle_participant_bind_host(nullptr));
    CHECK(game::lifecycle_participant_prepare_save_slot_load());
    for (int i = 0; i < 8; ++i) CHECK(g_prepare_calls[i] == 1);

    g_prepare_fail = 3;
    CHECK(!game::lifecycle_participant_prepare_save_slot_load());
    CHECK(g_prepare_calls[2] == 2 && g_prepare_calls[3] == 2 && g_prepare_calls[4] == 1);
    g_prepare_fail = -1;

    g_enter_match = 5;
    CHECK(game::lifecycle_participant_is_inventory_enter(123));
    CHECK(g_enter_calls[4] == 1 && g_enter_calls[5] == 1 && g_enter_calls[6] == 0);

    g_state_value[0] = -1;
    g_state_value[1] = 7;
    CHECK(game::lifecycle_participant_inventory_state_id() == 7);
    CHECK(g_state_calls[0] == 1 && g_state_calls[1] == 1 && g_state_calls[2] == 0);

    g_available[0] = 0;
    g_available[1] = 0;
    g_available[2] = 0;
    g_available[3] = 0;
    g_available[4] = 0;
    g_available[5] = 0;
    g_available[6] = 0;
    g_available[7] = 0;
    CHECK(game::lifecycle_participant_prepare_save_slot_load());
    CHECK(!game::lifecycle_participant_is_inventory_enter(123));
    CHECK(game::lifecycle_participant_prepare_main_menu());
    CHECK(game::lifecycle_participant_inventory_state_id() == -1);

    CHECK(contracts::kHostSdkMinor == 18);
    CHECK(offsetof(contracts::QolHostSdk, lifecycle_participant_register) ==
          offsetof(contracts::QolHostSdk, ticket_result) + sizeof(contracts::QolTicketResultApi));
    CHECK(offsetof(contracts::QolHostSdk, item_handle) ==
          offsetof(contracts::QolHostSdk, lifecycle_participant_register) +
              sizeof(contracts::QolLifecycleParticipantRegisterFn));
    std::printf("lifecycle_participants: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
