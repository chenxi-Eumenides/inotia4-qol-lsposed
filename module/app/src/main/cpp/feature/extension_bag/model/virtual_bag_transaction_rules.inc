// 原版删除函数无可靠返回值；跨域事务只能以重新读取源物理槽为空作为成功后置条件。
inline bool original_to_extension_source_slot_postcondition(void* expected_item,
                                                             void* actual_item) {
    return expected_item != nullptr && actual_item == nullptr;
}

inline bool valid_index(int index) {
    return index >= 0 && index < kBagCount;
}

inline bool valid_original_transaction_bag(int bag) {
    return bag >= 0 && bag < kOriginalTransactionBagCount;
}

inline bool valid_extension_logical_bag(int bag) {
    return bag >= kExtensionLogicalBagFirst && bag <= kExtensionLogicalBagLast;
}

inline int extension_internal_bag(int logical_bag) {
    return valid_extension_logical_bag(logical_bag)
               ? logical_bag - kExtensionLogicalBagFirst
               : -1;
}

inline int extension_logical_bag(int internal_bag) {
    return internal_bag >= 0 && internal_bag < kBagCount
               ? kExtensionLogicalBagFirst + internal_bag
               : -1;
}

inline bool valid_ext2ext_transfer_slots(int src_bag, int src_slot, int dst_bag,
                                         int dst_slot) {
    return valid_index(src_bag) && valid_index(dst_bag) && src_slot >= 0 &&
           src_slot < kSlotCount && dst_slot >= 0 && dst_slot < kSlotCount &&
           (src_bag != dst_bag || src_slot != dst_slot);
}

inline bool valid_transaction_domain(uint8_t direction, int src_bag, int src_slot,
                                      int dst_bag, int dst_slot) {
    if (src_slot < 0 || src_slot >= kSlotCount || dst_slot < 0 || dst_slot >= kSlotCount) {
        return false;
    }
    if (direction == kTransferOriginalToExtension) {
        return valid_original_transaction_bag(src_bag) && valid_index(dst_bag);
    }
    if (direction == kTransferExtensionToOriginal) {
        return valid_index(src_bag) && valid_original_transaction_bag(dst_bag);
    }
    return false;
}

// pending 域校验（进程内事务）：额外接受 ext→ext；journal v1 域校验不复用它，
// 因为 ext→ext 恒不创建 prepare journal（§4.1）。
inline bool valid_pending_transaction_domain(uint8_t direction, int src_bag, int src_slot,
                                             int dst_bag, int dst_slot) {
    if (direction == kTransferExtensionToExtension) {
        return valid_ext2ext_transfer_slots(src_bag, src_slot, dst_bag, dst_slot);
    }
    return valid_transaction_domain(direction, src_bag, src_slot, dst_bag, dst_slot);
}

inline bool valid_pending_transfer_domain(const PendingTransfer& pending) {
    return valid_pending_transaction_domain(pending.direction, pending.src_bag, pending.src_slot,
                                            pending.dst_bag, pending.dst_slot);
}

inline bool valid_pending_transfer_payload(const PendingTransfer& pending) {
    if (validate_serialized_payload_buffer(pending.payload.data(), pending.payload.size(),
                                           pending.payload_size) != PayloadValidation::kOk) {
        return false;
    }
    return pending.source_payload_size == 0 ||
           validate_serialized_payload_buffer(pending.source_payload.data(),
                                              pending.source_payload.size(),
                                              pending.source_payload_size) == PayloadValidation::kOk;
}

inline bool valid_capacity(int capacity) {
    return capacity >= 0 && capacity <= kMaxCapacity;
}

inline bool valid_type(int type) {
    return type >= static_cast<int>(BagType::kNone) &&
           type <= static_cast<int>(BagType::kLargeBackpack);
}

// 容量派生唯一入口：扩展袋容量由其已装备的背包物品 BagType 决定（ITEMSTATICBASE 镜像）。
inline uint8_t derive_capacity(int bag_type) {
    return valid_type(bag_type) ? kBagTypeCapacities[bag_type] : 0;
}

inline bool valid_payload(const Item& item) {
    return validate_serialized_payload_buffer(item.payload.data(), item.payload.size(),
                                              item.payload_size) == PayloadValidation::kOk;
}

