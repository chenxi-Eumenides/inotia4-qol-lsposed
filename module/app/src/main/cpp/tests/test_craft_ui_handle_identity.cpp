#include "plugins/craft_ui/feature.h"

#include <cassert>
#include <cstdint>
#include <cstring>

#include "contracts/craft_commands.h"
#include "contracts/plugin.h"
#include "contracts/ui.h"

namespace {

bool g_slot_current = true;
int g_identity_queries = 0;
uint32_t g_command_id = 0;
uint32_t g_command_size = 0;
qol::contracts::CraftSetStuffItemPayload g_command{};

QolResult identify_current_stuff_control(void* control, int32_t* out_index) {
    ++g_identity_queries;
    if (out_index == nullptr) return qol::contracts::error(kQolErrInvalidArgument, "bad", 3);
    *out_index = -1;
    if (!g_slot_current || control != reinterpret_cast<void*>(0x55)) {
        return qol::contracts::error(kQolErrNotFound, "not found", 9);
    }
    *out_index = 1;
    return qol::contracts::ok();
}

QolResult post_typed(uint32_t handler, const void* payload, uint32_t size,
                     qol::contracts::QolCommandTicket* out_ticket) {
    if (payload == nullptr || size != sizeof(g_command) || out_ticket == nullptr) {
        return qol::contracts::error(kQolErrInvalidArgument, "bad payload", 11);
    }
    g_command_id = handler;
    g_command_size = size;
    std::memcpy(&g_command, payload, sizeof(g_command));
    *out_ticket = 1;
    return qol::contracts::ok();
}

}  // namespace

int main() {
    qol::contracts::QolUiAdapter ui{};
    ui.size = sizeof(ui);
    ui.major = qol::contracts::kUiAdapterMajor;
    ui.minor = qol::contracts::kUiAdapterMinor;
    ui.craft_stuff_slot_index = &identify_current_stuff_control;

    qol::contracts::QolHostSdk sdk{};
    sdk.ui = &ui;
    sdk.commands.post_typed = &post_typed;
    qol::contracts::PluginContext context{};
    context.sdk = &sdk;
    qol::features::craft_ui::set_context(context);

    void* const transient_control = reinterpret_cast<void*>(0x55);
    assert(qol::features::craft_ui::is_stuff_ctrl(transient_control));
    assert(g_identity_queries == 1);
    g_slot_current = false;
    assert(!qol::features::craft_ui::is_stuff_ctrl(transient_control));
    assert(g_identity_queries == 2);

    const qol::contracts::QolItemHandle item{31, 47};
    qol::features::craft_ui::craft_set_stuff_item(2, item);
    assert(g_command_id == qol::contracts::kCraftSetStuffItemHandlerId);
    assert(g_command_size == sizeof(qol::contracts::CraftSetStuffItemPayload));
    assert(g_command.index == 2 && g_command.item.slot == item.slot &&
           g_command.item.generation == item.generation);

    qol::features::craft_ui::craft_set_stuff_item(2, qol::contracts::kInvalidItemHandle);
    assert(g_command.index == 2 && g_command.item.slot == 0 && g_command.item.generation == 0);
    return 0;
}
