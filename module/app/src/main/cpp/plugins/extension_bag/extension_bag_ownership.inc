// P3 所有权账本：借出对象四态跟踪。
ownership::Ledger g_ownership_ledger{};

struct DeferredItemFree {
    void* item = nullptr;
    uint32_t handle = 0;
    uint64_t generation = 0;
    uint64_t enqueued_at_ms = 0;
};

constexpr size_t kDeferredFreeQueueCapacity = 16;
// The custody tier is a separate fixed retention budget, not a consequence of the live ledger
// population: successfully deferred releases free ledger slots, so a long touch window can
// accumulate more retired objects than the ledger can hold at one time.
constexpr size_t kDeferredFreeCustodyCapacity = 128;
constexpr size_t kDeferredFreeMaxPending =
    kDeferredFreeQueueCapacity + kDeferredFreeCustodyCapacity;
std::array<DeferredItemFree, kDeferredFreeQueueCapacity> g_deferred_free_queue{};
size_t g_deferred_free_queue_head = 0;
size_t g_deferred_free_queue_size = 0;
// 总上限固定为 16+128 项；满载时入队 fail-closed，调用方必须保留源指针/所有权状态，
// 不得据此替换或摘除对象。两层容量不随积压动态扩张。
std::array<DeferredItemFree, kDeferredFreeCustodyCapacity> g_deferred_free_custody{};
size_t g_deferred_free_custody_size = 0;

bool object_touch_window_active_locked();

uint64_t deferred_free_now_ms_locked() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

uint64_t module_object_generation_locked(void* item, uint32_t handle) {
    for (int bag = 0; bag < virtual_bag::kBagCount; ++bag) {
        for (int slot = 0; slot < virtual_bag::kSlotCount; ++slot) {
            if (g_module_objects[bag][slot] == item) {
                return g_module_object_use[bag][slot].generation;
            }
        }
    }
    return static_cast<uint64_t>(handle >> 8);
}

bool enqueue_deferred_item_free_locked(void* item, uint32_t handle, uint64_t generation) {
    if (item == nullptr) return false;
    for (size_t offset = 0; offset < g_deferred_free_queue_size; ++offset) {
        const size_t index = (g_deferred_free_queue_head + offset) % kDeferredFreeQueueCapacity;
        if (g_deferred_free_queue[index].item == item) return true;
    }
    for (size_t index = 0; index < g_deferred_free_custody_size; ++index) {
        if (g_deferred_free_custody[index].item == item) return true;
    }
    const DeferredItemFree entry = {
        item, handle, generation, deferred_free_now_ms_locked()};
    if (g_deferred_free_queue_size >= kDeferredFreeQueueCapacity) {
        // 队列和保管区均满时拒绝入队；调用方保留对象及其所有权状态供后续重试。
        if (g_deferred_free_custody_size >= g_deferred_free_custody.size()) {
            VIRTBAG_LOG_ERROR("ERROR deferred free backlog full pending=%zu capacity=%zu item=%p handle=%u generation=%llu",
                              g_deferred_free_queue_size + g_deferred_free_custody_size,
                              kDeferredFreeMaxPending, item, handle,
                              static_cast<unsigned long long>(generation));
            return false;
        }
        g_deferred_free_custody[g_deferred_free_custody_size++] = entry;
        VIRTBAG_LOG("deferred free queue full; custody retained item=%p handle=%u generation=%llu",
                    item, handle, static_cast<unsigned long long>(generation));
        return true;
    }
    const size_t tail = (g_deferred_free_queue_head + g_deferred_free_queue_size) %
                        kDeferredFreeQueueCapacity;
    g_deferred_free_queue[tail] = entry;
    ++g_deferred_free_queue_size;
    VIRTBAG_LOG("deferred free enqueue item=%p handle=%u generation=%llu", item, handle,
                static_cast<unsigned long long>(generation));
    return true;
}

}  // namespace

