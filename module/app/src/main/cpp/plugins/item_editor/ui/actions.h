#pragma once

#include "../plugin.h"
#include "../model/draft.h"
#include "../../../contracts/ui.h"
#include "../../../contracts/item_payload_codec.h"

#include <cstdint>

// 主面板最右侧动作列（放入背包 / 生成掉落 / 替换物品）。
// 纯计算部分（payload 字段推导、按钮启用条件）以内联函数放在本头文件，
// 供 UI 与 host 单测共用；控件生命周期、能力调用与动作执行在 actions.cpp。
namespace qol::features::item_editor::panel_internal {

// 按钮启用条件：draft 有具体 item_id 且 class 有效。
inline bool action_draft_ready(const ItemEditorDraft& draft) {
    return draft_has_item(draft) && item_class_valid(draft.class_index);
}

// 按 codec 口径从 draft 推导 payload 头字段；uid 固定 0（create 路径由
// kQolItemCreateFlagAllocateUid 分配新 UID）。数值按各字段宽度截断。
inline contracts::QolItemPayloadHeader action_payload_header(const ItemEditorDraft& draft) {
    contracts::QolItemPayloadHeader header{};
    header.uid = 0;
    header.type_flags = static_cast<uint16_t>(
        ((static_cast<uint32_t>(draft.item_id) & 0x3ffu) << 6) |
        ((static_cast<uint32_t>(draft.raw_rarity) & 0xfu) << 2));
    if (draft.class_index == kItemClassGem) {
        // 宝石本体：属性类型/值占用 count_field，不带数量。
        header.count_field =
            ((static_cast<uint32_t>(draft.jewel_attribute_type) & 0x3fu) << 18) |
            (static_cast<uint32_t>(draft.jewel_value) & 0x7ffu);
    } else if (item_class_is_equipment(draft.class_index)) {
        header.count_field = 100u << 25;
        // 混沌位域与 100<<25 同字段：仅 chaos_flags 命中时写入。
        if ((draft.chaos_flags & 3) != 0) {
            header.count_field |=
                (static_cast<uint32_t>(draft.chaos_level) & 0x7fu) |
                ((static_cast<uint32_t>(draft.chaos_rate) & 0xffu) << 8);
        }
    } else {
        header.count_field = (static_cast<uint32_t>(draft.count) & 0xffu) << 25;
    }
    header.magic_rate = static_cast<uint8_t>(draft.magic_rate & 0xff);
    header.socket = static_cast<uint8_t>(((draft.socket_total & 0xf) << 4) |
                                         (draft.socket_filled & 0xf));
    header.enchant = static_cast<uint16_t>(
        ((draft.enchant_id & 0x1f) << 11) | ((draft.enchant_level & 0x1f) << 6) |
        ((draft.enchant_remaining & 0xf) << 2) | (draft.chaos_flags & 3));
    return header;
}

// native → jewel → enchant 合并词条为 payload options；跳过 id < 0 的空词条，
// code = (type << 13) | (id & 0x1fff)，value 为 s16。返回需要的词条总数；
// out != nullptr 时最多写 capacity 条（超出部分只计数不写出）。
inline uint32_t action_payload_options(const ItemEditorDraft& draft,
                                       contracts::QolItemPayloadOption* out,
                                       uint32_t capacity) {
    uint32_t total = 0;
    const auto append = [&](int32_t type,
                            const contracts::QolItemInventoryOptionView* options,
                            uint32_t count) {
        for (uint32_t index = 0; index < count; ++index) {
            if (options[index].id < 0) continue;
            if (out != nullptr && total < capacity) {
                out[total].code = static_cast<uint16_t>((type << 13) | (options[index].id & 0x1fff));
                out[total].value = static_cast<int16_t>(options[index].value);
            }
            ++total;
        }
    };
    append(0, draft.native_options, draft.native_option_count);
    append(1, draft.jewel_options, draft.jewel_option_count);
    append(2, draft.enchant_options, draft.enchant_option_count);
    return total;
}

// 面板创建时调用：创建独立 action root 与三个按钮（替换按钮仅 source_valid 时创建）。
// 任一控件失败时销毁已创建部分并返回 false。
bool actions_create();

// 面板销毁时调用：关闭未完成的确认框并销毁 action root。
void actions_destroy();

// panel_process 的 UI 生命周期外调用：执行确认后的 pending 动作，避免在 begin_frame 内做库存写。
void actions_process();

// 每帧 panel_process 绘制块内调用：只绘制按钮与状态文本。
void actions_draw();

// 原生事件派发：主面板未消费时派发给 action root；返回消费结果。
uint32_t actions_dispatch_event(uint64_t event, uint64_t param, uint64_t param2);

}  // namespace qol::features::item_editor::panel_internal
