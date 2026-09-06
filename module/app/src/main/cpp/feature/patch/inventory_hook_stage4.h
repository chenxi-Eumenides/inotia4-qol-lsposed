#pragma once

#include <cstddef>
#include <cstdint>

using Stage4HaveBackup = int (*)(int32_t);
using Stage4CountBackup = int (*)(int32_t);
using Stage4EmptyBackup = int (*)(int32_t, int32_t);
using Stage4CountExtension = int (*)(int32_t);
using Stage4EmptyExtension = bool (*)(int32_t, int32_t);

int stage4_have_item(int32_t category, Stage4HaveBackup backup,
                     Stage4CountExtension extension_count, bool& recursive_guard);
int stage4_get_item_count(int32_t category, Stage4CountBackup backup,
                          Stage4CountExtension extension_count, bool& recursive_guard);
int stage4_is_having_empty_slot(int32_t needed, int32_t include_task_bag,
                                Stage4EmptyBackup backup,
                                Stage4EmptyExtension extension_has_empty,
                                bool& recursive_guard);

using Stage4IdentifyItem = bool (*)(void* item, int32_t* bag, int32_t* slot);
using Stage4ConsumeExtension = bool (*)(void* item);
using Stage4ConsumeBackup = void (*)(void* item);
using Stage4RemoveExtension = bool (*)(void* item);
using Stage4RemoveBackup = int (*)(void* item);

void stage4_consume_item(void* item, Stage4IdentifyItem identify,
                         Stage4ConsumeExtension extension_consume,
                         Stage4ConsumeBackup backup, bool& recursive_guard);
int stage4_remove_item(void* item, Stage4IdentifyItem identify,
                       Stage4RemoveExtension extension_remove,
                       Stage4RemoveBackup backup, bool& recursive_guard);

using Stage4ItemAt = void* (*)(int32_t bag, int32_t slot);
using Stage4EquipExtension = bool (*)(void* item, int32_t bag, int32_t slot,
                                      int32_t equip_slot);
using Stage4EquipBackup = int (*)(void* character, int32_t bag, int32_t slot,
                                  int32_t equip_slot);
int stage4_equip_item(void* character, int32_t bag, int32_t slot, int32_t equip_slot,
                      Stage4ItemAt item_at, Stage4IdentifyItem identify,
                      Stage4EquipExtension extension_equip,
                      Stage4EquipBackup backup);

using Stage4JewelBackup = int (*)(void* equip_item, void* jewel_item);
using Stage4JewelExtension = int (*)(void* equip_item, void* jewel_item,
                                     Stage4JewelBackup backup);
int stage4_put_jewel(void* equip_item, void* jewel_item, Stage4IdentifyItem identify,
                     Stage4JewelBackup backup, Stage4JewelExtension extension_put);

using Stage4HookInstall = int (*)(void* target, void* replacement, void** backup);
using Stage4HookUninstall = int (*)(void* target);
struct Stage4HookSpec {
    void* target;
    void* replacement;
    void** backup;
};
bool stage4_install_transaction(const Stage4HookSpec* hooks, std::size_t count,
                                Stage4HookInstall install,
                                Stage4HookUninstall uninstall);
