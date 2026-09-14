#pragma once

#include <cstddef>
#include <cstdint>

struct NativeChoiceSpec {
    const char* const* items;
    int count;
    const char* title;
    int close_index;
    void (*on_select)(int index, void* user);
    void* user;
};

// 纯逻辑规则同时供 native_choice 实现和 host 测试使用，不触碰游戏内存。
namespace native_choice_detail {

constexpr int kMaxItems = 6;

inline bool valid_spec(const NativeChoiceSpec& spec) {
    if (spec.items == nullptr || spec.count < 1 || spec.count > kMaxItems) return false;
    if (spec.close_index < -1 || spec.close_index >= spec.count) return false;
    for (int i = 0; i < spec.count; ++i) {
        if (spec.items[i] == nullptr) return false;
    }
    return true;
}

inline bool can_open(bool active, const NativeChoiceSpec& spec) {
    return !active && valid_spec(spec);
}

inline bool is_close_index(int index, int close_index) {
    return close_index >= 0 && index == close_index;
}

inline bool address_in_range(uintptr_t address, uintptr_t begin, size_t size) {
    return size != 0 && address >= begin && address - begin < size;
}

}  // namespace native_choice_detail

bool native_choice_install_if_ready();
bool native_choice_open(const NativeChoiceSpec& spec);
bool native_choice_active();
void native_choice_close();