inline uint32_t payload_count(const Item& item) {
    uint32_t v = 0;
    for (size_t i = 0; i < 4; ++i) {
        v |= uint32_t(item.payload[kPayloadCountOffset + i]) << (8 * i);
    }
    return v;
}

using CategoryStackCountPredicate = stack_codec::CountEncoding (*)(int category);

// S2 数量写入唯一入口（模块侧）：
// - 类别门控 fail-closed：仅 count-encoded（可堆叠）类别可写；宝石（bits18-23 选项位）、
//   袋容量（bits0-24）、装备 marker（bits25-31）等非可堆叠类别一律拒绝，绝不触碰
//   bits22-24（R-46）。
// - 进位/借位内建：count 为当前模式视图数量（启用态 0..999 / 关闭态 0..99，调用方
//   先按模式 clamp 与视图换算），写入经 effective_write_count（R-47 决策 b）：
//   enabled=true 全量 s2_write_count 拆段（a=count>>7、b=count&0x7F，b 溢出 127
//   自动进位到 a、a 借位自动落到 b+128）；enabled=false 只写 b 段、a（bits22-24）
//   与 bits0-21 原样保留。默认 enabled=true（canonical 全量语义），供身份归一化等
//   与模式无关的比较路径使用；调用方只做模式视图算术，不得自行拆写单个位段
//   （R-45/R-49）。
inline bool patch_payload_count(Item* item, uint32_t count,
                                CategoryStackCountPredicate is_count_encoded,
                                bool enabled = true) {
    if (item == nullptr || !valid_payload(*item)) return false;
    if (is_count_encoded == nullptr ||
        is_count_encoded(item->category) != stack_codec::CountEncoding::kEncoded) {
        std::fprintf(stderr,
                     "virtual_bag patch_payload_count ignored category=%d reason=not_count_encoded\n",
                     item->category);
        return false;
    }
    const uint32_t updated =
        stack_codec::effective_write_count(payload_count(*item), count, enabled);
    for (size_t i = 0; i < 4; ++i) {
        item->payload[kPayloadCountOffset + i] = static_cast<uint8_t>((updated >> (8 * i)) & 0xFF);
    }
    return true;
}

inline uint32_t payload_hash(const Item& item) {
    uint32_t h = 2166136261u;
    h ^= static_cast<uint32_t>(item.category);
    h *= 16777619u;
    h ^= static_cast<uint32_t>(item.count);
    h *= 16777619u;
    h ^= static_cast<uint32_t>(item.payload_size);
    h *= 16777619u;
    for (size_t i = 0; i < item.payload_size; ++i) {
        h ^= item.payload[i];
        h *= 16777619u;
    }
    return h;
}

// 合并数量：入参为当前模式视图数量（启用态 canonical 128a+b / 关闭态 b 视图），
// 相加后按堆叠上限（stack_limit_enabled ? 999 : 99）clamp；clamp 结果经
// patch_payload_count 的模式感知写回（启用态全量拆段进位内建、关闭态只写 b
// 保留 a，R-47 决策 b）。
inline uint32_t merge_count(int existing_count, int added_count, bool extended) {
    const int64_t sum = static_cast<int64_t>(existing_count) + added_count;
    if (sum <= 0) return 0;
    return stack_codec::effective_clamp(static_cast<uint32_t>(sum), extended);
}

