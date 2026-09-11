bool is_backpack_category(int category) {
    // 这是 State::types 使用的 BagType 判定：1..4 派生容量 4/8/12/16，
    // 只适用于已归一化的扩展袋类型。它不是原生物品类别判定；原生物品
    // 详情必须使用 category_is_extension_backpack 的 ITEMCLASSBASE +2 规则。
    return category >= 1 && category <= 4;
}

uint64_t extension_tab_item_proc(void* ctrl, uint64_t event, void* x2, void* param) {
    const uint64_t observation_token = virtual_bag_observe_item_proc_pre(ctrl, event, x2, param);
    const auto finish = [observation_token, ctrl, event, x2, param](uint64_t result) {
        virtual_bag_observe_item_proc_post(observation_token, ctrl, event, x2, param, result);
        return result;
    };
    if (event == 0x02) {
        {
            std::unique_lock<std::mutex> lock(g_virtual_bag_mtx);
            const int index = extension_tab_index(ctrl);
            if (index >= 0) {
                queue_extension_tab_click_locked(index);
            }
        }
        return finish(1);  // 松开确认已处理（原版袋标签点击切袋同样返回 1，b8c60）
    }
    // 0x04 = 拖动 drop 到袋控件。无论原版源还是扩展源，只要目标袋未装备且
    // 源类别为背包物品（category 1..4）就走装备；其他情况走移动。装备路径
    // 返回 0，让 TouchHandle 按原版语义复位 moving。
    if (event == 0x04) {
        uint64_t result = 0;
        {
            std::unique_lock<std::mutex> lock(g_virtual_bag_mtx);
            const int index = extension_tab_index(ctrl);
            if (index >= 0 &&
                g_extension_drag_session.phase != virtual_bag::DragPhase::kIdle) {
                const int logical_bag = virtual_bag::extension_logical_bag(index);
                const bool handled = logical_bag >= 0 &&
                                     route_projected_session_to_tab_locked(lock, logical_bag);
                if (logical_bag < 0) {
                    VIRTBAG_LOG("cross tab reject reason=target_tab_index_invalid index=%d", index);
                }
                result = handled ? 1 : 0;
            } else {
                void* moving = touch_moving_item_control_locked();
                // 原版不可堆叠装备在 drop 事件到达时可能已经清掉
                // ITEM_CTRL_MOVING_FLAG，但事件参数仍保留真实 source control。
                // 优先使用 TouchHandle 状态，只有状态已清空时才回退到事件源，避免
                // 放宽普通点击/无效控件的处理范围。
                if (moving == nullptr && param != nullptr) {
                    moving = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(param) + 8);
                }
                if (index >= 0 && moving != nullptr) {
                    void* data = fn_control_object_get_data != nullptr
                        ? fn_control_object_get_data(moving) : nullptr;
                    void* item = data != nullptr ? *reinterpret_cast<void**>(data) : nullptr;
                    int module_bag = -1;
                    int module_slot = -1;
                    const bool extension_source =
                        item != nullptr && module_slot_of_item_locked(item, &module_bag, &module_slot);
                    const int category = extension_source
                        ? g_virtual_bag_state.items[module_bag][module_slot].category
                        : original_item_category_locked(item);
                    const bool backpack_item = is_backpack_category(category);
                    if (g_virtual_bag_state.types[index] == 0 && backpack_item) {
                        // 背包类物品进入空扩展标签 = 装备为该袋的袋对象；来源可以是
                        // 原版背包，也可以是另一个扩展袋。两者都由同一事务处理。
                        try_equip_on_extension_tab_drop_locked(lock, index, moving);
                    } else if (extension_source) {
                        // 目标袋已有背包对象，或来源是装备/其他物品：一律按普通
                        // 扩展袋间移动处理，不能把“背包槽装备”误当成人物装备。
                        const bool moved = move_extension_to_extension_locked(
                            module_bag, module_slot, index, -1);
                        if (moved) {
                            persist_state_locked(false);
                            refresh_projected_module_view_after_move_locked();
                        }
                        VIRTBAG_LOG("extension tab drop extension move result=%d index=%d category=%d",
                                    moved ? 1 : 0, index, category);
                    } else {
                        const bool moved = move_original_to_extension_locked(lock, index, moving);
                        VIRTBAG_LOG("extension tab drop original move result=%d index=%d category=%d equipped=%d",
                                    moved ? 1 : 0, index, category,
                                    g_virtual_bag_state.types[index] != 0 ? 1 : 0);
                    }
                }
            }
        }
        return finish(result);
    }
    // 其余事件（含 0x10 拖动发起查询：TouchHandle_MoveOn @a2fec-a304c 派发，
    // proc 返回 1 即登记 moving 控件）一律返回 0——兜底返回 1 会让标签成为
    // 拖动源（标签可被拖出背包物品的真因，且拖出物可被 drop 入库造成复制）。
    // 原版控件对未处理事件同样返回 0。
    return finish(0);
}