bool defer_item_free_locked(void* item) {
    uint32_t handle = 0;
    for (int bag = 0; bag < virtual_bag::kBagCount; ++bag) {
        for (int slot = 0; slot < virtual_bag::kSlotCount; ++slot) {
            if (g_module_objects[bag][slot] == item) {
                handle = g_module_object_handles[bag][slot];
                break;
            }
        }
        if (handle != 0) break;
    }
    return enqueue_deferred_item_free_locked(
        item, handle, module_object_generation_locked(item, handle));
}

namespace {

bool deferred_item_referenced_locked(void* item) {
    for (int bag = 0; bag < virtual_bag::kBagCount; ++bag) {
        for (int slot = 0; slot < virtual_bag::kSlotCount; ++slot) {
            if (g_module_objects[bag][slot] == item) return true;
        }
    }
    if (g_projected_item_root == nullptr) {
        return item_pointer_known_locked(item);
    }
    int child_count = 0;
    if (!extension_bag_ui_child_count(g_projected_item_root, &child_count)) {
        return item_pointer_known_locked(item);
    }
    for (int slot = 0; slot < child_count; ++slot) {
        void* control = extension_bag_ui_child_at(g_projected_item_root, slot);
        if (control == nullptr) continue;
        if (extension_bag_ui_item(control) == item) return true;
    }
    // 兜底覆盖原版库存和角色装备等非 projection root 的已知引用；宁可
    // 保留队列项，也不能让 handover 失败路径把仍由游戏持有的对象释放掉。
    return item_pointer_known_locked(item);
}

void drain_deferred_frees_locked() {
    if (object_touch_window_active_locked()) return;
    const auto can_free = [](const DeferredItemFree& entry) {
        const auto* api = extension_bag_write_api();
        return !deferred_item_referenced_locked(entry.item) && api != nullptr &&
               api->inventory.free_item != nullptr;
    };
    const auto free_entry = [](const DeferredItemFree& entry) {
        if (extension_free_item(entry.item)) return true;
        VIRTBAG_LOG_ERROR("ERROR deferred free failed item=%p handle=%u generation=%llu",
                          entry.item, entry.handle,
                          static_cast<unsigned long long>(entry.generation));
        return false;
    };

    const size_t queue_count = g_deferred_free_queue_size;
    const auto queue_result = qol::features::extension_bag::model::drain_deferred_free_entries(
        g_deferred_free_queue, g_deferred_free_queue_head, queue_count, can_free, free_entry);
    if (queue_result.unprocessed != 0) {
        VIRTBAG_LOG_ERROR("ERROR deferred free queue size exceeded capacity size=%zu capacity=%zu",
                          queue_count, kDeferredFreeQueueCapacity);
    }
    g_deferred_free_queue_size = queue_result.kept;

    const size_t custody_count = g_deferred_free_custody_size;
    const auto custody_result =
        qol::features::extension_bag::model::drain_deferred_free_entries(
            g_deferred_free_custody, 0, custody_count, can_free, free_entry);
    if (custody_result.unprocessed != 0) {
        VIRTBAG_LOG_ERROR("ERROR deferred free custody size exceeded capacity size=%zu capacity=%zu",
                          custody_count, kDeferredFreeCustodyCapacity);
    }
    g_deferred_free_custody_size = custody_result.kept;
    VIRTBAG_LOG("deferred free drain queue=%zu/%zu custody=%zu/%zu freed=%zu",
                queue_result.kept, queue_count, custody_result.kept, custody_count,
                queue_result.freed + custody_result.freed);
}

void log_ownership_audit_locked(const char* context) {
    const ownership::Audit report = ownership::audit(g_ownership_ledger);
    VIRTBAG_LOG(
        "ownership audit context=%s balanced=%d allocated=%u released=%u handed_over=%u "
        "live=%u objects=%u borrows=%u inventory_owned=%u",
        context, report.balanced ? 1 : 0, g_ownership_ledger.total_allocated,
        g_ownership_ledger.total_released, g_ownership_ledger.total_handed_over,
        report.live_handles, report.outstanding_objects, report.outstanding_borrows,
        report.inventory_owned);
}

bool object_touch_window_active_locked() {
    if (g_extension_touch_capture || g_extension_drag.active) return true;
    if (!g_module_view_installed || g_projected_item_root == nullptr ||
        !virtual_bag::valid_index(g_module_view_index)) return false;
    void* moving = nullptr;
    void* drop_source = nullptr;
    int64_t touch_x = 0;
    int64_t touch_y = 0;
    if (!extension_bag_ui_touch_state(&moving, &drop_source, &touch_x, &touch_y)) return false;
    (void)drop_source;
    (void)touch_x;
    (void)touch_y;
    if (moving == nullptr) return false;
    const int capacity = g_virtual_bag_state.capacities[g_module_view_index];
    for (int slot = 0; slot < capacity; ++slot) {
        if (valid_child_locked(g_projected_item_root, slot) == moving) return true;
    }
    return false;
}

bool retire_custody_item_locked(uint32_t handle, void* item, const char* context) {
    if (item == nullptr || !ownership::live_state(g_ownership_ledger, handle,
                                                   ownership::State::kModuleOwned)) {
        VIRTBAG_LOG("ownership release refused context=%s invalid_handle=%u item=%p", context,
                    handle, item);
        return false;
    }

    const bool touch_window = object_touch_window_active_locked();
    if (ownership::reserve_release(&g_ownership_ledger, handle) != ownership::Outcome::kOk) {
        VIRTBAG_LOG_ERROR("ERROR ownership release reservation failed context=%s handle=%u",
                          context, handle);
        return false;
    }

    bool deferred = touch_window || extension_bag_write_api() == nullptr ||
                    extension_bag_write_api()->inventory.free_item == nullptr;
    if (deferred) {
        const uint64_t generation = module_object_generation_locked(item, handle);
        if (!enqueue_deferred_item_free_locked(item, handle, generation)) {
            if (ownership::cancel_reserved_release(&g_ownership_ledger, handle) !=
                ownership::Outcome::kOk) {
                VIRTBAG_LOG_ERROR("ERROR ownership release reservation rollback failed "
                                  "context=%s handle=%u item=%p",
                                  context, handle, item);
            }
            return false;
        }
    } else if (!extension_free_item(item)) {
        VIRTBAG_LOG_ERROR("ERROR native item free failed context=%s handle=%u item=%p; retry deferred",
                          context, handle, item);
        const uint64_t generation = module_object_generation_locked(item, handle);
        if (!enqueue_deferred_item_free_locked(item, handle, generation)) {
            if (ownership::cancel_reserved_release(&g_ownership_ledger, handle) !=
                ownership::Outcome::kOk) {
                VIRTBAG_LOG_ERROR("ERROR ownership release reservation rollback failed "
                                  "context=%s handle=%u item=%p",
                                  context, handle, item);
            }
            VIRTBAG_LOG_ERROR("ERROR native item free retry queue full context=%s handle=%u item=%p",
                              context, handle, item);
            return false;
        }
        deferred = true;
    }
    if (ownership::commit_reserved_release(&g_ownership_ledger, handle) !=
        ownership::Outcome::kOk) {
        VIRTBAG_LOG_ERROR("ERROR ownership release commit failed context=%s handle=%u",
                          context, handle);
        return false;
    }
    extension_bag_invalidate_item_handle(item);
    if (deferred) {
        VIRTBAG_LOG("ownership release deferred context=%s touch_window=%d handle=%u", context,
                    touch_window ? 1 : 0, handle);
    }
    log_ownership_audit_locked(context);
    return true;
}

void* load_item_payload_tracked_locked(const uint8_t* payload, int payload_size,
                                       const char* context, uint32_t* out_handle) {
    *out_handle = 0;
    void* item = load_item_payload_locked(payload, payload_size, context);
    if (item == nullptr) return nullptr;
    uint32_t handle = 0;
    if (ownership::allocate(&g_ownership_ledger, &handle) != ownership::Outcome::kOk) {
        release_temporary_item_locked(item);
        VIRTBAG_LOG("ownership allocate reject; temp freed context=%s", context);
        return nullptr;
    }
    *out_handle = handle;
    return item;
}

void handover_tracked_item_locked(uint32_t handle, void* item, const char* context) {
    if (ownership::handover_to_inventory(&g_ownership_ledger, handle) !=
        ownership::Outcome::kOk) {
        if (defer_item_free_locked(item)) {
            VIRTBAG_LOG("ownership handover reject; custody retained context=%s item=%p",
                        context, item);
        } else {
            VIRTBAG_LOG_ERROR("ERROR ownership handover reject; custody unavailable "
                              "context=%s handle=%u item=%p",
                              context, handle, item);
        }
    }
    log_ownership_audit_locked(context);
}

void release_tracked_item_locked(uint32_t handle, void* item, const char* context) {
    if (ownership::release(&g_ownership_ledger, handle) == ownership::Outcome::kOk) {
        release_temporary_item_locked(item);
    } else {
        if (defer_item_free_locked(item)) {
            VIRTBAG_LOG("ownership release reject; custody retained context=%s item=%p",
                        context, item);
        } else {
            VIRTBAG_LOG_ERROR("ERROR ownership release reject; custody unavailable "
                              "context=%s handle=%u item=%p",
                              context, handle, item);
        }
    }
    log_ownership_audit_locked(context);
}

// 槽位可分配 = 位于该袋**派生容量**内且 descriptor/对象/handle/use 全部为空态。
// 容量外槽位一律不可分配：数组固定 16 槽只是物理上限，有效范围是
// derive_capacity(types[bag])；缺少容量上界会让入库/收编在容量内槽满后继续
// 占用 slot >= capacity 的槽（超容写入）。
bool module_slot_is_assignable_locked(int bag, int slot) {
    if (bag < 0 || bag >= virtual_bag::kBagCount || slot < 0 ||
        slot >= g_virtual_bag_state.capacities[bag]) {
        return false;
    }
    const virtual_bag::Item& descriptor = g_virtual_bag_state.items[bag][slot];
    const ModuleUseState& use = g_module_object_use[bag][slot];
    // handle==0 is the only valid empty-slot ownership state. A pending-release
    // object remains physically live until finish and must never be reused.
    return descriptor.category == 0 && descriptor.count == 0 &&
           descriptor.payload_size == 0 && g_module_objects[bag][slot] == nullptr &&
           !use.active && !use.pending_release && g_module_object_handles[bag][slot] == 0;
}

bool module_object_replacement_allowed_locked(int bag, int slot, const char* context) {
    if (bag < 0 || bag >= virtual_bag::kBagCount || slot < 0 ||
        slot >= virtual_bag::kSlotCount) {
        return false;
    }
    const ModuleUseState& use = g_module_object_use[bag][slot];
    if (use.active || use.pending_release) {
        VIRTBAG_LOG("module object replacement rejected while confirm-use active context=%s bag=%d slot=%d item=%p generation=%llu",
                    context, bag, slot, use.item,
                    static_cast<unsigned long long>(use.generation));
        return false;
    }
    return true;
}

bool module_object_release_allowed_locked(int bag, int slot, void* item, const char* context,
                                          bool allow_owner, const ModuleUseToken* token) {
    if (bag < 0 || bag >= virtual_bag::kBagCount || slot < 0 ||
        slot >= virtual_bag::kSlotCount) {
        return false;
    }
    const ModuleUseState& use = g_module_object_use[bag][slot];
    if (use.pending_release) {
        VIRTBAG_LOG("module object release rejected while pending release context=%s bag=%d slot=%d item=%p",
                    context, bag, slot, item);
        return false;
    }
    if (!use.active) return true;
    const bool token_matches = token != nullptr && token->item == item &&
                               token->item == use.item &&
                               token->generation == use.generation &&
                               token->owner == use.owner &&
                               g_module_objects[bag][slot] == token->item;
    if (allow_owner && token_matches && token->owner == std::this_thread::get_id()) return true;
    VIRTBAG_LOG("module object release rejected while confirm-use active context=%s bag=%d slot=%d item=%p",
                context, bag, slot, item);
    return false;
}

bool module_object_generation_advance_locked(int bag, int slot, const char* context) {
    if (bag < 0 || bag >= virtual_bag::kBagCount || slot < 0 ||
        slot >= virtual_bag::kSlotCount) {
        return false;
    }
    ModuleUseState& use = g_module_object_use[bag][slot];
    if (!module_object_replacement_allowed_locked(bag, slot, context)) return false;
    ++use.generation;
    if (use.generation == 0) ++use.generation;
    use.active = false;
    use.consumed = false;
    use.pending_release = false;
    use.item = nullptr;
    use.owner = {};
    return true;
}

bool module_use_begin_locked(int bag, int slot, void* item, ModuleUseToken* out_token) {
    if (out_token == nullptr || bag < 0 || bag >= virtual_bag::kBagCount || slot < 0 ||
        slot >= virtual_bag::kSlotCount || item == nullptr || g_module_objects[bag][slot] != item) {
        return false;
    }
    ModuleUseState& use = g_module_object_use[bag][slot];
    if (use.active) return false;
    ++use.generation;
    if (use.generation == 0) ++use.generation;
    use.active = true;
    use.consumed = false;
    use.pending_release = false;
    use.item = item;
    use.owner = std::this_thread::get_id();
    *out_token = {bag, slot, item, use.generation, use.owner};
    return true;
}

bool module_use_finish_locked(const ModuleUseToken& token, bool* out_consumed,
                              ModuleUseReleasePlan* out_release) {
    if (out_release != nullptr) *out_release = {};
    if (token.bag < 0 || token.bag >= virtual_bag::kBagCount || token.slot < 0 ||
        token.slot >= virtual_bag::kSlotCount) {
        VIRTBAG_LOG_ERROR("ERROR module use finish token mismatch invalid slot active=-1 "
                    "pending_release=-1 generation=0 item=%p handle=0 owner_thread=0 "
                    "token_bag=%d token_slot=%d token_generation=%llu token_item=%p",
                    token.item, token.bag, token.slot,
                    static_cast<unsigned long long>(token.generation), token.item);
        return false;
    }
    ModuleUseState& use = g_module_object_use[token.bag][token.slot];
    if (!use.active || use.item != token.item || use.generation != token.generation ||
        use.owner != token.owner || g_module_objects[token.bag][token.slot] != token.item) {
        const auto thread_key = [](const std::thread::id& id) -> unsigned long long {
            return static_cast<unsigned long long>(std::hash<std::thread::id>{}(id));
        };
        VIRTBAG_LOG_ERROR("ERROR module use finish token mismatch bag=%d slot=%d active=%d "
                    "pending_release=%d generation=%llu item=%p handle=%u owner_thread=%llu "
                    "token_bag=%d token_slot=%d token_generation=%llu token_item=%p "
                    "token_owner_thread=%llu current_object=%p",
                    token.bag, token.slot, use.active ? 1 : 0,
                    use.pending_release ? 1 : 0,
                    static_cast<unsigned long long>(use.generation), use.item,
                    g_module_object_handles[token.bag][token.slot], thread_key(use.owner),
                    token.bag, token.slot, static_cast<unsigned long long>(token.generation),
                    token.item, thread_key(token.owner),
                    g_module_objects[token.bag][token.slot]);
        return false;
    }
    if (out_consumed != nullptr) *out_consumed = use.consumed;
    const bool pending_release = use.pending_release;
    const int bag = token.bag;
    const int slot = token.slot;
    if (pending_release) {
        ModuleUseReleasePlan release{};
        release.pending = true;
        release.bag = bag;
        release.slot = slot;
        release.item = token.item;
        release.handle = g_module_object_handles[bag][slot];
        release.generation = use.generation;
        if (g_module_view_installed && g_module_view_index == bag &&
            g_projected_item_root != nullptr) {
            release.control = valid_child_locked(g_projected_item_root, slot);
        }
        // 事务状态、对象索引和 handle 在锁内摘除；native 控件写入及 itempool
        // 释放由调用方在解锁后完成，避免重入 Remove/Save/Consume hook。
        g_module_objects[bag][slot] = nullptr;
        g_module_object_categories[bag][slot] = 0;
        g_module_object_hashes[bag][slot] = 0;
        g_module_object_handles[bag][slot] = 0;
        use.active = false;
        use.consumed = false;
        use.item = token.item;
        use.owner = {};
        if (out_release != nullptr) *out_release = release;
        return true;
    }
    use.active = false;
    use.consumed = false;
    use.pending_release = false;
    use.item = nullptr;
    use.owner = {};
    return true;
}

bool module_use_abort_locked(const ModuleUseToken& token) {
    if (token.bag < 0 || token.bag >= virtual_bag::kBagCount || token.slot < 0 ||
        token.slot >= virtual_bag::kSlotCount) {
        VIRTBAG_LOG_ERROR("ERROR module use abort token mismatch; slot may remain active invalid slot "
                    "active=-1 pending_release=-1 generation=0 item=%p handle=0 owner_thread=0 "
                    "token_bag=%d token_slot=%d token_generation=%llu token_item=%p",
                    token.item, token.bag, token.slot,
                    static_cast<unsigned long long>(token.generation), token.item);
        return false;
    }
    ModuleUseState& use = g_module_object_use[token.bag][token.slot];
    if (!use.active || use.item != token.item || use.generation != token.generation ||
        use.owner != token.owner || g_module_objects[token.bag][token.slot] != token.item) {
        const auto thread_key = [](const std::thread::id& id) -> unsigned long long {
            return static_cast<unsigned long long>(std::hash<std::thread::id>{}(id));
        };
        VIRTBAG_LOG_ERROR("ERROR module use abort token mismatch; slot may remain active "
                    "bag=%d slot=%d active=%d pending_release=%d generation=%llu item=%p "
                    "handle=%u owner_thread=%llu token_bag=%d token_slot=%d "
                    "token_generation=%llu token_item=%p token_owner_thread=%llu current_object=%p",
                    token.bag, token.slot, use.active ? 1 : 0,
                    use.pending_release ? 1 : 0,
                    static_cast<unsigned long long>(use.generation), use.item,
                    g_module_object_handles[token.bag][token.slot], thread_key(use.owner),
                    token.bag, token.slot, static_cast<unsigned long long>(token.generation),
                    token.item, thread_key(token.owner),
                    g_module_objects[token.bag][token.slot]);
        return false;
    }
    use.active = false;
    use.consumed = false;
    use.pending_release = false;
    use.item = nullptr;
    use.owner = {};
    return true;
}

bool module_use_abort_isolate_locked(const ModuleUseToken& token) {
    if (token.bag < 0 || token.bag >= virtual_bag::kBagCount || token.slot < 0 ||
        token.slot >= virtual_bag::kSlotCount) {
        return false;
    }
    ModuleUseState& use = g_module_object_use[token.bag][token.slot];
    if (!use.active) return true;
    // mismatch 后不释放未知 native 对象，只清除 active 标记并保留 pending 对象的
    // 隔离状态；generation 保留以拒绝旧 token 重入。
    VIRTBAG_LOG_ERROR("ERROR module use abort isolation bag=%d slot=%d item=%p pending_release=%d",
                token.bag, token.slot, use.item, use.pending_release ? 1 : 0);
    use.active = false;
    use.consumed = false;
    if (!use.pending_release) use.item = nullptr;
    use.owner = {};
    return true;
}

bool module_use_release_native(const ModuleUseReleasePlan& release) {
    if (!release.pending || release.item == nullptr || release.handle == 0) return false;
    bool deferred = false;
    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        if (release.control != nullptr) (void)extension_bag_ui_set_control_item(release.control, nullptr);
        if (object_touch_window_active_locked() || extension_bag_write_api() == nullptr ||
            extension_bag_write_api()->inventory.free_item == nullptr) {
            deferred = enqueue_deferred_item_free_locked(release.item, release.handle,
                                                         release.generation);
            if (!deferred) {
                // module_use_finish_locked keeps this pointer in use.item with pending_release;
                // failure leaves the slot reserved and the caller must not report completion.
                VIRTBAG_LOG_ERROR("ERROR module-use release retained bag=%d slot=%d handle=%u item=%p",
                                  release.bag, release.slot, release.handle, release.item);
                return false;
            }
        }
    }
    if (!deferred && !extension_free_item(release.item)) {
        VIRTBAG_LOG_ERROR("ERROR module-use native free failed bag=%d slot=%d handle=%u item=%p; retry deferred",
                          release.bag, release.slot, release.handle, release.item);
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        if (!enqueue_deferred_item_free_locked(release.item, release.handle,
                                              release.generation)) {
            VIRTBAG_LOG_ERROR("ERROR module-use native free retry queue full bag=%d slot=%d handle=%u item=%p",
                              release.bag, release.slot, release.handle, release.item);
            return false;
        }
        deferred = true;
    }
    return true;
}