// ---- 堆叠身份判据（VM-39 拾取不并入扩展堆的缺陷根因修复）----
// SAVE_SaveItem@0x1274f0 序列化布局（payload = u8 长度前缀 + 18B 头 + 4B×N 词缀）：
//   payload[0]      长度前缀（= size-1，结构字段）
//   payload[1..8]   u64 UID（item+0x00，APPINFO_AllocateItemUID 产物）
//   payload[9..10]  u16 I_TYPE 位域（bit2-5 稀有度 + bit6-15 类别）
//   payload[11..14] u32 数量位域（bits22-31，S2 a/b 段）
//   payload[15]     u8 I_MAGIC_RATE
//   payload[16]     u8 I_SOCKET（镶嵌）
//   payload[17..18] u16 I_ENCHANT
//   payload[19..]   词缀链（每节点 u16 编码 + s16 值，步长 4）
//
// 原版「同类可堆叠」身份判据（反汇编基线，2026-09-11 冻结）：
//   INVEN_FindSaveSlot@0x103b48-0x103b9c：先读 ITEMCLASSBASE[category]+6 bit0
//     （可堆叠位，判据同 ITEM_GetCumulateCount@0x1060e0-0x1060f8），再比较
//     I_TYPE bit6-15（类别，0x103b48-0x103b74），两堆 ITEM_GetCumulateCount 之和
//     ≤0x63（99）才选中；全程不比较 UID/词缀/镶嵌。
//   INVEN_SaveItemDirect@0x103c88-0x103cb0：同样只比较类别 + 可堆叠位。
//   INVEN_MoveItem@0x104a50-0x104a7c：同样只比较类别 + 可堆叠位。
// 因此 UID 与数量位绝不参与合并判据。模块为保护扩展 sidecar 的 payload 身份，
// 在类别之外把 I_TYPE 其余位、镶嵌、附魔与词缀链一并纳入比较（这些是堆叠身份
// 字段），仅排除 UID 与数量位两个每实例可变字段；绝不整份 memcmp（缺陷根因：
// 拾取对象与扩展堆 UID 不同 → 整份 memcmp 永不相等 → 永不合并）。
constexpr size_t kPayloadUidOffset = 1;        // payload 内 UID 偏移
constexpr size_t kPayloadUidSize = 8;          // UID 字节数（每实例可变，排除）
constexpr size_t kPayloadTypeOffset = kPayloadUidOffset + kPayloadUidSize;  // 9
constexpr size_t kPayloadTypeSize = 2;         // I_TYPE 位域字节数
constexpr size_t kPayloadCountSize = 4;        // 数量位域字节数
constexpr size_t kPayloadPostCountOffset = kPayloadCountOffset + kPayloadCountSize;  // 15

// 载荷身份比较（不含类别与可堆叠门控，见 mergeable_items）：
// 只比较 I_TYPE 位域 [9,11) 与数量位域之后的身份区 [15,size)（magic/socket/
// enchant/词缀链），排除长度前缀、UID [1,9) 与数量位域 [11,15)。词缀条数属于
// 身份（条数不同 = 身份不同），长度差异在身份区逐字节比较中体现，不做整份预拒。
inline bool mergeable_identity_equal(const Item& existing, const Item& source) {
    if (existing.payload_size < kPayloadHeaderSize ||
        source.payload_size < kPayloadHeaderSize) {
        return false;
    }
    if (std::memcmp(existing.payload.data() + kPayloadTypeOffset,
                    source.payload.data() + kPayloadTypeOffset, kPayloadTypeSize) != 0) {
        return false;
    }
    const size_t existing_tail =
        static_cast<size_t>(existing.payload_size) - kPayloadPostCountOffset;
    const size_t source_tail =
        static_cast<size_t>(source.payload_size) - kPayloadPostCountOffset;
    if (existing_tail != source_tail) return false;
    return existing_tail == 0 ||
           std::memcmp(existing.payload.data() + kPayloadPostCountOffset,
                       source.payload.data() + kPayloadPostCountOffset, existing_tail) == 0;
}

inline bool mergeable_items(const Item& existing, const Item& source,
                            CategoryStackCountPredicate is_count_encoded) {
    if (existing.category <= 0 || existing.count <= 0 || source.category <= 0 ||
        source.count <= 0 || existing.category != source.category) {
        return false;
    }
    if (is_count_encoded == nullptr ||
        is_count_encoded(existing.category) != stack_codec::CountEncoding::kEncoded ||
        is_count_encoded(source.category) != stack_codec::CountEncoding::kEncoded) {
        return false;
    }
    if (!valid_payload(existing) || !valid_payload(source)) return false;
    return mergeable_identity_equal(existing, source);
}

inline bool same_extension_bag_merge_allowed(int src_bag, int dst_bag) {
    return src_bag == dst_bag;
}

inline bool same_extension_bag_mergeable_items(const Item& existing, const Item& source,
                                                CategoryStackCountPredicate is_count_encoded) {
    return mergeable_items(existing, source, is_count_encoded);
}

