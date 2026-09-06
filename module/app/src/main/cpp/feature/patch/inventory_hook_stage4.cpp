#include "inventory_hook_stage4.h"

int stage4_have_item(int32_t category, Stage4HaveBackup backup,
                     Stage4CountExtension extension_count, bool& recursive_guard) {
    if (recursive_guard) return backup == nullptr ? 0 : backup(category);
    recursive_guard = true;
    const int original = backup == nullptr ? 0 : backup(category);
    const int result = original != 0
        ? original
        : (extension_count == nullptr ? 0 : (extension_count(category) > 0 ? 1 : 0));
    recursive_guard = false;
    return result;
}

int stage4_get_item_count(int32_t category, Stage4CountBackup backup,
                          Stage4CountExtension extension_count, bool& recursive_guard) {
    if (recursive_guard) return backup == nullptr ? 0 : backup(category);
    recursive_guard = true;
    const int original = backup == nullptr ? 0 : backup(category);
    const int extension = extension_count == nullptr ? 0 : extension_count(category);
    recursive_guard = false;
    return original + extension;
}

int stage4_is_having_empty_slot(int32_t needed, int32_t include_task_bag,
                                Stage4EmptyBackup backup,
                                Stage4EmptyExtension extension_has_empty,
                                bool& recursive_guard) {
    if (needed <= 0) return 0;
    if (recursive_guard) {
        return backup == nullptr ? 0 : backup(needed, include_task_bag);
    }
    recursive_guard = true;
    const int original = backup == nullptr ? 0 : backup(needed, include_task_bag);
    const int result = original != 0
        ? original
        : (extension_has_empty == nullptr || !extension_has_empty(needed, include_task_bag) ? 0 : 1);
    recursive_guard = false;
    return result;
}

void stage4_consume_item(void* item, Stage4IdentifyItem identify,
                         Stage4ConsumeExtension extension_consume,
                         Stage4ConsumeBackup backup, bool& recursive_guard) {
    int32_t bag = -1;
    int32_t slot = -1;
    if (!recursive_guard && identify != nullptr && identify(item, &bag, &slot)) {
        if (extension_consume != nullptr) extension_consume(item);
        return;
    }
    if (backup == nullptr) return;
    if (recursive_guard) {
        backup(item);
        return;
    }
    recursive_guard = true;
    backup(item);
    recursive_guard = false;
}

int stage4_remove_item(void* item, Stage4IdentifyItem identify,
                       Stage4RemoveExtension extension_remove,
                       Stage4RemoveBackup backup, bool& recursive_guard) {
    int32_t bag = -1;
    int32_t slot = -1;
    if (!recursive_guard && identify != nullptr && identify(item, &bag, &slot) &&
        extension_remove != nullptr && extension_remove(item)) {
        return 1;
    }
    if (backup == nullptr) return 0;
    if (recursive_guard) return backup(item);
    recursive_guard = true;
    const int result = backup(item);
    recursive_guard = false;
    return result;
}

int stage4_equip_item(void* character, int32_t bag, int32_t slot, int32_t equip_slot,
                      Stage4ItemAt item_at, Stage4IdentifyItem identify,
                      Stage4EquipExtension extension_equip,
                      Stage4EquipBackup backup) {
    if (backup == nullptr) return 0;
    void* item = item_at == nullptr ? nullptr : item_at(bag, slot);
    int32_t module_bag = -1;
    int32_t module_slot = -1;
    if (item != nullptr && identify != nullptr && identify(item, &module_bag, &module_slot) &&
        extension_equip != nullptr) {
        return extension_equip(item, module_bag, module_slot, equip_slot) ? 1 : 0;
    }
    return backup(character, bag, slot, equip_slot);
}

int stage4_put_jewel(void* equip_item, void* jewel_item, Stage4IdentifyItem identify,
                     Stage4JewelBackup backup, Stage4RemoveExtension extension_remove) {
    if (backup == nullptr) return 3;
    int32_t bag = -1;
    int32_t slot = -1;
    if (identify == nullptr || !identify(jewel_item, &bag, &slot)) {
        return backup(equip_item, jewel_item);
    }
    const int result = backup(equip_item, jewel_item);
    if (result == 0 && (extension_remove == nullptr || !extension_remove(jewel_item))) return 3;
    return result;
}

bool stage4_install_transaction(const Stage4HookSpec* hooks, std::size_t count,
                                Stage4HookInstall install,
                                Stage4HookUninstall uninstall) {
    if (hooks == nullptr || install == nullptr || uninstall == nullptr) return false;
    std::size_t installed = 0;
    for (; installed < count; ++installed) {
        const Stage4HookSpec& hook = hooks[installed];
        const int result = install(hook.target, hook.replacement, hook.backup);
        if (result != 0 || hook.backup == nullptr || *hook.backup == nullptr) {
            for (std::size_t index = installed + (result == 0 ? 1 : 0); index > 0; --index) {
                const Stage4HookSpec& rollback = hooks[index - 1];
                if (uninstall(rollback.target) == 0 && rollback.backup != nullptr) {
                    *rollback.backup = nullptr;
                }
            }
            return false;
        }
    }
    return true;
}
