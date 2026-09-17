// P3 所有权账本：借出对象四态跟踪。
ownership::Ledger g_ownership_ledger{};
uint32_t g_module_object_handles[virtual_bag::kBagCount][virtual_bag::kSlotCount]{};

struct DeferredItemFree {
    void* item = nullptr;
    uint32_t handle = 0;
    uint64_t generation = 0;
    uint64_t enqueued_at_ms = 0;
    // 入队时的逻辑帧号（data_frame_count()；未就绪为 -1）。释放要求跨过帧边界，
    // 避免调用方在同一帧内仍然使用该对象；帧号不可用时回退到 enqueued_at_ms 下限。
    int64_t enqueued_frame = -1;
};

constexpr size_t kDeferredFreeQueueCapacity = 16;
// 帧龄下限（毫秒）：仅覆盖帧号不可用的早期路径；正常路径按帧号判定跨帧。
constexpr uint64_t kDeferredFreeMinResidenceMs = 50;
// 池临界水位：池占用仅作诊断，状态跨越该值时记一次 WARN，不参与释放门禁。
constexpr size_t kCriticalWatermark = 276;
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
        item, handle, generation, deferred_free_now_ms_locked(), data_frame_count()};
    if (g_deferred_free_queue_size >= kDeferredFreeQueueCapacity) {
        // 正常调用方均持有 ownership ledger handle；该保管区容量覆盖 ledger
        // 上限，因此队列满不会把释放失败传播给事务或丢失对象指针。
        if (g_deferred_free_custody_size >= g_deferred_free_custody.size()) {
            VIRTBAG_LOG_ERROR("ERROR deferred free custody overflow item=%p handle=%u generation=%llu",
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

// 只读物品对象池占用。池 GOT 或基址未就绪（主菜单早期 / 池销毁重建）时返回 false，
// 不阻塞任何调用路径；调用方据 false 跳过诊断。占用位取对象 +0x1c bit0。
// GOT 槽内值是 &ITEMPOOL_pPool 而非池基址，须先校验槽值再解一层；校验失败说明
// 重定位未就绪或版本不符，此时不盲扫，直接返回 false。池对象步长 0x28，池基址实测
// 4 字节对齐（地址 ≡ 4 mod 8），8 字节检查会误拒。
bool extension_bag_item_pool_usage(size_t* out_used, size_t* out_total) {
    if (out_used == nullptr || out_total == nullptr) return false;
    *out_used = 0;
    *out_total = 0;
    if (g_base == 0) return false;
    const uint8_t* const* got_slot =
        reinterpret_cast<const uint8_t* const*>(g_base + G_ITEMPOOL_BASE_GOT_VMA);
    const uint8_t* got = *got_slot;
    if (got != reinterpret_cast<const uint8_t*>(g_base + ITEMPOOL_POOL_VAR_VMA)) return false;
    const uint8_t* pool = *reinterpret_cast<const uint8_t* const*>(got);
    if (pool == nullptr || (reinterpret_cast<uintptr_t>(pool) & 0x3u) != 0) return false;
    size_t used = 0;
    for (size_t index = 0; index < ITEMPOOL_CAPACITY; ++index) {
        const uint8_t flags = pool[index * ITEMPOOL_OBJECT_STRIDE + ITEMPOOL_USED_FLAG_OFFSET];
        if ((flags & 1u) != 0) ++used;
    }
    *out_used = used;
    *out_total = ITEMPOOL_CAPACITY;
    return true;
}

// 触摸窗口与帧龄下限始终生效，逐对象引用检查始终生效。池水位不参与门禁：控件释放把
// data 先清再入队时，moving 控件身份与未扫描 UI 持有无法被逐对象检查覆盖，放宽门禁
// 会在该窗口释放仍被引用的对象。
void drain_deferred_frees_locked() {
    if (object_touch_window_active_locked()) return;
    const size_t queue_size = g_deferred_free_queue_size;
    const size_t custody_size = g_deferred_free_custody_size;
    if (queue_size + custody_size == 0) return;

    const int64_t current_frame = data_frame_count();
    const uint64_t now_ms = deferred_free_now_ms_locked();
    const auto residence_ready = [&](const DeferredItemFree& entry) {
        const uint64_t elapsed =
            now_ms >= entry.enqueued_at_ms ? now_ms - entry.enqueued_at_ms : 0;
        return deferred_free_rules::residence_met(entry.enqueued_frame, current_frame, elapsed,
                                                  kDeferredFreeMinResidenceMs);
    };

    // 第一段只遍历环形队列；custody 由第二段独立遍历，避免 custody 项被环形回绕重复访问。
    std::array<DeferredItemFree,
               deferred_free_rules::kept_capacity(kDeferredFreeQueueCapacity,
                                                  ownership::kLedgerCapacity)>
        kept{};
    size_t kept_count = 0;
    size_t freed_count = 0;
    bool logged_null_queue = false;
    const size_t queue_bound =
        deferred_free_rules::queue_iteration_bound(queue_size, custody_size);
    for (size_t offset = 0; offset < queue_bound; ++offset) {
        const size_t index = (g_deferred_free_queue_head + offset) % kDeferredFreeQueueCapacity;
        const DeferredItemFree entry = g_deferred_free_queue[index];
        if (entry.item == nullptr) {
            if (!logged_null_queue) {
                VIRTBAG_LOG_ERROR("ERROR deferred free drain null queue item index=%zu", index);
                logged_null_queue = true;
            }
            continue;
        }
        if (!residence_ready(entry) || deferred_item_referenced_locked(entry.item) ||
            fn_itempool_free == nullptr) {
            kept[kept_count++] = entry;
            continue;
        }
        fn_itempool_free(entry.item);
        ++freed_count;
    }
    g_deferred_free_queue = {};
    g_deferred_free_queue_head = 0;
    g_deferred_free_queue_size = 0;
    // 队列回填上界是队列容量；第一段只产生队列项，越界即记账错误而非扩容。
    if (kept_count <= kDeferredFreeQueueCapacity) {
        for (size_t index = 0; index < kept_count; ++index) {
            g_deferred_free_queue[index] = kept[index];
        }
        g_deferred_free_queue_size = kept_count;
    } else {
        VIRTBAG_LOG_ERROR("ERROR deferred free drain kept overflow queue kept=%zu capacity=%zu",
                          kept_count, kDeferredFreeQueueCapacity);
    }

    std::array<DeferredItemFree, ownership::kLedgerCapacity> kept_custody{};
    size_t kept_custody_count = 0;
    bool logged_null_custody = false;
    for (size_t index = 0; index < custody_size; ++index) {
        const DeferredItemFree entry = g_deferred_free_custody[index];
        if (entry.item == nullptr) {
            if (!logged_null_custody) {
                VIRTBAG_LOG_ERROR("ERROR deferred free drain null custody item index=%zu", index);
                logged_null_custody = true;
            }
            continue;
        }
        if (!residence_ready(entry) || deferred_item_referenced_locked(entry.item) ||
            fn_itempool_free == nullptr) {
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

    if (!deferred_free_rules::freed_within_bound(freed_count, queue_size, custody_size)) {
        VIRTBAG_LOG_ERROR(
            "ERROR deferred free drain freed bound exceeded freed=%zu queue=%zu custody=%zu",
            freed_count, queue_size, custody_size);
    }
    size_t pool_used = 0;
    size_t pool_total = 0;
    if (extension_bag_item_pool_usage(&pool_used, &pool_total)) {
        VIRTBAG_LOG("deferred free drain count=%zu freed=%zu kept=%zu pool=%zu/%zu",
                    queue_size + custody_size, freed_count, kept_count + kept_custody_count,
                    pool_used, pool_total);
    } else {
        VIRTBAG_LOG("deferred free drain count=%zu freed=%zu kept=%zu",
                    queue_size + custody_size, freed_count, kept_count + kept_custody_count);
    }
}

// 世界态帧任务（kFramePointLogicPre）：在任意界面/世界状态按帧排空延迟释放队列，
// 使排空不再依赖背包/装备界面被绘制（Scene_Draw_EQUIP draw-end）。try_lock 拿不到锁
// 时直接返回，下帧重试，避免主线程等待 JNI/HTTP/lifecycle 线程持锁。
bool extension_bag_deferred_free_tick(int64_t /*frame*/, void* /*ctx*/) {
    if (g_deferred_free_queue_size + g_deferred_free_custody_size == 0) return true;
    size_t pool_used = 0;
    size_t pool_total = 0;
    // 池占用仅作诊断，不参与门禁；仅记录 critical 状态的跨越沿，避免每帧刷屏。
    const bool pool_known = extension_bag_item_pool_usage(&pool_used, &pool_total);
    const bool critical = pool_known && pool_used >= kCriticalWatermark;
    static bool s_critical_logged = false;
    if (critical && !s_critical_logged) {
        VIRTBAG_LOG_WARN("WARN pool critical used=%zu total=%zu", pool_used, pool_total);
    }
    s_critical_logged = critical;
    std::unique_lock<std::mutex> lock(g_virtual_bag_mtx, std::try_to_lock);
    if (!lock.owns_lock()) return true;
    drain_deferred_frees_locked();
    return true;
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
        VIRTBAG_LOG_ERROR("ERROR ownership release changed during custody retirement context=%s handle=%u",
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