bool module_use_finalize_release_locked(const ModuleUseReleasePlan& release) {
    if (!release.pending || release.bag < 0 || release.bag >= virtual_bag::kBagCount ||
        release.slot < 0 || release.slot >= virtual_bag::kSlotCount) {
        return false;
    }
    ModuleUseState& use = g_module_object_use[release.bag][release.slot];
    if (!use.pending_release || use.item != release.item || use.generation != release.generation) {
        VIRTBAG_LOG_ERROR("ERROR module use release finalize mismatch bag=%d slot=%d item=%p",
                    release.bag, release.slot, release.item);
        return false;
    }
    if (ownership::live_state(g_ownership_ledger, release.handle,
                              ownership::State::kBorrowedForView)) {
        ownership::return_from_view(&g_ownership_ledger, release.handle);
    }
    if (ownership::release(&g_ownership_ledger, release.handle) != ownership::Outcome::kOk) {
        VIRTBAG_LOG_ERROR("ERROR module use release ledger mismatch bag=%d slot=%d handle=%u",
                    release.bag, release.slot, release.handle);
        return false;
    }
    use.active = false;
    use.consumed = false;
    use.pending_release = false;
    use.item = nullptr;
    use.owner = {};
    return module_object_generation_advance_locked(release.bag, release.slot,
                                                   "module-use-native-release");
}

