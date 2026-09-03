#pragma once

#include "game_access.h"

#include <chrono>
#include <thread>

inline uint32_t popup_stack_count() {
    if (g_popup_stack == nullptr) return 0;
    return *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(g_popup_stack) + 8);
}

inline bool wait_for_popup_stack_empty() {
    constexpr int kMaxWaitMs = 1000;
    for (int waited = 0; waited < kMaxWaitMs; waited += 16) {
        if (popup_stack_count() == 0) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    return popup_stack_count() == 0;
}