// ---- adopt 合并计划（VM-39 缺陷修复，纯函数供 Host 断言）----
// INVEN_SaveItem 失败收编（virtual_bag_adopt_native_item）在新建扩展槽之前，
// 必须先尝试并入扩展袋已有同类堆：原版 SaveItem→FindSaveSlot 对物理袋自带
// 「同类堆自动合并」语义，adopt 替代的是同一入库动作的失败分支，因此合并
// 不受 move_merge_enabled（拖动合并开关）门控。判据与移动合并同源：
// mergeable_items（category + mergeable_identity_equal，UID/数量位不参与）+ 模式
// 视图总量 ≤ 上限（启用 999 / 关闭 99，R-47）。合并产物 canonical 与移动合并
// 一致：保留 existing payload、按模式视图相加后经 patch_payload_count 写回、
// descriptor.count 回填 s2 解码 canonical。
// 返回 false = 不合并（判据不符/超上限/门控失败），调用方走新建空槽。
inline bool adopt_merge_into(const Item& existing, const Item& source,
                             CategoryStackCountPredicate is_count_encoded,
                             bool limit_enabled, Item* merged_out) {
    if (merged_out == nullptr) return false;
    if (!same_extension_bag_mergeable_items(existing, source, is_count_encoded)) {
        return false;
    }
    const uint32_t existing_view =
        stack_codec::effective_view_count(static_cast<uint32_t>(existing.count),
                                          limit_enabled);
    const uint32_t source_view =
        stack_codec::effective_view_count(static_cast<uint32_t>(source.count),
                                          limit_enabled);
    const uint64_t total = static_cast<uint64_t>(existing_view) +
                           static_cast<uint64_t>(source_view);
    if (total > stack_codec::max_count(limit_enabled)) return false;
    Item merged = existing;
    if (!patch_payload_count(&merged,
                             merge_count(static_cast<int>(existing_view),
                                         static_cast<int>(source_view), limit_enabled),
                             is_count_encoded, limit_enabled)) {
        return false;
    }
    merged.count = static_cast<int>(stack_codec::s2_read_count(payload_count(merged)));
    *merged_out = merged;
    return true;
}

// ---- 入库落位计划（R-53 单一 owner 的纯函数 seam，供 Host 断言）----
// 拾取/收编（SaveItem backup 前合并 + backup 失败后收编）共用同一「命中后落位
// 驱动」：本纯函数只决定落位目标（并入既有堆 / 新建空槽 / 无空间 / 拒绝），
// 不触碰任何运行时状态（g_module_objects / 锁 / persist / refresh / free）。
// owner（virtual_bag_place_native_item）消费本计划后执行副作用。
// 判据单源：合并复用 adopt_merge_into（same_extension_bag_mergeable_items +
// 模式视图总量 ≤ 上限）；空槽扫描与既有 adopt 逐位一致（仅扫已装备袋、不设
// 容量上界——module_slot_is_assignable_locked 自身已覆盖身份/对象态）。
enum class PlaceOutcome : uint8_t {
    kNotApplicable,  // 调用方前置门控不符（null/禁用/已在模块槽）：保持原版
    kMerged,         // 并入既有扩展同类堆（两路径均可）
    kPlacedNewSlot,  // 新建扩展空槽（仅 allow_new_slot）
    kNoSpace,        // 无合并目标且无空槽
    kRejected,       // source 非法（serialize/类别/对象 generation 拒绝）
};

struct PlacePlan {
    PlaceOutcome outcome = PlaceOutcome::kNotApplicable;
    int bag = -1;
    int slot = -1;
    Item merged{};  // outcome==kMerged 时有效（已含 canonical 回填）
};

// 空槽判定的运行时注入点：owner 传 module_slot_is_assignable_locked（含对象/
// use/handle 身份态）；Host 不传时退化为 descriptor 空判定。
using SlotAssignablePredicate = bool (*)(int bag, int slot);

inline bool descriptor_slot_empty(const Item& item) {
    return item.category == 0 && item.count == 0 && item.payload_size == 0;
}