// 扩展标签 = ControlItem（与原版袋标签同类）：挂袋容器（0x3049e0+0x50），
// SetUserType(2) + SetControlProc（自定义，0x02 松开确认切换/详情 toggle）。
// data[0] = 按袋 types 物化的真实背包物品对象（TouchHandle/详情链合法）。
// 手工父链累加（自上而下）：计算控件绝对位置。CO_RECT 为 i64 相对父值。
void install_extension_tab_buttons_locked() {
    if (g_base == 0 || fn_control_object_get_child == nullptr ||
        fn_control_item_set_item == nullptr || fn_mem_malloc == nullptr ||
        fn_control_object_set_control_proc == nullptr ||
        fn_control_object_set_user_type == nullptr ||
        fn_touch_handle_unuse_control_event_move == nullptr ||
        fn_touch_handle_delete_control == nullptr ||
        fn_ctrl_get_count == nullptr) {
        return;
    }
    void* bag_container = *reinterpret_cast<void**>(g_base + G_UIEQUIP_PANEL_BAG_CONTAINER_VMA);
    if (bag_container == nullptr) return;
    // 容器就位守卫：面板打开初期 0x50 槽位指向占位容器（无原版袋标签子控件，
    // rect 未定位）。此时挂载会让标签落在占位容器上（绝对 x≈296 出位）。
    // 等原版 6 袋标签就位（child count ≥ 6）后再挂载；未就位直接跳过，
    // 由 draw 每帧检测容器就位后重试。
    if (fn_ctrl_get_count != nullptr && fn_ctrl_get_count(bag_container) < 6) return;
    // 动态计算（用户方案）：打开面板后，原版袋 0 按钮的绝对位置 =
    // 袋容器绝对（父链累加，袋 0 相对容器为 (0,0)）。缓存至面板重建。
    // 挂载层 = 袋容器（与原版 6 袋标签同父，跟随容器动画，天然不漂移）。
    int64_t bag0_abs_x = 0, bag0_abs_y = 0;
    extension_bag_control_abs_pos(bag_container, &bag0_abs_x, &bag0_abs_y);
    void* mount_parent = bag_container;
    int64_t mount_abs_x = 0, mount_abs_y = 0;
    extension_bag_control_abs_pos(mount_parent, &mount_abs_x, &mount_abs_y);
    // 标签相对挂载层 = 原版袋列右侧（袋 0 绝对 + (68,2)，减挂载层绝对）
    const int64_t tab_rel_x = bag0_abs_x + 68 - mount_abs_x;
    const int64_t tab_rel_y = bag0_abs_y + 2 - mount_abs_y;
    if (g_extension_tab_generation == g_inventory_generation &&
        g_extension_tab_root == mount_parent && g_extension_tab_buttons[0] != nullptr) {
        return;
    }
    // 重建：先删旧标签（挂载层控件重建后旧指针无效）
    for (int index = 0; index < virtual_bag::kBagCount; ++index) {
        if (g_extension_tab_buttons[index] != nullptr) {
            fn_touch_handle_delete_control(g_extension_tab_buttons[index]);
            g_extension_tab_buttons[index] = nullptr;
        }
    }
    refresh_tab_bag_items_locked();
    for (int index = 0; index < virtual_bag::kBagCount; ++index) {
        // ControlItem_Create 语义组装：Add(parent, 0, 0, type=3, proc) + SetControlProc + SetUserType(2)。
        // 第 5 参 = proc，经 CreateControlInfo 写入 +0x90（CO_PROC，TouchHandle 统一事件分发）。
        // 传 nullptr 会让 +0x90 为空 → TouchHandle 命中后无法分发到 +0x98 control proc（点击失效）。
        const uintptr_t add_fn =
            g_base + fn_resolve("F_CONTROL_OBJECT_ADD_CONTROL_OBJECT_VMA",
                                F_CONTROL_OBJECT_ADD_CONTROL_OBJECT_VMA);
        typedef void* (*AddFn)(void*, void*, void*, uint32_t, void*);
        void* ctrl = reinterpret_cast<AddFn>(add_fn)(
            mount_parent, nullptr, nullptr, 3,
            reinterpret_cast<void*>(g_base + F_TOUCH_HANDLE_CONTROL_EVENT_PROC_VMA));
        if (ctrl == nullptr) {
            VIRTBAG_LOG("extension tab create failed index=%d", index);
            continue;
        }
        fn_control_object_set_control_proc(ctrl,
            reinterpret_cast<void*>(&extension_tab_item_proc));
        fn_control_object_set_user_type(ctrl, 2);
        fn_touch_handle_unuse_control_event_move(ctrl);
        // rect：与原版袋标签同尺寸 57x57（loc6/loc9 底框 47x47 居中填充成背景框）。
        // 相对场景根（不随切袋移动），由动态计算的原版袋列绝对位置换算。
        const int64_t row_y = tab_rel_y + index * 70;
        uint8_t* c = reinterpret_cast<uint8_t*>(ctrl);
        *reinterpret_cast<int64_t*>(c + CO_RECT_X) = tab_rel_x;
        *reinterpret_cast<int64_t*>(c + CO_RECT_Y) = row_y;
        *reinterpret_cast<int64_t*>(c + CO_RECT_W) = 57;
        *reinterpret_cast<int64_t*>(c + CO_RECT_H) = 57;
        // data：16B（+0x0=item、+0x8..0xb=标志），装箱背包物品对象
        void* data = fn_mem_malloc(0x10);
        if (data == nullptr) continue;
        std::memset(data, 0, 0x10);
        *reinterpret_cast<void**>(data) = g_tab_bag_items[index];
        fn_ctrl_set_data(ctrl, data);
        g_extension_tab_buttons[index] = ctrl;
    }
    g_extension_tab_root = mount_parent;
    g_extension_tab_generation = g_inventory_generation;
    VIRTBAG_LOG("extension tabs installed generation=%llu root=%p",
                static_cast<unsigned long long>(g_inventory_generation), bag_container);
}

void disable_extension_tab_buttons_locked() {
    // The game owns these controls. Invalidate module references without
    // touching them after popup teardown, where the pointers may be stale.
    ++g_inventory_generation;
    g_pending_extension_tab = -1;
    g_pending_extension_tab_generation = 0;
    g_extension_tab_buttons.fill(nullptr);
    g_extension_tab_root = nullptr;
    g_extension_tab_generation = 0;
}

int raw_direct_bag_locked() {
    if (g_base == 0) return -1;
    return *reinterpret_cast<uint8_t*>(g_base + G_UIEQUIP_CUR_BAG_VMA);
}

int raw_got_bag_locked() {
    if (g_base == 0) return -1;
    uint8_t** current_bag = reinterpret_cast<uint8_t**>(g_base + G_UIEQUIP_CUR_BAG_GOT_VMA);
    return current_bag != nullptr && *current_bag != nullptr ? **current_bag : -1;
}

int raw_desc_type_locked() {
    return g_base == 0 ? -1 : *reinterpret_cast<uint8_t*>(g_base + G_UIEQUIP_DESC_TYPE_VMA);
}

void log_exit_trace_locked(const char* phase, uint64_t event, uint64_t param, uint64_t param2,
                           int64_t x = -1, int64_t y = -1) {
    const uint64_t sequence = g_exit_trace_sequence.fetch_add(1) + 1;
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    VIRTBAG_LOG("exit seq=%llu ms=%lld phase=%s event=0x%llx param=0x%llx param2=0x%llx x=%lld y=%lld mode=%d selected=%d inspected=%d overlay=%d overlay_index=%d direct=%d got=%d desc=%d saved_direct=%u saved_got=%u",
                static_cast<unsigned long long>(sequence), static_cast<long long>(milliseconds), phase,
                static_cast<unsigned long long>(event), static_cast<unsigned long long>(param),
                static_cast<unsigned long long>(param2), static_cast<long long>(x), static_cast<long long>(y),
                static_cast<int>(g_virtual_bag_state.mode), g_virtual_bag_state.selected,
                g_virtual_bag_state.inspected, g_module_view_installed ? 1 : 0, g_module_view_index,
                raw_direct_bag_locked(), raw_got_bag_locked(), raw_desc_type_locked(),
                static_cast<unsigned int>(g_original_current_direct),
                static_cast<unsigned int>(g_original_current_got));
}

