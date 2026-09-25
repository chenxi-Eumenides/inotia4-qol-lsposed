#pragma once

#include <cstdint>
#include <type_traits>

#include "../result.h"
#include "../version.h"

namespace qol::contracts {

// 启动期由 Kotlin 下发的静态 enrichment 片段端点。
constexpr int32_t kQuestFragmentActive = 0;
constexpr int32_t kQuestFragmentDetails = 1;
constexpr int32_t kQuestFragmentCompleted = 2;
// 返回指定端点下某 quest 的 JSON 对象正文（不含外层花括号）；未收录返回 kQolErrNotFound。
using QolQuestFragmentFn = QolResult (*)(int32_t endpoint, uint32_t quest_id,
                                         const char** out_fragment);

struct QolQuestReadOnlyApi {
    QolResult (*state)(int32_t* out_state);
    QolResult (*active_slot_count)(uint32_t* out_count);
    QolResult (*active_slot_id)(uint32_t index, uint32_t* out_id);
    QolResult (*active_quest_id)(int32_t* out_quest_id);
    QolResult (*total_count)(uint32_t* out_count);
    QolResult (*quest_state)(uint32_t quest_id, int32_t* out_state);
    QolResult (*is_complete)(uint32_t quest_id, int32_t* out_complete);
    // 静态片段表只在启动期写入；运行期只读，供主线程快照构造时拼接。
    QolQuestFragmentFn fragment;
    // 静态片段表是否已由启动期下发（owner 快照据此推迟发布）。
    QolResult (*static_ready)(int32_t* out_ready);
};

struct QolQuestWriteApi {
    QolResult (*set_state)(uint32_t quest_id, int32_t state);
    // 执行退出任务（原版 Find + RemoveSlot）并把迁移前的结果 JSON
    // （{"ok":true,"state":<player>} 或原错误体）写入 out。主线程 owner 命令调用。
    QolResult (*quit_result)(int32_t quest_id, char* out, uint32_t capacity, uint32_t* out_size);
};

struct QolQuestPort {
    QolVersion abi_version;
    QolQuestReadOnlyApi read;
    QolQuestWriteApi write;
};

static_assert(std::is_standard_layout<QolQuestPort>::value,
              "QolQuestPort must remain standard layout");
static_assert(std::is_trivially_copyable<QolQuestPort>::value,
              "QolQuestPort must remain trivially copyable");

}  // namespace qol::contracts
