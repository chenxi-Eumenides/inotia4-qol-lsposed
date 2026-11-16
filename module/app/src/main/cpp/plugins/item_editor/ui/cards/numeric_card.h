#pragma once

#include <cstdint>

namespace qol::features::item_editor {

// 可复用数字调整卡：同一时间只允许一张；确认只写 draft，不写实际物品。
enum class NumericCardTarget : uint8_t {
    kCount,             // g_draft.count（数量卡）
    kEnchantRemaining,  // g_draft.enchant_remaining
    kEnchantStrength,   // 强化强度选择（低级/中级/高级/顶级/特级/混沌）
    kEnchantLevel,      // g_draft.enchant_level（已强化次数）
    kSocketTotal,       // g_draft.socket_total
    kChaosRate,         // g_draft.chaos_rate，确认后重算派生属性
};

struct NumericCardSpec {
    const char* id = nullptr;  // 日志/调试用
    NumericCardTarget target = NumericCardTarget::kEnchantRemaining;
    int32_t min_value = 0;
    int32_t max_value = 0;
    int32_t value = 0;
    // 增量按钮标签按 %+d 生成；行优先布局（index%4 为列，index/4 为行），
    // 最多 8 个：4 个以内保持单行，5 个及以上用四列两行网格。
    const int32_t* deltas = nullptr;
    uint32_t delta_count = 0;
    // has_preset 时在增量按钮后追加一个直接取 preset_value 的按钮。
    int32_t preset_value = 0;
    bool has_preset = false;
};

// 打开通用数字调整卡；打开前先关闭旧卡。spec 非法时记录日志并返回 false。
bool numeric_card_open(const NumericCardSpec& spec);
// 打开强化强度选择卡；用通用数字卡的 6 个选择按钮布局。
bool numeric_card_open_enchant_strength();

// 以下供 panel.cpp 路由：打开请求、创建/销毁、确认动作、绘制、事件与激活状态。
bool numeric_card_open_requested();
void numeric_card_clear_open_request();
bool create_numeric_card();
void destroy_numeric_card();
void numeric_card_apply();
void draw_numeric_card();
bool numeric_card_active();
bool numeric_card_handle_event(uint64_t event, uint64_t param, uint64_t param2,
                               uint32_t* consumed);

}  // namespace qol::features::item_editor