uint8_t* find_popup_state_entry(uintptr_t enter_target) {
    if (g_base == 0) return nullptr;
    void** got = reinterpret_cast<void**>(g_base + G_POPUP_STATE_LIST_GOT_VMA);
    if (got == nullptr || *got == nullptr) return nullptr;
    uint8_t* list = reinterpret_cast<uint8_t*>(*got);
    for (int index = 0; index < kPopupStateCount; ++index) {
        uint8_t* entry = list + index * kPopupStateSize;
        uintptr_t enter = *reinterpret_cast<uintptr_t*>(entry + POPUP_ENTRY_ENTER);
        if (enter == enter_target) {
            return entry;
        }
    }
    return nullptr;
}

uint8_t* find_inventory_state_entry() {
    return find_popup_state_entry(g_base + fn_resolve("F_PANEL_INVENTORY_ENTER", F_PANEL_INVENTORY_ENTER));
}

JNIEnv* current_env() {
    JavaVM* jvm = g_jvm();
    if (jvm == nullptr) return nullptr;
    JNIEnv* env = nullptr;
    if (jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK) return env;
    return jvm->AttachCurrentThread(&env, nullptr) == JNI_OK ? env : nullptr;
}

void ensure_state_loaded_locked();
bool persist_state_locked(bool force = false);
void clear_original_desc_locked();
void clear_original_item_selection_locked();
bool extension_tab_hit(int index, int64_t x, int64_t y);
int original_bag_locked();
void set_original_bag_locked(int bag);
uint32_t* bag_size_word_locked(int bag);
int original_bag_button_index(int64_t x, int64_t y);
bool install_module_view_locked(int bag);
void restore_module_view_locked();
void refresh_projection_if_overwritten_locked();
void refresh_projected_module_view_after_move_locked();
void refresh_module_item_area_locked(int index);
void* module_item_locked(int bag, int slot);
void recover_pending_transaction_locked(std::unique_lock<std::mutex>& lock);
bool prepare_main_menu_locked();
bool free_module_object_locked(int bag, int slot, void* use_token = nullptr);
bool module_slot_is_assignable_locked(int bag, int slot);
bool module_object_replacement_allowed_locked(int bag, int slot, const char* context);
bool move_original_to_extension_locked(std::unique_lock<std::mutex>& lock, int dst_bag,
                                       void* moving_control);
bool move_original_to_extension_slot_locked(std::unique_lock<std::mutex>& lock, int src_bag,
                                            int src_slot, int dst_bag);
bool move_extension_to_original_locked(std::unique_lock<std::mutex>& lock, int src_bag,
                                       int src_slot, int target_bag);
bool move_extension_to_extension_locked(int src_bag, int src_slot, int dst_bag,
                                        int requested_dst_slot = -1);
bool handle_bag_drop_release_locked(std::unique_lock<std::mutex>& lock, int64_t x, int64_t y);
bool defer_item_free_locked(void* item);
bool item_pointer_known_locked(void* item);
void* valid_child_locked(void* root, int slot);
void make_desc_equip_gate(void* ctrl, void* arg);
void extension_desc_item_execute(void* button);
void reset_extension_desc_item_hooks_locked();
void install_extension_desc_item_hooks_locked(void* item);
void restore_extension_desc_popup_callbacks_locked();
bool consume_extension_item_after_native_locked(int bag, int slot, void* item,
                                                int before_count, int observed_before,
                                                const char* context, void* use_token);

void reset_extension_desc_unequip_hook_locked() {
    // 卸下按钮不再有 execute-slot PtrHook（函数级 ButtonUnequipExe hook 承担），
    // 仅清 desc 袋记录。
    g_extension_desc_unequip_bag = -1;
}

void install_extension_desc_unequip_hook_locked(int extension_bag) {
    reset_extension_desc_unequip_hook_locked();
    if (!virtual_bag::valid_index(extension_bag)) return;
    // 扩展袋 desc 打开时记录袋位，供函数级 ButtonUnequipExe hook
    // （virtual_bag_handle_original_bag_unequip）识别扩展袋解除路径。
    g_extension_desc_unequip_bag = extension_bag;
    VIRTBAG_LOG("extension desc unequip armed bag=%d", extension_bag);
}

// P3：扩展袋切换音效，与原版袋按钮同款（UIEquip_InvenBagControlEventProc b8c34：Play(0x11)）。
void play_extension_switch_sound() {
    if (fn_sound_system_play == nullptr || g_snd_fx == nullptr) return;
    fn_sound_system_play(0x11);
}

