// custom_recipe 单 owner 的**真实 descriptor + production feature-mode 装配** fixture。
//
// 目标（L4）：用生产的 plugins/custom_recipe/plugin.cpp 描述符驱动真实 feature registry /
// 资源事务，验证：
//   · 单 descriptor：13 条资源（12 UI hook + ownership 最后）、2 个写端口、0 个 provides、
//     1 个可见 feature（custom_recipe.recipes，默认 false）；
//   · 5 条核心 hook 逐个安装失败 → 逆序回滚、注册净额归零、表未发布、业务门禁保持关闭；
//   · 7 条装饰 hook 逐个失败 → 核心继续、失败资源保持 Free（fail-soft）、不回滚整插件；
//   · ownership acquire 失败 → 原表不发布、门禁保持关闭；
//   · off/on：清理订阅与 typed handler、还原表、门禁关闭，重新启用可再次就绪。
//
// 该 fixture 不替代 L3 的通用机制用例（真实 registry 动态循环）；这里绑定的是产品描述符。

#include "host/features/reconciler.h"
#include "host/core_settings_config_service.h"
#include "host/owner_identity.h"
#include "host/plugin_registry.h"
#include "host/save_route.h"
#include "infra/capabilities.h"
#include "infra/status.h"
#include "infra/store.h"
#include "hooks/registry.h"
#include "hooks/transaction.h"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include "contracts/attribute_quality.h"
#include "contracts/craft_facts.h"
#include "contracts/craft_commands.h"
#include "contracts/game_host.h"
#include "contracts/game_ports/craft_ui_write_port.h"
#include "contracts/game_ports/custom_recipe_write_port.h"
#include "contracts/item_facts.h"
#include "contracts/plugin.h"
#include "contracts/text_layer.h"
#include "plugins/custom_recipe/feature.h"
#include "plugins/custom_recipe/plugin.h"
#include "plugins/custom_recipe/table.h"
#include "plugins/custom_recipe/text_table.h"
#include "plugins/custom_recipe/ui/feature.h"
#include "plugins/custom_recipe/ui/runtime.h"

namespace qol::host {
// 宿主运行时效果投影：fixture 只记录最近一次 actual。
std::string g_actual_plugin;
bool g_actual_value = false;
void runtime_effects_publish_actual(const char* plugin_id, bool enabled) {
    if (plugin_id != nullptr) {
        g_actual_plugin = plugin_id;
        g_actual_value = enabled;
    }
}
void runtime_effects_unsubscribe_plugin(const char*) {}

// test_game_port_stubs.cpp 的默认端口为空；本 fixture 注入「就绪」的生产形状端口。
int fixture_typed_registrations();
int fixture_typed_unregistrations();
int fixture_event_registrations();
int fixture_event_unregistrations();
}  // namespace qol::host

namespace qol::gametest {
// test_game_port_stubs.cpp 提供的端口注入点（未注入时保持空桩）。
void set_craft_ui_write_port_for_test(const qol::contracts::QolCraftUiWritePort* port);
void set_custom_recipe_write_port_for_test(const qol::contracts::QolCustomRecipeWritePort* port);
}  // namespace qol::gametest

