#include "game/access/item_pool.h"

#include "game/access/game_access.h"
#include "game/symbols/game_symbols.h"

#include <cstring>

namespace qol::game {
namespace {

bool read_pointer(const void* address, void** out) {
    if (address == nullptr || out == nullptr) return false;
    if (!game_memory_accessible(address, sizeof(void*), 'r')) return false;
    std::memcpy(out, address, sizeof(void*));
    return true;
}

}  // namespace

bool item_pool_usage(uint32_t* out_used, uint32_t* out_capacity) {
    if (out_used == nullptr || out_capacity == nullptr) return false;
    *out_used = 0;
    *out_capacity = 0;
    if (g_base == 0) return false;

    // 两层：GOT 槽 → ITEMPOOL_pPool 变量地址 → 池基址。直接读变量地址与
    // ITEMPOOL_Allocate 0x10810c/0x108128 的取值路径一致；把 GOT 槽内值当池基址
    // 会扫描 libgame .bss（game_symbols.h 已登记该陷阱）。
    void* pool = nullptr;
    if (!read_pointer(reinterpret_cast<const void*>(g_base + ITEMPOOL_POOL_VAR_VMA), &pool) ||
        pool == nullptr) {
        return false;
    }
    constexpr size_t kPoolBytes =
        static_cast<size_t>(ITEMPOOL_CAPACITY) * ITEMPOOL_OBJECT_STRIDE;
    if (!game_memory_accessible(pool, kPoolBytes, 'r')) return false;

    const auto* base = static_cast<const volatile uint8_t*>(pool);
    uint32_t used = 0;
    for (size_t i = 0; i < ITEMPOOL_CAPACITY; ++i) {
        const uint8_t flags = base[i * ITEMPOOL_OBJECT_STRIDE + ITEMPOOL_USED_FLAG_OFFSET];
        if ((flags & 1u) != 0) ++used;
    }
    *out_used = used;
    *out_capacity = ITEMPOOL_CAPACITY;
    return true;
}

}  // namespace qol::game