bool free_module_object_locked(int bag, int slot, void* raw_use_token) {
    if (bag < 0 || bag >= virtual_bag::kBagCount || slot < 0 || slot >= virtual_bag::kSlotCount) {
        return false;
    }
    void* item = g_module_objects[bag][slot];
    if (item == nullptr) {
        if (g_module_object_use[bag][slot].active) {
            VIRTBAG_LOG_ERROR("ERROR module free rejected active token with empty slot bag=%d slot=%d",
                        bag, slot);
            return false;
        }
        return module_object_generation_advance_locked(bag, slot, "free-empty");
    }
    ModuleUseToken* use_token = static_cast<ModuleUseToken*>(raw_use_token);
    const ModuleUseState& current_use = g_module_object_use[bag][slot];
    if (current_use.active && use_token == nullptr) {
        VIRTBAG_LOG("module free rejected without real token bag=%d slot=%d item=%p",
                    bag, slot, item);
        return false;
    }
    if (!module_object_release_allowed_locked(
            bag, slot, item, "free_module_object", true, use_token)) {
        return false;
    }
    if (current_use.active) {
        ModuleUseState& use = g_module_object_use[bag][slot];
        use.consumed = true;
        use.pending_release = true;
        return true;
    }
    if (item != nullptr) {
        if (g_module_view_installed && g_module_view_index == bag &&
            g_projected_item_root != nullptr) {
            void* ctrl = valid_child_locked(g_projected_item_root, slot);
            if (ctrl != nullptr) (void)extension_bag_ui_set_control_item(ctrl, nullptr);
        }
        const uint32_t handle = g_module_object_handles[bag][slot];
        if (ownership::live_state(g_ownership_ledger, handle,
                                  ownership::State::kBorrowedForView)) {
            ownership::return_from_view(&g_ownership_ledger, handle);
        }
        if (!retire_custody_item_locked(handle, item, "free_module_object")) return false;
    }
    g_module_objects[bag][slot] = nullptr;
    g_module_object_categories[bag][slot] = 0;
    g_module_object_hashes[bag][slot] = 0;
    g_module_object_handles[bag][slot] = 0;
    return module_object_generation_advance_locked(bag, slot, "free-object");
}
