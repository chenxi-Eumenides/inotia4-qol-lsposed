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

inline bool patch_payload_count(Item* item, uint32_t count,
                                CategoryStackCountPredicate is_count_encoded) {
    if (item == nullptr || !valid_payload(*item)) return false;
    if (is_count_encoded == nullptr ||
        is_count_encoded(item->category) != stack_codec::CountEncoding::kEncoded) {
        std::fprintf(stderr,
                     "virtual_bag patch_payload_count ignored category=%d reason=not_count_encoded\n",
                     item->category);
        return false;
    }
    const uint32_t updated = stack_codec::write_count(payload_count(*item), count);
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

// 合并数量：按堆叠上限（stack_limit_enabled ? 999 : 99）clamp。
inline uint32_t merge_count(int existing_count, int added_count, bool extended) {
    const int64_t sum = static_cast<int64_t>(existing_count) + added_count;
    if (sum <= 0) return 0;
    return stack_codec::clamp_count(static_cast<uint32_t>(sum), extended);
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
    if (!valid_payload(existing) || !valid_payload(source) ||
        existing.payload_size != source.payload_size) {
        return false;
    }
    Item existing_identity = existing;
    Item source_identity = source;
    if (!patch_payload_count(&existing_identity, 1, is_count_encoded) ||
        !patch_payload_count(&source_identity, 1, is_count_encoded)) {
        return false;
    }
    return std::memcmp(existing_identity.payload.data(), source_identity.payload.data(),
                       existing_identity.payload_size) == 0;
}

inline bool same_extension_bag_merge_allowed(int src_bag, int dst_bag) {
    return src_bag == dst_bag;
}

inline bool same_extension_bag_mergeable_items(const Item& existing, const Item& source,
                                               CategoryStackCountPredicate is_count_encoded) {
    return mergeable_items(existing, source, is_count_encoded);
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