void handle_extension_tab_click_locked(int extension_bag) {
    if (!virtual_bag::valid_index(extension_bag)) return;
    ensure_state_loaded_locked();
    if (g_virtual_bag_state.mode == virtual_bag::Mode::kOriginal) {
        const int original_bag = original_bag_locked();
        if (original_bag == virtual_bag::kOriginalTaskBag) {
            constexpr uint32_t kCapacityMask = (1u << 25) - 1u;
            void* panel_root = g_base != 0
                ? *reinterpret_cast<void**>(g_base + G_UIEQUIP_PANEL_CTRL_VMA)
                : nullptr;
            const int target_capacity = g_virtual_bag_state.capacities[extension_bag];
            const bool target_ready = target_capacity > 0 &&
                                       fn_control_item_set_item != nullptr &&
                                       fn_control_object_get_child != nullptr &&
                                       panel_root != nullptr;
            int projection_host = -1;
            if (target_ready) {
                for (int candidate = 0;
                     candidate < virtual_bag::kOriginalTransactionBagCount; ++candidate) {
                    uint32_t* size_word = bag_size_word_locked(candidate);
                    if (size_word == nullptr || (*size_word & kCapacityMask) == 0) continue;
                    bool controls_ready = true;
                    for (int slot = 0; slot < target_capacity; ++slot) {
                        if (valid_child_locked(panel_root, slot) == nullptr) {
                            controls_ready = false;
                            break;
                        }
                    }
                    if (controls_ready) {
                        projection_host = candidate;
                        break;
                    }
                }
            }
            if (projection_host < 0) {
                VIRTBAG_LOG(
                    "extension tab reject reason=projection_host_unavailable source_original_bag=%d target=%d",
                    original_bag, extension_bag);
                return;
            }
            set_original_bag_locked(projection_host);
            g_original_current_direct = static_cast<uint8_t>(projection_host);
            g_original_current_got = static_cast<uint8_t>(projection_host);
            if (!install_module_view_locked(extension_bag)) {
                set_original_bag_locked(original_bag);
                g_original_current_direct = static_cast<uint8_t>(original_bag);
                g_original_current_got = static_cast<uint8_t>(original_bag);
                VIRTBAG_LOG(
                    "extension tab reject reason=projection_host_unavailable source_original_bag=%d target=%d host=%d",
                    original_bag, extension_bag, projection_host);
                return;
            }
            if (virtual_bag::click(&g_virtual_bag_state, extension_bag) !=
                virtual_bag::ClickResult::kSelected) {
                restore_module_view_locked();
                set_original_bag_locked(original_bag);
                g_original_current_direct = static_cast<uint8_t>(original_bag);
                g_original_current_got = static_cast<uint8_t>(original_bag);
                VIRTBAG_LOG(
                    "extension tab reject reason=projection_host_unavailable source_original_bag=%d target=%d host=%d",
                    original_bag, extension_bag, projection_host);
                return;
            }
            persist_state_locked();
            VIRTBAG_LOG("extension tab selected bag=%d original_bag=%d projection_host=%d",
                        extension_bag, original_bag, projection_host);
            return;
        }
        if (virtual_bag::valid_original_transaction_bag(original_bag)) {
            g_original_current_direct = static_cast<uint8_t>(original_bag);
            g_original_current_got = static_cast<uint8_t>(original_bag);
            if (virtual_bag::click(&g_virtual_bag_state, extension_bag) !=
                virtual_bag::ClickResult::kIgnored) {
                install_module_view_locked(extension_bag);
                persist_state_locked();
                VIRTBAG_LOG("extension tab selected bag=%d original_bag=%d", extension_bag,
                            original_bag);
            }
        } else {
            VIRTBAG_LOG("extension tab reject original window reason=invalid_transaction_domain bag=%d",
                        original_bag);
        }
        return;
    }
    if (g_virtual_bag_state.mode == virtual_bag::Mode::kModule) {
        if (g_virtual_bag_state.selected != extension_bag) {
            if (virtual_bag::click(&g_virtual_bag_state, extension_bag) !=
                virtual_bag::ClickResult::kIgnored) {
                install_module_view_locked(extension_bag);
                play_extension_switch_sound();
                VIRTBAG_LOG("extension tab switched bag=%d", extension_bag);
            }
            return;
        }
        // 二次点击 = 原版袋信息面板（原版语义：desc_type=1 + UIEquip_MakeDesc）。
        // MakeDesc 读控件物品（data[0]=袋物品对象）生成详情，SetDescMenu 按
        // desc_type=1 加解除按钮；关闭详情经 UIDesc_SetOff。
        if (virtual_bag::click(&g_virtual_bag_state, extension_bag) ==
            virtual_bag::ClickResult::kInspected) {
            play_extension_switch_sound();
            void* ctrl = g_extension_tab_buttons[extension_bag];
            if (ctrl != nullptr && fn_ui_equip_make_desc != nullptr && g_base != 0) {
                // 门禁掩码（P3）：UIEquip_SetDescMenu desc_type=1 分支经 GOT 槽读
                // 当前原版袋索引，0（初始袋）/5（任务袋）短路不生成解除按钮
                // （b8570-b8578）。扩展袋与该全局无关——进入前所在原版袋决定
                // 按钮有无。MakeDesc 同步尾跳 SetDescMenu（b89c0），临时把
                // direct+GOT 双字节掩码为 1 使按钮恒生成，返回后立即恢复快照
                // （成对写约定同 set_original_bag_locked；按钮在 panel+0x68，
                // 不随恢复消失，点击回调已被模块 hook 接管）。此路径仅在游戏
                // 线程的 commit_pending_extension_tab 内可达，无跨线程窗口。
                uint8_t* direct_bag =
                    reinterpret_cast<uint8_t*>(g_base + G_UIEQUIP_CUR_BAG_VMA);
                uint8_t** current_bag =
                    reinterpret_cast<uint8_t**>(g_base + G_UIEQUIP_CUR_BAG_GOT_VMA);
                const bool got_valid = current_bag != nullptr && *current_bag != nullptr;
                const uint8_t saved_direct = *direct_bag;
                const uint8_t saved_got = got_valid ? **current_bag : 0;
                *direct_bag = 1;
                if (got_valid) **current_bag = 1;
                *reinterpret_cast<uint8_t*>(g_base + G_UIEQUIP_DESC_TYPE_VMA) = 1;
                fn_ui_equip_make_desc(ctrl, nullptr);
                *direct_bag = saved_direct;
                if (got_valid) **current_bag = saved_got;
                install_extension_desc_unequip_hook_locked(extension_bag);
            }
            VIRTBAG_LOG("extension bag desc opened bag=%d", extension_bag);
        }
    }
}

void queue_extension_tab_click_locked(int extension_bag) {
    if (!virtual_bag::valid_index(extension_bag) ||
        g_extension_tab_generation != g_inventory_generation) {
        return;
    }
    if (g_pending_extension_tab == extension_bag &&
        g_pending_extension_tab_generation == g_inventory_generation) {
        return;
    }
    g_pending_extension_tab = extension_bag;
    g_pending_extension_tab_generation = g_inventory_generation;
    VIRTBAG_LOG("extension tab pending bag=%d generation=%llu", extension_bag,
                static_cast<unsigned long long>(g_inventory_generation));
}

void extension_tab_button_clicked(void* ctrl) {
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    const int extension_bag = extension_tab_index(ctrl);
    queue_extension_tab_click_locked(extension_bag);
}

void commit_pending_extension_tab_locked() {
    if (!virtual_bag::valid_index(g_pending_extension_tab) ||
        g_pending_extension_tab_generation != g_inventory_generation ||
        g_extension_tab_generation != g_inventory_generation) {
        g_pending_extension_tab = -1;
        g_pending_extension_tab_generation = 0;
        return;
    }
    const int extension_bag = g_pending_extension_tab;
    g_pending_extension_tab = -1;
    g_pending_extension_tab_generation = 0;
    handle_extension_tab_click_locked(extension_bag);
}

