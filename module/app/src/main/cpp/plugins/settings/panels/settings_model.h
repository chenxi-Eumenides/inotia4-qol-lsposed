#pragma once

// 设置页数据驱动模型的纯函数（无游戏依赖，host 可测）。
//
// 页面本体（settings.cpp）只负责原生控件与绘制，分类/条目映射、开关目标值、
// 失败文案等规则集中在此，避免呈现层出现第二份实现。

#include <cstdint>
#include <cstring>
#include <utility>
#include <string>
#include <vector>

#include "contracts/settings.h"
#include "contracts/settings_config.h"
#include "contracts/ui.h"

namespace qol::features::core_ui::settings_model {

constexpr int32_t kDividerThickness = 2;
constexpr int32_t kTextLineAdvance = 32;
constexpr int32_t kTextGlyphHeight = 24;
constexpr int32_t kTextColumnGap = 16;
constexpr int32_t kDescriptionTop = 42;
constexpr int32_t kMinimumRightRowHeight = 96;
constexpr int32_t kToggleWidth = 220;
constexpr int32_t kToggleHeight = 80;
constexpr int32_t kRightControlRightInset = 20;

struct WrappedText {
    std::vector<std::string> lines;
    int32_t estimated_width = 0;
};

struct RowTextLayout {
    int32_t title_y;
    int32_t description_y;
    bool has_description;
};

// Native font metrics are not exposed by draw_control_text. Reserve 24 logic units per CJK
// codepoint and 12 per ASCII codepoint; line boxes advance by 32 to keep the conservative
// estimate readable instead of shrinking the game font. This is a geometry estimate, not a
// claim of exact glyph measurement.
inline int32_t codepoint_width(const unsigned char* bytes, size_t remaining, size_t* length) {
    *length = 1;
    if (bytes[0] < 0x80) return 12;
    if ((bytes[0] & 0xe0u) == 0xc0u && remaining >= 2 && (bytes[1] & 0xc0u) == 0x80u)
        *length = 2;
    else if ((bytes[0] & 0xf0u) == 0xe0u && remaining >= 3 &&
             (bytes[1] & 0xc0u) == 0x80u && (bytes[2] & 0xc0u) == 0x80u)
        *length = 3;
    else if ((bytes[0] & 0xf8u) == 0xf0u && remaining >= 4 &&
             (bytes[1] & 0xc0u) == 0x80u && (bytes[2] & 0xc0u) == 0x80u &&
             (bytes[3] & 0xc0u) == 0x80u)
        *length = 4;
    return *length == 1 ? 12 : 24;
}

inline WrappedText truncate_text(const char* text, int32_t max_width) {
    WrappedText result;
    if (text == nullptr || text[0] == '\0' || max_width <= 0) return result;
    constexpr char kEllipsis[] = "…";
    const auto* cursor = reinterpret_cast<const unsigned char*>(text);
    size_t remaining = std::strlen(text);
    std::string line;
    int32_t line_width = 0;
    while (remaining > 0) {
        size_t codepoint_bytes = 1;
        const bool line_break = cursor[0] == '\n' || cursor[0] == '\r';
        const int32_t width = line_break ? 12 : codepoint_width(cursor, remaining, &codepoint_bytes);
        if (line_break) codepoint_bytes = 1;
        const bool has_more = remaining > codepoint_bytes;
        const int32_t ellipsis_width = 24;
        if (line_width + width + (has_more ? ellipsis_width : 0) > max_width) {
            while (!line.empty() && line_width + ellipsis_width > max_width) {
                const auto* bytes = reinterpret_cast<const unsigned char*>(line.data());
                size_t offset = 0;
                size_t last_start = 0;
                int32_t last_width = 0;
                while (offset < line.size()) {
                    last_start = offset;
                    size_t length = 1;
                    last_width = codepoint_width(bytes + offset, line.size() - offset, &length);
                    offset += length;
                }
                line.resize(last_start);
                line_width -= last_width;
            }
            if (line_width + ellipsis_width <= max_width) {
                line.append(kEllipsis);
                line_width += ellipsis_width;
            }
            break;
        }
        if (line_break) line.push_back(' ');
        else line.append(reinterpret_cast<const char*>(cursor), codepoint_bytes);
        line_width += width;
        cursor += codepoint_bytes;
        remaining -= codepoint_bytes;
    }
    if (!line.empty()) result.lines.push_back(std::move(line));
    result.estimated_width = line_width;
    return result;
}

inline int32_t right_row_height(size_t max_description_lines) {
    if (max_description_lines == 0) return kMinimumRightRowHeight;
    const int32_t required = kDescriptionTop +
        static_cast<int32_t>(max_description_lines) * kTextLineAdvance + 8;
    return required > kMinimumRightRowHeight ? required : kMinimumRightRowHeight;
}

inline RowTextLayout row_text_layout(bool has_description, int32_t row_height) {
    if (has_description) return {8, kDescriptionTop, true};
    return {(row_height - kTextGlyphHeight) / 2, 0, false};
}

inline int32_t toggle_top(int32_t row_height) {
    return (row_height - kToggleHeight) / 2;
}

inline int32_t right_control_left(int32_t row_width) {
    return row_width - kToggleWidth - kRightControlRightInset;
}

inline int32_t description_width_for_toggle(int32_t toggle_left) {
    return toggle_left - 16 - kTextColumnGap;
}

struct RowPaintPlan {
    enum class Operation : uint8_t { kSelectedFill, kDivider };
    Operation operations[2];
    uint8_t operation_count;
    contracts::UiRect divider;
};

inline RowPaintPlan row_paint_plan(int64_t x, int64_t y, int32_t width, int32_t height,
                                   bool selected) {
    const contracts::UiRect divider{x, y + height - kDividerThickness, width,
                                    kDividerThickness};
    return selected
        ? RowPaintPlan{{RowPaintPlan::Operation::kSelectedFill,
                        RowPaintPlan::Operation::kDivider}, 2, divider}
        : RowPaintPlan{{RowPaintPlan::Operation::kDivider,
                        RowPaintPlan::Operation::kDivider}, 1, divider};
}

inline bool use_item_description(const char* description) {
    return description != nullptr && description[0] != '\0';
}

inline const char* row_description(const contracts::SettingsItemView& item) {
    return item.description;
}

// 右栏行号（该类别内按目录 items 顺序计数）→ 目录全局 item 下标；越界返回 -1。
inline int row_to_item_index(const contracts::SettingsItemView* items, uint32_t item_count,
                             const char* category_id, uint32_t row) {
    if (items == nullptr || category_id == nullptr) return -1;
    uint32_t seen = 0;
    for (uint32_t i = 0; i < item_count; ++i) {
        if (std::strcmp(items[i].category_id, category_id) != 0) continue;
        if (seen == row) return static_cast<int>(i);
        ++seen;
    }
    return -1;
}

// 某类别在目录中的条目数。
inline int category_item_count(const contracts::SettingsItemView* items, uint32_t item_count,
                               const char* category_id) {
    if (items == nullptr || category_id == nullptr) return 0;
    int count = 0;
    for (uint32_t i = 0; i < item_count; ++i) {
        if (std::strcmp(items[i].category_id, category_id) == 0) ++count;
    }
    return count;
}

// 开关目标值：0↔1；非 0 一律视为开。
inline int32_t next_toggle_value(int32_t current) { return current == 0 ? 1 : 0; }

// A toggle is writable only when both authoritative sources are known and all transient
// gesture/request work has settled. owner_enable is presentation metadata, not availability.
inline bool toggle_enabled(bool has_widget, bool confirmed_valid, bool availability_valid,
                           bool available, bool gesture_requested, uint64_t request_id,
                           bool request_queued) {
    return has_widget && confirmed_valid && availability_valid && available &&
           !gesture_requested && request_id == 0 && !request_queued;
}

// 动作 invoke_action 失败码 → 用户文案。
inline const char* action_error_text(int32_t code) {
    switch (code) {
        case kQolErrConflict: return "操作冲突：设置目录已更新，请返回后重进";
        case kQolErrNotReady: return "操作未就绪：所属功能当前不可用";
        case kQolErrBlocked: return "操作被阻止：请先关闭其他界面";
        case kQolErrNotFound: return "入口不存在";
        case kQolErrUnsupported: return "不支持的操作";
        case kQolErrInvalidArgument: return "参数无效";
        case kQolErrCancelled: return "操作已取消";
        case kQolErrTimeout: return "操作超时";
        case kQolErrInternal: return "内部错误";
        default: return "操作失败";
    }
}

// 设置请求入队失败码 → 用户文案。
inline const char* request_error_text(int32_t code) {
    switch (code) {
        case kQolErrNotReady: return "配置尚未就绪";
        case kQolErrBlocked: return "请求队列已满，请稍后再试";
        case kQolErrNotFound: return "该配置键不可写";
        case kQolErrInvalidArgument: return "配置参数无效";
        default: return "保存请求失败";
    }
}

// 成功回执的 apply 态 → 内部状态文案；设置页不将正常保存状态绘制到错误区。
inline const char* apply_pending_text(contracts::SettingsConfigApplyState apply) {
    switch (apply) {
        case contracts::SettingsConfigApplyState::kPendingFrame: return "已保存，生效中";
        case contracts::SettingsConfigApplyState::kPlatformPending: return "已保存，生效中";
        case contracts::SettingsConfigApplyState::kRejected: return "设置被拒绝";
        case contracts::SettingsConfigApplyState::kNotSubmitted:
        case contracts::SettingsConfigApplyState::kApplied:
        case contracts::SettingsConfigApplyState::kUnchanged:
            return "";
    }
    return "";
}

}  // namespace qol::features::core_ui::settings_model
