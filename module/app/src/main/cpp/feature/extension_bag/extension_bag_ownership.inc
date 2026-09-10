// P3 所有权账本：借出对象四态跟踪。
ownership::Ledger g_ownership_ledger{};
uint32_t g_module_object_handles[virtual_bag::kBagCount][virtual_bag::kSlotCount]{};

struct DeferredItemFree {
    void* item = nullptr;
    uint32_t handle = 0;
    uint64_t generation = 0;
    uint64_t enqueued_at_ms = 0;
};

constexpr size_t kDeferredFreeQueueCapacity = 16;
std::array<DeferredItemFree, kDeferredFreeQueueCapacity> g_deferred_free_queue{};
size_t g_deferred_free_queue_head = 0;
size_t g_deferred_free_queue_size = 0;
// 账本最多同时追踪 ownership::kLedgerCapacity 个对象；队列满时使用该有界
// 保管区承接剩余对象，保证 custody 不丢失且事务不会因回收排队失败而失败。
std::array<DeferredItemFree, ownership::kLedgerCapacity> g_deferred_free_custody{};
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
        // 正常调用方均持有 ownership ledger handle；该保管区容量覆盖 ledger
        // 上限，因此队列满不会把释放失败传播给事务或丢失对象指针。
        if (g_deferred_free_custody_size >= g_deferred_free_custody.size()) {
            VIRTBAG_LOG("ERROR deferred free custody overflow item=%p handle=%u generation=%llu",
                        item, handle, static_cast<unsigned long long>(generation));
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

bool deferred_item_referenced_locked(void* item) {
    for (int bag = 0; bag < virtual_bag::kBagCount; ++bag) {
        for (int slot = 0; slot < virtual_bag::kSlotCount; ++slot) {
            if (g_module_objects[bag][slot] == item) return true;
        }
    }
    if (g_projected_item_root == nullptr || fn_ctrl_get_count == nullptr ||
        fn_control_object_get_child == nullptr || fn_control_object_get_data == nullptr) {
        return item_pointer_known_locked(item);
    }
    const int child_count = static_cast<int>(fn_ctrl_get_count(g_projected_item_root));
    for (int slot = 0; slot < child_count; ++slot) {
        void* control = fn_control_object_get_child(g_projected_item_root, slot);
        if (control == nullptr) continue;
        void* data = fn_control_object_get_data(control);
        if (data != nullptr && *reinterpret_cast<void**>(data) == item) return true;
    }
    // 兜底覆盖原版库存和角色装备等非 projection root 的已知引用；宁可
    // 保留队列项，也不能让 handover 失败路径把仍由游戏持有的对象释放掉。
    return item_pointer_known_locked(item);
}

void drain_deferred_frees_locked() {
    if (object_touch_window_active_locked()) return;
    const size_t count = g_deferred_free_queue_size + g_deferred_free_custody_size;
    if (count == 0) return;

    std::array<DeferredItemFree, kDeferredFreeQueueCapacity> kept{};
    size_t kept_count = 0;
    size_t freed_count = 0;
    for (size_t offset = 0; offset < count; ++offset) {
        const size_t index = (g_deferred_free_queue_head + offset) % kDeferredFreeQueueCapacity;
        const DeferredItemFree entry = g_deferred_free_queue[index];
        if (deferred_item_referenced_locked(entry.item) || fn_itempool_free == nullptr) {
            kept[kept_count++] = entry;
            continue;
        }
        fn_itempool_free(entry.item);
        ++freed_count;
    }
    g_deferred_free_queue = {};
    g_deferred_free_queue_head = 0;
    g_deferred_free_queue_size = 0;
    for (size_t index = 0; index < kept_count; ++index) {
        g_deferred_free_queue[index] = kept[index];
    }
    g_deferred_free_queue_size = kept_count;

    std::array<DeferredItemFree, ownership::kLedgerCapacity> kept_custody{};
    size_t kept_custody_count = 0;
    for (size_t index = 0; index < g_deferred_free_custody_size; ++index) {
        const DeferredItemFree entry = g_deferred_free_custody[index];
        if (deferred_item_referenced_locked(entry.item) || fn_itempool_free == nullptr) {
            kept_custody[kept_custody_count++] = entry;
            continue;
        }
        fn_itempool_free(entry.item);
        ++freed_count;
    }
    g_deferred_free_custody = {};
    g_deferred_free_custody_size = kept_custody_count;
    for (size_t index = 0; index < kept_custody_count; ++index) {
        g_deferred_free_custody[index] = kept_custody[index];
    }
    VIRTBAG_LOG("deferred free drain count=%zu freed=%zu kept=%zu", count, freed_count,
                kept_count + kept_custody_count);
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
    if (g_base == 0 || !g_module_view_installed || g_projected_item_root == nullptr ||
        !virtual_bag::valid_index(g_module_view_index)) return false;
    const uint8_t* touch_state = reinterpret_cast<const uint8_t*>(g_base + G_TOUCH_STATE_VMA);
    const void* moving = *reinterpret_cast<const void* const*>(touch_state + TOUCH_STATE_MOVING_CTRL);
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
    if (touch_window || fn_itempool_free == nullptr) {
        const uint64_t generation = module_object_generation_locked(item, handle);
        if (!enqueue_deferred_item_free_locked(item, handle, generation)) return false;
    }
    if (ownership::release(&g_ownership_ledger, handle) != ownership::Outcome::kOk) {
        VIRTBAG_LOG("ERROR ownership release changed during custody retirement context=%s handle=%u",
                    context, handle);
        return false;
    }
    if (!touch_window && fn_itempool_free != nullptr) fn_itempool_free(item);
    if (touch_window || fn_itempool_free == nullptr) {
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
        defer_item_free_locked(item);
        VIRTBAG_LOG("ownership handover reject; deferred context=%s", context);
    }
    log_ownership_audit_locked(context);
}

void release_tracked_item_locked(uint32_t handle, void* item, const char* context) {
    if (ownership::release(&g_ownership_ledger, handle) == ownership::Outcome::kOk) {
        release_temporary_item_locked(item);
    } else {
        defer_item_free_locked(item);
        VIRTBAG_LOG("ownership release reject; deferred context=%s", context);
    }
    log_ownership_audit_locked(context);
}

bool module_slot_is_assignable_locked(int bag, int slot) {
    if (bag < 0 || bag >= virtual_bag::kBagCount || slot < 0 ||
        slot >= virtual_bag::kSlotCount) {
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
        VIRTBAG_LOG("ERROR module use finish token mismatch invalid slot active=-1 "
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
        VIRTBAG_LOG("ERROR module use finish token mismatch bag=%d slot=%d active=%d "
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
            g_projected_item_root != nullptr && fn_control_item_set_item != nullptr) {
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
        VIRTBAG_LOG("ERROR module use abort token mismatch; slot may remain active invalid slot "
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
        VIRTBAG_LOG("ERROR module use abort token mismatch; slot may remain active "
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
    VIRTBAG_LOG("ERROR module use abort isolation bag=%d slot=%d item=%p pending_release=%d",
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
        if (release.control != nullptr && fn_control_item_set_item != nullptr) {
            fn_control_item_set_item(release.control, nullptr);
        }
        if (object_touch_window_active_locked() || fn_itempool_free == nullptr) {
            deferred = enqueue_deferred_item_free_locked(release.item, release.handle,
                                                         release.generation);
            if (!deferred) return false;
        }
    }
    if (!deferred) fn_itempool_free(release.item);
    return true;
}

bool module_use_finalize_release_locked(const ModuleUseReleasePlan& release) {
    if (!release.pending || release.bag < 0 || release.bag >= virtual_bag::kBagCount ||
        release.slot < 0 || release.slot >= virtual_bag::kSlotCount) {
        return false;
    }
    ModuleUseState& use = g_module_object_use[release.bag][release.slot];
    if (!use.pending_release || use.item != release.item || use.generation != release.generation) {
        VIRTBAG_LOG("ERROR module use release finalize mismatch bag=%d slot=%d item=%p",
                    release.bag, release.slot, release.item);
        return false;
    }
    if (ownership::live_state(g_ownership_ledger, release.handle,
                              ownership::State::kBorrowedForView)) {
        ownership::return_from_view(&g_ownership_ledger, release.handle);
    }
    if (ownership::release(&g_ownership_ledger, release.handle) != ownership::Outcome::kOk) {
        VIRTBAG_LOG("ERROR module use release ledger mismatch bag=%d slot=%d handle=%u",
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
            VIRTBAG_LOG("ERROR module free rejected active token with empty slot bag=%d slot=%d",
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
            g_projected_item_root != nullptr && fn_control_object_get_child != nullptr &&
            fn_control_item_set_item != nullptr) {
            void* ctrl = valid_child_locked(g_projected_item_root, slot);
            if (ctrl != nullptr) fn_control_item_set_item(ctrl, nullptr);
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