bool module_cache_clear_preflight_locked() {
    for (int bag = 0; bag < virtual_bag::kBagCount; ++bag) {
        for (int slot = 0; slot < virtual_bag::kSlotCount; ++slot) {
            const ModuleUseState& use = g_module_object_use[bag][slot];
            if (use.active) {
                VIRTBAG_LOG("module cache clear preflight rejected active object bag=%d slot=%d item=%p generation=%llu",
                            bag, slot, use.item,
                            static_cast<unsigned long long>(use.generation));
                return false;
            }
        }
    }
    return true;
}

bool clear_module_cache_locked() {
    if (!module_cache_clear_preflight_locked()) return false;
    for (int bag = 0; bag < virtual_bag::kBagCount; ++bag) {
        for (int slot = 0; slot < virtual_bag::kSlotCount; ++slot) {
            // Scheme C objects never enter g_inven, so the module owns them
            // until an explicit transfer is implemented. Drop them on slot
            // exit instead of retaining stale pointers across saves.
            if (!free_module_object_locked(bag, slot)) {
                VIRTBAG_LOG("module cache clear aborted; retained object bag=%d slot=%d item=%p",
                            bag, slot, g_module_objects[bag][slot]);
                return false;
            }
        }
    }
    cancel_projected_drag_session_locked("module-cache-clear");
    reset_extension_desc_item_hooks_locked();
    g_extension_desc_item = nullptr;
    g_extension_desc_item_bag = -1;
    g_extension_desc_item_slot = -1;
    g_original_inventory.fill(nullptr);
    // 标签袋物品对象是 ItemPool 里的裸指针，跨存档槽位加载必须失效：游戏
    // SAVE_LoadItem 重置/回收池内存，旧指针会被新读入的存档物品复用（真机
    // 实证：返回主菜单不保存重进存档后，扩展标签/信息页显示成药水、金币）。
    // 此处只清引用不释放——对象可能已被池回收，再 ItemPool_Free 是 UAF；
    // 下次 install_extension_tab_buttons 按 types 重建新对象。
    for (void*& tab_item : g_tab_bag_items) tab_item = nullptr;
    g_original_bag_size_word = nullptr;
    g_original_bag_size = 0;
    g_module_window_original_bag = -1;
    g_original_current_direct = 0;
    g_original_current_got = 0;
    g_module_view_installed = false;
    g_module_view_index = -1;
    g_item_state_dirty = false;
    g_extension_touch_capture = false;
    g_extension_drag = {};
    return true;
}

bool serialize_item_payload_locked(void* item,
                                   std::array<uint8_t, virtual_bag::kSerializedItemBuffer>* out,
                                   int* out_size) {
    return virtual_bag::save_item_payload(fn_save_save_item, item, out, out_size) ==
           virtual_bag::PayloadValidation::kOk;
}

void release_temporary_item_locked(void* item) {
    if (item != nullptr && fn_itempool_free != nullptr) fn_itempool_free(item);
}

void* load_item_payload_locked(const uint8_t* payload, int payload_size, const char* context) {
    const virtual_bag::ManagedLoadResult result = virtual_bag::load_item_payload_exact(
        payload, payload_size, fn_save_save_item, fn_save_load_item, fn_itempool_free);
    if (result.item != nullptr) return result.item;

    VIRTBAG_LOG("payload bridge reject context=%s reason=%s validation=%s size=%d",
                context,
                virtual_bag::managed_load_failure_reason(result.failure),
                virtual_bag::payload_validation_reason(result.validation), payload_size);
    return nullptr;
}

void ensure_state_loaded_locked() {
    const int slot = current_save_slot();
    if (slot < 0 || slot > 2 || slot == g_loaded_slot) return;
    if (!module_cache_clear_preflight_locked()) {
        VIRTBAG_LOG("state load blocked by active module object slot=%d", slot);
        return;
    }
    if (g_module_view_installed) restore_module_view_locked();
    if (!clear_module_cache_locked()) {
        VIRTBAG_LOG("module cache clear incomplete; retaining loaded slot=%d", slot);
        return;
    }
    g_virtual_bag_state = {};
    const bool state_loaded = extension_bag_load_state_from_store(slot);
    if (!state_loaded) {
        VIRTBAG_LOG("virtual bag state slot=%d unavailable; using empty state", slot);
    }
    // mode/selected 是背包面板会话作用域字段：读档时世界界面必然从原版视图开始。
    // 仅清除无 pending 的 kModule 残留（kExitingModule / pending 交给恢复流程）。
    if (g_virtual_bag_state.mode == virtual_bag::Mode::kModule &&
        !g_virtual_bag_state.pending.valid) {
        g_virtual_bag_state.mode = virtual_bag::Mode::kOriginal;
        g_virtual_bag_state.selected = -1;
        g_virtual_bag_state.inspected = -1;
        VIRTBAG_LOG("view state residue cleared on load slot=%d", slot);
    }
    g_loaded_slot = slot;
}

// ---- 跨包物品移动事务（v0.7.0）----
// 顺序固定：快照/校验目标 → 内存提交 → 变更原版 → fn_save → sidecar 成功后清理回滚日志。
// 未保存的改动不写入 sidecar；pending 仅保留当前进程内的事务状态。

bool inventory_slot_locked(int bag, int slot, void** out) {
    if (g_inven == nullptr || out == nullptr || bag < 0 || bag >= 6 || slot < 0 ||
        slot >= virtual_bag::kSlotCount) {
        return false;
    }
    *out = static_cast<void**>(g_inven)[bag * kInventorySlotStride + slot];
    return true;
}

bool original_inventory_contains_payload_locked(const uint8_t* payload, int payload_size,
                                                int target_bag) {
    if (g_inven == nullptr || payload == nullptr || fn_save_save_item == nullptr ||
        payload_size <= 0 || !virtual_bag::valid_original_transaction_bag(target_bag)) {
        return false;
    }
    void** inventory = static_cast<void**>(g_inven);
    for (int slot = 0; slot < virtual_bag::kSlotCount; ++slot) {
        void* item = inventory[target_bag * kInventorySlotStride + slot];
        if (item == nullptr) continue;
        std::array<uint8_t, virtual_bag::kSerializedItemBuffer> serialized{};
        int size = 0;
        if (!serialize_item_payload_locked(item, &serialized, &size) || size != payload_size) continue;
        if (std::memcmp(serialized.data(), payload, static_cast<size_t>(size)) == 0) return true;
    }
    return false;
}

