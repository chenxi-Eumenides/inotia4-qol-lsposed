#include "contracts/game_ports/quest_port.h"
#include "contracts/game_ports/character_port.h"
#include "contracts/game_ports/character_http_port.h"
#include "contracts/game_ports/choice_port.h"
#include "contracts/game_ports/world_action_port.h"
#include "contracts/game_ports/item_port.h"
#include "contracts/game_ports/world_port.h"
#include "contracts/game_ports/extension_bag_write_port.h"
#include "contracts/game_ports/save_call_port.h"
#include "contracts/game_ports/save_port.h"
#include "contracts/game_ports/craft_port.h"
#include "contracts/game_ports/craft_ui_write_port.h"
#include "contracts/game_ports/custom_recipe_write_port.h"
#include "contracts/game_ports/character_extras_port.h"
#include "contracts/game_ports/inventory_extras_port.h"
#include "contracts/game_ports/item_extras_port.h"
#include "contracts/game_ports/system_port.h"
#include "contracts/game_ports/save_write_port.h"
#include "contracts/game_ports/save_transaction_port.h"
#include "contracts/game_ports/world_write_port.h"
#include "contracts/game_ports/choice_write_port.h"
#include "contracts/game_ports/ui_write_port.h"
#include "contracts/game_ports/item_money_write_port.h"
#include "contracts/game_ports/item_dice_write_port.h"
#include "contracts/game_ports/item_inventory_state_write_port.h"
#include "contracts/game_ports/character_write_ops_port.h"
#include "contracts/game_ports/character_state_port.h"
#include "contracts/game_ports/move_merge_port.h"
#include "game/ops/game_ops_common.h"
#include "game/state/game_state.h"

#include <cstring>
#include <string>
#include "host/character_state_port.h"

