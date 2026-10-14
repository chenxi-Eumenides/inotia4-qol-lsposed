// custom_recipe UI 子层（原 craft_ui）：控件身份判定、typed command 非同步入队、
// 本地文本 scope 与文案查表。合并后结果/文案均走单 owner 内部接口，不经 capability。
#include "plugins/custom_recipe/ui/feature.h"

#include <cassert>
#include <cstdint>
#include <cstring>

#include "contracts/craft_commands.h"
#include "contracts/game_ports/craft_ui_write_port.h"
#include "contracts/plugin.h"
#include "contracts/ui.h"
#include "plugins/custom_recipe/result.h"
#include "plugins/custom_recipe/text_table.h"

namespace {

bool g_slot_current = true;
int g_identity_queries = 0;
uint32_t g_command_id = 0;
uint32_t g_command_size = 0;
int g_post_typed_calls = 0;
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
    ++g_post_typed_calls;
    g_command_id = handler;
    g_command_size = size;
    std::memcpy(&g_command, payload, sizeof(g_command));
    *out_ticket = 1;
    return qol::contracts::ok();
}

// 同步 UI 写面计数器：craft_write_stuff_need / craft_set_stuff_item 必须只入队，
// 不得在 UI 回调里同步调用任何 UI 写原语。
int g_sync_ui_writes = 0;
QolResult sync_show_text(uint32_t) {
    ++g_sync_ui_writes;
    return qol::contracts::ok();
}
QolResult sync_set_target_slot_item(qol::contracts::QolItemHandle) {
    ++g_sync_ui_writes;
    return qol::contracts::ok();
}

int g_push_calls = 0;
int g_pop_calls = 0;
int32_t g_last_pushed_scope = -1;
QolResult text_scope_push(int32_t scope) {
    ++g_push_calls;
    g_last_pushed_scope = scope;
    return qol::contracts::ok();
}
QolResult text_scope_pop() {
    ++g_pop_calls;
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
    sdk.text_scope.push = &text_scope_push;
    sdk.text_scope.pop = &text_scope_pop;

    qol::contracts::QolCraftUiWritePort write_port{};
    write_port.abi_version = qol::contracts::kQolPortCraftUiWriteVersion;
    write_port.show_text = &sync_show_text;
    write_port.set_target_slot_item = &sync_set_target_slot_item;

    qol::contracts::PluginContext context{};
    context.sdk = &sdk;
    qol::features::custom_recipe::ui::set_context(context);
    qol::features::custom_recipe::ui::set_write_port(&write_port);

    void* const transient_control = reinterpret_cast<void*>(0x55);
    assert(qol::features::custom_recipe::ui::is_stuff_ctrl(transient_control));
    assert(g_identity_queries == 1);
    g_slot_current = false;
    assert(!qol::features::custom_recipe::ui::is_stuff_ctrl(transient_control));
    assert(g_identity_queries == 2);

    // typed command 入队：只 post_typed，不同步调用写面（避免 UI 回调直接改游戏状态）。
    const qol::contracts::QolItemHandle item{31, 47};
    qol::features::custom_recipe::ui::craft_set_stuff_item(2, item);
    assert(g_command_id == qol::contracts::kCraftSetStuffItemHandlerId);
    assert(g_command_size == sizeof(qol::contracts::CraftSetStuffItemPayload));
    assert(g_command.index == 2 && g_command.item.slot == item.slot &&
           g_command.item.generation == item.generation);

    qol::features::custom_recipe::ui::craft_set_stuff_item(2, qol::contracts::kInvalidItemHandle);
    assert(g_command.index == 2 && g_command.item.slot == 0 && g_command.item.generation == 0);

    const int posts_before_need = g_post_typed_calls;
    qol::features::custom_recipe::ui::craft_write_stuff_need(0, 15, 3);
    assert(g_post_typed_calls == posts_before_need + 1);
    assert(g_command_id == qol::contracts::kCraftWriteStuffNeedHandlerId);
    assert(g_sync_ui_writes == 0);  // 非同步：UI 写原语未被直接调用

    // 本地文本 scope：绘制窗口 2/3/4 进 host scope；弹窗专用 scope(5/6) 只留本地。
    {
        qol::features::custom_recipe::ui::TextScopeGuard guard(2);
        assert(g_push_calls == 1 && g_last_pushed_scope == 2);
        assert(qol::features::custom_recipe::ui::current_text_scope() == 2);
        assert(qol::features::custom_recipe::ui::current_text_scope() !=
               qol::features::custom_recipe::kTextScopePageTab);
    }
    assert(g_pop_calls == 1);
    assert(qol::features::custom_recipe::ui::current_text_scope() == 0);

    {
        qol::features::custom_recipe::ui::TextScopeGuard guard(
            qol::features::custom_recipe::kTextScopeMaterialMayHaveBeenConsumed);
        assert(g_push_calls == 2 && g_last_pushed_scope == 0);  // 弹窗 scope 不进入 host
        assert(qol::features::custom_recipe::ui::current_text_scope() ==
               qol::features::custom_recipe::kTextScopeMaterialMayHaveBeenConsumed);
    }
    assert(g_pop_calls == 2);
    assert(qol::features::custom_recipe::ui::current_text_scope() == 0);

    // 文案查表：scope 与 id 同时命中才返回；页签 scope 不登记（不得改页签名）。
    using qol::features::custom_recipe::kTextScopeMaterialMayHaveBeenConsumed;
    using qol::features::custom_recipe::kTextScopePageTab;
    using qol::features::custom_recipe::kTextScopeQualityUnavailable;
    using qol::features::custom_recipe::kTextScopeRecipeButton;
    using qol::features::custom_recipe::text_lookup;
    assert(std::strcmp(text_lookup(kTextScopeRecipeButton, 35291), "宝石升阶") == 0);
    assert(text_lookup(kTextScopePageTab, 35291) == nullptr);
    assert(text_lookup(kTextScopeQualityUnavailable, 97) != nullptr);
    assert(text_lookup(kTextScopeRecipeButton, 97) == nullptr);
    assert(text_lookup(kTextScopeMaterialMayHaveBeenConsumed, 94) != nullptr);

    // 内部结果契约：11 个既有操作 + 追加的失败阶段枚举保持非零语义。
    using qol::features::custom_recipe::result::CraftExecuteResult;
    assert(static_cast<uint8_t>(CraftExecuteResult::kOk) == 0);
    assert(static_cast<uint8_t>(CraftExecuteResult::kPrecheckFailed) == 5);
    assert(static_cast<uint8_t>(CraftExecuteResult::kProductPrepareFailed) == 6);
    assert(static_cast<uint8_t>(CraftExecuteResult::kMaterialRemoveFailed) == 7);
    assert(static_cast<uint8_t>(CraftExecuteResult::kMaterialObservationFailed) == 8);
    assert(static_cast<uint8_t>(CraftExecuteResult::kProductSaveFailed) == 9);
    return 0;
}