int original_inventory_payload_slot_locked(const uint8_t* payload, int payload_size,
                                           int target_bag) {
    if (g_inven == nullptr || payload == nullptr || fn_save_save_item == nullptr ||
        payload_size <= 0 || !virtual_bag::valid_original_transaction_bag(target_bag)) {
        return -1;
    }
    void** inventory = static_cast<void**>(g_inven);
    for (int slot = 0; slot < virtual_bag::kSlotCount; ++slot) {
        void* item = inventory[target_bag * kInventorySlotStride + slot];
        if (item == nullptr) continue;
        std::array<uint8_t, virtual_bag::kSerializedItemBuffer> serialized{};
        int size = 0;
        if (serialize_item_payload_locked(item, &serialized, &size) && size == payload_size &&
            std::memcmp(serialized.data(), payload, static_cast<size_t>(size)) == 0) {
            return slot;
        }
    }
    return -1;
}

// ---- P4.4 五态事务协调：三条移动路径共享的唯一推进/回滚实现 ----

uint64_t g_transaction_sequence = 0;

void generate_transaction_id_locked(char* out) {
    const uint64_t seq = ++g_transaction_sequence;
    std::snprintf(out, virtual_bag::kMaxTransactionIdChars, "p4-%llu",
                  static_cast<unsigned long long>(seq));
}

// pending-recorded 阶段：事务 pending 写入 State。durable=false：仅本进程记录。
bool txn_record_pending_locked(virtual_bag::TransactionContext* txn) {
    virtual_bag::txn_build_pending(txn);
    if (!virtual_bag::valid_pending_transfer_domain(txn->pending) ||
        !virtual_bag::valid_pending_transfer_payload(txn->pending)) {
        VIRTBAG_LOG("txn pending reject id=%s reason=invalid_domain", txn->transaction_id);
        return false;
    }
    g_virtual_bag_state.pending = txn->pending;
    g_item_state_dirty = true;
    VIRTBAG_LOG("txn stage=pending-recorded durable=false id=%s direction=%u src=%u/%u dst=%u/%u",
                txn->transaction_id, static_cast<unsigned int>(txn->direction),
                static_cast<unsigned int>(txn->src_bag),
                static_cast<unsigned int>(txn->src_slot),
                static_cast<unsigned int>(txn->dst_bag),
                static_cast<unsigned int>(txn->dst_slot));
    return true;
}

// 按失败阶段回滚：先还原逻辑槽与 pending，再由调用方释放仍归模块所有的临时对象；
// 已移交原版库存的对象不参与（所有权账本终态）。
uint64_t isolation_now_ms_locked() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

// P4.5：pending 恢复失败隔离——原始袋槽值原样保留（可能越界，不正则化），
// payload 字节留作只读诊断。
void isolate_pending_locked(const char* reason, const char* detail) {
    const virtual_bag::PendingTransfer& pending = g_virtual_bag_state.pending;
    virtual_bag::IsolationRecord record{};
    record.valid = true;
    record.reason = reason;
    record.observed_at_ms = isolation_now_ms_locked();
    std::strncpy(record.transaction_id, pending.transaction_id,
                 virtual_bag::kMaxTransactionIdChars);
    record.direction = pending.direction;
    record.src_bag = pending.src_bag;
    record.src_slot = pending.src_slot;
    record.dst_bag = pending.dst_bag;
    record.dst_slot = pending.dst_slot;
    record.payload_size = pending.payload_size;
    record.payload = pending.payload;
    record.source_payload_size = pending.source_payload_size;
    record.source_payload = pending.source_payload;
    record.detail = detail;
    push_isolation(&g_virtual_bag_state, record);
}

// P4.5：ext→orig 失败隔离——扩展源逻辑物品保留（payload 字节未丢），记录
// 事务上下文与失败阶段供审计。
void isolate_ext2orig_failure_locked(const virtual_bag::TransactionContext& txn,
                                     virtual_bag::TxnStage phase, const char* reason,
                                     const char* detail) {
    virtual_bag::IsolationRecord record{};
    record.valid = true;
    record.reason = reason;
    record.observed_at_ms = isolation_now_ms_locked();
    std::strncpy(record.transaction_id, txn.transaction_id,
                 virtual_bag::kMaxTransactionIdChars);
    record.direction = txn.direction;
    record.phase = static_cast<int>(phase);
    record.src_bag = txn.src_bag;
    record.src_slot = txn.src_slot;
    record.dst_bag = txn.dst_bag;
    record.dst_slot = txn.dst_slot;
    record.payload_size = txn.source.payload_size;
    record.payload = txn.source.payload;
    record.detail = detail;
    push_isolation(&g_virtual_bag_state, record);
}

void txn_abort_locked(const virtual_bag::TransactionContext& txn,
                      virtual_bag::TxnStage failed_at, const char* reason) {
    virtual_bag::txn_rollback_logical(&g_virtual_bag_state, txn, failed_at);
    g_item_state_dirty = true;
    VIRTBAG_LOG("txn abort stage=%d id=%s reason=%s direction=%u src=%u/%u dst=%u/%u",
                static_cast<int>(failed_at), txn.transaction_id, reason,
                static_cast<unsigned int>(txn.direction),
                static_cast<unsigned int>(txn.src_bag),
                static_cast<unsigned int>(txn.src_slot),
                static_cast<unsigned int>(txn.dst_bag),
                static_cast<unsigned int>(txn.dst_slot));
}