namespace {

using namespace qol::contracts;
using qol::host::FeatureController;
using qol::host::FeatureReconciler;
using qol::host::HostDeps;

// ===========================================================================
// 表注入替身（host 上 table.cpp 只编译纯构造器）
// ===========================================================================
// typed acquire 替身状态（真实 descriptor/ownership 路径消费；不再折叠为 bool）。
// g_table_acquire_status 用 QolResult.ok 的数值表达：0=OK，否则为错误码。
int32_t g_table_acquire_ok = 0;                 // 0 = OK；其它 = 原样返回的错误码
bool g_table_ensure_result = true;              // 兼容旧断言：false 时返回 NotReady
int g_table_ensure_calls = 0;
int g_table_deactivate_calls = 0;
bool g_table_published = false;

// ===========================================================================
// UI 交互 / 槽位事实 fixture 状态（真实 wrapper 经 hook 记录替换指针后由测试调用）
// ===========================================================================
int32_t g_ui_show_text_id = -1;
int g_ui_show_text_calls = 0;
int g_ui_reset_calls = 0;
int g_ui_refresh_calls = 0;
int g_ui_select_calls = 0;
int g_ui_init_mixing_calls = 0;
int g_ui_play_sound_calls = 0;
int g_popup_calls = 0;
bool g_popup_fail = false;
// 写入/读取失败注入与产物写计数（供 UI 失败路径断言）。
bool g_set_stuff_item_fail = false;
int g_stuff_item_fail_index = -1;
int g_category_calls = 0;
int g_category_override_after = -1;  // >0：该次数之后返回不一致类别（模拟 facts 变化）
int g_category_fail_after = -1;      // >0：该次数之后类别读取报错
int g_create_calls = 0;
int g_save_calls = 0;
int g_random_calls = 0;
int g_word_write_calls = 0;
QolCraftPopupCallback g_popup_ok = nullptr;
QolCraftPopupCallback g_popup_cancel = nullptr;

// 记录每个已安装 entry hook 的 wrapper 替换指针（模拟游戏侧按目标调用 wrapper）。
void* g_wrapper_by_target[0x2010] = {};

struct FixtureItemFact {
    QolItemHandle handle;
    int32_t category;
    bool stackable;
};
constexpr FixtureItemFact kFixtureItems[] = {
    {{11, 11}, 35, true},
    {{12, 12}, 41, true},
    {{13, 13}, 5, true},
    {{14, 14}, 5, true},
    {{15, 15}, 16, true},
};

QolItemHandle g_slot_items[3] = {kInvalidItemHandle, kInvalidItemHandle, kInvalidItemHandle};
QolItemHandle g_selected_inventory_item = kInvalidItemHandle;
int64_t g_selected_stuff_index = 0;
int32_t g_craft_mix_type = 69;  // 目录第 0 条（kThreeSlotCraft）的 mixType
int32_t g_slot_category_held[8] = {5, 5, 5, 5, 5, 5, 5, 5};

const FixtureItemFact* find_fixture_item(QolItemHandle handle) {
    for (const FixtureItemFact& fact : kFixtureItems) {
        if (fact.handle.slot == handle.slot && fact.handle.generation == handle.generation) {
            return &fact;
        }
    }
    return nullptr;
}

// ===========================================================================
// 能力替身（provider 插件的 provides 目标）
// ===========================================================================
int g_text_subscribe_calls = 0;
int g_text_unsubscribe_calls = 0;
uint64_t g_last_text_consumer = 0;
const char* (*g_last_text_callback)(uint32_t, const char*, void*) = nullptr;

uint64_t fake_text_subscribe(int32_t, int32_t, const char* (*fn)(uint32_t, const char*, void*),
                             void*) {
    ++g_text_subscribe_calls;
    g_last_text_callback = fn;
    g_last_text_consumer = 900u + static_cast<uint64_t>(g_text_subscribe_calls);
    return g_last_text_consumer;
}
void fake_text_unsubscribe(uint64_t) { ++g_text_unsubscribe_calls; }
const TextLayerApi kTextLayerApi{&fake_text_subscribe, &fake_text_unsubscribe};

QolResult fake_category(QolItemHandle handle, int32_t* out) {
    if (out == nullptr) return qol::contracts::error(kQolErrInvalidArgument, nullptr, 0);
    ++g_category_calls;
    const FixtureItemFact* fact = find_fixture_item(handle);
    if (fact == nullptr) return qol::contracts::error(kQolErrNotFound, nullptr, 0);
    if (g_category_fail_after >= 0 && g_category_calls > g_category_fail_after) {
        return qol::contracts::error(kQolErrNotReady, "category unavailable", 20);
    }
    if (g_category_override_after >= 0 && g_category_calls > g_category_override_after) {
        *out = 999;  // 与调用方上下文不一致
        return qol::contracts::ok();
    }
    *out = fact->category;
    return qol::contracts::ok();
}
QolResult fake_is_real_equip(QolItemHandle handle, int32_t* out) {
    if (out == nullptr) return qol::contracts::error(kQolErrInvalidArgument, nullptr, 0);
    const FixtureItemFact* fact = find_fixture_item(handle);
    if (fact == nullptr) return qol::contracts::error(kQolErrNotFound, nullptr, 0);
    *out = fact->stackable ? 0 : 1;
    return qol::contracts::ok();
}
QolResult fake_is_jewel_category(int32_t, int32_t* out) {
    if (out == nullptr) return qol::contracts::error(kQolErrInvalidArgument, nullptr, 0);
    *out = 0;
    return qol::contracts::ok();
}
const ItemFactsApi kItemFactsApi{sizeof(ItemFactsApi), &fake_category, &fake_is_real_equip,
                                 &fake_is_jewel_category, nullptr, nullptr, nullptr, nullptr,
                                 nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};

QolResult fake_mix_type(uint32_t* out) {
    if (out == nullptr) return qol::contracts::error(kQolErrInvalidArgument, nullptr, 0);
    *out = static_cast<uint32_t>(g_craft_mix_type);
    return qol::contracts::ok();
}
QolResult fake_stuff_item(uint32_t index, QolItemHandle* out) {
    if (out == nullptr || index >= 3) return qol::contracts::error(kQolErrInvalidArgument,
                                                                   nullptr, 0);
    if (g_stuff_item_fail_index >= 0 && static_cast<int>(index) == g_stuff_item_fail_index) {
        return qol::contracts::error(kQolErrNotReady, "stuff read unavailable", 22);
    }
    // 控件存在但槽内无物品 = 正常空槽 → OK + 无效 handle（与生产 adapter 一致）。
    *out = g_slot_items[index];
    return qol::contracts::ok();
}
QolResult fake_category_held_count(int32_t category, int32_t* out) {
    if (out == nullptr) return qol::contracts::error(kQolErrInvalidArgument, nullptr, 0);
    *out = (category >= 0 && category < 8) ? g_slot_category_held[category] : 0;
    return qol::contracts::ok();
}
QolResult fake_character_level(int32_t* out) {
    if (out == nullptr) return qol::contracts::error(kQolErrInvalidArgument, nullptr, 0);
    *out = 50;
    return qol::contracts::ok();
}
QolResult fake_selected_index(int64_t* out) {
    if (out == nullptr) return qol::contracts::error(kQolErrInvalidArgument, nullptr, 0);
    *out = g_selected_stuff_index;
    return qol::contracts::ok();
}
QolResult fake_selected_inventory_item(QolItemHandle* out) {
    if (out == nullptr) return qol::contracts::error(kQolErrInvalidArgument, nullptr, 0);
    *out = g_selected_inventory_item;
    return qol::contracts::ok();
}
QolResult fake_selected_stuff_item(QolItemHandle* out) {
    if (out == nullptr) return qol::contracts::error(kQolErrInvalidArgument, nullptr, 0);
    *out = g_slot_items[0];
    return qol::contracts::ok();
}
QolResult fake_stuff_slot_available(uint32_t, int32_t* out) {
    if (out == nullptr) return qol::contracts::error(kQolErrInvalidArgument, nullptr, 0);
    *out = 1;
    return qol::contracts::ok();
}

CraftFactsApi make_craft_facts() {
    CraftFactsApi api{};
    api.read.mix_type = &fake_mix_type;
    api.read.selected_stuff_index = &fake_selected_index;
    api.read.selected_stuff_item = &fake_selected_stuff_item;
    api.read.stuff_item = &fake_stuff_item;
    api.read.category_held_count = &fake_category_held_count;
    api.read.character_level = &fake_character_level;
    api.read_only.mix_type = &fake_mix_type;
    api.read_only.selected_stuff_index = &fake_selected_index;
    api.read_only.selected_stuff_item = &fake_selected_stuff_item;
    api.read_only.stuff_item = &fake_stuff_item;
    api.read_only.category_held_count = &fake_category_held_count;
    api.read_only.character_level = &fake_character_level;
    api.read_only.stuff_slot_available = &fake_stuff_slot_available;
    api.read_only.selected_inventory_item = &fake_selected_inventory_item;
    return api;
}
const CraftFactsApi kCraftFactsApi = make_craft_facts();

QolResult fake_query_item(QolItemHandle, const AttributeQualityOptions*,
                          AttributeQualityItemResult*) {
    return qol::contracts::error(kQolErrNotReady, nullptr, 0);
}
QolResult fake_query_option(QolItemHandle, int32_t, int32_t, const AttributeQualityOptions*,
                            AttributeQualityItem*) {
    return qol::contracts::error(kQolErrNotReady, nullptr, 0);
}
QolResult fake_evaluate(int32_t, AttributeRange, const AttributeQualityOptions*,
                        AttributeQuality*) {
    return qol::contracts::error(kQolErrNotReady, nullptr, 0);
}
const AttributeQualityApi kAttributeQualityApi{
    static_cast<uint32_t>(sizeof(AttributeQualityApi)), &fake_query_item, &fake_query_option,
    &fake_evaluate};

// ===========================================================================
// 两个写端口的就绪形状（函数指针非空即可；本 fixture 不执行业务写入）
// ===========================================================================
QolResult stub_ok() { return qol::contracts::ok(); }
QolResult stub_ok_i32(int32_t*) { return qol::contracts::ok(); }
QolResult stub_ok_int64(int64_t) { return qol::contracts::ok(); }
QolResult stub_ok_pair(int32_t, int64_t) { return qol::contracts::ok(); }
QolResult stub_ok_handle(QolItemHandle) { return qol::contracts::ok(); }
// 独立 TEXTDATABASE 摘录（不引用 text_table.h 常量，避免自证）：
// 数字直接取自 TEXTDATABASE（表 index 73，record_size 2）：row = 弹窗索引，行内 u16 = MEMORYTEXT id。
// 工程 raw 与三份 supported 输入 APK 在 row 94/97/99 上一致（见本轮 .tmp 证据）。
struct FixtureTextDatabaseRow {
    uint32_t popup_index;
    uint32_t memory_text_id;
};
constexpr FixtureTextDatabaseRow kFixtureTextDatabase[] = {
    {94, 35410},
    {97, 35413},
    {99, 35415},
};

uint32_t fixture_memory_text_id(uint32_t popup_index) {
    for (const FixtureTextDatabaseRow& row : kFixtureTextDatabase) {
        if (row.popup_index == popup_index) return row.memory_text_id;
    }
    return 0;  // 未摘录的弹窗索引：不猜测
}

// 原生弹窗桩：模拟 UIPopupMsg_CreateOKFromTextData(index) 的两步链 ——
//   索引 → TEXTDATABASE 行内 u16 → MEMORYTEXT_GetText(id) → 生产文本消费者（本地 scope 内同步）。
// 因此捕获到的文案来自真实 dispatch/consumer 路径，而不是直接用消费者常量自证。
int32_t g_ui_show_memory_text_id = -1;
const char* g_last_shown_text = nullptr;
QolResult stub_ok_text(uint32_t popup_index) {
    ++g_ui_show_text_calls;
    g_ui_show_text_id = static_cast<int32_t>(popup_index);
    g_ui_show_memory_text_id = static_cast<int32_t>(fixture_memory_text_id(popup_index));
    g_last_shown_text = (g_ui_show_memory_text_id != 0 && g_last_text_callback != nullptr)
                            ? g_last_text_callback(static_cast<uint32_t>(g_ui_show_memory_text_id),
                                                   "orig", nullptr)
                            : nullptr;
    return qol::contracts::ok();
}
// 忠实契约：adapter 要求 ok/cancel 都非空，否则 InvalidArgument（弹窗不会创建）。
QolResult stub_ok_yesno(uint32_t, QolCraftPopupCallback ok, QolCraftPopupCallback cancel) {
    ++g_popup_calls;
    if (ok == nullptr || cancel == nullptr) {
        return qol::contracts::error(kQolErrInvalidArgument, "null popup callback", 19);
    }
    if (g_popup_fail) return qol::contracts::error(kQolErrNotReady, "popup unavailable", 17);
    g_popup_ok = ok;
    g_popup_cancel = cancel;
    return qol::contracts::ok();
}
QolResult stub_reset_stuff() {
    ++g_ui_reset_calls;
    return qol::contracts::ok();
}
QolResult stub_refresh_inventory() {
    ++g_ui_refresh_calls;
    return qol::contracts::ok();
}
QolResult stub_select_empty(int32_t* out) {
    ++g_ui_select_calls;
    if (out != nullptr) *out = 0;
    return qol::contracts::ok();
}
QolResult stub_init_mixing() {
    ++g_ui_init_mixing_calls;
    return qol::contracts::ok();
}
QolResult stub_play_sound(int32_t) {
    ++g_ui_play_sound_calls;
    return qol::contracts::ok();
}
int g_fixture_needs_writes = 0;
int g_fixture_set_item_writes = 0;
QolResult stub_needs(uint32_t, int32_t, int32_t) {
    ++g_fixture_needs_writes;
    return qol::contracts::ok();
}
QolResult stub_set_item(uint32_t index, QolItemHandle) {
    ++g_fixture_set_item_writes;
    if (g_set_stuff_item_fail && static_cast<int>(index) >= 0) {
        return qol::contracts::error(kQolErrBlocked, "stuff write blocked", 19);
    }
    return qol::contracts::ok();
}
QolResult stub_create(int32_t, QolItemHandle* out) {
    ++g_create_calls;
    if (out != nullptr) *out = qol::contracts::kInvalidItemHandle;
    return qol::contracts::ok();
}
QolResult stub_create4(int32_t, int32_t, int32_t, int32_t, QolItemHandle* out) {
    ++g_create_calls;
    if (out != nullptr) *out = qol::contracts::kInvalidItemHandle;
    return qol::contracts::ok();
}
QolResult stub_save(QolItemHandle, int32_t* out_saved) {
    ++g_save_calls;
    if (out_saved != nullptr) *out_saved = 1;
    return qol::contracts::ok();
}
QolResult stub_word_read(QolItemHandle, uint32_t* out) {
    if (out == nullptr) return qol::contracts::error(kQolErrInvalidArgument, nullptr, 0);
    *out = 0;
    return qol::contracts::ok();
}
QolResult stub_word_write(QolItemHandle, uint32_t) {
    ++g_word_write_calls;
    return qol::contracts::ok();
}
QolResult stub_random(int32_t lo, int32_t, int32_t* out) {
    ++g_random_calls;
    if (out == nullptr) return qol::contracts::error(kQolErrInvalidArgument, nullptr, 0);
    *out = lo;
    return qol::contracts::ok();
}

QolCraftUiWritePort make_ui_write_port() {
    QolCraftUiWritePort port{};
    port.abi_version = kQolPortCraftUiWriteVersion;
    port.init_mixing_state = &stub_init_mixing;
    port.reset_stuff_item_control = &stub_reset_stuff;
    port.refresh_inventory_items = &stub_refresh_inventory;
    port.select_first_empty_stuff_slot = &stub_select_empty;
    port.set_ui_type = &stub_ok_int64;
    port.set_selected_recipe = &stub_ok_pair;
    port.play_sound = &stub_play_sound;
    port.show_text = &stub_ok_text;
    port.show_yesno_from_text = &stub_ok_yesno;
    port.set_target_slot_item = &stub_ok_handle;
    port.set_cost_zero = &stub_ok;
    return port;
}



QolCustomRecipeWritePort g_recipe_port{};
QolCraftUiWritePort g_ui_port{};

void install_ready_ports() {
    g_recipe_port = QolCustomRecipeWritePort{};
    g_recipe_port.abi_version = kQolPortCustomRecipeWriteVersion;
    g_recipe_port.write_stuff_need = &stub_needs;
    g_recipe_port.set_stuff_item = &stub_set_item;
    g_recipe_port.description_set_off = &stub_ok;
    g_recipe_port.create_perfect_item = &stub_create;
    g_recipe_port.create_item = &stub_create4;
    g_recipe_port.save_item = &stub_save;
    g_recipe_port.free_item = &stub_ok_handle;
    g_recipe_port.read_jewel_word = &stub_word_read;
    g_recipe_port.write_jewel_word = &stub_word_write;
    g_recipe_port.set_max_socket_enchant = &stub_ok_handle;
    g_recipe_port.random_inclusive = &stub_random;

    g_ui_port = make_ui_write_port();
    qol::gametest::set_custom_recipe_write_port_for_test(&g_recipe_port);
    qol::gametest::set_craft_ui_write_port_for_test(&g_ui_port);
}

// ===========================================================================
// 资源安装注入：core / optional 符号地址与失败集合
// ===========================================================================
constexpr const char* kCoreSymbols[] = {
    "F_UIMIX_BUTTON_INVEN_ITEM_SELECT_EXE_VMA",
    "F_UIMIX_BUTTON_MIXING_EXE_VMA",
    "F_MAKE_MIX_VMA",
    "F_UIMIX_BUTTON_MENU_LIST_EXE_VMA",
    "F_UIMIX_BUTTON_RECIPE_EXE_VMA",
};
constexpr const char* kOptionalSymbols[] = {
    "F_XTEXTCTRL_SET_TEXT_CONTROL_VMA",
    "F_CONTROL_ITEM_DRAW_VMA",
    "F_ITEM_DRAW_PORTING_VMA",
    "F_UIMIX_BUTTON_RECIPE_DRAW_VMA",
    "F_UIMIX_DRAW_VMA",
    "F_UIMIX_BUTTON_MENU_LIST_DRAW_VMA",
    "F_UIMIX_RESET_STUFF_ITEM_CONTROL_VMA",
};
constexpr int kCoreSymbolCount = 5;
constexpr int kOptionalSymbolCount = 7;

const char* g_fail_symbol = nullptr;
int g_hook_calls = 0;
int g_unhook_calls = 0;

// 宿主机制分派替身：production 由 host/assembly.cpp 注入 dispatch_install/dispatch_release；
// fixture 只覆盖 custom_recipe 用到的 kOwnership（ownership_ops acquire/release）。
qol::hooks::TxDeps::MechanismOutcome fixture_install_other(const ResourceClaim& claim,
                                                           uintptr_t) {
    if (claim.mechanism != ResourceMechanism::kOwnership) {
        return {1, "mechanism unsupported"};
    }
    const OwnershipOps* ops = ownership_ops(claim.handler);
    if (ops == nullptr || ops->acquire == nullptr) return {1, "ownership ops missing"};
    // 与 Host 一致：保留 acquire 的原始失败码（tx 软失败步据此区分 NotReady 与永久错误）。
    const int32_t code = ownership_acquire(ops, claim.id);
    if (code != 0) return {code, "ownership acquire failed"};
    return {0, nullptr};
}

qol::hooks::TxDeps::MechanismOutcome fixture_release_other(const ResourceClaim& claim,
                                                           uintptr_t) {
    if (claim.mechanism != ResourceMechanism::kOwnership) {
        return {1, "mechanism unsupported"};
    }
    ownership_release(ownership_ops(claim.handler), claim.id);
    return {0, nullptr};
}

uintptr_t resolve_symbol(const char* symbol, bool* out_followed) {
    if (out_followed != nullptr) *out_followed = false;
    if (symbol == nullptr) return 0;
    for (int i = 0; i < kCoreSymbolCount; ++i) {
        if (std::strcmp(symbol, kCoreSymbols[i]) == 0) {
            return 0x1000 + static_cast<uintptr_t>(i);
        }
    }
    for (int i = 0; i < kOptionalSymbolCount; ++i) {
        if (std::strcmp(symbol, kOptionalSymbols[i]) == 0) {
            return 0x2000 + static_cast<uintptr_t>(i);
        }
    }
    return 0x4000;  // 未知符号（fixture 不依赖）
}

int hook_symbol(void* target, void* replacement, void** backup) {
    const uintptr_t address = reinterpret_cast<uintptr_t>(target);
    if (address > 0 && address < 0x2010) {
        g_wrapper_by_target[address] = replacement;
    }
    ++g_hook_calls;
    for (int i = 0; i < kCoreSymbolCount; ++i) {
        if (reinterpret_cast<uintptr_t>(target) == 0x1000 + static_cast<uintptr_t>(i) &&
            g_fail_symbol != nullptr && std::strcmp(g_fail_symbol, kCoreSymbols[i]) == 0) {
            return 1;
        }
    }
    for (int i = 0; i < kOptionalSymbolCount; ++i) {
        if (reinterpret_cast<uintptr_t>(target) == 0x2000 + static_cast<uintptr_t>(i) &&
            g_fail_symbol != nullptr && std::strcmp(g_fail_symbol, kOptionalSymbols[i]) == 0) {
            return 1;
        }
    }
    if (backup != nullptr) *backup = reinterpret_cast<void*>(0xB000 + g_hook_calls);
    return 0;
}
int unhook_symbol(void* target) {
    (void)target;
    ++g_unhook_calls;
    return 0;
}
bool gate_fn(const char*) { return true; }

// ===========================================================================
// provider 插件（4 个）
// ===========================================================================
int provider_install(PluginContext&) { return 0; }
void provider_noop(PluginContext&) {}
// Toggle feature 必须带 on_toggle（Host 的 feature 校验规则）。
QolResult provider_toggle(PluginContext&, uint8_t) { return qol::contracts::ok(); }

const CapabilityProvisionRef kItemFactsProvides[] = {
    {kItemFactsCapabilityId, {kItemFactsVersion.major, kItemFactsVersion.minor}, &kItemFactsApi}};
const CapabilityProvisionRef kCraftFactsProvides[] = {
    {kCraftFactsCapabilityId, {kCraftFactsVersion.major, kCraftFactsVersion.minor}, &kCraftFactsApi}};
const CapabilityProvisionRef kTextLayerProvides[] = {
    {kTextLayerCapabilityId,
     {static_cast<uint16_t>(kTextLayerVersionMajor), static_cast<uint16_t>(kTextLayerVersionMinor)},
     &kTextLayerApi}};
const CapabilityProvisionRef kAttributeQualityProvides[] = {
    {kAttributeQualityCapabilityId,
     {kAttributeQualityVersionMajor, kAttributeQualityVersionMinor}, &kAttributeQualityApi}};

// 零 feature 插件被禁止注册：每个 provider 声明一个 Hidden/默认开启的常驻 feature。
const FeatureDefinition kItemFactsFeatures[] = {
    {"item_facts.provider", 1, FeatureCategory::kHidden, FeatureType::kToggle, "item.facts", "",
     &provider_toggle, nullptr}};
const FeatureDefinition kCraftFactsFeatures[] = {
    {"craft_facts.provider", 1, FeatureCategory::kHidden, FeatureType::kToggle, "craft.facts", "",
     &provider_toggle, nullptr}};
const FeatureDefinition kTextLayerFeatures[] = {
    {"text_layer.provider", 1, FeatureCategory::kHidden, FeatureType::kToggle, "text_layer", "",
     &provider_toggle, nullptr}};
const FeatureDefinition kAttributeQualityFeatures[] = {
    {"attribute_quality.provider", 1, FeatureCategory::kHidden, FeatureType::kToggle,
     "attribute.quality", "", &provider_toggle, nullptr}};

PluginDescriptor provider_descriptor(const char* id, const CapabilityProvisionRef* provides,
                                     const FeatureDefinition* features) {
    PluginDescriptor descriptor{};
    descriptor.id = id;
    descriptor.version = "0.1.0";
    descriptor.install = &provider_install;
    descriptor.deactivate = &provider_noop;
    descriptor.on_enabled = &provider_noop;
    descriptor.on_disabled = &provider_noop;
    descriptor.provides = provides;
    descriptor.provides_count = 1;
    descriptor.features = features;
    descriptor.feature_count = 1;
    return descriptor;
}

// ===========================================================================
// fixture 生命周期
// ===========================================================================
FeatureController* g_controller = nullptr;
FeatureReconciler* g_reconciler = nullptr;
std::thread::id g_main_thread;

void frames(int count = 24) {
    for (int i = 0; i < count; ++i) g_reconciler->step();
}

FeatureReceipt await_receipt(FeatureReceipt receipt) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    while (std::chrono::steady_clock::now() < deadline) {
        assert(succeeded(g_controller->receipt(receipt.request_id, &receipt)));
        if (receipt.persistence != FeaturePersistence::kPending) return receipt;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::fprintf(stderr, "receipt timed out\n");
    std::abort();
}

void set_recipes(bool value) {
    FeatureReceipt receipt{};
    assert(succeeded(
        g_controller->request_set_feature("custom_recipe", "custom_recipe.recipes",
                                          value ? 1 : 0, &receipt)));
    receipt = await_receipt(receipt);
    assert(receipt.persistence == FeaturePersistence::kPersisted);
}

int resource_state_code(const char* id) { return static_cast<int>(qol::hooks::state_of(id)); }
bool resource_free(const char* id) {
    return qol::hooks::state_of(id) == ResourceState::kFree;
}
bool resource_live(const char* id) {
    return qol::hooks::state_of(id) == ResourceState::kLive;
}
// 13 条资源 id（与 ui/runtime.cpp / plugin.cpp 声明一致）。
constexpr const char* kResourceIds[] = {
    "hook:UIMix_ButtonInvenItemSelectExe",
    "hook:UIMix_ButtonMixingExe",
    "hook:MIXSYSTEM_MakeItem",
    "hook:UIMix_ButtonMenuListExe",
    "hook:UIMix_ButtonRecipeExe",
    "hook:X_TEXTCTRL_SetTextControl",
    "hook:ControlItem_Draw",
    "hook:ITEM_DrawPorting",
    "hook:UIMix_ButtonRecipeDraw",
    "hook:UIMix_Draw",
    "hook:UIMix_ButtonMenuListDraw",
    "hook:UIMix_ResetStuffItemControl",
    "own:RECIPEBASE_MIXTUREBASE",
};

void reset_gate_and_table() {
    g_table_ensure_result = true;
    g_table_acquire_ok = 0;
    g_table_published = false;
    g_fail_symbol = nullptr;
}

void assert_all_resources_free() {
    for (const char* id : kResourceIds) {
        assert(resource_free(id));
    }
}

void assert_core_resources_live() {
    for (int i = 0; i < kCoreSymbolCount; ++i) {
        assert(resource_live(kResourceIds[i]));
    }
}

void assert_no_public_craft_result_capability() {
    assert(qol::infra::require_capability("craft_result", {1, 1}) == nullptr);
}

void assert_registrations_net_zero() {
    assert(qol::host::fixture_typed_registrations() == qol::host::fixture_typed_unregistrations());
    assert(qol::host::fixture_event_registrations() == qol::host::fixture_event_unregistrations());
    assert(g_text_subscribe_calls == g_text_unsubscribe_calls);
}

// ---------------------------------------------------------------------------
// UI 确认链路（真实 wrapper 经 hook 记录后由测试调用；根因回归）
// ---------------------------------------------------------------------------
void seed_fixed_recipe_slots() {
    // 静态配方 {35, 0, 41}：皮革 + 空 + 魔法衣料 → 背包（大）。
    g_slot_items[0] = QolItemHandle{11, 11};
    g_slot_items[1] = qol::contracts::kInvalidItemHandle;
    g_slot_items[2] = QolItemHandle{12, 12};
    g_slot_category_held[35] = 5;
    g_slot_category_held[41] = 5;
    g_craft_mix_type = 69;
    g_popup_ok = nullptr;
    g_popup_cancel = nullptr;
    g_popup_fail = false;
    g_popup_calls = 0;
    g_ui_show_text_calls = 0;
    g_ui_show_text_id = -1;
    g_ui_show_memory_text_id = -1;
    g_last_shown_text = nullptr;
    g_ui_reset_calls = 0;
    g_ui_refresh_calls = 0;
    g_ui_select_calls = 0;
    g_fixture_needs_writes = 0;
    g_fixture_set_item_writes = 0;
    g_create_calls = 0;
    g_save_calls = 0;
    g_random_calls = 0;
    g_word_write_calls = 0;
    g_set_stuff_item_fail = false;
    g_stuff_item_fail_index = -1;
    g_category_calls = 0;
    g_category_override_after = -1;
    g_category_fail_after = -1;
    g_selected_inventory_item = QolItemHandle{13, 13};  // 类别 5（可堆叠）
    g_selected_stuff_index = 0;
}

constexpr const char* kMsgRecipeMismatch = "配方不匹配，请检查材料组合。";
constexpr const char* kMsgCraftUnavailable = "合成暂不可用，材料未扣。";
constexpr const char* kMsgStateChanged = "合成状态已变化，请重新填入材料。";
constexpr const char* kMsgPlaceWriteFailed = "放入失败，请重试。";

// 提示 94 时断言实际文案（nullptr = 未被本地 scope 替换，保留原「材料不足」原文）。
void expect_prompt_94(const char* expected) {
    assert(g_ui_show_text_calls == 1);
    assert(g_ui_show_text_id == 94);                                  // 弹窗索引域
    assert(g_ui_show_memory_text_id == 35410);                        // 经 TEXTDATABASE row 94 解析
    if (expected == nullptr) {
        assert(g_last_shown_text == nullptr);                          // 未登记 scope → 保留原文
    } else {
        assert(g_last_shown_text != nullptr && std::strcmp(g_last_shown_text, expected) == 0);
    }
}

// ---------------------------------------------------------------------------
// 弹窗索引域 vs MEMORYTEXT 域：域分工与「不匹配」负测（本轮根因回归）
// ---------------------------------------------------------------------------
void test_popup_text_id_domain() {
    using qol::features::custom_recipe::kMemoryTextAlreadyPlaced;
    using qol::features::custom_recipe::kMemoryTextMaterialInsufficient;
    using qol::features::custom_recipe::kMemoryTextOnlyJewel;
    using qol::features::custom_recipe::kPopupIndexMaterialInsufficient;
    using qol::features::custom_recipe::kTextScopeRecipeMismatch;
    using qol::features::custom_recipe::ui::TextScopeGuard;

    // 独立摘录（fixture 内字面量）与生产表常量交叉核对：两处独立来源必须一致。
    assert(fixture_memory_text_id(94) == kMemoryTextMaterialInsufficient);
    assert(fixture_memory_text_id(97) == kMemoryTextOnlyJewel);
    assert(fixture_memory_text_id(99) == kMemoryTextAlreadyPlaced);
    assert(fixture_memory_text_id(98) == 0);  // 未摘录索引不猜测

    assert(g_last_text_callback != nullptr);
    {
        TextScopeGuard scope(kTextScopeRecipeMismatch);
        // 弹窗索引不是消费者匹配键：索引 94 在 scope 8 内必须落空（修前会误改其它弹窗）。
        assert(g_last_text_callback(kPopupIndexMaterialInsufficient, "orig", nullptr) == nullptr);
        // MEMORYTEXT 域才命中同一 scope 的替换文案。
        const char* const resolved =
            g_last_text_callback(kMemoryTextMaterialInsufficient, "orig", nullptr);
        assert(resolved != nullptr && std::strcmp(resolved, kMsgRecipeMismatch) == 0);
    }
    // scope 退出后恢复原文（本地 scope 守卫语义）。
    assert(g_last_text_callback(kMemoryTextMaterialInsufficient, "orig", nullptr) == nullptr);
}

void test_ui_confirm_paths() {
    assert(qol::features::custom_recipe::enabled());
    void* const mixing = g_wrapper_by_target[0x1001];
    void* const place = g_wrapper_by_target[0x1000];
    assert(mixing != nullptr && place != nullptr);

    // 1) 有效配方：prepare 命中 → 弹窗必须收到两个非空回调（原根因：cancel=nullptr 被 adapter 拒绝）。
    seed_fixed_recipe_slots();
    reinterpret_cast<void (*)(void*)>(mixing)(nullptr);
    assert(g_popup_calls == 1);
    assert(g_popup_ok != nullptr && g_popup_cancel != nullptr);
    assert(g_ui_show_text_calls == 0 && g_ui_reset_calls == 0);

    // 2) 取消：零扣料/零产物/零随机/零保存，且不清理已填格。
    g_popup_cancel();
    assert(g_ui_reset_calls == 0 && g_ui_refresh_calls == 0);
    assert(g_fixture_needs_writes == 0 && g_fixture_set_item_writes == 0);
    assert(g_create_calls == 0 && g_save_calls == 0 && g_random_calls == 0);

    // 取消后旧确认已作废：再触发旧 ok 不得进入产出（提示状态已变化 + 现有清格行为）。
    g_popup_ok();
    assert(g_create_calls == 0 && g_save_calls == 0 && g_random_calls == 0);
    expect_prompt_94(kMsgStateChanged);
    assert(g_ui_reset_calls == 1);

    // 3) 重新点击 → 新 token 的弹窗；ok 到达结果层（fixture 无 remove 端口 → 前置失败），
    //    不静默、不伪成功、零材料/产物写、保留已填格。
    seed_fixed_recipe_slots();
    reinterpret_cast<void (*)(void*)>(mixing)(nullptr);
    assert(g_popup_calls == 1 && g_popup_ok != nullptr && g_popup_cancel != nullptr);
    g_popup_ok();
    assert(g_create_calls == 0 && g_save_calls == 0 && g_random_calls == 0);
    assert(g_fixture_needs_writes == 0 && g_fixture_set_item_writes == 0);
    expect_prompt_94(kMsgCraftUnavailable);  // 合成暂不可用（材料未扣）
    assert(g_ui_reset_calls == 0);

    // 4) 弹窗创建失败：明确提示 + 作废 pending + 不清理已有材料（不再只有日志）。
    seed_fixed_recipe_slots();
    g_popup_fail = true;
    reinterpret_cast<void (*)(void*)>(mixing)(nullptr);
    assert(g_popup_calls == 1 && g_popup_ok == nullptr && g_popup_cancel == nullptr);
    expect_prompt_94(kMsgCraftUnavailable);
    assert(g_ui_reset_calls == 0 && g_ui_refresh_calls == 0);
    assert(g_fixture_needs_writes == 0 && g_create_calls == 0 && g_save_calls == 0 &&
           g_random_calls == 0);

    // 5) 真正未命中配方 → 「配方不匹配」提示（不是材料不足）。
    seed_fixed_recipe_slots();
    g_slot_items[1] = QolItemHandle{13, 13};  // {35, 5, 41} 不在任何配方
    reinterpret_cast<void (*)(void*)>(mixing)(nullptr);
    assert(g_popup_calls == 0);
    expect_prompt_94(kMsgRecipeMismatch);
    assert(g_ui_reset_calls == 1);
    // 与「材料不足」「暂不可用」文案必须不同（各自独立 scope 覆盖同一 popup id）。
    {
        using qol::features::custom_recipe::kTextScopeCraftUnavailable;
        using qol::features::custom_recipe::kTextScopeMaterialMayHaveBeenConsumed;
        using qol::features::custom_recipe::kTextScopeRecipeMismatch;
        using qol::features::custom_recipe::kTextScopeStateChanged;
        using qol::features::custom_recipe::text_lookup;
        // 消费者匹配键 = MEMORYTEXT id（35410，TEXTDATABASE row 94），不是弹窗索引 94。
        constexpr uint32_t kMemText = qol::features::custom_recipe::kMemoryTextMaterialInsufficient;
        const char* const mismatch = text_lookup(kTextScopeRecipeMismatch, kMemText);
        const char* const unavailable = text_lookup(kTextScopeCraftUnavailable, kMemText);
        const char* const consumed = text_lookup(kTextScopeMaterialMayHaveBeenConsumed, kMemText);
        const char* const changed = text_lookup(kTextScopeStateChanged, kMemText);
        assert(mismatch != nullptr && unavailable != nullptr && consumed != nullptr &&
               changed != nullptr);
        assert(std::strcmp(mismatch, unavailable) != 0 && std::strcmp(mismatch, consumed) != 0 &&
               std::strcmp(unavailable, consumed) != 0 && std::strcmp(changed, consumed) != 0);
    }

    // 6a) 槽位读取失败：UI 读取即失败 → 不进入配方判定 → 「合成暂不可用，材料未扣」，
    //     不弹窗、不清格（不得把读取失败伪装成空槽 → 配方不匹配）。
    seed_fixed_recipe_slots();
    g_stuff_item_fail_index = 2;
    reinterpret_cast<void (*)(void*)>(mixing)(nullptr);
    assert(g_popup_calls == 0);
    expect_prompt_94(kMsgCraftUnavailable);
    assert(g_ui_reset_calls == 0 && g_ui_refresh_calls == 0);

    // 6b) 类别读取失败（facts 缺失/报错）同样 → 「合成暂不可用」，不伪装空槽、不清格。
    seed_fixed_recipe_slots();
    g_category_fail_after = 2;  // UI 读到第 3 次（prepare 复核）时失败
    reinterpret_cast<void (*)(void*)>(mixing)(nullptr);
    assert(g_popup_calls == 0);
    expect_prompt_94(kMsgCraftUnavailable);
    assert(g_ui_reset_calls == 0);

    // 6b2) 半无效 handle（slot=0/generation!=0 或 slot!=0/generation=0）→ 畸形身份，
    //      明确「合成暂不可用」，不得当作空槽。
    seed_fixed_recipe_slots();
    g_slot_items[1] = QolItemHandle{0, 7};
    reinterpret_cast<void (*)(void*)>(mixing)(nullptr);
    assert(g_popup_calls == 0);
    expect_prompt_94(kMsgCraftUnavailable);
    assert(g_ui_reset_calls == 0);

    seed_fixed_recipe_slots();
    g_slot_items[2] = QolItemHandle{7, 0};
    reinterpret_cast<void (*)(void*)>(mixing)(nullptr);
    assert(g_popup_calls == 0);
    expect_prompt_94(kMsgCraftUnavailable);
    assert(g_ui_reset_calls == 0);

    // 6c) 真空槽保护：合法空槽 + 未命中配方 → 仍为「配方不匹配」并清格重来。
    seed_fixed_recipe_slots();
    g_slot_items[2] = qol::contracts::kInvalidItemHandle;  // {35, 0, 0} 合法空槽但未命中
    reinterpret_cast<void (*)(void*)>(mixing)(nullptr);
    assert(g_popup_calls == 0);
    expect_prompt_94(kMsgRecipeMismatch);
    assert(g_ui_reset_calls == 1);

    // 6d) 确认回调读取失败：不 execute（零扣料/产物），提示「材料未扣」，作废 pending。
    seed_fixed_recipe_slots();
    reinterpret_cast<void (*)(void*)>(mixing)(nullptr);
    assert(g_popup_calls == 1 && g_popup_ok != nullptr);
    g_stuff_item_fail_index = 0;  // 确认回调里读取失败
    g_ui_show_text_calls = 0;
    g_last_shown_text = nullptr;
    g_popup_ok();
    assert(g_create_calls == 0 && g_save_calls == 0 && g_random_calls == 0);
    assert(g_fixture_needs_writes == 0 && g_fixture_set_item_writes == 0);
    expect_prompt_94(kMsgCraftUnavailable);
    assert(g_ui_reset_calls == 0);

    // 7) 放料写入失败 → 准确写入失败提示（popup id 99），保留已填格。
    seed_fixed_recipe_slots();
    g_set_stuff_item_fail = true;
    reinterpret_cast<void (*)(void*)>(place)(nullptr);
    assert(g_fixture_set_item_writes == 1);
    assert(g_ui_show_text_calls == 1 && g_ui_show_text_id == 99);
    assert(g_ui_show_memory_text_id == 35415);  // TEXTDATABASE row 99
    assert(g_last_shown_text != nullptr && std::strcmp(g_last_shown_text, kMsgPlaceWriteFailed) == 0);
    assert(g_ui_reset_calls == 0);

    // 8) 仅 2 药占 3 格（同类可堆叠）→ 真实「材料不足」（94），不是配方不匹配。
    seed_fixed_recipe_slots();
    g_craft_mix_type = 69;
    g_selected_inventory_item = QolItemHandle{13, 13};  // 类别 5
    g_slot_items[0] = QolItemHandle{13, 13};
    g_slot_items[1] = QolItemHandle{13, 13};
    g_slot_items[2] = qol::contracts::kInvalidItemHandle;
    g_selected_stuff_index = 2;   // 目标格 = 第 3 格（0/1 已各占一格）
    g_slot_category_held[5] = 2;  // 持有 2 个，已占 2 格 → 第三次放料必须被拒
    g_fixture_set_item_writes = 0;
    g_ui_show_text_calls = 0;
    reinterpret_cast<void (*)(void*)>(place)(nullptr);
    expect_prompt_94(nullptr);  // 未被本地 scope 替换 → 保留原「材料不足」原文
    assert(g_fixture_set_item_writes == 0);
    g_slot_category_held[5] = 5;
}

// ---------------------------------------------------------------------------
// 跨 lane：真实 feature-mode 冷启动 → ownership NotReady 终态 → SaveEnter 主帧单 claim 恢复
// 生产链（registry 状态机 + 真实 descriptor + 真实 plugin.on_resource_ready + typed table 替身）。
// 不用 stub 计数替代 registry 状态：同时核对 hooks 资源态、registry active 与 infra status。
// ---------------------------------------------------------------------------
const char* kOwnershipClaimId = "own:RECIPEBASE_MIXTUREBASE";

bool registry_state(const char* id, qol::infra::FeatureStatus* out) {
    return qol::infra::status_get(id, out);
}

// 以「table 先 NotReady」重新激活，得到冷启动等价终态：owner Active、ownership Free、门禁关。
void arm_startup_retry_terminal() {
    set_recipes(false);
    frames();
    g_table_acquire_ok = kQolErrNotReady;
    set_recipes(true);
    frames();
}

void test_cross_lane_startup_retry() {
    const qol::contracts::PluginDescriptor& descriptor =
        qol::features::custom_recipe::plugin_descriptor();
    assert(descriptor.resources_count == 13);

    // ---- 1) 冷启动等价：ownership acquire 返回 NotReady → owner Active 但 ownership Free。----
    set_recipes(false);
    frames();
    g_table_acquire_ok = kQolErrNotReady;
    const int acquire_before = g_table_ensure_calls;

    set_recipes(true);
    frames();
    // 装配完成后的基线（12 hook 已装、text/handler 已注册）：恢复期不得再动这些。
    const int hooks_after_install = g_hook_calls;
    const int typed_reg_after_install = qol::host::fixture_typed_registrations();
    const int typed_unreg_after_install = qol::host::fixture_typed_unregistrations();
    const int text_sub_after_install = g_text_subscribe_calls;
    const int event_reg_after_install = qol::host::fixture_event_registrations();

    assert(qol::host::feature_registry_active("custom_recipe"));       // registry：owner Active
    qol::infra::FeatureStatus status{};
    assert(registry_state("custom_recipe", &status));
    assert(status.state == qol::infra::FeatureState::kEnabled && status.enabled);
    assert(qol::hooks::state_of(kOwnershipClaimId) == ResourceState::kFree);  // 真实资源态
    for (int i = 0; i < kCoreSymbolCount; ++i) assert(resource_live(kResourceIds[i]));
    assert(!qol::features::custom_recipe::ownership_ready());
    assert(!qol::features::custom_recipe::enabled());                 // 业务门禁保持关闭
    assert(g_table_ensure_calls == acquire_before + 1);              // 只尝试一次
    assert(g_hook_calls == hooks_after_install);                     // 12 hook 安装次数不增
    assert(qol::host::fixture_typed_registrations() == typed_reg_after_install);  // 未二次 install
    assert(qol::host::fixture_typed_unregistrations() == typed_unreg_after_install);
    assert(g_text_subscribe_calls == text_sub_after_install);        // text consumer 未重复注册
    assert(qol::host::fixture_event_registrations() == event_reg_after_install);

    // ---- 2) 多帧无 Save 信号：不重复 acquire。----
    const int armed_acquires = g_table_ensure_calls;
    frames(30);
    assert(g_table_ensure_calls == armed_acquires);
    assert(qol::hooks::state_of(kOwnershipClaimId) == ResourceState::kFree);

    // ---- 3) 表可用 + SaveEnter 信号 → registry 帧推进后单 claim 恢复。----
    g_table_acquire_ok = 0;
    qol::host::feature_registry_notify_save_ready();
    frames();                                                    // feature registry 帧推进
    assert(qol::host::feature_registry_retry_pending_ownership() == 1);
    assert(g_table_ensure_calls == armed_acquires + 1);           // 真实 own_acquire 恰好一次
    assert(qol::hooks::state_of(kOwnershipClaimId) == ResourceState::kLive);
    assert(registry_state("custom_recipe", &status));
    assert(status.state == qol::infra::FeatureState::kEnabled && status.enabled);
    assert(qol::features::custom_recipe::ownership_ready());
    assert(qol::features::custom_recipe::enabled());              // 真实 on_resource_ready 开门禁
    assert(g_hook_calls == hooks_after_install);                  // 12 hook 安装次数不增
    assert(qol::host::fixture_typed_registrations() == typed_reg_after_install);  // 不重复注册
    assert(g_text_subscribe_calls == text_sub_after_install);
    assert(qol::host::fixture_event_registrations() == event_reg_after_install);
    assert(qol::features::custom_recipe::ui::core_backups_ready());

    // ---- 4) 每信号一次：重复帧 / 重复调用不再 acquire。----
    const int after_recovery = g_table_ensure_calls;
    frames(20);
    assert(qol::host::feature_registry_retry_pending_ownership() == 0);
    qol::host::feature_registry_notify_save_ready();
    frames();
    assert(qol::host::feature_registry_retry_pending_ownership() == 0);  // claim 已 Live → 0
    assert(g_table_ensure_calls == after_recovery);

    // ---- 5) 真实 owner 拒绝恢复：desired 关闭。----
    arm_startup_retry_terminal();
    assert(qol::hooks::state_of(kOwnershipClaimId) == ResourceState::kFree);
    g_table_acquire_ok = 0;
    const int acquire_armed_closed = g_table_ensure_calls;
    set_recipes(false);                                          // desired 关闭
    frames();
    qol::host::feature_registry_notify_save_ready();
    frames();
    assert(qol::host::feature_registry_retry_pending_ownership() == 0);   // 已关闭 → 不恢复
    assert(g_table_ensure_calls == acquire_armed_closed);
    assert(qol::hooks::state_of(kOwnershipClaimId) == ResourceState::kFree);
    assert(!qol::features::custom_recipe::enabled());

    // ---- 6) 真实 owner 拒绝恢复：永久错误（Unsupported）不武装。----
    set_recipes(false);
    frames();
    g_table_acquire_ok = kQolErrUnsupported;
    const int acquire_before_permanent = g_table_ensure_calls;
    set_recipes(true);
    frames();
    assert(qol::hooks::state_of(kOwnershipClaimId) == ResourceState::kFree);
    assert(!qol::features::custom_recipe::enabled());
    assert(g_table_ensure_calls == acquire_before_permanent + 1);
    g_table_acquire_ok = 0;
    qol::host::feature_registry_notify_save_ready();
    frames();
    assert(qol::host::feature_registry_retry_pending_ownership() == 0);   // 永久错误不武装
    assert(g_table_ensure_calls == acquire_before_permanent + 1);
    assert(qol::hooks::state_of(kOwnershipClaimId) == ResourceState::kFree);

    // ---- 7) 恢复现场：再做一次正常激活（后续用例依赖 enabled 状态）。----
    set_recipes(false);
    frames();
    g_table_acquire_ok = 0;
    set_recipes(true);
    frames();
    assert(qol::features::custom_recipe::enabled());
    assert(qol::hooks::state_of(kOwnershipClaimId) == ResourceState::kLive);
}

// ---------------------------------------------------------------------------
// 启动/定向 ownership 恢复：typed acquire 错误域 + on_resource_ready 只更新门禁
// ---------------------------------------------------------------------------
void test_startup_ready_notification() {
    const auto& ops = qol::features::custom_recipe::ownership_ops();
    const qol::contracts::PluginDescriptor& descriptor =
        qol::features::custom_recipe::plugin_descriptor();
    qol::contracts::PluginContext ctx{};
    assert(qol::host::feature_registry_context("custom_recipe", &ctx));

    // a) 未就绪时的定向通知不得开门禁（只基于 ownership_ready() && core hook 备份）。
    //    先归一化：release 清 ready 并 total deactivate（前面装配周期已 acquire 过）。
    ops.release("own:RECIPEBASE_MIXTUREBASE");
    qol::features::custom_recipe::set_enabled(false);
    assert(!qol::features::custom_recipe::ownership_ready());
    assert(descriptor.on_resource_ready(ctx, "own:RECIPEBASE_MIXTUREBASE", nullptr) == 0);
    assert(!qol::features::custom_recipe::enabled());

    // b) NotReady：原码返回 + total 清理 + ready false；通知仍不开门禁。
    g_table_acquire_ok = kQolErrNotReady;
    const int deactivate_before = g_table_deactivate_calls;
    const int acquire_before = g_table_ensure_calls;
    assert(ops.acquire("own:RECIPEBASE_MIXTUREBASE") == kQolErrNotReady);
    assert(!qol::features::custom_recipe::ownership_ready());
    assert(!g_table_published);
    assert(g_table_deactivate_calls == deactivate_before + 1);  // 失败即 total 清理
    assert(g_table_ensure_calls == acquire_before + 1);         // 只走一次 typed acquire
    assert(descriptor.on_resource_ready(ctx, "own:RECIPEBASE_MIXTUREBASE", nullptr) == 0);
    assert(!qol::features::custom_recipe::enabled());

    // c) 永久错误（Unsupported）与重入（Busy）原码透传，不折叠成 NotReady。
    g_table_acquire_ok = kQolErrUnsupported;
    assert(ops.acquire("own:RECIPEBASE_MIXTUREBASE") == kQolErrUnsupported);
    assert(!qol::features::custom_recipe::ownership_ready());
    g_table_acquire_ok = kQolErrBusy;
    assert(ops.acquire("own:RECIPEBASE_MIXTUREBASE") == kQolErrBusy);
    assert(!qol::features::custom_recipe::ownership_ready());

    // d) 成功：ready true / 表已发布；定向通知（backup=nullptr）只更新门禁。
    g_table_acquire_ok = 0;
    const int events_before = qol::host::fixture_event_registrations();
    assert(ops.acquire("own:RECIPEBASE_MIXTUREBASE") == 0);
    assert(qol::features::custom_recipe::ownership_ready() && g_table_published);
    const int acquire_after_ok = g_table_ensure_calls;
    assert(!qol::features::custom_recipe::enabled());  // 通知前门禁保持关闭
    assert(descriptor.on_resource_ready(ctx, "own:RECIPEBASE_MIXTUREBASE", nullptr) == 0);
    assert(qol::features::custom_recipe::enabled());
    assert(g_table_ensure_calls == acquire_after_ok);                  // 不重复 table acquire
    assert(qol::host::fixture_event_registrations() == events_before);  // 不重复注册

    // e) 其他 hook id 仍走 UI 现有 backup 路径（未知 id → 拒绝，不误判为 ownership）。
    assert(descriptor.on_resource_ready(ctx, "own:NOT_OURS", nullptr) != 0);

    // f) release：清 ready 并 total deactivate。
    ops.release("own:RECIPEBASE_MIXTUREBASE");
    assert(!qol::features::custom_recipe::ownership_ready());
}

}  // namespace

