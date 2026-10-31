#include "consume_item.h"

uint32_t native_equip_sell_count(uint32_t field,
                                 const qol::contracts::QolItemCountApi* item_count) {
    // 原版 0x1261c4 的 b 段直读 + clamp（证据见头注释）：只解释低七位视图，
    // b==0 或 b>99 一律回退 1（装备 marker 100 与 b∈[100,127] 的误判区间由此兜底）。
    // 通过 facade 先解码，再按原版范围判断，避免在消费者复制字段布局。
    if (item_count == nullptr || item_count->canonical_from_field == nullptr) return 1u;
    uint32_t canonical = 0;
    if (!qol::contracts::succeeded(item_count->canonical_from_field(field, &canonical))) {
        return 1u;
    }
    const uint32_t b = canonical % 128u;
    return (b >= 1u && b <= 99u) ? b : 1u;
}

VanillaSellRoute vanilla_sell_route(int32_t bag, bool count_encoded,
                                    uint32_t canonical_count, bool button_dry_run) {
    // 只有实际使用 S2 数量表示的物品由模块接管；原版可表示物品与任务袋透传。
    if (bag == qol::contracts::kQolTaskBagIndex || !count_encoded || canonical_count <= 127u) {
        return VanillaSellRoute::kBackup;
    }
    // 按钮预演只回填展示金额，绝不结算；弹窗 OK 才真实接管。
    return button_dry_run ? VanillaSellRoute::kPreview : VanillaSellRoute::kTakeover;
}

bool vanilla_sell_detail_context_matches(
    const qol::contracts::QolItemDetailContext& preview,
    const qol::contracts::QolItemDetailContext& confirmation) {
    return preview.desc_type == 2 && confirmation.desc_type == 2 &&
           preview.bag == confirmation.bag && preview.slot == confirmation.slot &&
           preview.item.slot != 0 && preview.item.generation != 0 &&
           preview.item.slot == confirmation.item.slot &&
           preview.item.generation == confirmation.item.generation;
}

uint32_t stage4_make_item_writeback_count(int32_t category, int32_t arg2, int32_t flag) {
    // MakeItem 的 arg2 是静态表查找/品质参数而非数量（证据见头注释）；产物数量
    // 由原版 CAL 公式写点生成且 ≤99（b 写即全量）。任何入参都不构成回写依据，
    // 恒 0 = wrapper 纯透传（fail-closed：拿不到可信数量源就不写，R-46/R-49）。
    (void)category;
    (void)arg2;
    (void)flag;
    return 0;
}

int stage4_equip_item(void* character, int32_t bag, int32_t slot, int32_t equip_slot,
                      Stage4ItemAt item_at, Stage4IdentifyItem identify,
                      Stage4EquipExtension extension_equip,
                      Stage4EquipBackup backup,
                      Stage4ItemAt extension_item_at) {
    if (backup == nullptr) return 0;
    void* item = item_at == nullptr ? nullptr : item_at(bag, slot);
    // 扩展视图下原版把控件 index 当 INVEN 坐标读，而 INVEN 全程真实（扩展
    // 物品只投影到控件不进 INVEN），源槽会读出 null。此时坐标与扩展袋槽
    // 1:1 对应，由带视图门禁的 extension_item_at 从扩展逻辑物化兜底，让
    // identify 命中后走扩展装备路径。
    if (item == nullptr && extension_item_at != nullptr) {
        item = extension_item_at(bag, slot);
    }
    int32_t module_bag = -1;
    int32_t module_slot = -1;
    if (item != nullptr && identify != nullptr && identify(item, &module_bag, &module_slot) &&
        extension_equip != nullptr) {
        return extension_equip(character, item, module_bag, module_slot, equip_slot) ? 1 : 0;
    }
    return backup(character, bag, slot, equip_slot);
}

int stage4_put_jewel(void* equip_item, void* jewel_item, Stage4IdentifyItem identify,
                     Stage4JewelBackup backup, Stage4JewelExtension extension_put) {
    if (backup == nullptr) return 3;
    int32_t bag = -1;
    int32_t slot = -1;
    if (identify == nullptr || !identify(jewel_item, &bag, &slot)) {
        return backup(equip_item, jewel_item);
    }
    int32_t equip_bag = -1;
    int32_t equip_slot = -1;
    if (extension_put != nullptr && identify(equip_item, &equip_bag, &equip_slot)) {
        return extension_put(equip_item, jewel_item, backup);
    }
    return backup(equip_item, jewel_item);
}

int stage4_unequip_item_to_inven(void* character, int32_t equip_slot,
                                 Stage4UnequipBackup backup,
                                 Stage4UnequipExtension extension_adopt,
                                 bool& recursive_guard) {
    if (backup == nullptr) return 0;
    if (recursive_guard) return backup(character, equip_slot);
    recursive_guard = true;
    const int original = backup(character, equip_slot);
    recursive_guard = false;
    if (original != 0) return original;
    return extension_adopt != nullptr && extension_adopt(character, equip_slot) ? 1 : 0;
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
