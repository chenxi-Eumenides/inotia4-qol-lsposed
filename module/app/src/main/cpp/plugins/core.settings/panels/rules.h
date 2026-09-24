#pragma once

// 设置页分页/项过滤纯逻辑（ARCHITECTURE.md §7.1 core.ui；无游戏依赖，host 可测）。
// 分页算法唯一实现：settings_geometry.inc 只做薄封装，避免同一规则出现两份实现。
//
// 页模型：条目声明逻辑页码（`page`，<=0 不参与分页）；物理页 = 对用到的逻辑页升序去重后的下标，
// 空逻辑页自动压缩。每页容量 = 列数 × 可见行数。

namespace qol::features::core_ui::panels_rules {

struct Item {
    const char* key;    // 空串 / nullptr = 非开关入口（动作项，如「存档备份」）
    const char* label;  // 展示文本
    int page;           // 逻辑页码；<=0 表示不参与分页
};

constexpr int kColumnCount = 2;
constexpr int kVisibleRows = 4;
constexpr int kPageCapacity = kColumnCount * kVisibleRows;
constexpr int kMaxItems = 32;
constexpr int kMaxPages = 16;

// 收集用到的逻辑页，升序去重写入 pages；返回页数。
inline int collect_logical_pages(const Item* items, int count, int* pages, int capacity) {
    if (items == nullptr || pages == nullptr || count <= 0 || capacity <= 0) return 0;
    int pages_count = 0;
    for (int i = 0; i < count; ++i) {
        const int logical_page = items[i].page;
        if (logical_page <= 0) continue;
        int pos = 0;
        while (pos < pages_count && pages[pos] < logical_page) ++pos;
        if (pos < pages_count && pages[pos] == logical_page) continue;
        if (pages_count >= capacity) continue;
        for (int j = pages_count; j > pos; --j) pages[j] = pages[j - 1];
        pages[pos] = logical_page;
        ++pages_count;
    }
    return pages_count;
}

inline int page_count(const Item* items, int count) {
    int pages[kMaxPages] = {};
    return collect_logical_pages(items, count, pages, kMaxPages);
}

inline int logical_page_at(const Item* items, int count, int physical_page) {
    if (physical_page < 0) return -1;
    int pages[kMaxPages] = {};
    const int pages_count = collect_logical_pages(items, count, pages, kMaxPages);
    if (physical_page >= pages_count) return -1;
    return pages[physical_page];
}

inline int item_physical_page(const Item* items, int count, int index) {
    if (items == nullptr || index < 0 || index >= count) return -1;
    const int logical_page = items[index].page;
    if (logical_page <= 0) return -1;
    int pages[kMaxPages] = {};
    const int pages_count = collect_logical_pages(items, count, pages, kMaxPages);
    for (int i = 0; i < pages_count; ++i) {
        if (pages[i] == logical_page) return i;
    }
    return -1;
}

// 动作项（无开关 key）：当前仅「存档备份」。
inline bool item_is_action(const Item* items, int count, int index) {
    if (items == nullptr || index < 0 || index >= count) return false;
    return items[index].key == nullptr || items[index].key[0] == 0;
}

// 当前物理页第 cell 格对应的条目下标；空格 / 越界返回 -1。
inline int item_index_at_cell(const Item* items, int count, int physical_page, int cell) {
    if (cell < 0 || cell >= kPageCapacity) return -1;
    const int logical_page = logical_page_at(items, count, physical_page);
    if (logical_page < 0) return -1;
    int local_cell = 0;
    for (int i = 0; i < count; ++i) {
        if (items[i].page != logical_page) continue;
        if (local_cell == cell) return i;
        ++local_cell;
    }
    return -1;
}

// 条目在给定物理页内的格序；不在该页 / 越界返回 -1。
inline int item_cell_at_page(const Item* items, int count, int index, int physical_page) {
    if (items == nullptr || index < 0 || index >= count) return -1;
    if (item_physical_page(items, count, index) != physical_page) return -1;
    const int logical_page = logical_page_at(items, count, physical_page);
    if (logical_page < 0) return -1;
    int cell = 0;
    for (int i = 0; i < index; ++i) {
        if (items[i].page == logical_page) ++cell;
    }
    return cell < kPageCapacity ? cell : -1;
}

// 同步页码：空表压成 1 页 0 下标；超界钳到末页；负数归 0。结果写回两个出参。
inline void sync_page_state(const Item* items, int count, int* page_index, int* total_pages) {
    if (page_index == nullptr || total_pages == nullptr) return;
    int total = page_count(items, count);
    if (total <= 0) {
        total = 1;
        *page_index = 0;
    } else if (*page_index >= total) {
        *page_index = total - 1;
    } else if (*page_index < 0) {
        *page_index = 0;
    }
    *total_pages = total;
}

}  // namespace qol::features::core_ui::panels_rules