// host 上 table.cpp 只编译纯构造器；表注入行为由 fixture 替身提供（可注入失败）。
namespace custom_recipe {

void set_context(const qol::contracts::QolGameHost*, const qol::contracts::QolHostSdk*) {}

QolResult custom_recipe_table_acquire() {
    ++g_table_ensure_calls;
    const int32_t code = g_table_acquire_ok != 0
                             ? g_table_acquire_ok
                             : (g_table_ensure_result ? 0 : kQolErrNotReady);
    if (code != 0) {
        g_table_published = false;
        static constexpr char kAcquireFailedMessage[] = "fixture table acquire failed";
        return qol::contracts::error(code, kAcquireFailedMessage,
                                     sizeof(kAcquireFailedMessage) - 1);
    }
    g_table_published = true;
    ::custom_recipe::bind_base_record_count(69);
    return qol::contracts::ok();
}

// bool 薄包装（与生产 table.cpp 的薄包装同语义）。
bool custom_recipe_table_ensure() { return custom_recipe_table_acquire().ok == kQolOk; }

void custom_recipe_table_deactivate() {
    ++g_table_deactivate_calls;
    g_table_published = false;
}

}  // namespace custom_recipe

int main(int argc, char** argv) {
    assert(argc == 2);
    g_main_thread = std::this_thread::get_id();
    install_ready_ports();

    qol::host::plugin_registry_reset_for_test();
    qol::hooks::registry_reset_for_test();
    qol::hooks::tx_reset_for_test();
    qol::hooks::tx_set_deps(qol::hooks::TxDeps{&resolve_symbol, &hook_symbol, &unhook_symbol,
                                               &gate_fn, &fixture_install_other,
                                               &fixture_release_other});

    PluginDescriptor item_facts =
        provider_descriptor("item_facts", kItemFactsProvides, kItemFactsFeatures);
    PluginDescriptor craft_facts =
        provider_descriptor("craft_facts", kCraftFactsProvides, kCraftFactsFeatures);
    PluginDescriptor text_layer =
        provider_descriptor("text_layer", kTextLayerProvides, kTextLayerFeatures);
    PluginDescriptor attribute_quality = provider_descriptor(
        "attribute_quality", kAttributeQualityProvides, kAttributeQualityFeatures);
    const PluginDescriptor& custom_recipe = qol::features::custom_recipe::plugin_descriptor();

    // ---- 单 descriptor 形状 ----
    assert(std::strcmp(custom_recipe.id, "custom_recipe") == 0);
    assert(custom_recipe.provides_count == 0 && custom_recipe.provides == nullptr);
    assert(custom_recipe.requires_count == 3);
    assert(custom_recipe.optional_count == 1);
    assert(custom_recipe.jni_count == 0);
    assert(custom_recipe.port_count == 2);
    assert(custom_recipe.ports[0].id == QolPortId::QOL_PORT_CUSTOM_RECIPE_WRITE);
    assert(custom_recipe.ports[1].id == QolPortId::QOL_PORT_CRAFT_UI_WRITE);
    assert(custom_recipe.feature_count == 1);
    assert(std::strcmp(custom_recipe.features[0].id, "custom_recipe.recipes") == 0);
    assert(custom_recipe.features[0].default_enabled == 0);
    assert(custom_recipe.resources_count == 13);
    for (int i = 0; i < 12; ++i) {
        assert(std::strcmp(custom_recipe.resources[i].id, kResourceIds[i]) == 0);
        assert(custom_recipe.resources[i].mechanism == ResourceMechanism::kEntryHook);
        const bool required = i < kCoreSymbolCount;
        assert((custom_recipe.resources[i].requirement == ResourceRequirement::kRequired) ==
               required);
        assert((custom_recipe.resources[i].failure == ResourceFailure::kFailClosed) == required);
    }
    // ownership 必须在最后（宿主逆序释放 → 表先还原，再释放 hook）。
    assert(std::strcmp(custom_recipe.resources[12].id, kResourceIds[12]) == 0);
    assert(custom_recipe.resources[12].mechanism == ResourceMechanism::kOwnership);
    assert(custom_recipe.resources[12].failure == ResourceFailure::kFailSoft);
    // descriptor 不声明 craft_result provides（能力已内部化）。
    assert(!qol::features::custom_recipe::enabled());
    assert(!qol::features::custom_recipe::ownership_ready());

    for (const PluginDescriptor* provider : {&item_facts, &craft_facts, &text_layer,
                                             &attribute_quality}) {
        assert(qol::host::register_plugin(*provider));
    }
    assert(qol::host::register_plugin(custom_recipe));

    qol::host::HostDeps deps{&resolve_symbol, &hook_symbol, &unhook_symbol};
    qol::host::FeatureDirectory directory;
    for (const PluginDescriptor* provider : {&item_facts, &craft_facts, &text_layer,
                                             &attribute_quality}) {
        assert(directory.add(*provider).ok());
    }
    assert(directory.add(custom_recipe).ok());

    std::string path = std::string(argv[1]) + "/custom-recipe-owner-XXXXXX";
    assert(::mkdtemp(path.data()) != nullptr);
    qol::host::CoreSettingsConfigService service;
    FeatureController control(service);
    FeatureReconciler reconcile(control);
    g_controller = &control;
    g_reconciler = &reconcile;
    assert(control.initialize(directory, path, &FeatureReconciler::pending_desired, &reconcile)
               .ok());
    assert(reconcile.bind({&resolve_symbol, &hook_symbol, &unhook_symbol}));

    // ---- 门禁开：真实描述符装配成功 ----
    set_recipes(true);
    frames();
    assert(qol::host::feature_registry_active("custom_recipe"));
    assert(qol::features::custom_recipe::enabled());
    assert(qol::features::custom_recipe::ownership_ready());
    assert(g_table_published && g_table_ensure_calls >= 1);
    assert_core_resources_live();
    for (int i = kCoreSymbolCount; i < 12; ++i) assert(resource_live(kResourceIds[i]));
    assert(resource_live(kResourceIds[12]));
    assert_no_public_craft_result_capability();
    assert(qol::features::custom_recipe::ui::core_backups_ready());
    assert(qol::host::fixture_typed_registrations() == 2);
    assert(g_text_subscribe_calls >= 1);
    std::puts("owner_fixture: single descriptor installs (13 live resources, gate open, no public craft_result)");

    // ---- 7 条装饰 hook 逐个失败：核心继续，失败资源 Free，插件保持 active ----
    for (int optional = 0; optional < kOptionalSymbolCount; ++optional) {
        set_recipes(false);
        frames();
        reset_gate_and_table();
        g_table_published = false;
        const int ensure_before = g_table_ensure_calls;
        g_fail_symbol = kOptionalSymbols[optional];
        set_recipes(true);
        frames();
        assert(qol::host::feature_registry_active("custom_recipe"));
        assert(qol::features::custom_recipe::enabled());
        assert_core_resources_live();
        assert(resource_free(kResourceIds[kCoreSymbolCount + optional]));
        assert(g_table_published && g_table_ensure_calls == ensure_before + 1);
        assert_no_public_craft_result_capability();
    }
    std::puts("owner_fixture: optional hook failures degrade only that resource (core stays live, fail-soft Free)");

    // ---- 5 条核心 hook 逐个失败：整插件回滚、注册净零、表未发布、门禁关闭 ----
    for (int core = 0; core < kCoreSymbolCount; ++core) {
        set_recipes(false);
        frames();
        reset_gate_and_table();
        const int ensure_before = g_table_ensure_calls;
        const int deactivate_before = g_table_deactivate_calls;
        const int typed_before = qol::host::fixture_typed_registrations();
        const int typed_unreg_before = qol::host::fixture_typed_unregistrations();
        const int event_before = qol::host::fixture_event_registrations();
        const int event_unreg_before = qol::host::fixture_event_unregistrations();
        const int text_sub_before = g_text_subscribe_calls;
        const int text_unsub_before = g_text_unsubscribe_calls;

        g_fail_symbol = kCoreSymbols[core];
        set_recipes(true);
        frames();
        assert(!qol::host::feature_registry_active("custom_recipe"));
        assert(!qol::features::custom_recipe::enabled());
        assert(!qol::features::custom_recipe::ownership_ready());
        assert_all_resources_free();
        assert(!g_table_published);
        assert(g_table_ensure_calls == ensure_before);  // 核心失败→表未开始注入
        // ownership 从未 publish；安装失败清理只允许调用一次 deactivate（绝不残留已发布表）。
        assert(g_table_deactivate_calls <= deactivate_before + 1);
        // 本次 install 的注册必须净额归零。
        assert(qol::host::fixture_typed_registrations() == typed_before + 2);
        assert(qol::host::fixture_typed_unregistrations() == typed_unreg_before + 2);
        assert(qol::host::fixture_event_registrations() == event_before + 1);
        assert(qol::host::fixture_event_unregistrations() == event_unreg_before + 1);
        assert(g_text_subscribe_calls == text_sub_before + 1);
        assert(g_text_unsubscribe_calls == text_unsub_before + 1);
        assert(!qol::features::custom_recipe::ui::core_backups_ready());
        assert_no_public_craft_result_capability();
    }
    assert_registrations_net_zero();
    std::puts("owner_fixture: each core hook failure rolls back to Free with net-zero registrations and no table publish");

    // ---- ownership acquire 失败：claim 为 required×fail-soft，插件保持装配但门禁必须关闭 ----
    reset_gate_and_table();
    g_table_ensure_result = false;
    set_recipes(true);
    frames();
    assert(qol::host::feature_registry_active("custom_recipe"));  // fail-soft 不级联停用
    assert(!qol::features::custom_recipe::enabled());
    assert(!qol::features::custom_recipe::ownership_ready());
    assert(g_table_ensure_calls >= 1);
    assert(!g_table_published);
    assert(resource_free(kResourceIds[12]));  // 失败资源保持 Free（可重试），不 tombstone
    assert_core_resources_live();
    assert_no_public_craft_result_capability();

    // gate=false（ownership 未就绪）：旧队列里排队的 typed 写命令不得写回。
    {
        const qol::contracts::CraftWriteStuffNeedPayload need{1, 15, 3};
        const qol::contracts::CraftSetStuffItemPayload set{2, qol::contracts::QolItemHandle{1, 1}};
        const auto* need_bytes = reinterpret_cast<const uint8_t*>(&need);
        const auto* set_bytes = reinterpret_cast<const uint8_t*>(&set);
        const int needs_before = g_fixture_needs_writes;
        const int set_before = g_fixture_set_item_writes;
        assert(!qol::features::custom_recipe::enabled());
        assert(!qol::features::custom_recipe::handle_write_stuff_need_command(need_bytes,
                                                                             sizeof(need)));
        assert(!qol::features::custom_recipe::handle_set_stuff_item_command(set_bytes,
                                                                           sizeof(set)));
        assert(g_fixture_needs_writes == needs_before && g_fixture_set_item_writes == set_before);

        // 恢复就绪后同一批 payload 才允许写回。
        reset_gate_and_table();
        set_recipes(false);
        frames();
        set_recipes(true);
        frames();
        assert(qol::features::custom_recipe::enabled());
        assert(qol::features::custom_recipe::handle_write_stuff_need_command(need_bytes,
                                                                            sizeof(need)));
        assert(qol::features::custom_recipe::handle_set_stuff_item_command(set_bytes, sizeof(set)));
        assert(g_fixture_needs_writes == needs_before + 1 &&
               g_fixture_set_item_writes == set_before + 1);
    }
    std::puts("owner_fixture: ownership acquire failure keeps the gate closed and publishes no table");

    // ---- 恢复：停用后表可用，再次启用应重新就绪 ----
    set_recipes(false);
    frames();
    reset_gate_and_table();
    set_recipes(true);
    frames();
    assert(qol::host::feature_registry_active("custom_recipe"));
    assert(qol::features::custom_recipe::enabled());
    assert(qol::features::custom_recipe::ownership_ready());
    assert(g_table_published);

    // ---- off：清理订阅/typed handler、还原表、门禁关闭；再 on 可恢复 ----
    set_recipes(false);
    frames();
    assert(!qol::features::custom_recipe::enabled());
    assert(!qol::features::custom_recipe::ownership_ready());
    assert(g_table_deactivate_calls >= 1 && !g_table_published);
    assert_registrations_net_zero();
    assert_all_resources_free();
    assert(!qol::features::custom_recipe::ui::core_backups_ready());
    assert_no_public_craft_result_capability();

    set_recipes(true);
    frames();
    assert(qol::host::feature_registry_active("custom_recipe"));
    assert(qol::features::custom_recipe::enabled());
    assert(qol::features::custom_recipe::ownership_ready());
    assert_core_resources_live();

    test_ui_confirm_paths();
    test_popup_text_id_domain();
    std::puts("owner_fixture: ui confirm/cancel/popup-failure/prompt paths verified");
    std::puts("owner_fixture: popup-index vs MEMORYTEXT text id domains verified");
    test_startup_ready_notification();
    std::puts("owner_fixture: typed table acquire error domains + ownership ready notification verified");
    test_cross_lane_startup_retry();
    std::puts("owner_fixture: cross-lane startup retry (registry + real descriptor + SaveEnter signal) verified");

    std::puts("custom_recipe_owner_fixture_tests: pass (single descriptor, per-hook rollback/degrade, ownership gate, off/on cleanup)");
    return 0;
}
