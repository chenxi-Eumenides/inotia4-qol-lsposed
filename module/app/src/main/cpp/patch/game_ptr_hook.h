#pragma once

#include <cstdint>

// 函数指针包装（v0.5.18）：覆盖游戏内存中的函数指针字段，wrapper 内可回调原函数。
//
// 适用场景：游戏通过「ldr 读指针 + blr 间接调用」的函数指针字段（按钮 ExecuteProc、
// 控件 Proc/ControlProc、各类回调表等）。这类字段只存一个 8 字节函数地址，覆盖它
// 即可把调用目标换成模块自己的函数，wrapper 内再通过 call_orig 回调原函数，实现
// 「函数开头/结尾插入逻辑」的效果（before/after hook）。
//
// 与指令 patch（game_patch.h，改代码段立即数）互补：本工具只改数据段指针，
// 无需 mprotect / 指令缓存刷新，无 inline hook 的 trampoline lr 污染问题。
//
// 调用约定约束：wrapper 签名必须与被覆盖函数完全一致（参数寄存器 x0-x7、返回值、
// 被调用者保存寄存器 x19-x28、16 字节栈对齐），否则寄存器/栈错乱。
struct PtrHook {
    void** slot = nullptr;  // 函数指针字段地址（游戏内存）
    void* orig = nullptr;   // 原函数地址（覆盖前保存，供 call_orig 回调）

    // 把 slot_addr 指向的函数指针覆盖为 replacement，保存原地址到 orig。
    bool install(void* slot_addr, void* replacement) {
        if (slot_addr == nullptr || replacement == nullptr) return false;
        slot = static_cast<void**>(slot_addr);
        orig = *slot;
        *slot = replacement;
        return true;
    }

    // 还原原函数指针。
    void uninstall() {
        if (slot != nullptr && orig != nullptr) *slot = orig;
        slot = nullptr;
        orig = nullptr;
    }

    bool installed() const { return slot != nullptr && orig != nullptr; }

    template <typename R, typename... Args>
    bool install_typed(void* slot_addr, R (*replacement)(Args...)) {
        return install(slot_addr, reinterpret_cast<void*>(replacement));
    }

    // 回调原函数（wrapper 内使用）。R 为返回类型，Args 为参数类型。
    template <typename R = void, typename... Args>
    R call_orig(Args... args) const {
        return reinterpret_cast<R (*)(Args...)>(orig)(args...);
    }
};
