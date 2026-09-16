// game/clock.h 实现：读取游戏帧计数。
// 本函数是 game 层唯一的帧计数实现；既有 game_system.cpp 的 data_frame_count() 在后续
// 收口时改为委托本函数。当前两者同源同语义：g_base + G_FRAME_COUNT_VMA 先取 GOT 槽，
// 再解引用读 u64 计数，g_base == 0 或指针为空返回 -1。

#include "game/clock.h"

#include "data/native/game_symbols.h"

#include <cstdint>

#if defined(__ANDROID__)
// Android 构建走正式声明（含其他符号解析入口）。
#include "game_access.h"
#else
// game_access.h 拉入 <jni.h> 且其内部 "game_symbols.h" 只在 NDK 包含路径下可解析，
// host 单测（仅 -I.）无法编译，属 Android-only 头；host 分支只声明本文件用到的 g_base，
// 类型与 game_access.h 的 extern uintptr_t g_base 一致。
extern uintptr_t g_base;
#endif

namespace qol::game {

int64_t frame_count() {
    if (g_base == 0) return -1;
    // 帧计数 GOT 槽（G_FRAME_COUNT_VMA）：先解引用取 u64 计数指针，再读计数。
    uintptr_t* slot = reinterpret_cast<uintptr_t*>(g_base + G_FRAME_COUNT_VMA);
    uint64_t* cnt = reinterpret_cast<uint64_t*>(*slot);
    return cnt != nullptr ? static_cast<int64_t>(*cnt) : -1;
}

}  // namespace qol::game
