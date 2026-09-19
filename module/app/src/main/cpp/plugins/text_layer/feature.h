#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "../../contracts/plugin.h"
#include "../../contracts/text_layer.h"

namespace qol::hooks {
enum class DispatchPhase : int32_t { kBefore = 0, kAfter = 1 };
enum class ConsumerKind : int32_t { kObserve = 0, kModify = 1, kIntercept = 2 };
}  // namespace qol::hooks

namespace qol::infra {
enum class TextScope : uint8_t;
TextScope active_text_scope();
}  // namespace qol::infra

// 模块自定义文本层（text_layer 插件，ARCHITECTURE.md §3.3/§4.5/§4.6）。
//
// 提供共享资源 `hook:MEMORYTEXT_GetText`（全游戏唯一文本读取热点，232 调用点）与
// text_layer v1 能力。分派顺序严格按 §4.5：before → original → after：
//   wrapper：调原函数得到**原始文本**，作为 `original` 交给 dispatch_run_text；
//   owner 的 14 条内置表是 after 阶段的 Modify 消费者（install 时最先注册 →
//   after 逆序最后执行 → 内置文案仍覆盖第三方修改，与旧行为等价）。
// 值类型资源的 `original` 由 owner 在进入分派前计算（输入不可变），故 before 阶段消费者
// 看到的是原函数文本，语义等价于 §4.5 的 `before → original → after`。
//
// 消费者不再各自挂 GetText（LSPosed 同址二次挂载会失败），改经能力 subscribe；
// 本文件的表与查表逻辑是纯逻辑（不触游戏内存），编入 host 单测。