inline PlacePlan place_plan(
    const std::array<std::array<Item, kSlotCount>, kBagCount>& items,
    const std::array<uint8_t, kBagCount>& types,
    const std::array<uint8_t, kBagCount>& capacities, const Item& source,
    bool allow_new_slot, bool limit_enabled, CategoryStackCountPredicate is_count_encoded,
    SlotAssignablePredicate slot_assignable = nullptr) {
    PlacePlan plan{};
    if (source.category <= 0 || source.count <= 0 || !valid_payload(source)) {
        plan.outcome = PlaceOutcome::kRejected;
        return plan;
    }
    // 合并优先：与 adopt_merge_into 同扫描域（已装备袋、capacity∩kSlotCount）。
    for (int bag = 0; bag < kBagCount; ++bag) {
        if (types[bag] == 0) continue;
        const int capacity = capacities[bag];
        for (int slot = 0; slot < capacity && slot < kSlotCount; ++slot) {
            const Item& existing = items[bag][slot];
            if (existing.category <= 0 || existing.count <= 0) continue;
            Item merged{};
            if (!adopt_merge_into(existing, source, is_count_encoded, limit_enabled, &merged)) {
                continue;
            }
            plan.outcome = PlaceOutcome::kMerged;
            plan.bag = bag;
            plan.slot = slot;
            plan.merged = merged;
            return plan;
        }
    }
    if (!allow_new_slot) {
        plan.outcome = PlaceOutcome::kNoSpace;
        return plan;
    }
    // 空槽扫描：与 adopt 现状同域（kSlotCount 全扫；运行时谓词保证 assignable）。
    for (int bag = 0; bag < kBagCount; ++bag) {
        if (types[bag] == 0) continue;
        for (int slot = 0; slot < kSlotCount; ++slot) {
            const bool assignable = slot_assignable != nullptr
                ? slot_assignable(bag, slot)
                : descriptor_slot_empty(items[bag][slot]);
            if (assignable) {
                plan.outcome = PlaceOutcome::kPlacedNewSlot;
                plan.bag = bag;
                plan.slot = slot;
                return plan;
            }
        }
    }
    plan.outcome = PlaceOutcome::kNoSpace;
    return plan;
}

// 新建槽 descriptor 落位/回滚纯模型（owner 复用，Host 断言失败路径一致性）：
// generation_ok=true → 写出 source 的 payload/category/count（保留 previous 的
// 其余字节）；false → 原样返回 previous，即 descriptor 与对象表保持失败前态。
inline Item apply_new_slot_descriptor(const Item& previous, const Item& source,
                                      bool generation_ok) {
    if (!generation_ok) return previous;
    Item next = previous;
    next.payload = source.payload;
    next.payload_size = source.payload_size;
    next.category = source.category;
    next.count = source.count;
    return next;
}

inline bool extension_swap_tokens_available(bool source_active, bool source_pending_release,
                                            bool destination_active,
                                            bool destination_pending_release) {
    return !source_active && !source_pending_release && !destination_active &&
           !destination_pending_release;
}

inline bool projection_drop_context_matches(int view_bag, int source_bag, bool root_matches,
                                            bool source_control_matches,
                                            bool destination_control_matches) {
    return view_bag == source_bag && root_matches && source_control_matches &&
           destination_control_matches;
}

inline uint32_t handle_if_object_matches(const void* expected_object, const void* slot_object,
                                          uint32_t current_handle) {
    return expected_object == slot_object ? current_handle : 0;
}

inline bool extension_source_protection_required(bool extension_bag_enabled,
                                                 bool move_merge_enabled) {
    return extension_bag_enabled || move_merge_enabled;
}

inline bool projected_release_owns_event(uint64_t event, bool session_has_projected_item) {
    return event == 0x18 && session_has_projected_item;
}

