// game_feature.h —— 在 game_variant（构建校验值/能力位）之上的功能抽象层。
//
// 职责：把「当前 libgame.so 属于哪个构建」翻译成「有没有某功能 / 该功能当前是否可用」。
// 依赖方向：core 层，依赖 core/native/game_variant.h 与 STL；不得依赖 game_access.h 等上层。
#pragma once

#include <cstdint>

namespace qol {

enum class GameFeature : uint8_t {
    kPersonalWarehouse      = 0,  // 个人仓库（96 格）机制存在（wh4 或 block3 内嵌任一）
    kWarehouseInlineInSave  = 1,  // 仓库内嵌在存档 block3（新写法）
    kWarehouseCompanionFile = 2,  // 仓库伴生文件 save{N}.dat.wh4-<id>（外部写法）
    kItemCountUpperBound    = 3,  // 物品数量上界校验（monster 自写包装器）
};

enum class FeatureState : uint8_t {
    kUnknown = 0,   // 构建信息未知（game_variant 未初始化或未命中表且无法判定系列）
    kUnsupported,   // 该构建没有这个功能
    kAvailable,     // 有，且当前可用
    kUnavailable,   // 有，但当前不可用（运行时条件不满足）
};

FeatureState game_feature_state(GameFeature f);
const char* game_feature_name(GameFeature f);          // "personal_warehouse" 等小写下划线 token
const char* game_feature_state_name(FeatureState s);   // "unknown"/"unsupported"/"available"/"unavailable"

// 运行时可用性注入：上层可在安装完成后注入一个回调，用于把「本机是否具备操作该功能的运行时条件」
// （例如 fn_hub_save_get_key / fn_encrypt_process2 是否已解析）纳入判断。
// 未注入时仅按构建能力判断。回调返回 false 时，supported 的功能降级为 kUnavailable。
using FeatureUsabilityFn = bool (*)(GameFeature);
void game_feature_set_usability_fn(FeatureUsabilityFn fn);

namespace game_feature_detail {
// 纯逻辑（host 可测）：由能力位 + 系列是否已知 + 可用性回调结果 + 是否 monster 算出状态。
FeatureState state_from(uint32_t caps, bool variant_known, bool usability_ok,
                        bool series_is_monster, GameFeature f);
// 能力位是否表达该功能。kItemCountUpperBound 不由能力位编码（见 .cpp 说明），恒返回 false，
// 其支持性由 state_from 用 series_is_monster 判定。
bool supported_by_caps(uint32_t caps, GameFeature f);
}  // namespace game_feature_detail

}  // namespace qol