void recover_pending_transaction_locked(std::unique_lock<std::mutex>& lock) {
    const virtual_bag::PendingTransfer pending = g_virtual_bag_state.pending;
    if (!pending.valid) return;
    if (!virtual_bag::valid_pending_transfer_domain(pending)) {
        isolate_pending_locked(virtual_bag::isolation_reason::kInvalidTransactionDomain,
                               "pending recovery");
        VIRTBAG_LOG("pending isolated reason=invalid_transaction_domain direction=%u src=%u/%u dst=%u/%u",
                    static_cast<unsigned int>(pending.direction),
                    static_cast<unsigned int>(pending.src_bag),
                    static_cast<unsigned int>(pending.src_slot),
                    static_cast<unsigned int>(pending.dst_bag),
                    static_cast<unsigned int>(pending.dst_slot));
        g_virtual_bag_state.pending = {};
        persist_state_locked();
        return;
    }
    if (!virtual_bag::valid_pending_transfer_payload(pending)) {
        const virtual_bag::PayloadValidation payload_validation =
            virtual_bag::validate_serialized_payload_buffer(pending.payload.data(),
                                                            pending.payload.size(),
                                                            pending.payload_size);
        const virtual_bag::PayloadValidation source_validation =
            pending.source_payload_size == 0
                ? virtual_bag::PayloadValidation::kOk
                : virtual_bag::validate_serialized_payload_buffer(
                      pending.source_payload.data(), pending.source_payload.size(),
                      pending.source_payload_size);
        isolate_pending_locked(virtual_bag::isolation_reason::kInvalidPayload,
                               payload_validation != virtual_bag::PayloadValidation::kOk
                                   ? virtual_bag::payload_validation_reason(payload_validation)
                                   : virtual_bag::payload_validation_reason(source_validation));
        VIRTBAG_LOG("pending isolated reason=invalid_payload payload=%s source_payload=%s src=%u/%u dst=%u/%u",
                    virtual_bag::payload_validation_reason(payload_validation),
                    virtual_bag::payload_validation_reason(source_validation),
                    static_cast<unsigned int>(pending.src_bag),
                    static_cast<unsigned int>(pending.src_slot),
                    static_cast<unsigned int>(pending.dst_bag),
                    static_cast<unsigned int>(pending.dst_slot));
        g_virtual_bag_state.pending = {};
        persist_state_locked();
        return;
    }
    const bool restore_module = g_module_view_installed;
    const int restore_bag = g_module_view_index;
    if (restore_module) restore_module_view_locked();
    const virtual_bag::RecoveryAction action =
        virtual_bag::recovery_action(g_virtual_bag_state, pending);
    VIRTBAG_LOG("pending recovery direction=%d action=%d", static_cast<int>(pending.direction),
                static_cast<int>(action));
    bool clear_pending = action == virtual_bag::RecoveryAction::kRollback;
    if (action == virtual_bag::RecoveryAction::kComplete) {
        if (pending.direction == virtual_bag::kTransferOriginalToExtension) {
            void* src = nullptr;
            if (!inventory_slot_locked(pending.src_bag, pending.src_slot, &src) || src == nullptr) {
                clear_pending = true;
            } else if (pending.source_payload_size > 0 &&
                       fn_remove_item_direct != nullptr) {
                std::array<uint8_t, virtual_bag::kSerializedItemBuffer> source_payload{};
                int source_size = 0;
                if (!serialize_item_payload_locked(src, &source_payload, &source_size)) {
                    VIRTBAG_LOG("pending recovery: source serialization rejected");
                    clear_pending = false;
                } else {
                    const bool same_source = source_size == pending.source_payload_size &&
                                             std::memcmp(source_payload.data(),
                                                         pending.source_payload.data(),
                                                         pending.source_payload_size) == 0;
                    if (same_source) {
                        const bool source_removed = remove_item_direct_unlocked(
                            lock, pending.src_bag, pending.src_slot, src, "pending recovery");
                        if (fn_ui_equip_refresh_item_area != nullptr) {
                            virtual_bag_call_original_refresh_item_area_with_guard();
                        }
                        if (source_removed) {
                            clear_pending = true;
                        } else {
                            VIRTBAG_LOG("pending recovery: source removal not confirmed; retain pending");
                            clear_pending = false;
                        }
                    } else {
                        VIRTBAG_LOG("pending recovery: source identity mismatch; retain pending");
                    }
                }
            }
        } else if (pending.direction == virtual_bag::kTransferExtensionToExtension) {
            // ext→ext 恒纯逻辑（§4.1）：此分支 = dst 槽 payload 不匹配，仅损坏/手工
            // 构造的 sidecar 可达。pending.payload 是 dst 提交态而非可入原版库存的
            // 源载荷，禁止按 ext→orig 重放（§4.2 不尝试重放或推断目标）；
            // 按 kRollback 语义仅清 pending。
            clear_pending = true;
        } else {
            if (original_inventory_contains_payload_locked(pending.payload.data(),
                                                           pending.payload_size,
                                                           pending.dst_bag)) {
                clear_pending = true;
            } else {
                set_original_bag_locked(pending.dst_bag);
                uint32_t item_handle = 0;
                void* item = load_item_payload_tracked_locked(
                    pending.payload.data(), pending.payload_size,
                    "recover_pending_extension_to_original", &item_handle);
                bool loaded = item != nullptr;
                if (loaded && fn_inven_save_item_on_empty != nullptr) {
                    if (!fn_inven_save_item_on_empty(item, pending.dst_bag)) {
                        release_tracked_item_locked(item_handle, item,
                                                    "recover ext2orig insert failed");
                        loaded = false;
                    }
                } else if (item != nullptr) {
                    release_tracked_item_locked(item_handle, item,
                                                "recover ext2orig save unavailable");
                    loaded = false;
                }
                if (loaded && !original_inventory_contains_payload_locked(
                                   pending.payload.data(), pending.payload_size, pending.dst_bag)) {
                    VIRTBAG_LOG("pending recovery: target payload not confirmed; retain pending");
                    loaded = false;
                }
                if (loaded) {
                    handover_tracked_item_locked(item_handle, item,
                                                 "recover ext2orig handover");
                    clear_pending = true;
                } else {
                    VIRTBAG_LOG("pending recovery: extension->original retry retained");
                }
            }
        }
    }
    if (clear_pending) {
        g_virtual_bag_state.pending = {};
    }
    persist_state_locked();
    (void)restore_module;
    (void)restore_bag;
}


#include "feature/extension_bag/extension_bag_ownership.inc"

void* materialize_module_item_locked(const virtual_bag::Item& descriptor) {
    return load_item_payload_locked(descriptor.payload.data(), descriptor.payload_size,
                                    "materialize_module_item");
}

#include "feature/extension_bag/extension_bag_input.inc"

