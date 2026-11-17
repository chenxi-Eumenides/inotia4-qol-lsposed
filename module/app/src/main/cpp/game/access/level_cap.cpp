#include "game/access/level_cap.h"

#include "game/access/game_access.h"

namespace qol::game {
namespace {

// CONSTBASE_pData GOT 槽双层解引用：槽值 = pData 全局变量地址，再解引用得记录数组。
// 与 game/access/world.cpp 的 MAPINFOBASE 写法一致；访问前逐层检查可读性，
// 表未装载（槽为空/指向不可读内存）时返回 false。
int16_t* level_cap_data(char permission) {
    if (g_constbase_pdata_got == nullptr) return nullptr;
    auto* slot = reinterpret_cast<void**>(g_constbase_pdata_got);
    if (!game_memory_accessible(slot, sizeof(void*), 'r') || *slot == nullptr) return nullptr;
    auto* data = *reinterpret_cast<int16_t**>(*slot);
    if (data == nullptr || !game_memory_accessible(data, sizeof(int16_t), permission)) {
        return nullptr;
    }
    return data;
}

}  // namespace

bool level_cap_ready() {
    return level_cap_data('r') != nullptr;
}

bool level_cap_get(int32_t* out_cap) {
    if (out_cap == nullptr) return false;
    int16_t* data = level_cap_data('r');
    if (data == nullptr) return false;
    *out_cap = static_cast<int32_t>(data[0]);
    return true;
}

bool level_cap_set(int32_t cap) {
    // 等级字段 ch+0xe 是 u8，写大于 255 会让升级循环的 level+1 在写字节时回绕，
    // 因此入口只接受 1..255。
    if (cap < kLevelCapFieldMin || cap > kLevelCapFieldMax) return false;
    int16_t* data = level_cap_data('w');
    if (data == nullptr) return false;
    data[0] = static_cast<int16_t>(cap);
    return true;
}

}  // namespace qol::game
