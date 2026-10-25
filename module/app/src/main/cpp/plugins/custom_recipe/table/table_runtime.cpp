// table_runtime.cpp —— 表注入的运行时适配层。
//
// game/ 经 QolRecipeTablePort 提供 RECIPEBASE/MIXTUREBASE 槽位；本文件只把端口输出映射为
// 便携核心的 TableAccess，并承接全部与宿主/平台的接线（日志绑定、模块侧状态）。
// 地址与 VMA 不在此目录出现：槽位由 game 适配层装配。

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "../model/catalog.h"
#include "../model/rules.h"
#include "table.h"

namespace custom_recipe {
namespace {

const qol::contracts::QolGameHost* g_game = nullptr;
const qol::contracts::QolHostSdk* g_sdk = nullptr;
const qol::contracts::QolRecipeTablePort* g_table_port = nullptr;

// 模块侧状态与日志去重（本 TU 单一实例；核心按状态机处理重入与快照）。
TableState g_state;
TableLogDedupe g_dedupe;

void table_log_line(int32_t level, const char* message) {
    if (g_sdk == nullptr || g_sdk->log.write == nullptr || message == nullptr) return;
    const size_t length = std::strlen(message);
    if (length == 0) return;
    const uint32_t size = length >= qol::contracts::kMessageCap
                              ? qol::contracts::kMessageCap
                              : static_cast<uint32_t>(length);
    (void)g_sdk->log.write(level, 15, "plugins/custom_recipe/table", message, size);
}

TableAccess resolve_access() {
    TableAccess access{};
    if (g_table_port == nullptr || g_table_port->resolve == nullptr) return access;
    qol::contracts::QolRecipeTableSlots slots{};
    g_table_port->resolve(&slots);
    access.base = slots.base;
    access.recipe_ptr_slot = slots.recipe_ptr_slot;
    access.mixture_ptr_slot = slots.mixture_ptr_slot;
    access.recipe_size_slot = slots.recipe_size_slot;
    access.mixture_size_slot = slots.mixture_size_slot;
    access.recipe_count_slot = slots.recipe_count_slot;
    return access;
}

}  // namespace

// 端口绑定：plugin install 校验通过后注入，deactivate 时解绑。
void table_bind_port(const qol::contracts::QolRecipeTablePort* port) { g_table_port = port; }

// 表日志绑定：宿主在 install/激活期经 PluginContext 注入（feature.cpp set_context 转发）。
// 表状态日志经 table_log_line 使用此绑定；未绑定时静默。
void set_context(const qol::contracts::QolGameHost* game, const qol::contracts::QolHostSdk* sdk) {
    g_game = game;
    g_sdk = sdk;
}

// 主线程契约：仅日志级观测，不改变表状态机（表槽位只在游戏主线程读写）。
static void note_thread_contract(const char* entry) {
    if (g_game == nullptr || g_game->thread.is_main_thread == nullptr) return;
    if (g_game->thread.is_main_thread() != 0) return;
    static std::atomic<bool> reported{false};
    if (reported.exchange(true)) return;
    table_log_line(2, entry);
}

QolResult custom_recipe_table_acquire() {
    note_thread_contract("wrong thread entry=recipe_table acquire");
    // 端口缺失或 base 未就绪时槽位地址本身无意义，必须先判（不得带着垃圾指针读取）。
    const TableAccess access = resolve_access();
    if (access.base == 0) return table_status_result(TableAcquireStatus::kNotReady);
    size_t catalog_len = 0;
    const Def* catalog_ptr = catalog(&catalog_len);
    // busy 检测/置位/复位全部在 core 内（RAII），本入口不重复管理，避免自我拒绝。
    return table_status_result(table_acquire_core(access, &g_state, catalog_ptr, catalog_len,
                                                  &std::malloc, &std::free, &table_log_line,
                                                  &g_dedupe));
}

bool custom_recipe_table_ensure() { return custom_recipe_table_acquire().ok == kQolOk; }

void custom_recipe_table_deactivate() {
    note_thread_contract("wrong thread entry=recipe_table deactivate");
    const TableAccess access = resolve_access();
    if (access.base == 0) return;
    (void)table_deactivate_core(access, &g_state);
}

}  // namespace custom_recipe
