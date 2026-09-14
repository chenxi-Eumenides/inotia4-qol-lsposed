// game_variant.h —— libgame.so 校验值 → 游戏变体/能力 的运行时查表接口。
//
// 依赖方向：core 层，仅依赖 STL + core/native/qol_log.h；不得依赖 game_access.h / g_base 等上层。
// 数据来源：data/native/game_variant_table.inc（由 scripts/data/game_variant_table.py 离线生成）。
#pragma once

#include <cstdint>

namespace qol {

enum class GameSeries : uint8_t {
    kUnknown = 0,
    kOriginal,
    kOverhaul,
    kMonster,
};

enum : uint32_t {
    kCapHiddenSegment   = 1u << 0,  // 存在隐藏可执行 PT_LOAD 段（monster 注入段）
    kCapWarehouse       = 1u << 1,  // 隐藏段内含 ASCII WH4JRN01（wh4 仓库容器）
    kCapWarehouseInline = 1u << 2,  // block3 内嵌 WH96v002 仓库段（语义位，未验证为 0）
};

struct GameVariant {
    GameSeries series = GameSeries::kUnknown;
    const char* version_label = "";   // 表内静态字符串
    uint32_t capabilities = 0;
    uint8_t  md5[16] = {0};
    bool     known = false;           // 是否命中表
};

// 幂等；首次调用解析 /proc/self/maps 定位 libgame.so、算 md5 并查表，结果缓存。
bool game_variant_init();
// 未初始化时返回 {kUnknown, ...}。
const GameVariant& game_variant();
// "unknown" / "original" / "overhaul" / "monster"
const char* game_series_name(GameSeries s);

namespace game_variant_detail {
// md5[16] 是否等于 32 位小写十六进制串。
bool md5_hex_equals(const uint8_t md5[16], const char* hex32);
// 命中返回表内条目指针，未命中返回 nullptr。
const GameVariant* lookup(const uint8_t md5[16]);
// 回退分类：libgame 可执行映射里命中 WH4JRN01 → kMonster，否则 kUnknown。
GameSeries classify_fallback(bool has_wh4_marker);
}  // namespace game_variant_detail

}  // namespace qol