namespace qol::features::text_layer {

// 资源 id：plugin.cpp 的 ResourceClaim 与 wrapper 的分派键共用。
inline constexpr const char* kResourceIdGetText = "hook:MEMORYTEXT_GetText";
// 角色面板绘制窗口入口 hook（私有资源，owner=text_layer）：handler 见 charinfo_scope.h。
inline constexpr const char* kResourceIdCharinfoScope = "hook:Scene_Draw_Popup_SC_CharacterInfo";

struct Entry {
    const char* key;              // 语义键（唯一；下方 host test 断言唯一性）
    uint16_t text_id;             // 被替换的游戏 text id
    qol::infra::TextScope scope;  // 生效窗口（与 Host SDK scope 数值一致）
    // nullptr = 明确保持游戏原文（占位用，维持连续 id 区间不被打破），不做替换。
    const char* text;
};

// ---- 内置文本表（唯一真源）----
//
// ① 角色面板战斗属性标签（kCharacterPanel）：游戏文本表漏翻了 11 个属性缩写。
//    35181 LV / 35182 EXP / 35183 HP / 35184 MP **保持原文**；
//    35188 M. DEF 面板未显示，显式登记 nullptr 占位以维持 35185..35196 连续区间。
//    文案不超过四个汉字：物攻 / 法攻 / 防御力 / 暴击率 / 命中率 / 爆伤 /
//    物抗 / 法抗 / 闪避率 / 武器格挡 / 盾牌格挡。35192 的含义为 P.RES = 物理抗性。
// ② UIMix 配方按钮（kUimixRecipeButton）：模块「宝石强化」条目改为显示「宝石升阶」。
//    该 id（35291）同时是宝石强化页的**页签名**，而页签由 UIMix_ButtonMenuListDraw 绘制、
//    不在本作用域内，故只有配方按钮改名（见 lookup 矩阵用例）。文案表里不存在这一串，
//    因此只能由模块提供。
inline constexpr Entry kEntries[] = {
    {"charinfo.dmg", 35185, static_cast<qol::infra::TextScope>(1), "物攻"},        // DMG
    {"charinfo.magic_dmg", 35186, static_cast<qol::infra::TextScope>(1), "法攻"},  // M. DMG
    {"charinfo.def", 35187, static_cast<qol::infra::TextScope>(1), "防御力"},      // DEF
    {"charinfo.magic_def", 35188, static_cast<qol::infra::TextScope>(1), nullptr},  // 面板未显示
    {"charinfo.crit_rate", 35189, static_cast<qol::infra::TextScope>(1), "暴击率"},
    {"charinfo.hit_rate", 35190, static_cast<qol::infra::TextScope>(1), "命中率"},
    {"charinfo.crit_dmg", 35191, static_cast<qol::infra::TextScope>(1), "爆伤"},
    {"charinfo.phys_res", 35192, static_cast<qol::infra::TextScope>(1), "物抗"},
    {"charinfo.magic_res", 35193, static_cast<qol::infra::TextScope>(1), "法抗"},
    {"charinfo.evade", 35194, static_cast<qol::infra::TextScope>(1), "闪避率"},
    {"charinfo.weapon_block", 35195, static_cast<qol::infra::TextScope>(1), "武器格挡"},
    {"charinfo.shield_block", 35196, static_cast<qol::infra::TextScope>(1), "盾牌格挡"},
    {"recipe.jewel_tier_up", 35291, static_cast<qol::infra::TextScope>(2), "宝石升阶"},
    {"recipe.jewel_tier_up.title", 35291, static_cast<qol::infra::TextScope>(3), "宝石升阶"},
};

inline constexpr size_t kEntryCount = sizeof(kEntries) / sizeof(kEntries[0]);

// 编译期保证角色面板段位于表首、连续升序且恰好覆盖 35185..35196（无空洞）。
inline constexpr uint16_t kCharinfoIdMin = 35185;
inline constexpr uint16_t kCharinfoIdMax = 35196;
inline constexpr size_t kCharinfoIdCount = static_cast<size_t>(kCharinfoIdMax - kCharinfoIdMin) + 1;
constexpr bool charinfo_block_ok() {
    for (size_t i = 0; i < kCharinfoIdCount; ++i) {
        if (static_cast<uint8_t>(kEntries[i].scope) != 1) return false;
        if (kEntries[i].text_id != static_cast<uint16_t>(kCharinfoIdMin + i)) return false;
    }
    return kEntryCount > kCharinfoIdCount &&
           static_cast<uint8_t>(kEntries[kCharinfoIdCount].scope) != 1;
}
static_assert(charinfo_block_ok(),
              "角色面板文本段必须位于表首并覆盖 35185..35196 连续升序、无空洞");

inline const Entry* entries(size_t* out_count) {
    if (out_count != nullptr) *out_count = kEntryCount;
    return kEntries;
}

// 查表：text_id 命中、scope == active、且 text != nullptr 时返回字面量；否则 nullptr。
inline const char* lookup(uint16_t text_id, qol::infra::TextScope active) {
    if (static_cast<uint8_t>(active) == 0) return nullptr;
    for (size_t i = 0; i < kEntryCount; ++i) {
        if (kEntries[i].text_id != text_id) continue;
        if (kEntries[i].scope != active) continue;
        return kEntries[i].text;  // 可能是 nullptr（显式保持原文）
    }
    return nullptr;
}

inline const Entry* entry_by_key(const char* key) {
    if (key == nullptr) return nullptr;
    for (size_t i = 0; i < kEntryCount; ++i) {
        if (kEntries[i].key != nullptr && std::strcmp(kEntries[i].key, key) == 0) {
            return &kEntries[i];
        }
    }
    return nullptr;
}

// 业务门禁：true = 表替换与能力查询生效；false = 撤销期，wrapper 透传原函数。
void set_active(bool active);
bool active();
uint8_t active_scope_value();
void set_active_scope_value(uint8_t value);

// 内置表消费者（声明式登记，§4.6）：after 阶段 Modify——在 after 逆序链最后执行，
// 使内置文案覆盖第三方修改（install 时最先注册）。
inline constexpr qol::hooks::DispatchPhase kOwnerTablePhase =
    static_cast<qol::hooks::DispatchPhase>(1);
inline constexpr qol::hooks::ConsumerKind kOwnerTableKind =
    static_cast<qol::hooks::ConsumerKind>(1);

// 内置表 after-Modify 的纯逻辑：按当前线程作用域查表；命中返回字面量，未命中返回 nullptr。
// 可在 host 单测直接断言（不触游戏内存、不依赖分派链接）。
inline const char* owner_table_lookup(uint32_t text_id) {
    return lookup(static_cast<uint16_t>(text_id), qol::infra::active_text_scope());
}

// 内置表 Modify 回调体（分派消费者）：委托 owner_table_lookup；纯函数（无副作用）。
const char* owner_table_modify(uint32_t text_id, const char* current, void* ctx);

// 注册内置表消费者（install 调用，幂等）；容量满等失败返回 false → install 失败（fail-closed）。
bool register_builtin_consumers();
// 注销内置表消费者（deactivate 调用，幂等）。
void unregister_builtin_consumers();

// 宿主在资源安装成功后交还 backup（原函数入口）；未知 id 忽略，恒返回 0。
int32_t on_resource_ready(contracts::PluginContext& ctx, const char* resource_id, void* backup);

// 能力接口实现：转发 hooks/dispatch（phase/kind 分别取 DispatchPhase/ConsumerKind 数值）。
uint64_t api_subscribe(int32_t phase, int32_t kind, const char* (*fn)(uint32_t, const char*, void*),
                       void* ctx);
void api_unsubscribe(uint64_t id);

// 共享资源 wrapper：由 plugin_descriptor 的 ResourceClaim.handler 声明，宿主安装。
const char* memorytext_get_text_wrapper(uint16_t text_id);

}  // namespace qol::features::text_layer