stack_codec::CountEncoding category_is_equip(int category) {
    if (g_base == 0 || category <= 0) return stack_codec::CountEncoding::kUnknown;
    uint8_t* class_data = *reinterpret_cast<uint8_t**>(
        *reinterpret_cast<void**>(g_base + G_ITEMCLASS_DATA_GOT_VMA));
    uint8_t* size_ptr = *reinterpret_cast<uint8_t**>(g_base + G_ITEMCLASS_SIZE_GOT_VMA);
    if (class_data == nullptr || size_ptr == nullptr) {
        return stack_codec::CountEncoding::kUnknown;
    }
    const uint8_t stride = *size_ptr;
    if (stride == 0) return stack_codec::CountEncoding::kUnknown;
    return (class_data[category * stride + 6] & 1) == 0
        ? stack_codec::CountEncoding::kNotEncoded
        : stack_codec::CountEncoding::kEncoded;
}

// 原版 ButtonEquipExe 的背包身份判定（反汇编 b7c74）：ITEMCLASSBASE 记录
// +2 == 0x1f 即背包类。category 只是 ITEMDATABASE 索引，不能承担装备语义；
// +2 是记录字段偏移（非 VMA），表基址/步长仍来自 game_symbols.h 的
// G_ITEMCLASS_DATA/SIZE GOT 槽。
bool category_is_extension_backpack(int category) {
    if (g_base == 0 || category <= 0) return false;
    uint8_t* class_data = *reinterpret_cast<uint8_t**>(
        *reinterpret_cast<void**>(g_base + G_ITEMCLASS_DATA_GOT_VMA));
    uint8_t* size_ptr = *reinterpret_cast<uint8_t**>(g_base + G_ITEMCLASS_SIZE_GOT_VMA);
    if (class_data == nullptr || size_ptr == nullptr) return false;
    const uint8_t stride = *size_ptr;
    if (stride == 0) return false;
    return class_data[category * stride + 2] == 0x1f;
}

#include "feature/extension_bag/extension_bag_transaction.inc"
bool persist_state_locked(bool force) {
    (void)force;
    // Sidecar writes are owned exclusively by core module_save_game's
    // prepare/original-save/commit coordinator. SAVE_SaveInventory can run
    // inside the original save before its final result is known, so it must
    // never persist the extension state directly.
    return true;
}

bool extension_tab_hit(int index, int64_t x, int64_t y) {
    if (!virtual_bag::valid_index(index)) return false;
    if (g_virtual_bag_state.capacities[index] == 0) {
        // 未装备袋只有在当前拖动物品满足装备规则时可命中；否则仍保持
        // G-12 的不可命中门禁，避免普通物品误落到未装备标签。
        const int category = drag_source_category_locked();
        if (!is_backpack_category(category)) return false;
    }
    // 标签位置是动态换算的（tab_rel + 袋容器绝对），用控件真实绝对 rect 判断，
    // 不用旧硬编码 kCellX/kCellY（标签左移后二者偏移 36px 致命中失效）。
    void* ctrl = g_extension_tab_buttons[index];
    if (ctrl == nullptr) return false;
    int64_t ax = 0, ay = 0;
    extension_bag_control_abs_pos(ctrl, &ax, &ay);
    uint8_t* c = reinterpret_cast<uint8_t*>(ctrl);
    const int64_t w = *reinterpret_cast<int64_t*>(c + CO_RECT_W);
    const int64_t h = *reinterpret_cast<int64_t*>(c + CO_RECT_H);
    return x >= ax && x < ax + w && y >= ay && y < ay + h;
}

// GetChild 结果必须过双重校验：面板重建窗口里 GetChild(slot≥count) 返回不可读的
// 垃圾指针（tombstone_02），先 GetCount 判界消除越界源，再 GetUserType==2 验类型。
// root 本身也可能被面板重建替换——调用方应实时读 *(G_UIEQUIP_PANEL_CTRL_VMA)，
// 不要使用 install 时的快照。
void* valid_child_locked(void* root, int slot) {
    return extension_bag_valid_child(root, slot);
}

// ---- G-8 投影拖动状态机（模块侧，独立于 TouchHandle 内部拖动）----
// press 只读记录命中扩展槽（不触碰 TouchHandle）；move 更新触点 + 吞 panel 层事件
// （抑制 TouchHandle 拖动与场景溢出）；drop 在控件层路由到既有跨包事务：
//   落到物品控件（0x02）→ ext→ext；落到袋控件（0x04）→ ext→orig（经 SaveItemOnEmpty 门禁）。
// G-6：投影命中走控件 AbsoluteRect。GetAbsoluteRect 是 x8 sret 函数禁止 C++ 直调
// （真机 SIGSEGV），用手工父链累加（ctrl_abs_point 同款，纯内存读）。
// w/h 用贴图固定尺寸 kGridCell（GetAbsoluteRect 也只输出 x/y 两个 i64）。
// 物品指针合法性：模块缓存、INVEN_pItem、实时角色装备槽三集合任一命中即合法。
// 装备槽必须实时读取，不能缓存对象指针；卸下或重建后的旧指针仍由 G-6 门禁阻断。
// （借出对象不再真释放，无需隔离区）。
bool item_pointer_known_locked(void* item) {
    if (item == nullptr) return true;  // 空指针由调用方各自处理
    for (int bag = 0; bag < virtual_bag::kBagCount; ++bag) {
        for (int slot = 0; slot < virtual_bag::kSlotCount; ++slot) {
            if (g_module_objects[bag][slot] == item) return true;
        }
    }
    if (g_inven != nullptr) {
        void** inventory = static_cast<void**>(g_inven);
        for (int idx = 0; idx < 6 * virtual_bag::kSlotCount; ++idx) {
            if (inventory[idx] == item) return true;
        }
    }
    for (int role = 0; role < 3; ++role) {
        uint8_t* character = static_cast<uint8_t*>(member_or_null(role));
        if (character == nullptr) continue;
        for (int slot = 0; slot < C_EQUIP_SLOTS; ++slot) {
            void* equipped = *reinterpret_cast<void**>(character + C_EQUIP + slot * sizeof(void*));
            if (equipped == item) return true;
        }
    }
    return false;
}

// 扩展标签绘制。挂在 DrawInvenBag wrapper（原版袋标签之后、DrawMovingItem 之前）
// 以避免遮挡拖动中的物品。绘制顺序对齐原版 UIEquip_Draw：
//   DrawInvenBackground 底框倒序(5→0) → DrawInvenBag 图标+高亮正序(0→5)。
#include "feature/extension_bag/extension_bag_render.inc"
#include "feature/extension_bag/extension_bag_lifecycle.inc"