// ---- ext→orig 事务的投影宿主守卫（VM-40 缺陷修复，纯函数供 Host 断言）----
// 1) 宿主容量字恢复：INVEN_GetBagSize 与 INVEN_SaveItemOnEmpty 的空槽扫描都读
//    袋对象 +0x10 的容量位（bit0..24）；扩展投影安装（install_module_view_locked）
//    会把宿主袋容量字临时替换为扩展容量。目标袋 == 投影宿主袋时，ext→orig 事务
//    的满包预检与原版插入都按被污染容量判定：扩展容量 < 真实容量时误判"已满"
//    拒绝（真机实证：无法移入原版袋 0，袋 1 正常——袋 1 非宿主不受污染），
//    > 真实容量时越窗扫描。事务期必须恢复真实容量（纯内存写），任何出口写回
//    投影值（刷新由调用方既有路径完成）。
inline bool ext2orig_requires_host_capacity_restore(bool view_installed,
                                                    int target_bag,
                                                    int window_original_bag) {
    return view_installed && window_original_bag >= 0 && target_bag == window_original_bag;
}

// 2) 显示袋切换：投影安装中 CUR_BAG（direct/GOT 成对写，R-27）是投影宿主身份
//    的一部分；ext→orig 的提交/失败路径不得切换，否则宿主记录
//    （g_module_window_original_bag）与 CUR_BAG 分叉——下一次 install 会把
//    用户目标袋误选为宿主：原版袋容量字被写扩展容量、扩展内容投影上去
//    （真机实证："视图变成原版袋 0 容量、内容却是扩展袋 0，物品未真正移动"）。
//    非投影（API）路径保留切到目标袋的既有显示语义。
inline bool ext2orig_should_switch_display_bag(bool view_installed) {
    return !view_installed;
}

constexpr size_t kPhysicalInventorySnapshotSlots = 6u * kSlotCount;

inline bool physical_inventory_snapshot_changed(
    const std::array<void*, kPhysicalInventorySnapshotSlots>& before,
    const std::array<void*, kPhysicalInventorySnapshotSlots>& after) {
    return before != after;
}

inline bool item_matches_payload(const Item& item, const PendingTransfer& pending) {
    return pending.payload_size > 0 && item.payload_size == pending.payload_size &&
           std::memcmp(item.payload.data(), pending.payload.data(), pending.payload_size) == 0;
}

// 纯判定逻辑（不涉及 native 调用，host 测试覆盖）：
// 以「状态中对应槽的载荷是否等于 pending 载荷」区分事务已提交/未提交，产出确定性恢复动作。
inline RecoveryAction recovery_action(const State& state, const PendingTransfer& pending) {
    if (!pending.valid) return RecoveryAction::kNone;
    if (!valid_pending_transfer_domain(pending) || !valid_pending_transfer_payload(pending)) {
        return RecoveryAction::kRollback;
    }
    if (pending.direction == kTransferOriginalToExtension) {
        return item_matches_payload(state.items[pending.dst_bag][pending.dst_slot], pending)
                   ? RecoveryAction::kComplete
                   : RecoveryAction::kRollback;
    }
    if (pending.direction == kTransferExtensionToExtension) {
        return item_matches_payload(state.items[pending.dst_bag][pending.dst_slot], pending)
                   ? RecoveryAction::kComplete
                   : RecoveryAction::kRollback;
    }
    return item_matches_payload(state.items[pending.src_bag][pending.src_slot], pending)
               ? RecoveryAction::kRollback
               : RecoveryAction::kComplete;
}

// ---- P4.4 进程内五态事务（唯一协调入口的纯模型核心，host 注入测试覆盖）----
// prepared → pending-recorded → logical-state-updated → original-state-updated → committed。
// 状态严格单向推进；失败只允许按阶段回滚（还原逻辑槽与 pending），不做猜测性逆转。
// ext→ext 恒无 original 阶段：不调用原版库存移动，不创建 prepare journal（§4.1）。

enum class TxnStage : uint8_t {
    kPrepared = 0,
    kPendingRecorded,
    kLogicalUpdated,
    kOriginalUpdated,
    kCommitted,
};

// prepared 阶段收集的不可变事务快照；三条移动路径只负责构造它，推进与回滚共享唯一实现。
struct TransactionContext {
    uint8_t direction = kTransferOriginalToExtension;
    uint8_t src_bag = 0;
    uint8_t src_slot = 0;
    uint8_t dst_bag = 0;
    uint8_t dst_slot = 0;
    bool merged = false;
    bool swapped = false;
    bool original_stage_applicable = true;
    Item source{};        // 源逻辑项快照（ext→ext/ext→orig 清源还原依据）
    Item committed{};     // 目标提交态（合并时数量位段已修补）
    Item previous_dst{};  // 目标槽先前态（回滚还原依据）
    char transaction_id[kMaxTransactionIdChars + 1]{};
    PendingTransfer pending{};  // txn_build_pending 产出，pending-recorded 阶段写入 State
};

