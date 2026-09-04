#pragma once

#include <cstdint>

// 游戏状态层：全局状态检测 + 跨域查询原语 + 跨域遍历原语（无构建/导航依赖）。

bool game_in_world();
int current_save_slot();
const char* ui_blocked();
int tutorial_state();
void tutorial_cancel();
const char* tutorial_block_error();

void* member_or_null(int role);
void* lead_member();
void* find_char_by_merc_slot(int slot);

enum class InventoryItemKind : uint8_t {
    kOriginal,
    kExtension,
};

struct InventoryItemRef {
    InventoryItemKind kind = InventoryItemKind::kOriginal;
    int bag = -1;
    int slot = -1;
    int category = 0;
    int count = 0;
    void* native_item = nullptr;
};

using InventoryItemFn = bool (*)(const InventoryItemRef& item, void* ctx);

int inventory_count();
int inventory_quantity(int category);
void* find_inventory_item(int category);
void* inventory_item_at(int bag, int slot);
bool find_inventory_item_ref(int category, InventoryItemRef* out);
bool inventory_item_ref_at(int bag, int slot, InventoryItemRef* out);
void for_each_inventory_item(InventoryItemFn fn, void* ctx);

using BagSlotFn = bool (*)(void* item, int bag, int slot, void* ctx);
void for_each_bag_slot(BagSlotFn fn, void* ctx);
bool pool_obj_valid(const uint8_t* obj);
