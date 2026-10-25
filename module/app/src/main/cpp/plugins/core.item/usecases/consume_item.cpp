#include "inventory_hook_stage4.h"

#include <cassert>

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

int stage4_have_item_original_only(int32_t category, Stage4HaveBackup backup,
                                   bool& recursive_guard) {
    return stage4_have_item(category, backup, nullptr, recursive_guard);
}

int stage4_get_item_count_original_only(int32_t category, Stage4CountBackup backup,
                                        bool& recursive_guard) {
    return stage4_get_item_count(category, backup, nullptr, recursive_guard);
}

int stage4_get_cumulate_count(void* item, const qol::contracts::QolItemCountApi* item_count,
                              Stage4CumulateBackup backup) {
    if (item != nullptr && item_count != nullptr && item_count->effective != nullptr) {
        uint32_t count = 0;
        const QolResult result = item_count->effective(item, &count);
        assert(result.code != kQolErrInvalidArgument);
        if (qol::contracts::succeeded(result)) return static_cast<int>(count);
    }
    return backup == nullptr ? 0 : backup(item);
}

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
    if (bag == 5 || !count_encoded || canonical_count <= 127u) {
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

int stage4_is_having_empty_slot(int32_t needed, int32_t include_task_bag,
                                Stage4EmptyBackup backup,
                                Stage4EmptyExtension extension_has_empty,
                                bool& recursive_guard) {
    // 原版语义（0x103460 反汇编 0x10347c b.le → 0x1035d8 mov w0,#1）：
    // needed<=0（物品可全部叠进现有堆、无需新槽）返回 1 = 可放。必须放行，
    // 否则任务奖励等"全可叠"场景被误报背包已满（真机实证）。
    if (needed <= 0) return 1;
    if (recursive_guard) {
        return backup == nullptr ? 0 : backup(needed, include_task_bag);
    }
    recursive_guard = true;
    const int original = backup == nullptr ? 0 : backup(needed, include_task_bag);
    // 原版 csel 语义（0x103460 反汇编 0x103488/0x103490）：include_task_bag!=0 只查
    // 物理任务袋 5，==0 只查普通袋 0..4。扩展逻辑袋不属于任务袋域，任务袋查询的
    // 原版 0 结果不得被扩展空位改判为 1——否则上层（QUESTSYSTEM_CheckPrepare/
    // CheckReward 等）会误判任务袋有空间并继续，随后 SaveItem 按 class 路由到袋 5
    // 失败被 H-13 收编进扩展袋（R-53 边界漂移）。扩展兜底只回答普通袋域。
    const bool extension_applicable =
        extension_has_empty != nullptr && include_task_bag == 0;
    const int result = original != 0
        ? original
        : (extension_applicable && extension_has_empty(needed, include_task_bag) ? 1 : 0);
    recursive_guard = false;
    return result;
}

int stage4_is_having_empty_slot_original_only(int32_t needed, int32_t include_task_bag,
                                              Stage4EmptyBackup backup,
                                              bool& recursive_guard) {
    return stage4_is_having_empty_slot(needed, include_task_bag, backup, nullptr,
                                       recursive_guard);
}

bool stage4_consume_item(void* item, Stage4IdentifyItem identify,
                         Stage4ConsumeExtension extension_consume,
                         Stage4ConsumeBackup backup, bool& recursive_guard) {
    int32_t bag = -1;
    int32_t slot = -1;
    if (!recursive_guard && identify != nullptr && identify(item, &bag, &slot)) {
        return extension_consume != nullptr && extension_consume(item);
    }
    if (backup == nullptr) return false;
    if (recursive_guard) {
        backup(item);
        return true;
    }
    recursive_guard = true;
    backup(item);
    recursive_guard = false;
    return true;
}

int stage4_remove_item(void* item, Stage4IdentifyItem identify,
                       Stage4RemoveExtension extension_remove,
                       Stage4RemoveBackup backup, bool& recursive_guard) {
    int32_t bag = -1;
    int32_t slot = -1;
    if (!recursive_guard && identify != nullptr && identify(item, &bag, &slot)) {
        if (extension_remove == nullptr) return 0;
        // 扩展对象的释放失败必须停在扩展分支，不能再落入原版 backup
        // （确认使用期间 backup 不认识逻辑对象且可能破坏失败语义）。
        return extension_remove(item) ? 1 : 0;
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

Stage4RemoveDataPlan stage4_remove_item_data_plan(
    const qol::contracts::QolItemCountApi* item_count, const Stage4RemoveDataEntry* pre,
    const Stage4RemoveDataPost* post, int entry_count, int32_t count) {
    Stage4RemoveDataPlan plan;
    if (pre == nullptr || post == nullptr || entry_count <= 0 || count <= 0) return plan;
    if (item_count == nullptr || item_count->view_limit == nullptr ||
        item_count->field_from_canonical == nullptr) return plan;
    uint32_t count_limit = 0;
    if (!qol::contracts::succeeded(item_count->view_limit(1, &count_limit))) return plan;

    uint32_t sum_vanished = 0;
    for (int index = 0; index < entry_count; ++index) {
        if (post[index].item != pre[index].item && post[index].item != nullptr) {
            return Stage4RemoveDataPlan{};
        }
        if (post[index].item == nullptr) {
            sum_vanished += pre[index].pre_full;
        }
    }
    if (sum_vanished > static_cast<uint32_t>(count)) return Stage4RemoveDataPlan{};
    const uint32_t deleted_here = static_cast<uint32_t>(count) - sum_vanished;

    if (deleted_here == 0) return plan;

    int candidate_index = -1;
    for (int index = 0; index < entry_count; ++index) {
        if (post[index].item == nullptr) continue;

        const uint32_t pre_full = pre[index].pre_full;
        if (deleted_here >= pre_full) {
            if (post[index].post_raw != pre[index].pre_raw) return Stage4RemoveDataPlan{};
            continue;
        }

        const uint32_t remain = pre_full - deleted_here;
        const uint32_t pre_a_block = (pre_full / 128u) * 128u;
        const uint32_t expected_view = pre_a_block + (remain % 128u);
        bool b_only_shape = false;
        if (expected_view <= 999u) {
            uint32_t expected_raw = 0;
            const QolResult result = item_count->field_from_canonical(
                pre[index].pre_raw, expected_view, 1, &expected_raw);
            if (!qol::contracts::succeeded(result)) return Stage4RemoveDataPlan{};
            b_only_shape = post[index].post_raw == expected_raw;
        } else {
            // field_from_canonical 与 effective 读视图都在 count_limit 处 clamp；该窄带
            // 只有 pre_a=7 且 b 段 >103 时出现，故以 clamp 后的视图相等作等价判定。
            b_only_shape = post[index].post_view == count_limit;
        }

        if (b_only_shape) {
            if (deleted_here >= pre_full || remain >= pre_full || remain > count_limit) {
                return Stage4RemoveDataPlan{};
            }
            if (candidate_index >= 0) return Stage4RemoveDataPlan{};
            candidate_index = index;
        } else if (post[index].post_raw != pre[index].pre_raw) {
            return Stage4RemoveDataPlan{};
        }
    }

    if (candidate_index < 0) return plan;
    const uint32_t remain = pre[candidate_index].pre_full - deleted_here;
    if (remain == post[candidate_index].post_view) return plan;
    plan.correct = true;
    plan.entry_index = candidate_index;
    plan.remain = remain;
    return plan;
}

int stage4_remove_data_extension_shortfall(int total_before, int total_after,
                                           int requested) {
    if (requested <= 0) return 0;
    const int physical_removed = total_before - total_after;
    if (physical_removed <= 0) return requested;  // 原版未扣到任何物理堆
    if (physical_removed >= requested) return 0;  // 物理已足额
    return requested - physical_removed;
}