// 依据方向填充 pending（payload 语义与 JournalRecord v1 字段一一映射，durable=false）：
//   orig→ext：payload=目标提交态载荷，source_payload=原版源载荷（恢复时重建原版源）；
//   ext→orig：payload=被移动物品源载荷（恢复时重建入库）；
//   ext→ext：payload=目标提交态载荷。
inline void txn_build_pending(TransactionContext* txn) {
    if (txn == nullptr || txn->transaction_id[0] == '\0') return;
    PendingTransfer pending{};
    pending.valid = true;
    pending.direction = txn->direction;
    pending.src_bag = txn->src_bag;
    pending.src_slot = txn->src_slot;
    pending.dst_bag = txn->dst_bag;
    pending.dst_slot = txn->dst_slot;
    std::strncpy(pending.transaction_id, txn->transaction_id, kMaxTransactionIdChars);
    pending.transaction_id[kMaxTransactionIdChars] = '\0';
    if (txn->direction == kTransferOriginalToExtension) {
        pending.payload_size = txn->committed.payload_size;
        pending.payload = txn->committed.payload;
        pending.source_payload_size = txn->source.payload_size;
        pending.source_payload = txn->source.payload;
    } else if (txn->direction == kTransferExtensionToOriginal) {
        pending.payload_size = txn->source.payload_size;
        pending.payload = txn->source.payload;
    } else {
        pending.payload_size = txn->committed.payload_size;
        pending.payload = txn->committed.payload;
    }
    txn->pending = pending;
}

// logical-state-updated：按方向仅更新扩展逻辑 source/target。
// ext→orig 的扩展源清空发生在原版接管成功之后（§4.3），logical 阶段不改逻辑数组。
inline void txn_apply_logical(State* state, const TransactionContext& txn) {
    if (state == nullptr) return;
    if (txn.direction == kTransferOriginalToExtension) {
        state->items[txn.dst_bag][txn.dst_slot] = txn.committed;
    } else if (txn.direction == kTransferExtensionToExtension) {
        if (txn.swapped) {
            state->items[txn.src_bag][txn.src_slot] = txn.previous_dst;
            state->items[txn.dst_bag][txn.dst_slot] = txn.source;
        } else {
            state->items[txn.src_bag][txn.src_slot] = {};
            state->items[txn.dst_bag][txn.dst_slot] = txn.committed;
        }
    }
}

// 按失败阶段还原：先还原逻辑槽，再清 pending。
// 已移交原版库存的对象不参与模块回滚（释放/移交由持有账本的调用层负责）。
inline void txn_rollback_logical(State* state, const TransactionContext& txn, TxnStage failed_at) {
    if (state == nullptr) return;
    if (failed_at >= TxnStage::kCommitted) return;  // 提交后不做猜测性逆转
    if (failed_at > TxnStage::kPendingRecorded) {
        if (txn.direction == kTransferOriginalToExtension) {
            state->items[txn.dst_bag][txn.dst_slot] = txn.previous_dst;
        } else if (txn.direction == kTransferExtensionToExtension) {
            state->items[txn.src_bag][txn.src_slot] = txn.source;
            state->items[txn.dst_bag][txn.dst_slot] = txn.previous_dst;
        }
    }
    state->pending = {};
}

// ---- 跨进程 prepare journal（ADR-007 v1，section: extensionbags.journal）----

enum class JournalRecovery {
    kDiscard,          // journal 损坏/非法：隔离并告警，禁止按其重放（静默覆盖禁令）
    kRollback,         // 原版侧未持久化：清 journal 与 pending，committed 不变
    kReplayToSidecar,  // 原版侧已持久化：把变更应用到 committed 并提交（幂等）
    kJustClear,        // 两侧均完成：仅清 journal
};