namespace qol::game {
namespace {

const contracts::QolQuestPort kQuestPort{{1, 0}, {}, {}};
const contracts::QolWorldPort kWorldPort{{1, 0}, {}};
contracts::QolItemPort kItemPort{contracts::kQolPortItemVersion,
                                 sizeof(contracts::QolItemPort), {}};
const contracts::QolCharacterPort kCharacterPort{{1, 0}, {}};
const contracts::QolCharacterHttpPort kCharacterHttpPort{{1, 0}, {}};
const contracts::QolChoicePort kChoicePort{{1, 0}, {}, {}};
const contracts::QolExtensionBagWritePort kExtensionBagWritePort{};
const contracts::QolSavePort kSavePort{{1, 0}, {}};
const contracts::QolSaveCallPort kSaveCallPort{{1, 0}, {}};
// 单 owner 生产装配 fixture 需要「就绪」的写端口；默认仍是空桩（其它 target 语义不变）。
const contracts::QolCraftUiWritePort* craft_ui_write_port_override = nullptr;
const contracts::QolCustomRecipeWritePort* custom_recipe_write_port_override = nullptr;

const contracts::QolCraftPort kCraftPort{contracts::kQolPortCraftVersion, {}, {}};
const contracts::QolCraftUiWritePort kCraftUiWritePort{contracts::kQolPortCraftUiWriteVersion,
                                                        nullptr, nullptr, nullptr,
                                                        nullptr, nullptr, nullptr, nullptr, nullptr,
                                                        nullptr, nullptr, nullptr};
const contracts::QolCustomRecipeWritePort kCustomRecipeWritePort{
    contracts::kQolPortCustomRecipeWriteVersion};

const contracts::QolCharacterExtrasPort kCharacterExtrasPort{{1, 0}, {}};
const contracts::QolInventoryExtrasPort kInventoryExtrasPort{{1, 0}, {}};
const contracts::QolItemExtrasPort kItemExtrasPort{{1, 0}, {}};
const contracts::QolSystemPort kSystemPort{{1, 0}, {}};
const contracts::QolSaveWritePort kSaveWritePort{{1, 0}, {}};
// P3c：save_transaction 端口 stub（未绑定 backup；registry tests 只验证私有端口守卫）。
const contracts::QolSaveTransactionPort kSaveTransactionPort{
    {sizeof(contracts::QolSaveTransactionPort),
     contracts::kQolPortSaveTransactionVersion.major,
     contracts::kQolPortSaveTransactionVersion.minor},
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
const contracts::QolWorldWritePort kWorldWritePort{{1, 0}, {}};
const contracts::QolChoiceWritePort kChoiceWritePort{{1, 0}, {}};
QolResult fake_ui_write(int32_t, const char*, char*, uint32_t, uint32_t*) {
    return contracts::error(kQolErrUnsupported, "fake UI write", 14);
}
QolResult fake_ui_write_step(uint64_t, uint32_t, int32_t, const char*, char*, uint32_t,
                             uint32_t*) {
    return contracts::error(kQolErrUnsupported, "fake UI write", 14);
}
QolResult fake_ui_write_cancel_pending(uint64_t) { return contracts::ok(); }
const contracts::QolUiWritePort kUiWritePort{
    contracts::kQolPortUiWriteVersion,
    {&fake_ui_write, &fake_ui_write_step, &fake_ui_write_cancel_pending}};
QolResult fake_ui_state_snapshot(char* screen, uint32_t screen_capacity,
                                 uint32_t* screen_size, char* panel,
                                 uint32_t panel_capacity, uint32_t* panel_size,
                                 char* dialog, uint32_t dialog_capacity,
                                 uint32_t* dialog_size) {
    const char* values[] = {"loading", "", "{\"type\":\"none\",\"options\":[]}"};
    char* outputs[] = {screen, panel, dialog};
    const uint32_t capacities[] = {screen_capacity, panel_capacity, dialog_capacity};
    uint32_t* sizes[] = {screen_size, panel_size, dialog_size};
    for (size_t i = 0; i < 3; ++i) {
        if (outputs[i] == nullptr || sizes[i] == nullptr || capacities[i] <= std::strlen(values[i])) {
            return contracts::error(kQolErrInvalidArgument, "bad snapshot output", 19);
        }
        *sizes[i] = static_cast<uint32_t>(std::strlen(values[i]));
        std::memcpy(outputs[i], values[i], *sizes[i] + 1);
    }
    return contracts::ok();
}
const contracts::QolUiStateReadPort kUiStateReadPort{
    contracts::kQolPortUiStateReadVersion, {&fake_ui_state_snapshot}};
const contracts::QolItemMoneyWritePort kItemMoneyWritePort{{1, 0}, nullptr};
const contracts::QolItemDiceWritePort kItemDiceWritePort{{1, 1}, nullptr, nullptr};
const contracts::QolCharacterWriteOpsPort kCharacterWriteOpsPort{
    contracts::kQolPortCharacterWriteOpsVersion, nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, nullptr};
const contracts::QolMoveMergePort kMoveMergePort{contracts::kQolMoveMergePortVersion, nullptr,
                                                  nullptr, nullptr};

}  // namespace

const contracts::QolQuestPort& quest_port() { return kQuestPort; }
const contracts::QolWorldPort& world_port() { return kWorldPort; }
const contracts::QolItemPort& item_port() { return kItemPort; }
void test_set_item_port_version(::QolVersion version) { kItemPort.abi_version = version; }
const contracts::QolCharacterPort& character_port() { return kCharacterPort; }
const contracts::QolCharacterHttpPort& character_http_port() { return kCharacterHttpPort; }
const contracts::QolChoicePort& choice_port() { return kChoicePort; }
const contracts::QolWorldActionPort& world_action_port() {
    static const contracts::QolWorldActionPort kWorldActionPort{};
    return kWorldActionPort;
}
const contracts::QolExtensionBagWritePort& extension_bag_write_port() {
    return kExtensionBagWritePort;
}
const contracts::QolSavePort& save_port() { return kSavePort; }
const contracts::QolSaveCallPort& save_call_port() { return kSaveCallPort; }
const contracts::QolCraftPort& craft_port() { return kCraftPort; }
const contracts::QolCraftUiWritePort& craft_ui_write_port() {
    return craft_ui_write_port_override != nullptr ? *craft_ui_write_port_override
                                                   : kCraftUiWritePort;
}
const contracts::QolCustomRecipeWritePort& custom_recipe_write_port() {
    return custom_recipe_write_port_override != nullptr ? *custom_recipe_write_port_override
                                                        : kCustomRecipeWritePort;
}
const contracts::QolCharacterExtrasPort& character_extras_port() {
    return kCharacterExtrasPort;
}
const contracts::QolInventoryExtrasPort& inventory_extras_port() {
    return kInventoryExtrasPort;
}
const contracts::QolItemExtrasPort& item_extras_port() { return kItemExtrasPort; }
const contracts::QolSystemPort& system_port() { return kSystemPort; }
const contracts::QolSaveWritePort& save_write_port() { return kSaveWritePort; }
const contracts::QolSaveTransactionPort& save_transaction_port() { return kSaveTransactionPort; }
const contracts::QolWorldWritePort& world_write_port() { return kWorldWritePort; }
const contracts::QolChoiceWritePort& choice_write_port() { return kChoiceWritePort; }
const contracts::QolUiWritePort& ui_write_port() { return kUiWritePort; }
const contracts::QolUiStateReadPort& ui_state_read_port() { return kUiStateReadPort; }
const contracts::QolItemMoneyWritePort& item_money_write_port() { return kItemMoneyWritePort; }
const contracts::QolItemDiceWritePort& item_dice_write_port() { return kItemDiceWritePort; }
const contracts::QolCharacterWriteOpsPort& character_write_ops_port() {
    return kCharacterWriteOpsPort;
}
const contracts::QolMoveMergePort& move_merge_port() { return kMoveMergePort; }

}  // namespace qol::game

// 单 owner 生产装配 fixture 的端口注入点：默认空桩不变，只有显式调用的 target 生效。
namespace qol::gametest {

void set_craft_ui_write_port_for_test(const qol::contracts::QolCraftUiWritePort* port) {
    qol::game::craft_ui_write_port_override = port;
}

void set_custom_recipe_write_port_for_test(const qol::contracts::QolCustomRecipeWritePort* port) {
    qol::game::custom_recipe_write_port_override = port;
}

}  // namespace qol::gametest

namespace qol::host {
namespace {

const contracts::QolCharacterStatePort kCharacterStatePort{
    contracts::kQolPortCharacterStateVersion, {nullptr, nullptr, nullptr}};

}  // namespace

const contracts::QolCharacterStatePort& character_state_port() { return kCharacterStatePort; }

}  // namespace qol::host

namespace qol::host {
namespace {

const qol::contracts::QolItemInventoryStateWritePort kItemInventoryStateWritePort{
    qol::contracts::kQolPortItemInventoryStateWriteVersion, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr};

}  // namespace

const qol::contracts::QolItemInventoryStateWritePort& item_inventory_state_write_port() {
    return kItemInventoryStateWritePort;
}

}  // namespace qol::host

// T2h-B3#2：下列 target 链接 game/read/item_class.cpp，其 debug raw 原语引用全局
// game_in_world/op_err/inventory_item_ref_at；这些用例不触发该路径，stub 仅满足链接。
bool game_in_world() { return false; }
std::string op_err(const char* msg) {
    return std::string("{\"ok\":false,\"error\":\"") + msg + "\"}";
}
bool inventory_item_ref_at(int, int, InventoryItemRef*) { return false; }