// JournalRecord 与 Item 的 payload 一致性（数量位段无关的全量比对，复用 Item 匹配语义）。
inline bool journal_slot_holds_payload(const Item& item, const JournalRecord& journal) {
    PendingTransfer pending{};
    pending.valid = true;
    pending.payload_size = journal.payload_size;
    pending.payload = journal.payload;
    return item_matches_payload(item, pending);
}

inline bool valid_journal_record(const JournalRecord& journal) {
    if (!journal.valid) return false;
    if (journal.stage > kJournalStageSidecarCommitted) return false;
    const size_t id_len = std::strlen(journal.transaction_id);
    if (id_len == 0 || id_len > kMaxTransactionIdChars) return false;
    if (!valid_transaction_domain(journal.direction, journal.src_bag, journal.src_slot,
                                  journal.dst_bag, journal.dst_slot)) {
        return false;
    }
    if (validate_serialized_payload_buffer(journal.payload.data(), journal.payload.size(),
                                           journal.payload_size) != PayloadValidation::kOk) {
        return false;
    }
    return journal.source_payload_size == 0 ||
           validate_serialized_payload_buffer(journal.source_payload.data(),
                                              journal.source_payload.size(),
                                              journal.source_payload_size) == PayloadValidation::kOk;
}

// 三方对照裁决（journal stage × committed state × 原版世界实态）。
// stage 标记可能落后于实态（原版保存成功后、stage 落盘前崩溃），因此世界探针优先于 stage：
//   orig→ext：原版源槽已无源物品 ⇒ 原版侧已持久化 ⇒ 重放扩展侧；仍有 ⇒ 回滚。
//   ext→orig：原版目标槽已有该物品 ⇒ 原版侧已持久化 ⇒ 重放扩展侧（删源槽）；没有 ⇒ 回滚。
// stage=2 或「committed 已等于目标态」时仅需清理（幂等）。
inline JournalRecovery journal_recovery_action(const State& state,
                                               const JournalRecord& journal,
                                               const WorldProbe& probe) {
    if (!valid_journal_record(journal)) return JournalRecovery::kDiscard;
    if (journal.stage == kJournalStageSidecarCommitted) return JournalRecovery::kJustClear;
    const bool original_to_extension = journal.direction == kTransferOriginalToExtension;
    const bool original_side_persisted = original_to_extension
                                             ? !probe.original_slot_holds_payload
                                             : probe.original_slot_holds_payload;
    if (!original_side_persisted) return JournalRecovery::kRollback;
    const bool sidecar_committed =
        original_to_extension
            ? journal_slot_holds_payload(state.items[journal.dst_bag][journal.dst_slot], journal)
            : state.items[journal.src_bag][journal.src_slot].category == 0;
    return sidecar_committed ? JournalRecovery::kJustClear : JournalRecovery::kReplayToSidecar;
}

// P4.5：journal kDiscard（结构/payload 损坏）时保留原始字节为只读隔离记录，
// 供告警与 P7 journal 对照；禁止按其重放。transaction_id 长度不可信，安全截断。
inline IsolationRecord isolation_from_journal_record(const JournalRecord& journal,
                                                     uint64_t observed_at_ms) {
    IsolationRecord record{};
    record.valid = true;
    record.observed_at_ms = observed_at_ms;
    record.reason = isolation_reason::kJournalInvalid;
    std::strncpy(record.transaction_id, journal.transaction_id, kMaxTransactionIdChars);
    record.transaction_id[kMaxTransactionIdChars] = '\0';
    record.generation = journal.generation;
    record.direction = journal.direction;
    record.phase = journal.stage;
    record.src_bag = journal.src_bag;
    record.src_slot = journal.src_slot;
    record.dst_bag = journal.dst_bag;
    record.dst_slot = journal.dst_slot;
    const size_t payload_copy =
        std::min<size_t>(journal.payload_size, journal.payload.size());
    record.payload_size = static_cast<uint16_t>(payload_copy);
    std::memcpy(record.payload.data(), journal.payload.data(), payload_copy);
    const size_t source_copy =
        std::min<size_t>(journal.source_payload_size, journal.source_payload.size());
    record.source_payload_size = static_cast<uint16_t>(source_copy);
    std::memcpy(record.source_payload.data(), journal.source_payload.data(), source_copy);
    record.detail = "kDiscard";
    return record;
}
