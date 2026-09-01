#include "game_ui_virtbag.h"

#include "game_access.h"
#include "game_inventory.h"
#include "game_ops_common.h"
#include "game_patch.h"
#include "game_ptr_hook.h"
#include "game_state.h"
#include "game_symbols.h"
#include "ownership_ledger.h"
#include "stack_codec.h"
#include "virtual_bag_state.h"
#include "game_ui_kit.h"

#include <android/log.h>

#include <atomic>
#include <cstdarg>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <mutex>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>

#define VIRTBAG_TAG "Inotia4VirtBag"

namespace {

std::mutex g_virtbag_log_mtx;

void virtbag_log(const char* format, ...) {
    char message[2048] = {};
    va_list args;
    va_start(args, format);
    std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    __android_log_print(ANDROID_LOG_INFO, VIRTBAG_TAG, "%s", message);

    const auto now = std::chrono::system_clock::now();
    const auto since_epoch = now.time_since_epoch();
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(since_epoch).count();
    const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
    std::tm local_time{};
    localtime_r(&seconds, &local_time);
    const int millisecond_part = static_cast<int>(milliseconds % 1000);
    std::lock_guard<std::mutex> lock(g_virtbag_log_mtx);
    const int fd = open("/sdcard/Android/data/com.com2us.inotia4.normal.freefull.google.global.android.common/files/inotia4-export.log",
                        O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0) return;
    dprintf(fd, "%04d-%02d-%02d %02d:%02d:%02d.%03d [N] %s\n",
            local_time.tm_year + 1900, local_time.tm_mon + 1, local_time.tm_mday,
            local_time.tm_hour, local_time.tm_min, local_time.tm_sec,
            millisecond_part, message);
    close(fd);
}

#define VIRTBAG_LOG(...) virtbag_log(__VA_ARGS__)

constexpr size_t kPopupStateSize = 0x40;
constexpr int kPopupStateCount = 27;
// 触摸事件使用 Scene_Draw 的绝对逻辑坐标；真机运行时宽度为 1408。
constexpr int64_t kCellX = 0x4c4;
constexpr int64_t kCellY = 0x86;
constexpr int64_t kCellStepY = 0x46;
// Phase one uses an independent read-only overlay. These coordinates stay in
// the same absolute logical space as the original equipment scene.
// UIEquip_CreateInvenControl absolute origin: bag button origin minus the
// bag container offset plus the item-container offset.
constexpr int64_t kGridX = 0x45c - 0x185 + 0x22; // 761
constexpr int64_t kGridY = 0x91 - 0x0c + 0x1b;   // 160
constexpr int64_t kGridCell = 0x4a;
constexpr int64_t kGridStep = 0x53;
constexpr int64_t kOriginalGridX = kGridX;
constexpr int64_t kOriginalGridY = kGridY;
constexpr size_t kInventorySlotStride = 16;
constexpr uint8_t kNoOriginalBagSelected = 6;
// Extension tab ControlObject rects are relative to the item-list root, whose
// absolute origin is (kGridX, kGridY). Their visuals keep the existing overlay
// positions so this phase changes input ownership without moving the UI.
constexpr int64_t kExtensionTabX = kCellX - kGridX;
constexpr int64_t kExtensionTabY = kCellY - kGridY;
constexpr int64_t kExtensionTabWidth = 0x70;
constexpr int64_t kExtensionTabHeight = 0x30;

using PopupEventFn = uint64_t (*)(uint64_t, uint64_t, uint64_t);
using PopupNoArgFn = void (*)();
using OriginalDrawInvenBagFn = void (*)();
using OriginalDrawInvenItemFn = void (*)();

std::mutex g_virtual_bag_mtx;
std::atomic<bool> g_inject_thread_started{false};
std::atomic<bool> g_lifecycle_thread_started{false};
std::atomic<bool> g_virtual_bag_enabled{false};
std::atomic<uint64_t> g_exit_trace_sequence{0};
jclass g_virtual_bag_bridge_class = nullptr;
uint8_t* g_state_entry = nullptr;
PopupEventFn g_orig_event = nullptr;
PopupNoArgFn g_orig_f3 = nullptr;
PopupNoArgFn g_orig_enter = nullptr;
PopupNoArgFn g_orig_save_enter = nullptr;
uint8_t* g_save_state_entry = nullptr;
uintptr_t g_draw_patch_addr = 0;
uintptr_t g_bag_draw_patch_addr = 0;
uintptr_t g_save_inventory_patch_addr = 0;
void* g_save_inventory_thunk = nullptr;
uintptr_t g_drop_gate_patch_addr = 0;
void* g_drop_gate_thunk = nullptr;
uintptr_t g_draw_gate_patch_addr = 0;
void* g_draw_gate_thunk = nullptr;
uintptr_t g_item_draw_patch_addr = 0;
uintptr_t g_desc_open_patch_addr = 0;
void* g_desc_open_thunk = nullptr;
void* g_draw_thunk = nullptr;
void* g_bag_draw_thunk = nullptr;
void* g_item_draw_thunk = nullptr;
virtual_bag::State g_virtual_bag_state{};
int g_loaded_slot = -2;

uint64_t isolation_now_ms_locked();
std::array<std::array<void*, virtual_bag::kSlotCount>, virtual_bag::kBagCount> g_module_objects{};
std::array<std::array<int, virtual_bag::kSlotCount>, virtual_bag::kBagCount> g_module_object_categories{};
std::array<std::array<uint32_t, virtual_bag::kSlotCount>, virtual_bag::kBagCount> g_module_object_hashes{};
std::array<void*, virtual_bag::kSlotCount> g_original_inventory{};
uint8_t g_original_current_direct = 0;
uint8_t g_original_current_got = 0;
uint32_t* g_original_bag_size_word = nullptr;
uint32_t g_original_bag_size = 0;
void* g_projected_item_root = nullptr;
bool g_module_view_installed = false;
int g_module_view_index = -1;
uint8_t g_exit_display_bag = kNoOriginalBagSelected;
bool g_inventory_frame_active = false;
bool g_item_state_dirty = false;
bool g_explicit_save_in_progress = false;
bool g_extension_touch_capture = false;
std::array<void*, virtual_bag::kBagCount> g_extension_tab_buttons{};
void* g_extension_tab_root = nullptr;
uint64_t g_inventory_generation = 0;
uint64_t g_extension_tab_generation = 0;
int g_pending_extension_tab = -1;
uint64_t g_pending_extension_tab_generation = 0;
PtrHook g_extension_desc_unequip_hook;
int g_extension_desc_unequip_bag = -1;
PtrHook g_extension_desc_equip_hook;
void* g_extension_desc_equip_item = nullptr;

struct ExtensionDrag {
    bool active = false;
    uint8_t bag = 0;
    uint8_t slot = 0;
    int64_t press_x = 0;
    int64_t press_y = 0;
    int64_t current_x = 0;
    int64_t current_y = 0;
};

ExtensionDrag g_extension_drag{};

std::atomic<uint64_t> g_p5_observation_sequence{0};
std::atomic<uint64_t> g_p5_last_item_query_sample_ms{0};
std::atomic<uint64_t> g_p5_last_panel_move_sample_ms{0};
thread_local bool g_p5_observation_active = false;
void* g_tab_bag_items[virtual_bag::kBagCount] = {};

int extension_tab_index(void* ctrl);
bool module_slot_of_item_locked(void* item, int* out_bag, int* out_slot);

constexpr uint64_t kP5SampleIntervalMs = 100;

bool p5_observation_enabled() {
    return __android_log_is_loggable(ANDROID_LOG_DEBUG, VIRTBAG_TAG, ANDROID_LOG_INFO) != 0;
}

bool p5_panel_observation_enabled() {
    return !g_p5_observation_active && p5_observation_enabled();
}

uint64_t p5_steady_now_ms() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

bool p5_take_sample(std::atomic<uint64_t>* last_sample_ms) {
    const uint64_t now = p5_steady_now_ms();
    uint64_t previous = last_sample_ms->load(std::memory_order_relaxed);
    for (;;) {
        if (now >= previous && now - previous < kP5SampleIntervalMs) return false;
        if (last_sample_ms->compare_exchange_weak(previous, now, std::memory_order_relaxed,
                                                  std::memory_order_relaxed)) {
            return true;
        }
    }
}

bool p5_should_begin_item_observation(uint64_t event) {
    return event != 0x10 || p5_take_sample(&g_p5_last_item_query_sample_ms);
}

bool p5_should_emit_observation(const char* phase, uint64_t event) {
    if (!p5_observation_enabled()) return false;
    if (g_p5_observation_active && std::strncmp(phase, "panel-", 6) == 0) return false;
    if (event != 0x19 || std::strncmp(phase, "panel-", 6) != 0) return true;
    if (std::strcmp(phase, "panel-entry") == 0) return false;
    return p5_take_sample(&g_p5_last_panel_move_sample_ms);
}

class P5ObservationScope {
public:
    P5ObservationScope() : was_active_(g_p5_observation_active) {
        g_p5_observation_active = true;
    }

    ~P5ObservationScope() {
        g_p5_observation_active = was_active_;
    }

private:
    bool was_active_;
};

enum class P5DataDomain : int {
    kUnavailable = -1,
    kNoData = 0,
    kEmpty = 1,
    kProjectedModuleItem = 2,
    kModuleTabItem = 3,
    kOriginalItem = 4,
    kUnknownItem = 5,
};

struct P5ControlObservation {
    int user_type = -1;
    int original_slot = -1;
    int projection_bag = -1;
    int projection_slot = -1;
    int tab_index = -1;
    P5DataDomain data_domain = P5DataDomain::kUnavailable;
    int parent_depth = -1;
    bool parent_reaches_projection_root = false;
};

struct P5TouchObservation {
    bool available = false;
    bool moving_present = false;
    bool drop_source_present = false;
    bool moving_matches_target = false;
    bool drop_source_matches_target = false;
    bool moving_matches_source = false;
    bool drop_source_matches_source = false;
    int64_t release_x = -1;
    int64_t release_y = -1;
};

P5ControlObservation p5_control_observation_locked(void* control) {
    P5ControlObservation observation;
    if (control == nullptr) return observation;

    if (fn_control_object_get_user_type != nullptr) {
        observation.user_type = static_cast<int>(fn_control_object_get_user_type(control));
    }
    if (fn_ui_equip_get_item_slot_index != nullptr) {
        observation.original_slot = fn_ui_equip_get_item_slot_index(control);
    }
    observation.tab_index = extension_tab_index(control);

    if (fn_control_object_get_data != nullptr) {
        void* data = fn_control_object_get_data(control);
        if (data == nullptr) {
            observation.data_domain = P5DataDomain::kNoData;
        } else {
            void* item = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(data) + ITEM_CTRL_ITEM);
            if (item == nullptr) {
                observation.data_domain = P5DataDomain::kEmpty;
            } else {
                int module_bag = -1;
                int module_slot = -1;
                if (module_slot_of_item_locked(item, &module_bag, &module_slot)) {
                    observation.data_domain = P5DataDomain::kProjectedModuleItem;
                } else if (observation.tab_index >= 0 &&
                           g_tab_bag_items[observation.tab_index] == item) {
                    observation.data_domain = P5DataDomain::kModuleTabItem;
                } else if (observation.original_slot >= 0 &&
                           observation.original_slot < virtual_bag::kSlotCount) {
                    observation.data_domain = P5DataDomain::kOriginalItem;
                } else {
                    observation.data_domain = P5DataDomain::kUnknownItem;
                }
            }
        }
    }

    constexpr int kP5ParentDepthLimit = 16;
    void* current = control;
    observation.parent_depth = 0;
    while (current != nullptr && observation.parent_depth < kP5ParentDepthLimit) {
        if (current == g_projected_item_root) {
            observation.parent_reaches_projection_root = true;
            break;
        }
        void* parent = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(current) + CO_PARENT);
        if (parent == current) break;
        current = parent;
        ++observation.parent_depth;
    }

    if (!g_module_view_installed || !virtual_bag::valid_index(g_module_view_index) ||
        g_projected_item_root == nullptr || fn_ctrl_get_count == nullptr ||
        fn_control_object_get_child == nullptr) {
        return observation;
    }
    const int child_count = static_cast<int>(fn_ctrl_get_count(g_projected_item_root));
    const int capacity = g_virtual_bag_state.capacities[g_module_view_index];
    const int limit = capacity < child_count ? capacity : child_count;
    for (int slot = 0; slot < limit; ++slot) {
        if (fn_control_object_get_child(g_projected_item_root, slot) == control) {
            observation.projection_bag = g_module_view_index;
            observation.projection_slot = slot;
            break;
        }
    }
    return observation;
}

P5TouchObservation p5_touch_observation_locked(void* target_control, void* source_control) {
    P5TouchObservation observation;
    if (g_base == 0) return observation;

    const uint8_t* state = reinterpret_cast<const uint8_t*>(g_base + G_TOUCH_STATE_VMA);
    const void* moving = *reinterpret_cast<const void* const*>(state + TOUCH_STATE_MOVING_CTRL);
    const void* drop_source =
        *reinterpret_cast<const void* const*>(state + TOUCH_STATE_DROP_SRC_CTRL);
    observation.available = true;
    observation.moving_present = moving != nullptr;
    observation.drop_source_present = drop_source != nullptr;
    observation.moving_matches_target =
        target_control != nullptr && moving != nullptr && moving == target_control;
    observation.drop_source_matches_target =
        target_control != nullptr && drop_source != nullptr && drop_source == target_control;
    observation.moving_matches_source = source_control != nullptr && moving == source_control;
    observation.drop_source_matches_source =
        source_control != nullptr && drop_source == source_control;
    observation.release_x = *reinterpret_cast<const int64_t*>(state + TOUCH_STATE_RELEASE_X);
    observation.release_y = *reinterpret_cast<const int64_t*>(state + TOUCH_STATE_RELEASE_Y);
    return observation;
}

uint64_t log_p5_observation_locked(const char* phase, void* target_control, uint64_t event,
                                   bool x2_present, bool param_present, void* source_control,
                                   bool result_known, uint64_t result, int64_t event_x = -1,
                                   int64_t event_y = -1, uint64_t related_sequence = 0) {
    if (!p5_should_emit_observation(phase, event)) return 0;
    P5ObservationScope observation_scope;
    const P5ControlObservation target = p5_control_observation_locked(target_control);
    const P5ControlObservation source = p5_control_observation_locked(source_control);
    const P5TouchObservation touch =
        p5_touch_observation_locked(target_control, source_control);
    const uint64_t sequence = g_p5_observation_sequence.fetch_add(1) + 1;
    VIRTBAG_LOG(
        "p5obs seq=%llu related_seq=%llu phase=%s event=0x%llx x2=%d param=%d result_known=%d result=0x%llx event_x=%lld event_y=%lld target_type=%d target_slot=%d target_projection=%d/%d target_tab=%d target_data=%d target_parent_depth=%d target_parent_projection_root=%d source_known=%d source_type=%d source_slot=%d source_projection=%d/%d source_tab=%d source_data=%d source_parent_depth=%d source_parent_projection_root=%d touch=%d moving=%d drop_source=%d moving_target=%d drop_target=%d moving_source=%d drop_source_match=%d release_x=%lld release_y=%lld mode=%d selected=%d overlay=%d overlay_index=%d inventory_generation=%llu tab_generation=%llu capture=%d legacy_drag=%d",
        static_cast<unsigned long long>(sequence),
        static_cast<unsigned long long>(related_sequence), phase,
        static_cast<unsigned long long>(event), x2_present ? 1 : 0, param_present ? 1 : 0,
        result_known ? 1 : 0, static_cast<unsigned long long>(result),
         static_cast<long long>(event_x), static_cast<long long>(event_y), target.user_type,
         target.original_slot, target.projection_bag, target.projection_slot, target.tab_index,
         static_cast<int>(target.data_domain), target.parent_depth,
         target.parent_reaches_projection_root ? 1 : 0, source_control != nullptr ? 1 : 0,
         source.user_type, source.original_slot, source.projection_bag, source.projection_slot,
         source.tab_index, static_cast<int>(source.data_domain), source.parent_depth,
         source.parent_reaches_projection_root ? 1 : 0,
        touch.available ? 1 : 0, touch.moving_present ? 1 : 0,
        touch.drop_source_present ? 1 : 0, touch.moving_matches_target ? 1 : 0,
        touch.drop_source_matches_target ? 1 : 0, touch.moving_matches_source ? 1 : 0,
        touch.drop_source_matches_source ? 1 : 0,
        static_cast<long long>(touch.release_x), static_cast<long long>(touch.release_y),
        static_cast<int>(g_virtual_bag_state.mode), g_virtual_bag_state.selected,
        g_module_view_installed ? 1 : 0, g_module_view_index,
        static_cast<unsigned long long>(g_inventory_generation),
        static_cast<unsigned long long>(g_extension_tab_generation),
        g_extension_touch_capture ? 1 : 0, g_extension_drag.active ? 1 : 0);
    return sequence;
}

void extension_tab_button_clicked(void* ctrl);

int extension_tab_index(void* ctrl) {
    for (int index = 0; index < virtual_bag::kBagCount; ++index) {
        if (g_extension_tab_buttons[index] == ctrl) return index;
    }
    return -1;
}

// 扩展标签的"已装备背包物品"对象（用户方案：与 RefreshBagArea 同款语义——
// data[0] 放真实物品）。按袋 types 物化（CreateItem(category)，types[i] 即
// ITEMDATABASE 记录下标 = 背包 category，1/2/3/4 → 手提/小/中/大包），
// 卸下/降级时经延迟释放退役（对象可能仍被 TouchState 引用，不真释放）。
void defer_item_free_locked(void* item);
void refresh_tab_bag_items_locked() {
    for (int i = 0; i < virtual_bag::kBagCount; ++i) {
        const uint8_t type = g_virtual_bag_state.types[i];
        void*& slot = g_tab_bag_items[i];
        if (type == 0) {
            if (slot != nullptr) {
                defer_item_free_locked(slot);  // 不真释放：TouchState 可能仍引用
                slot = nullptr;
            }
            continue;
        }
        // 类型一致性校验后复用：解除→换类型重装备时旧对象残留（unequip 不
        // 触发本刷新），无校验会复用旧类型对象——详情页显示旧背包的真因。
        // 对象同会话内不释放（defer 策略），档位加载边界已清空本数组，此处
        // 读 I_TYPE 无回收悬空风险。
        if (slot != nullptr) {
            if (fn_get_bit == nullptr) continue;
            const uint16_t flags =
                *reinterpret_cast<uint16_t*>(reinterpret_cast<uint8_t*>(slot) + I_TYPE);
            if (fn_get_bit(flags, 15, 6) == static_cast<int>(type)) continue;
            defer_item_free_locked(slot);
            slot = nullptr;
        }
        if (fn_create_item == nullptr) continue;
        slot = fn_create_item(static_cast<int32_t>(type), 0, 0, 0);
        if (slot != nullptr) {
            uint32_t count_flags =
                *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(slot) + I_COUNT);
            *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(slot) + I_COUNT) =
                stack_codec::write_count(count_flags, 1);
        }
    }
}

// 扩展标签控件事件处理：TouchHandle 转发的控件事件（0x02=click 松开确认，
// 0x04=drop 到袋）。挂袋容器后与原版袋标签同链：松开 → TouchHandle 判定。
void queue_extension_tab_click_locked(int extension_bag);
void* touch_moving_item_control_locked();
bool try_equip_on_extension_tab_drop_locked(int index, void* moving_control);
uint64_t extension_tab_item_proc(void* ctrl, uint64_t event, void* x2, void* param) {
    const uint64_t observation_token = virtual_bag_observe_item_proc_pre(ctrl, event, x2, param);
    const auto finish = [observation_token, ctrl, event, x2, param](uint64_t result) {
        virtual_bag_observe_item_proc_post(observation_token, ctrl, event, x2, param, result);
        return result;
    };
    if (event == 0x02) {
        {
            std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
            const int index = extension_tab_index(ctrl);
            if (index >= 0) {
                queue_extension_tab_click_locked(index);
            }
        }
        return finish(1);  // 松开确认已处理（原版袋标签点击切袋同样返回 1，b8c60）
    }
    // 0x04 = 拖动 drop 到袋控件：背包物品（category 1..4）落到扩展标签 =
    // 装备到该扩展位——原版拖放装备（InvenBagControlEventProc b8b30 块，写
    // INVEN_pBagSlot/清源槽）的扩展对应物。其余一律拒绝。无论成败都返回 0
    // 让 TouchHandle 复位 moving 控件（原版袋标签 proc 装备成功后同样返回
    // 0，b8c18；此前返回 1 会导致"格子被移动"悬停缺陷）。
    if (event == 0x04) {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        const int index = extension_tab_index(ctrl);
        void* moving = touch_moving_item_control_locked();
        if (index >= 0 && moving != nullptr) {
            try_equip_on_extension_tab_drop_locked(index, moving);
        }
        return finish(0);
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
void ctrl_abs_pos_locked(void* ctrl, int64_t* out_x, int64_t* out_y) {
    // 先收集父链（自下而上）
    void* chain[8] = {};
    int depth = 0;
    void* cur = ctrl;
    while (cur != nullptr && depth < 8) {
        chain[depth++] = cur;
        cur = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(cur) + CO_PARENT);
    }
    int64_t ax = 0, ay = 0;
    for (int i = depth - 1; i >= 0; --i) {
        uint8_t* c = reinterpret_cast<uint8_t*>(chain[i]);
        ax += *reinterpret_cast<int64_t*>(c + CO_RECT_X);
        ay += *reinterpret_cast<int64_t*>(c + CO_RECT_Y);
    }
    *out_x = ax;
    *out_y = ay;
}

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
    void* bag_container = *reinterpret_cast<void**>(g_base + G_UIEQUIP_PANEL_VMA + 0x50);
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
    ctrl_abs_pos_locked(bag_container, &bag0_abs_x, &bag0_abs_y);
    void* mount_parent = bag_container;
    int64_t mount_abs_x = 0, mount_abs_y = 0;
    ctrl_abs_pos_locked(mount_parent, &mount_abs_x, &mount_abs_y);
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
        uintptr_t enter = *reinterpret_cast<uintptr_t*>(entry + 0x10);
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

bool load_state_from_store(int slot) {
    JNIEnv* env = current_env();
    if (env == nullptr || g_virtual_bag_bridge_class == nullptr) return false;
    jmethodID method = env->GetStaticMethodID(g_virtual_bag_bridge_class, "loadStateJson", "(I)Ljava/lang/String;");
    if (method == nullptr) {
        env->ExceptionClear();
        return false;
    }
    jstring result = static_cast<jstring>(env->CallStaticObjectMethod(g_virtual_bag_bridge_class, method, slot));
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return false;
    }
    if (result == nullptr) return false;
    const char* utf = env->GetStringUTFChars(result, nullptr);
    bool parsed = false;
    if (utf != nullptr) {
        virtual_bag::PendingTransfer rejected_pending{};
        const bool has_pending = std::strstr(utf, "\"pending\":") != nullptr;
        const virtual_bag::PendingTransferParseResult pending_result =
            has_pending ? virtual_bag::parse_pending_transfer_result(utf, &rejected_pending)
                        : virtual_bag::PendingTransferParseResult::kOk;
        if (pending_result == virtual_bag::PendingTransferParseResult::kInvalidTransactionDomain) {
            VIRTBAG_LOG("pending isolated reason=invalid_transaction_domain direction=%u src=%u/%u dst=%u/%u",
                        static_cast<unsigned int>(rejected_pending.direction),
                        static_cast<unsigned int>(rejected_pending.src_bag),
                        static_cast<unsigned int>(rejected_pending.src_slot),
                        static_cast<unsigned int>(rejected_pending.dst_bag),
                        static_cast<unsigned int>(rejected_pending.dst_slot));
        } else if (pending_result == virtual_bag::PendingTransferParseResult::kMalformed) {
            VIRTBAG_LOG("pending isolated reason=malformed_pending_record");
        }
        parsed = virtual_bag::parse_state_json_at(utf, &g_virtual_bag_state,
                                                  isolation_now_ms_locked());
        if (has_pending &&
            pending_result != virtual_bag::PendingTransferParseResult::kOk) {
            virtual_bag::IsolationRecord record{};
            record.valid = true;
            record.observed_at_ms = isolation_now_ms_locked();
            if (pending_result ==
                virtual_bag::PendingTransferParseResult::kInvalidTransactionDomain) {
                record.reason = virtual_bag::isolation_reason::kInvalidTransactionDomain;
                record.direction = rejected_pending.direction;
                record.src_bag = rejected_pending.src_bag;
                record.src_slot = rejected_pending.src_slot;
                record.dst_bag = rejected_pending.dst_bag;
                record.dst_slot = rejected_pending.dst_slot;
                std::strncpy(record.transaction_id, rejected_pending.transaction_id,
                             sizeof(record.transaction_id) - 1);
                record.payload_size = std::min<uint16_t>(
                    rejected_pending.payload_size,
                    static_cast<uint16_t>(rejected_pending.payload.size()));
                record.payload = rejected_pending.payload;
                record.source_payload_size = std::min<uint16_t>(
                    rejected_pending.source_payload_size,
                    static_cast<uint16_t>(rejected_pending.source_payload.size()));
                record.source_payload = rejected_pending.source_payload;
                record.detail = "pending_rejected_at_load";
            } else {
                record.reason = virtual_bag::isolation_reason::kInvalidPayload;
                record.detail = "malformed_pending_record";
            }
            virtual_bag::push_isolation(&g_virtual_bag_state, record);
        }
    }
    if (utf != nullptr) env->ReleaseStringUTFChars(result, utf);
    env->DeleteLocalRef(result);
    return parsed;
}

bool save_state_to_store(int slot) {
    JNIEnv* env = current_env();
    if (env == nullptr || g_virtual_bag_bridge_class == nullptr) return false;
    jmethodID method = env->GetStaticMethodID(g_virtual_bag_bridge_class, "saveStateJson",
                                               "(ILjava/lang/String;)Ljava/lang/String;");
    if (method == nullptr) {
        env->ExceptionClear();
        return false;
    }
    const std::string json = virtual_bag::state_json(g_virtual_bag_state);
    jstring payload = env->NewStringUTF(json.c_str());
    if (payload == nullptr) return false;
    jstring result = static_cast<jstring>(env->CallStaticObjectMethod(g_virtual_bag_bridge_class, method, slot, payload));
    env->DeleteLocalRef(payload);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return false;
    }
    if (result == nullptr) return false;
    const char* utf = env->GetStringUTFChars(result, nullptr);
    const bool ok = utf != nullptr && strcmp(utf, "ok") == 0;
    if (utf != nullptr) env->ReleaseStringUTFChars(result, utf);
    env->DeleteLocalRef(result);
    return ok;
}

void ensure_state_loaded_locked();
bool persist_state_locked(bool force = false);
void clear_original_desc_locked();
void extension_desc_unequip_execute(void*);
void clear_original_item_selection_locked();
bool extension_tab_hit(int index, int64_t x, int64_t y);
int grid_slot_index(int64_t x, int64_t y, int64_t origin_x, int64_t origin_y);
int original_bag_locked();
void set_original_bag_locked(int bag);
int original_bag_button_index(int64_t x, int64_t y);
bool install_module_view_locked(int bag);
void restore_module_view_locked();
void refresh_projection_if_overwritten_locked();
void* module_item_locked(int bag, int slot);
void recover_pending_transaction_locked();
void prepare_main_menu_locked();
void free_module_object_locked(int bag, int slot);
bool move_original_to_extension_locked(int dst_bag, void* moving_control);
bool move_original_to_extension_slot_locked(int src_bag, int src_slot, int dst_bag);
bool move_extension_to_original_locked(int src_bag, int src_slot, int target_bag);
bool move_extension_to_extension_locked(int src_bag, int src_slot, int dst_bag,
                                        int requested_dst_slot = -1);
bool handle_bag_drop_release_locked(int64_t x, int64_t y);
void defer_item_free_locked(void* item);
void* valid_child_locked(void* root, int slot);
void make_desc_equip_gate(void* ctrl, void* arg);

void reset_extension_desc_unequip_hook_locked() {
    // Desc menu buttons are game-owned and can already be gone on teardown.
    // Do not write through PtrHook::slot while discarding our bookkeeping.
    g_extension_desc_unequip_hook = {};
    g_extension_desc_unequip_bag = -1;
}

void install_extension_desc_unequip_hook_locked(int extension_bag) {
    reset_extension_desc_unequip_hook_locked();
    if (!virtual_bag::valid_index(extension_bag) || g_base == 0) return;

    // UIEquip_SetDescMenu's desc_type=1 branch stores its newly created
    // ControlButton in UIEquip panel +0x68. Its callback lives in data+0x20.
    void* button = *reinterpret_cast<void**>(g_base + G_UIEQUIP_PANEL_VMA + 0x68);
    if (button == nullptr) {
        VIRTBAG_LOG("extension desc unequip hook skipped: button missing bag=%d", extension_bag);
        return;
    }
    void* data = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(button) + CO_DATA);
    if (data == nullptr) {
        VIRTBAG_LOG("extension desc unequip hook skipped: button data missing bag=%d", extension_bag);
        return;
    }
    void* execute_proc = reinterpret_cast<uint8_t*>(data) + CB_EXECUTE_PROC;
    if (!g_extension_desc_unequip_hook.install_typed(execute_proc,
                                                      &extension_desc_unequip_execute)) {
        VIRTBAG_LOG("extension desc unequip hook install failed bag=%d", extension_bag);
        return;
    }
    g_extension_desc_unequip_bag = extension_bag;
    VIRTBAG_LOG("extension desc unequip hook installed bag=%d button=%p", extension_bag,
                button);
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

void clear_module_cache_locked() {
    for (int bag = 0; bag < virtual_bag::kBagCount; ++bag) {
        for (int slot = 0; slot < virtual_bag::kSlotCount; ++slot) {
            // Scheme C objects never enter g_inven, so the module owns them
            // until an explicit transfer is implemented. Drop them on slot
            // exit instead of retaining stale pointers across saves.
            free_module_object_locked(bag, slot);
        }
    }
    g_original_inventory.fill(nullptr);
    // 标签袋物品对象是 ItemPool 里的裸指针，跨存档槽位加载必须失效：游戏
    // SAVE_LoadItem 重置/回收池内存，旧指针会被新读入的存档物品复用（真机
    // 实证：返回主菜单不保存重进存档后，扩展标签/信息页显示成药水、金币）。
    // 此处只清引用不释放——对象可能已被池回收，再 ItemPool_Free 是 UAF；
    // 下次 install_extension_tab_buttons 按 types 重建新对象。
    for (void*& tab_item : g_tab_bag_items) tab_item = nullptr;
    g_original_bag_size_word = nullptr;
    g_original_bag_size = 0;
    g_original_current_direct = 0;
    g_original_current_got = 0;
    g_module_view_installed = false;
    g_module_view_index = -1;
    g_item_state_dirty = false;
    g_extension_touch_capture = false;
    g_extension_drag = {};
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
    if (g_module_view_installed) restore_module_view_locked();
    clear_module_cache_locked();
    g_virtual_bag_state = {};
    if (!load_state_from_store(slot)) {
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

void recover_pending_transaction_locked() {
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
                        fn_remove_item_direct(pending.src_bag, pending.src_slot);
                        if (fn_ui_equip_refresh_item_area != nullptr) {
                            fn_ui_equip_refresh_item_area();
                        }
                        clear_pending = true;
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
                void* item = load_item_payload_locked(
                    pending.payload.data(), pending.payload_size, "recover_pending_extension_to_original");
                bool loaded = item != nullptr;
                if (loaded && fn_inven_save_item_on_empty != nullptr) {
                    if (!fn_inven_save_item_on_empty(item, pending.dst_bag)) {
                        release_temporary_item_locked(item);
                        loaded = false;
                    }
                } else if (item != nullptr) {
                    release_temporary_item_locked(item);
                    loaded = false;
                }
                if (loaded) {
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

// P3 所有权账本（ownership_ledger.h）：借出对象四态跟踪。
// materialize → kModuleOwned；install 投影 → kBorrowedForView；restore 收回 → kModuleOwned；
// free → kReleased；原版接管（P4 事务桥接）→ kInventoryOwned。
ownership::Ledger g_ownership_ledger{};
uint32_t g_module_object_handles[virtual_bag::kBagCount][virtual_bag::kSlotCount]{};

// 延迟释放队列：借出对象可能仍被控件 data[0] 引用（Scene_Draw 逐帧解引用），
// 释放后立即 ItemPool_Free 会造成悬空。对象先进隔离区，隔离 N 帧（覆盖一次
// 完整绘制循环）后确认框架不再持有再真释放。
// 借出对象释放策略（根治悬空）：对象可能仍被 TouchState/控件 data 残留引用，
// ItemPool_Free 后这些引用全部悬空（多轮修复未穷尽持有者）。改为**不真释放**：
// 仅清除模块引用，对象内存保留至进程结束（单个 ~48B，开发期可接受；
// P4 对象桥接后借出对象走真实所有权流动，此机制整体退役）。
void defer_item_free_locked(void* item) {
    (void)item;  // 有意不释放：防 TouchState/控件残留引用悬空
}

// P4.3：所有权审计日志。只输出计数与句柄状态，禁止 native 指针进日志/持久层。
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

// 触摸窗口（按下/拖动未结算）内控件与 TouchState 可能仍持有物品指针；
// 窗口外引用已撤销（投影控件已清、INVEN 未持有），允许真实释放。
bool object_touch_window_active_locked() {
    if (g_extension_touch_capture || g_extension_drag.active) return true;
    if (g_base == 0 || !g_module_view_installed || g_projected_item_root == nullptr ||
        !virtual_bag::valid_index(g_module_view_index)) {
        return false;
    }
    const uint8_t* touch_state = reinterpret_cast<const uint8_t*>(g_base + G_TOUCH_STATE_VMA);
    const void* moving = *reinterpret_cast<const void* const*>(touch_state + TOUCH_STATE_MOVING_CTRL);
    if (moving == nullptr) return false;
    const int capacity = g_virtual_bag_state.capacities[g_module_view_index];
    for (int slot = 0; slot < capacity; ++slot) {
        if (valid_child_locked(g_projected_item_root, slot) == moving) return true;
    }
    return false;
}

// P4.3 退役第 1 步：模块保管终止（对象未移交原版库存）。
// release 成功且非触摸窗口 → 真实 ITEMPOOL_Free；否则隔离区兜底（防悬空）。
void retire_custody_item_locked(uint32_t handle, void* item, const char* context) {
    const bool released =
        ownership::release(&g_ownership_ledger, handle) == ownership::Outcome::kOk;
    if (released && !object_touch_window_active_locked() && fn_itempool_free != nullptr) {
        fn_itempool_free(item);
    } else {
        defer_item_free_locked(item);
        VIRTBAG_LOG("ownership defer free context=%s released=%d touch_window=%d", context,
                    released ? 1 : 0, object_touch_window_active_locked() ? 1 : 0);
    }
    log_ownership_audit_locked(context);
}

// P4.3：临时对象生命周期入口 = Load 成功即 allocate 入账本。
// Load 成功但 ledger 分配失败 → 立即真释放并返回 nullptr（拒绝事务）。
// 仅用于从未暴露给控件/TouchState 的临时对象。
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

// 入库成功 → handover（原版库存接管终态，模块此后不得触碰指针）。
// handover 被拒 = 记账故障，对象归属不明：隔离区兜底（宁可泄漏不可 UAF）。
void handover_tracked_item_locked(uint32_t handle, void* item, const char* context) {
    if (ownership::handover_to_inventory(&g_ownership_ledger, handle) !=
        ownership::Outcome::kOk) {
        defer_item_free_locked(item);
        VIRTBAG_LOG("ownership handover reject; deferred context=%s", context);
    }
    log_ownership_audit_locked(context);
}

// 失败路径：release + 真释放恰好一次；release 被拒 = 记账故障 → 隔离区兜底。
void release_tracked_item_locked(uint32_t handle, void* item, const char* context) {
    if (ownership::release(&g_ownership_ledger, handle) == ownership::Outcome::kOk) {
        release_temporary_item_locked(item);
    } else {
        defer_item_free_locked(item);
        VIRTBAG_LOG("ownership release reject; deferred context=%s", context);
    }
    log_ownership_audit_locked(context);
}

void free_module_object_locked(int bag, int slot) {
    if (bag < 0 || bag >= virtual_bag::kBagCount || slot < 0 || slot >= virtual_bag::kSlotCount) {
        return;
    }
    void* item = g_module_objects[bag][slot];
    if (item != nullptr) {
        // 投影期间先清控件引用：Scene_Draw 会画控件 data[0]，释放悬空对象前
        // 必须置空（否则 ITEM_DrawPorting 解引用已释放内存崩溃，tombstone_06）。
        if (g_module_view_installed && g_module_view_index == bag &&
            g_projected_item_root != nullptr && fn_control_object_get_child != nullptr &&
            fn_control_item_set_item != nullptr) {
            void* ctrl = valid_child_locked(g_projected_item_root, slot);
            if (ctrl != nullptr) fn_control_item_set_item(ctrl, nullptr);
        }
        // 视图借用未归还时先归还（恢复路径之外的零散清除也保持借用成对）。
        const uint32_t handle = g_module_object_handles[bag][slot];
        if (ownership::live_state(g_ownership_ledger, handle,
                                  ownership::State::kBorrowedForView)) {
            ownership::return_from_view(&g_ownership_ledger, handle);
        }
        retire_custody_item_locked(handle, item, "free_module_object");
    }
    g_module_objects[bag][slot] = nullptr;
    g_module_object_categories[bag][slot] = 0;
    g_module_object_hashes[bag][slot] = 0;
    g_module_object_handles[bag][slot] = 0;
}

void* materialize_module_item_locked(const virtual_bag::Item& descriptor) {
    return load_item_payload_locked(descriptor.payload.data(), descriptor.payload_size,
                                    "materialize_module_item");
}

void* touch_moving_item_control_locked() {
    if (g_base == 0) return nullptr;
    uint8_t* state = reinterpret_cast<uint8_t*>(g_base + G_TOUCH_STATE_VMA);
    void* control = *reinterpret_cast<void**>(state + TOUCH_STATE_MOVING_CTRL);
    if (control == nullptr) {
        control = *reinterpret_cast<void**>(state + TOUCH_STATE_DROP_SRC_CTRL);
    }
    if (control == nullptr || fn_control_object_get_data == nullptr) return nullptr;
    void* data = fn_control_object_get_data(control);
    if (data == nullptr || *reinterpret_cast<void**>(data) == nullptr) return nullptr;
    if (*reinterpret_cast<uint8_t*>(reinterpret_cast<uint8_t*>(data) + ITEM_CTRL_MOVING_FLAG) == 0) {
        return nullptr;
    }
    return control;
}

void reset_drag_state_locked(void* moving_control) {
    if (g_base != 0) {
        uint8_t* state = reinterpret_cast<uint8_t*>(g_base + G_TOUCH_STATE_VMA);
        std::memset(state, 0, 0x10);
        *reinterpret_cast<void**>(state + 0x10) = nullptr;
        *reinterpret_cast<void**>(state + TOUCH_STATE_MOVING_CTRL) = nullptr;
        *reinterpret_cast<void**>(state + 0x38) = nullptr;
        *reinterpret_cast<void**>(state + 0x50) = nullptr;
        *reinterpret_cast<void**>(state + TOUCH_STATE_DROP_SRC_CTRL) = nullptr;
    }
    if (moving_control != nullptr && fn_control_object_get_data != nullptr) {
        void* data = fn_control_object_get_data(moving_control);
        if (data != nullptr) {
            *reinterpret_cast<uint8_t*>(reinterpret_cast<uint8_t*>(data) + ITEM_CTRL_MOVING_FLAG) = 0;
            *reinterpret_cast<uint8_t*>(reinterpret_cast<uint8_t*>(data) + ITEM_CTRL_ON_FLAG) = 0;
        }
    }
}

void clear_original_item_selection_locked() {
    if (g_base == 0 || fn_ctrl_get_child == nullptr || fn_control_object_get_data == nullptr) {
        return;
    }
    void* item_list = *reinterpret_cast<void**>(g_base + G_UIEQUIP_PANEL_CTRL_VMA);
    if (item_list == nullptr) return;
    for (uint32_t index = 0; index < virtual_bag::kSlotCount; ++index) {
        void* control = fn_ctrl_get_child(item_list, index);
        if (control == nullptr) continue;
        void* data = fn_control_object_get_data(control);
        if (data == nullptr) continue;
        *reinterpret_cast<uint8_t*>(reinterpret_cast<uint8_t*>(data) + ITEM_CTRL_MOVING_FLAG) = 0;
        *reinterpret_cast<uint8_t*>(reinterpret_cast<uint8_t*>(data) + ITEM_CTRL_ON_FLAG) = 0;
    }
}

int original_item_category_locked(void* item) {
    if (item == nullptr) return 0;
    const uint16_t flags = *reinterpret_cast<uint16_t*>(reinterpret_cast<uint8_t*>(item) + I_TYPE);
    return (flags >> 6) & 0x3FF;
}

bool category_is_equip(int category) {
    if (g_base == 0 || category <= 0) return false;
    uint8_t* class_data = *reinterpret_cast<uint8_t**>(
        *reinterpret_cast<void**>(g_base + G_ITEMCLASS_DATA_GOT_VMA));
    uint8_t* size_ptr = *reinterpret_cast<uint8_t**>(g_base + G_ITEMCLASS_SIZE_GOT_VMA);
    if (class_data == nullptr || size_ptr == nullptr) return false;
    const uint8_t stride = *size_ptr;
    if (stride == 0) return false;
    return (class_data[category * stride + 6] & 1) == 0;
}

bool move_original_to_extension_slot_locked(int src_bag, int src_slot, int dst_bag) {
    if (!virtual_bag::valid_original_transaction_bag(src_bag)) {
        VIRTBAG_LOG("cross move reject original->extension invalid source bag=%d", src_bag);
        return false;
    }
    if (src_slot < 0 || src_slot >= virtual_bag::kSlotCount) {
        VIRTBAG_LOG("cross move reject original->extension invalid source slot=%d bag=%d",
                    src_slot, src_bag);
        return false;
    }
    if (!virtual_bag::valid_index(dst_bag) || g_virtual_bag_state.capacities[dst_bag] == 0 ||
        fn_save_save_item == nullptr || fn_get_cumulate_count == nullptr ||
        fn_remove_item_direct == nullptr) {
        VIRTBAG_LOG("cross move reject original->extension dst=%d capacity=%d symbols=%d",
                    dst_bag,
                    virtual_bag::valid_index(dst_bag) ? g_virtual_bag_state.capacities[dst_bag] : 0,
                    fn_save_save_item != nullptr && fn_get_cumulate_count != nullptr &&
                    fn_remove_item_direct != nullptr ? 1 : 0);
        return false;
    }
    void* src_item = nullptr;
    if (!inventory_slot_locked(src_bag, src_slot, &src_item) || src_item == nullptr) {
        VIRTBAG_LOG("cross move reject original->extension source empty bag=%d slot=%d",
                    src_bag, src_slot);
        return false;
    }

    std::array<uint8_t, virtual_bag::kSerializedItemBuffer> source_payload{};
    int serialized_size = 0;
    if (!serialize_item_payload_locked(src_item, &source_payload, &serialized_size)) {
        VIRTBAG_LOG("cross move: item serialization rejected");
        return false;
    }
    const int src_count = fn_get_cumulate_count(src_item);
    const int src_category = original_item_category_locked(src_item);
    const int capacity = g_virtual_bag_state.capacities[dst_bag];

    // prepared：收集不可变事务快照。
    virtual_bag::TransactionContext txn{};
    txn.direction = virtual_bag::kTransferOriginalToExtension;
    txn.src_bag = static_cast<uint8_t>(src_bag);
    txn.src_slot = static_cast<uint8_t>(src_slot);
    txn.dst_bag = static_cast<uint8_t>(dst_bag);
    txn.source.category = src_category;
    txn.source.count = src_count;
    txn.source.payload_size = static_cast<uint16_t>(serialized_size);
    txn.source.payload = source_payload;

    virtual_bag::Item committed{};
    int dst_slot = -1;
    bool merged = false;
    if (move_merge_enabled()) {
        const uint32_t limit = stack_codec::max_count(stack_limit_enabled());
        for (int slot = 0; slot < capacity && dst_slot < 0; ++slot) {
            const virtual_bag::Item& existing = g_virtual_bag_state.items[dst_bag][slot];
            const uint64_t total = static_cast<uint64_t>(existing.count) +
                                   static_cast<uint64_t>(txn.source.count);
            if (!virtual_bag::mergeable_items(existing, txn.source) || total > limit) {
                continue;
            }
            committed = existing;
            committed.count = static_cast<int>(virtual_bag::merge_count(
                existing.count, txn.source.count, stack_limit_enabled()));
            virtual_bag::patch_payload_count(&committed, static_cast<uint32_t>(committed.count));
            dst_slot = slot;
            merged = true;
        }
    }
    if (dst_slot < 0) {
        for (int slot = 0; slot < capacity; ++slot) {
            const virtual_bag::Item& existing = g_virtual_bag_state.items[dst_bag][slot];
            if (existing.category != 0 || existing.count != 0) continue;
            committed = txn.source;
            dst_slot = slot;
            break;
        }
    }
    if (dst_slot < 0) {
        VIRTBAG_LOG("cross move reject original->extension destination full bag=%d capacity=%d",
                    dst_bag, capacity);
        return false;
    }
    txn.dst_slot = static_cast<uint8_t>(dst_slot);
    txn.merged = merged;
    txn.committed = committed;
    txn.previous_dst = g_virtual_bag_state.items[dst_bag][dst_slot];
    generate_transaction_id_locked(txn.transaction_id);
    if (txn.merged) {
        VIRTBAG_LOG("txn prepared id=%s original merge src=%d/%d -> extension=%d/%d count=%d+%d=%d",
                    txn.transaction_id, src_bag, src_slot, dst_bag, dst_slot, src_count,
                    txn.previous_dst.count, txn.committed.count);
    }

    // pending-recorded
    if (!txn_record_pending_locked(&txn)) return false;

    // logical-state-updated：写扩展逻辑目标；物化失败即回滚（源保持原版实态）。
    virtual_bag::txn_apply_logical(&g_virtual_bag_state, txn);
    g_item_state_dirty = true;
    if (module_item_locked(dst_bag, dst_slot) == nullptr) {
        txn_abort_locked(txn, virtual_bag::TxnStage::kLogicalUpdated,
                         "target extension item could not be materialized; source retained");
        return false;
    }

    // original-state-updated：删除原版源并以原版实态复核（不只信删除函数返回值）。
    fn_remove_item_direct(src_bag, src_slot);
    void* remaining_source = nullptr;
    if (inventory_slot_locked(src_bag, src_slot, &remaining_source) && remaining_source != nullptr) {
        txn_abort_locked(txn, virtual_bag::TxnStage::kOriginalUpdated,
                         "original source slot remained occupied");
        return false;
    }

    // committed：不做猜测性逆转。清 pending、回收模块投影对象、刷新原版网格。
    g_virtual_bag_state.pending = {};
    free_module_object_locked(dst_bag, dst_slot);
    // 用户决策（2026-08-31）：orig→ext 后保持当前视图（对齐原版"移动后停留"）。
    // 不切 mode、不装投影：mode 与投影始终成对，此前"切 mode 不装投影"的
    // overlay 错位从根上不可达；P2 移动锁也不再因 API 移动触发。
    // 原版网格残影由 RefreshItemArea 清除（INVEN 全程真实，线程先例：install 路径）。
    if (fn_ui_equip_refresh_item_area != nullptr) fn_ui_equip_refresh_item_area();
    VIRTBAG_LOG("txn committed id=%s original->extension bag=%d slot=%d cat=%d count=%d src=%d/%d",
                txn.transaction_id, dst_bag, dst_slot, src_category, txn.committed.count,
                src_bag, src_slot);
    return true;
}

bool move_original_to_extension_locked(int dst_bag, void* moving_control) {
    if (!virtual_bag::valid_index(dst_bag) || g_virtual_bag_state.capacities[dst_bag] == 0 ||
        fn_ui_equip_get_item_slot_index == nullptr) {
        VIRTBAG_LOG("cross move reject original->extension dst=%d control=%p capacity=%d symbol=%d",
                    dst_bag, moving_control,
                    virtual_bag::valid_index(dst_bag) ? g_virtual_bag_state.capacities[dst_bag] : 0,
                    fn_ui_equip_get_item_slot_index != nullptr ? 1 : 0);
        return false;
    }
    const int src_bag = original_bag_locked();
    const int src_slot = fn_ui_equip_get_item_slot_index(moving_control);
    const bool moved = move_original_to_extension_slot_locked(src_bag, src_slot, dst_bag);
    if (moved) reset_drag_state_locked(moving_control);
    return moved;
}

bool move_extension_to_original_locked(int src_bag, int src_slot, int target_bag) {
    if (!virtual_bag::valid_original_transaction_bag(target_bag) ||
        !virtual_bag::valid_index(src_bag) ||
        src_slot < 0 || src_slot >= virtual_bag::kSlotCount ||
        fn_inven_save_item_on_empty == nullptr || fn_remove_item_direct == nullptr) {
        VIRTBAG_LOG("cross move reject extension->original src=%d/%d target_bag=%d", src_bag,
                    src_slot, target_bag);
        return false;
    }
    if (fn_get_bag_size != nullptr && fn_get_bag_size(target_bag) <= 0) {
        VIRTBAG_LOG("cross move reject extension->original target bag unavailable=%d", target_bag);
        return false;
    }
    const virtual_bag::Item source = g_virtual_bag_state.items[src_bag][src_slot];
    if (source.category <= 0 || source.count <= 0) {
        VIRTBAG_LOG("cross move reject extension->original source empty=%d/%d", src_bag,
                    src_slot);
        return false;
    }
    const virtual_bag::PayloadValidation source_validation =
        virtual_bag::validate_serialized_payload_buffer(source.payload.data(), source.payload.size(),
                                                        source.payload_size);
    if (source_validation != virtual_bag::PayloadValidation::kOk) {
        VIRTBAG_LOG("cross move reject extension->original reason=%s src=%d/%d size=%u",
                    virtual_bag::payload_validation_reason(source_validation), src_bag, src_slot,
                    static_cast<unsigned int>(source.payload_size));
        return false;
    }

    // prepared：源载荷已验证；dst_slot 语义为占位 0（实际落位由 SaveItemOnEmpty 决定，
    // 恢复流程按 payload 扫描定位，不依赖该字段）。
    virtual_bag::TransactionContext txn{};
    txn.direction = virtual_bag::kTransferExtensionToOriginal;
    txn.src_bag = static_cast<uint8_t>(src_bag);
    txn.src_slot = static_cast<uint8_t>(src_slot);
    txn.dst_bag = static_cast<uint8_t>(target_bag);
    txn.source = source;
    txn.committed = source;
    generate_transaction_id_locked(txn.transaction_id);

    // pending-recorded
    if (!txn_record_pending_locked(&txn)) return false;

    // logical-state-updated：Load 临时对象并入账（P4.3：Load 成功即 allocate；扩展逻辑源未变）。
    // 失败先回滚逻辑态与 pending，再释放仍归模块所有的临时对象。
    uint32_t item_handle = 0;
    void* item = load_item_payload_tracked_locked(source.payload.data(), source.payload_size,
                                                  "move_extension_to_original", &item_handle);
    if (item != nullptr) set_original_bag_locked(target_bag);
    if (item == nullptr || !fn_inven_save_item_on_empty(item, target_bag)) {
        // P4.5：Load 失败或插入失败隔离。扩展源逻辑物品保留（payload 字节未丢），
        // 事务上下文留作只读诊断；不构造可移动替代项。
        isolate_ext2orig_failure_locked(txn,
                                        item == nullptr ? virtual_bag::TxnStage::kLogicalUpdated
                                                        : virtual_bag::TxnStage::kOriginalUpdated,
                                        item == nullptr ? virtual_bag::isolation_reason::kLoadFailed
                                                        : virtual_bag::isolation_reason::kInsertFailed,
                                        item == nullptr ? "move_extension_to_original load"
                                                        : "move_extension_to_original insert");
        txn_abort_locked(txn,
                         item != nullptr ? virtual_bag::TxnStage::kOriginalUpdated
                                         : virtual_bag::TxnStage::kLogicalUpdated,
                         "target insertion failed; extension source retained");
        release_tracked_item_locked(item_handle, item, "ext2orig insert failed");
        reset_drag_state_locked(nullptr);
        return false;
    }

    // original-state-updated：确认入库落位并以原版实态复核（指针身份扫描优先）。
    int inserted_slot = -1;
    void* inserted_item = nullptr;
    for (int slot = 0; slot < virtual_bag::kSlotCount; ++slot) {
        if (inventory_slot_locked(target_bag, slot, &inserted_item) && inserted_item == item) {
            inserted_slot = slot;
            break;
        }
    }
    if (inserted_slot < 0) {
        inserted_slot = original_inventory_payload_slot_locked(
            source.payload.data(), source.payload_size, target_bag);
    }
    if (inserted_slot < 0) {
        isolate_ext2orig_failure_locked(txn, virtual_bag::TxnStage::kOriginalUpdated,
                                        virtual_bag::isolation_reason::kSlotNotFound,
                                        "ext2orig insertion verify");
        txn_abort_locked(txn, virtual_bag::TxnStage::kOriginalUpdated,
                         "insertion slot not found; extension source retained");
        release_tracked_item_locked(item_handle, item, "ext2orig slot-not-found rollback");
        reset_drag_state_locked(nullptr);
        return false;
    }
    // P4.3：入库已确认 → 原版库存接管终态（此后模块不得触碰 item 指针）。
    handover_tracked_item_locked(item_handle, item, "ext2orig handover");

    // committed：清扩展源逻辑项、恢复投影、清 pending。已移交对象不参与模块回滚。
    g_virtual_bag_state.items[src_bag][src_slot] = {};
    g_item_state_dirty = true;
    free_module_object_locked(src_bag, src_slot);
    g_virtual_bag_state.pending = {};
    // 保持当前视图：仅拖放上下文（扩展视图安装中 = 当前视图即扩展袋）停留该视图；
    // API 路径（原版视图）不改写视图字段（用户 2026-08-31 决策）。
    if (g_module_view_installed) {
        g_virtual_bag_state.mode = virtual_bag::Mode::kModule;
        g_virtual_bag_state.selected = src_bag;
        g_virtual_bag_state.inspected = -1;
    }
    set_original_bag_locked(target_bag);
    reset_drag_state_locked(nullptr);
    VIRTBAG_LOG("txn committed id=%s extension->original src=%d/%d cat=%d count=%d target_bag=%d slot=%d",
                txn.transaction_id, src_bag, src_slot, txn.source.category, txn.source.count,
                target_bag, inserted_slot);
    return true;
}

bool move_extension_to_extension_locked(int src_bag, int src_slot, int dst_bag,
                                        int requested_dst_slot) {
    if (!virtual_bag::valid_index(src_bag) || !virtual_bag::valid_index(dst_bag) ||
        g_virtual_bag_state.capacities[dst_bag] == 0 || src_bag == dst_bag ||
        src_slot < 0 || src_slot >= virtual_bag::kSlotCount) {
        VIRTBAG_LOG("cross move reject extension->extension src=%d/%d dst_bag=%d requested_dst=%d",
                    src_bag, src_slot, dst_bag, requested_dst_slot);
        return false;
    }
    const virtual_bag::Item source = g_virtual_bag_state.items[src_bag][src_slot];
    if (source.category <= 0 || source.count <= 0) {
        VIRTBAG_LOG("cross move reject extension->extension source empty=%d/%d", src_bag,
                    src_slot);
        return false;
    }
    const virtual_bag::PayloadValidation source_validation =
        virtual_bag::validate_serialized_payload_buffer(source.payload.data(), source.payload.size(),
                                                        source.payload_size);
    if (source_validation != virtual_bag::PayloadValidation::kOk) {
        VIRTBAG_LOG("cross move reject extension->extension reason=%s src=%d/%d size=%u",
                    virtual_bag::payload_validation_reason(source_validation), src_bag, src_slot,
                    static_cast<unsigned int>(source.payload_size));
        return false;
    }

    const int capacity = g_virtual_bag_state.capacities[dst_bag];
    virtual_bag::Item committed{};
    int dst_slot = -1;
    bool merged = false;
    auto try_merge = [&](int slot) {
        const virtual_bag::Item& existing = g_virtual_bag_state.items[dst_bag][slot];
        if (!move_merge_enabled() || !virtual_bag::mergeable_items(existing, source)) return false;
        const uint32_t limit = stack_codec::max_count(stack_limit_enabled());
        const uint64_t total = static_cast<uint64_t>(existing.count) +
                               static_cast<uint64_t>(source.count);
        if (total > limit) return false;
        committed = existing;
        committed.count = static_cast<int>(virtual_bag::merge_count(
            existing.count, source.count, stack_limit_enabled()));
        virtual_bag::patch_payload_count(&committed, static_cast<uint32_t>(committed.count));
        dst_slot = slot;
        merged = true;
        return true;
    };
    if (requested_dst_slot >= 0) {
        if (requested_dst_slot >= capacity) {
            VIRTBAG_LOG("cross move reject extension->extension invalid destination=%d capacity=%d",
                        requested_dst_slot, capacity);
            return false;
        }
        const virtual_bag::Item& existing = g_virtual_bag_state.items[dst_bag][requested_dst_slot];
        if (!try_merge(requested_dst_slot)) {
            if (existing.category != 0 || existing.count != 0) {
                VIRTBAG_LOG("cross move reject extension->extension destination occupied=%d/%d",
                            dst_bag, requested_dst_slot);
                return false;
            }
            committed = source;
            dst_slot = requested_dst_slot;
        }
    } else if (move_merge_enabled()) {
        for (int slot = 0; slot < capacity && dst_slot < 0; ++slot) {
            try_merge(slot);
        }
    }
    if (dst_slot < 0) {
        for (int slot = 0; slot < capacity; ++slot) {
            const virtual_bag::Item& existing = g_virtual_bag_state.items[dst_bag][slot];
            if (existing.category != 0 || existing.count != 0) continue;
            committed = source;
            dst_slot = slot;
            break;
        }
    }
    if (dst_slot < 0) {
        VIRTBAG_LOG("cross move reject extension->extension destination full=%d capacity=%d",
                    dst_bag, capacity);
        return false;
    }

    // prepared：纯逻辑事务快照。ext→ext 恒无 original 阶段。
    virtual_bag::TransactionContext txn{};
    txn.direction = virtual_bag::kTransferExtensionToExtension;
    txn.original_stage_applicable = false;
    txn.src_bag = static_cast<uint8_t>(src_bag);
    txn.src_slot = static_cast<uint8_t>(src_slot);
    txn.dst_bag = static_cast<uint8_t>(dst_bag);
    txn.dst_slot = static_cast<uint8_t>(dst_slot);
    txn.merged = merged;
    txn.source = source;
    txn.committed = committed;
    txn.previous_dst = g_virtual_bag_state.items[dst_bag][dst_slot];
    generate_transaction_id_locked(txn.transaction_id);
    if (txn.merged) {
        VIRTBAG_LOG("txn prepared id=%s extension merge %d/%d -> %d/%d count=%d+%d=%d",
                    txn.transaction_id, src_bag, src_slot, dst_bag, dst_slot, source.count,
                    txn.previous_dst.count, txn.committed.count);
    }

    // pending-recorded
    if (!txn_record_pending_locked(&txn)) return false;

    // logical-state-updated：仅更新扩展逻辑 source/target（不调用原版库存移动，不建 journal）。
    virtual_bag::txn_apply_logical(&g_virtual_bag_state, txn);
    g_item_state_dirty = true;

    // committed
    g_virtual_bag_state.pending = {};
    free_module_object_locked(src_bag, src_slot);
    free_module_object_locked(dst_bag, dst_slot);
    reset_drag_state_locked(nullptr);
    VIRTBAG_LOG("txn committed id=%s extension->extension %d/%d -> %d/%d cat=%d count=%d",
                txn.transaction_id, src_bag, src_slot, dst_bag, dst_slot, txn.source.category,
                txn.committed.count);
    return true;
}

bool handle_bag_drop_release_locked(int64_t x, int64_t y) {
    ensure_state_loaded_locked();
    recover_pending_transaction_locked();
    VIRTBAG_LOG("drop release mode=%d selected=%d x=%lld y=%lld moving=%p extension_drag=%d",
                static_cast<int>(g_virtual_bag_state.mode), g_virtual_bag_state.selected,
                static_cast<long long>(x), static_cast<long long>(y),
                touch_moving_item_control_locked(), g_extension_drag.active ? 1 : 0);
    if (g_virtual_bag_state.mode == virtual_bag::Mode::kOriginal) {
        void* moving_control = touch_moving_item_control_locked();
        if (moving_control == nullptr) {
            VIRTBAG_LOG("drop reject original->extension no moving original control");
            return false;
        }
        for (int index = 0; index < virtual_bag::kBagCount; ++index) {
            if (!extension_tab_hit(index, x, y)) continue;
            return move_original_to_extension_locked(index, moving_control);
        }
        VIRTBAG_LOG("drop reject original->extension no extension tab hit x=%lld y=%lld",
                    static_cast<long long>(x), static_cast<long long>(y));
        return false;
    }
    if (g_virtual_bag_state.mode == virtual_bag::Mode::kModule && g_extension_drag.active) {
        const int target_slot = grid_slot_index(x, y, kGridX, kGridY);
            if (target_slot >= 0) {
            const bool handled = move_extension_to_extension_locked(
                g_extension_drag.bag, g_extension_drag.slot,
                g_virtual_bag_state.selected, target_slot);
            VIRTBAG_LOG("drop result direction=extension->extension handled=%d src=%d/%d dst=%d/%d",
                        handled ? 1 : 0, g_extension_drag.bag, g_extension_drag.slot,
                        g_virtual_bag_state.selected, target_slot);
            return handled;
        }
        for (int index = 0; index < virtual_bag::kBagCount; ++index) {
            if (index == g_extension_drag.bag) continue;
            if (!extension_tab_hit(index, x, y)) continue;
            const bool handled = move_extension_to_extension_locked(
                g_extension_drag.bag, g_extension_drag.slot, index, -1);
            VIRTBAG_LOG("drop result direction=extension->extension handled=%d src=%d/%d dst_bag=%d",
                        handled ? 1 : 0, g_extension_drag.bag, g_extension_drag.slot, index);
            return handled;
        }
        const int target_bag = original_bag_button_index(x, y);
        if (target_bag >= 0 && target_bag < 6) {
            if (!virtual_bag::valid_original_transaction_bag(target_bag)) {
                // 索引 5 = 原版任务袋（ADR-006）：不得成为扩展移动目标（触摸路径与 API 门禁对齐）。
                VIRTBAG_LOG("drop reject extension->original reason=invalid_transaction_domain target_bag=%d",
                            target_bag);
                return false;
            }
            const bool handled = move_extension_to_original_locked(
                g_extension_drag.bag, g_extension_drag.slot, target_bag);
            VIRTBAG_LOG("drop result direction=extension->original handled=%d src=%d/%d target_bag=%d",
                        handled ? 1 : 0, g_extension_drag.bag, g_extension_drag.slot, target_bag);
            return handled;
        }
        VIRTBAG_LOG("drop reject extension destination not found x=%lld y=%lld",
                    static_cast<long long>(x), static_cast<long long>(y));
    } else if (g_virtual_bag_state.mode == virtual_bag::Mode::kModule) {
        VIRTBAG_LOG("drop reject extension no active drag selected=%d",
                    g_virtual_bag_state.selected);
    }
    return false;
}

bool persist_state_locked(bool force) {
    (void)force;
    if (!g_explicit_save_in_progress) return true;
    const int slot = current_save_slot();
    if (slot < 0 || slot > 2) return false;
    if (!save_state_to_store(slot)) {
        VIRTBAG_LOG("virtual bag state slot=%d save failed", slot);
        return false;
    }
    g_loaded_slot = slot;
    if (g_explicit_save_in_progress) g_item_state_dirty = false;
    return true;
}

bool extension_tab_hit(int index, int64_t x, int64_t y) {
    if (!virtual_bag::valid_index(index)) return false;
    if (g_virtual_bag_state.capacities[index] == 0) return false;  // 未装备袋不可命中（G-12）
    // 标签位置是动态换算的（tab_rel + 袋容器绝对），用控件真实绝对 rect 判断，
    // 不用旧硬编码 kCellX/kCellY（标签左移后二者偏移 36px 致命中失效）。
    void* ctrl = g_extension_tab_buttons[index];
    if (ctrl == nullptr) return false;
    int64_t ax = 0, ay = 0;
    ctrl_abs_pos_locked(ctrl, &ax, &ay);
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
    if (root == nullptr || slot < 0 || fn_ctrl_get_count == nullptr ||
        fn_control_object_get_child == nullptr) {
        return nullptr;
    }
    if (static_cast<int>(fn_ctrl_get_count(root)) <= slot) return nullptr;
    void* ctrl = fn_control_object_get_child(root, static_cast<uint32_t>(slot));
    if (ctrl == nullptr || fn_control_object_get_user_type == nullptr ||
        fn_control_object_get_user_type(ctrl) != 2) {
        return nullptr;
    }
    return ctrl;
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

bool extension_grid_hit(int64_t x, int64_t y) {
    if (g_virtual_bag_state.mode == virtual_bag::Mode::kOriginal ||
        !virtual_bag::valid_index(g_virtual_bag_state.selected)) {
        return false;
    }
    const int64_t grid_width = 4 * kGridStep - (kGridStep - kGridCell);
    const int64_t grid_height = 4 * kGridStep - (kGridStep - kGridCell);
    return x >= kGridX && x < kGridX + grid_width && y >= kGridY && y < kGridY + grid_height;
}

int grid_slot_index(int64_t x, int64_t y, int64_t origin_x, int64_t origin_y) {
    if (x < origin_x || y < origin_y) return -1;
    const int64_t relative_x = x - origin_x;
    const int64_t relative_y = y - origin_y;
    const int column = static_cast<int>(relative_x / kGridStep);
    const int row = static_cast<int>(relative_y / kGridStep);
    if (column < 0 || column >= 4 || row < 0 || row >= 4) return -1;
    if (relative_x % kGridStep >= kGridCell || relative_y % kGridStep >= kGridCell) return -1;
    return row * 4 + column;
}

void extension_tab_abs_pos_locked(void* button, int64_t* ax, int64_t* ay) {
    uint8_t* c = reinterpret_cast<uint8_t*>(button);
    *ax = *reinterpret_cast<int64_t*>(c + CO_RECT_X);
    *ay = *reinterpret_cast<int64_t*>(c + CO_RECT_Y);
    void* p = *reinterpret_cast<void**>(c + CO_PARENT);
    while (p != nullptr) {
        uint8_t* pc = reinterpret_cast<uint8_t*>(p);
        *ax += *reinterpret_cast<int64_t*>(pc + CO_RECT_X);
        *ay += *reinterpret_cast<int64_t*>(pc + CO_RECT_Y);
        p = *reinterpret_cast<void**>(pc + CO_PARENT);
    }
}

// 扩展标签绘制。挂在 DrawInvenBag wrapper（原版袋标签之后、DrawMovingItem 之前）
// 以避免遮挡拖动中的物品。绘制顺序对齐原版 UIEquip_Draw：
//   DrawInvenBackground 底框倒序(5→0) → DrawInvenBag 图标+高亮正序(0→5)。
void draw_tab_buttons_in_frame_locked() {
    const bool can_draw_original_button = fn_grpx_draw_part != nullptr && fn_imgsys_get_group != nullptr &&
                                           fn_imgsys_get_loc != nullptr;
    void* group = can_draw_original_button ? fn_imgsys_get_group(0xf) : nullptr;
    // 标签挂袋容器（与原版 6 袋标签同父）：比对基准同步为袋容器句柄。
    void* current_bag_container =
        g_base != 0 ? *reinterpret_cast<void**>(g_base + G_UIEQUIP_PANEL_VMA + 0x50) : nullptr;
    // 容器就位重试：enter 时 install 可能因占位容器（child count<6）被跳过；
    // 每帧检测真容器就位且尚未挂载（root 变化）时重新 install。
    if (current_bag_container != nullptr &&
        fn_ctrl_get_count != nullptr && fn_ctrl_get_count(current_bag_container) >= 6 &&
        current_bag_container != g_extension_tab_root) {
        install_extension_tab_buttons_locked();
        current_bag_container =
            g_base != 0 ? *reinterpret_cast<void**>(g_base + G_UIEQUIP_PANEL_VMA + 0x50)
                        : nullptr;
    }
    const bool tabs_are_current =
        current_bag_container != nullptr && current_bag_container == g_extension_tab_root &&
        g_extension_tab_generation == g_inventory_generation;
    if (!tabs_are_current || !can_draw_original_button || group == nullptr) return;
    // 第一遍：底框 loc20（72x84），倒序 index 4→0（对齐原版 DrawInvenBackground 的 5→0）。
    for (int index = virtual_bag::kBagCount - 1; index >= 0; --index) {
        void* button = g_extension_tab_buttons[index];
        if (button == nullptr) continue;
        int64_t ax = 0, ay = 0;
        extension_tab_abs_pos_locked(button, &ax, &ay);
        void* bg = fn_imgsys_get_loc(0xf, 0x14);
        if (bg != nullptr) {
            fn_grpx_draw_part(group, static_cast<int32_t>(ax - 5),
                              static_cast<int32_t>(ay - 12), bg, 0, 1, 0);
        }
    }
    // 第二遍：袋图标 loc6/loc9 + 选中高亮 loc19，正序 index 0→4（对齐原版 DrawInvenBag 的 0→5）。
    for (int index = 0; index < virtual_bag::kBagCount; ++index) {
        void* button = g_extension_tab_buttons[index];
        if (button == nullptr) continue;
        const bool selected = g_virtual_bag_state.mode == virtual_bag::Mode::kModule &&
                              g_virtual_bag_state.selected == index;
        const bool equipped = g_virtual_bag_state.capacities[index] != 0;
        int64_t ax = 0, ay = 0;
        extension_tab_abs_pos_locked(button, &ax, &ay);
        if (selected) {
            void* icon = fn_imgsys_get_loc(0xf, 0x13);
            if (icon != nullptr) {
                fn_grpx_draw_part(group, static_cast<int32_t>(ax - 5),
                                  static_cast<int32_t>(ay - 12), icon, 0, 1, 0);
            }
            void* frame = fn_imgsys_get_loc(0xf, 6);
            if (frame != nullptr) {
                fn_grpx_draw_part(group, static_cast<int32_t>(ax + 5),
                                  static_cast<int32_t>(ay + 5), frame, 0, 1, 0);
            }
        } else if (equipped) {
            void* frame = fn_imgsys_get_loc(0xf, 6);
            if (frame != nullptr) {
                fn_grpx_draw_part(group, static_cast<int32_t>(ax + 5),
                                  static_cast<int32_t>(ay + 5), frame, 0, 1, 0x28);
            }
        } else {
            void* bag_icon = fn_imgsys_get_loc(0xf, 9);
            if (bag_icon != nullptr) {
                fn_grpx_draw_part(group, static_cast<int32_t>(ax + 5),
                                  static_cast<int32_t>(ay + 5), bag_icon, 0, 1, 0x28);
            }
        }
    }
}

void draw_cells_in_frame_locked() {
    const bool can_draw_original_button = fn_grpx_draw_part != nullptr && fn_imgsys_get_group != nullptr &&
                                           fn_imgsys_get_loc != nullptr;
    void* group = can_draw_original_button ? fn_imgsys_get_group(0xf) : nullptr;
    if (g_virtual_bag_state.mode != virtual_bag::Mode::kModule ||
        !virtual_bag::valid_index(g_virtual_bag_state.selected) || g_module_view_installed) {
        return;
    }

    const int bag = g_virtual_bag_state.selected;
    const int capacity = g_virtual_bag_state.capacities[bag];
    for (int slot = 0; slot < capacity; ++slot) {
        const int column = slot % 4;
        const int row = slot / 4;
        const UiRect rect{kGridX + column * kGridStep, kGridY + row * kGridStep,
                          kGridCell, kGridCell};
        const bool occupied = slot < capacity &&
                              g_virtual_bag_state.items[bag][slot].category > 0 &&
                              g_virtual_bag_state.items[bag][slot].count > 0;
        const bool dragging_source = g_extension_drag.active &&
                                     g_extension_drag.bag == bag &&
                                     g_extension_drag.slot == slot;
        if (can_draw_original_button && group != nullptr) {
            void* slot_background = fn_imgsys_get_loc(0xf, 0x29);
            if (slot_background != nullptr) {
                fn_grpx_draw_part(group, static_cast<int32_t>(rect.x), static_cast<int32_t>(rect.y),
                                  slot_background, 0, 1, 0);
            }
        }
        if (occupied && !dragging_source && fn_item_draw_porting != nullptr) {
            void* item = module_item_locked(bag, slot);
            if (item != nullptr) {
                // ITEM_DrawPorting applies the original +5 inset, rarity
                // frame, icon mapping and stack count itself.
                fn_item_draw_porting(item, static_cast<int32_t>(rect.x),
                                     static_cast<int32_t>(rect.y), 0, 1);
            }
        }
        if (g_virtual_bag_state.inspected == slot && fn_grpx_fill_rect != nullptr) {
            constexpr int32_t kInspectBorder = 3;
            constexpr uint32_t kInspectColor = 0xff75d8ff;
            fn_grpx_fill_rect(static_cast<int32_t>(rect.x), static_cast<int32_t>(rect.y),
                              kGridCell, kInspectBorder, kInspectColor);
            fn_grpx_fill_rect(static_cast<int32_t>(rect.x),
                              static_cast<int32_t>(rect.y + kGridCell - kInspectBorder),
                              kGridCell, kInspectBorder, kInspectColor);
            fn_grpx_fill_rect(static_cast<int32_t>(rect.x), static_cast<int32_t>(rect.y),
                              kInspectBorder, kGridCell, kInspectColor);
            fn_grpx_fill_rect(static_cast<int32_t>(rect.x + kGridCell - kInspectBorder),
                              static_cast<int32_t>(rect.y), kInspectBorder, kGridCell,
                              kInspectColor);
        }
    }
    if (g_extension_drag.active && g_extension_drag.bag == bag &&
        fn_item_draw_porting != nullptr) {
        void* item = module_item_locked(bag, g_extension_drag.slot);
        if (item != nullptr) {
            const int32_t ghost_x = static_cast<int32_t>(g_extension_drag.current_x - kGridCell / 2);
            const int32_t ghost_y = static_cast<int32_t>(g_extension_drag.current_y - kGridCell / 2);
            fn_item_draw_porting(item, ghost_x, ghost_y, 0, 1);
        }
    }
}

int original_bag_locked();

uint32_t* bag_size_word_locked(int bag) {
    if (g_base == 0) return nullptr;
    void*** table_slot = reinterpret_cast<void***>(g_base + G_BAG_TABLE_VMA);
    if (table_slot == nullptr || *table_slot == nullptr) return nullptr;
    if (bag < 0 || bag >= 6) return nullptr;
    void* bag_object = (*table_slot)[bag];
    if (bag_object == nullptr) return nullptr;
    return reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(bag_object) + 0x10);
}

uint32_t* original_bag_size_word_locked() {
    return bag_size_word_locked(original_bag_locked());
}

void refresh_module_item_area_locked(int index) {
    if (fn_ui_equip_refresh_item_area == nullptr) return;
    uint32_t* size_word = g_original_bag_size_word;
    if (size_word == nullptr) {
        fn_ui_equip_refresh_item_area();
        return;
    }
    constexpr uint32_t kCapacityMask = (1u << 25) - 1u;
    *size_word = (g_original_bag_size & ~kCapacityMask) |
                 (static_cast<uint32_t>(g_virtual_bag_state.capacities[index]) & kCapacityMask);
    fn_ui_equip_refresh_item_area();
}

void virtual_bag_draw_original_bag_wrapper() {
    const OriginalDrawInvenBagFn original =
        reinterpret_cast<OriginalDrawInvenBagFn>(g_base + fn_resolve("F_UIEQUIP_DRAW_INVEN_BAG_VMA",
                                                                     F_UIEQUIP_DRAW_INVEN_BAG_VMA));
    if (original == nullptr) return;
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    ensure_state_loaded_locked();
    const bool module_view = g_virtual_bag_state.mode == virtual_bag::Mode::kModule &&
                             virtual_bag::valid_index(g_virtual_bag_state.selected);
    static int last_module_view = -1;
    if (last_module_view != static_cast<int>(module_view)) {
        log_exit_trace_locked("draw_bag", 0, 0, 0);
        last_module_view = module_view ? 1 : 0;
    }
    // 进入扩展选中态（kModule）时，原版袋标签全部取消高亮（互斥）：
    // 画原版 DrawInvenBag 前把当前袋 GOT 临时设为 kNoOriginalBagSelected(6)，
    // 画完恢复。此前只在 !installed 分支屏蔽，installed 分支漏掉了。
    uint8_t** current_bag = reinterpret_cast<uint8_t**>(g_base + G_UIEQUIP_CUR_BAG_GOT_VMA);
    uint8_t saved_current = 0;
    bool masked = false;
    if (module_view && current_bag != nullptr && *current_bag != nullptr) {
        saved_current = **current_bag;
        **current_bag = kNoOriginalBagSelected;
        masked = true;
    }
    if (module_view && g_module_view_installed) {
        original();
        if (masked) **current_bag = saved_current;
        draw_tab_buttons_in_frame_locked();
        refresh_projection_if_overwritten_locked();
        return;
    }
    original();
    if (masked) **current_bag = saved_current;
    draw_tab_buttons_in_frame_locked();
}
void virtual_bag_draw_inven_item_wrapper() {
    const OriginalDrawInvenItemFn original =
        reinterpret_cast<OriginalDrawInvenItemFn>(g_base + fn_resolve("F_UIEQUIP_DRAW_INVEN_ITEM_VMA",
                                                                       F_UIEQUIP_DRAW_INVEN_ITEM_VMA));
    if (original == nullptr) return;
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    if (g_virtual_bag_state.mode == virtual_bag::Mode::kModule &&
        virtual_bag::valid_index(g_virtual_bag_state.selected)) {
        if (g_module_view_installed) {
            original();
            return;
        }
        // Extension items occupy the original inventory rectangle without
        // entering g_inven. Mask the original bag only for this draw call so
        // the underlying ControlItem visuals do not show through the overlay.
        uint8_t** current_bag = reinterpret_cast<uint8_t**>(g_base + G_UIEQUIP_CUR_BAG_GOT_VMA);
        uint8_t saved_current = 0;
        bool masked_for_draw = false;
        if (current_bag != nullptr && *current_bag != nullptr &&
            **current_bag != kNoOriginalBagSelected) {
            saved_current = **current_bag;
            **current_bag = kNoOriginalBagSelected;
            masked_for_draw = true;
        }
        original();
        if (masked_for_draw) **current_bag = saved_current;
        return;
    }
    uint8_t** current_bag = reinterpret_cast<uint8_t**>(g_base + G_UIEQUIP_CUR_BAG_GOT_VMA);
    uint8_t saved_current = 0;
    bool restored_for_draw = false;
    if (g_virtual_bag_state.mode == virtual_bag::Mode::kExitingModule &&
        g_exit_display_bag < kNoOriginalBagSelected && current_bag != nullptr &&
        *current_bag != nullptr && **current_bag == kNoOriginalBagSelected) {
        saved_current = **current_bag;
        **current_bag = g_exit_display_bag;
        restored_for_draw = true;
    }
    original();
    if (restored_for_draw) **current_bag = saved_current;
}

int original_bag_locked() {
    if (g_base == 0) return 0;
    uint8_t** current_bag = reinterpret_cast<uint8_t**>(g_base + G_UIEQUIP_CUR_BAG_GOT_VMA);
    if (current_bag != nullptr && *current_bag != nullptr && **current_bag < 6) {
        return **current_bag;
    }
    const uint8_t direct = *reinterpret_cast<uint8_t*>(g_base + G_UIEQUIP_CUR_BAG_VMA);
    if (direct < 6) return direct;
    return 0;
}

void set_original_bag_locked(int bag) {
    if (g_base == 0 || bag < 0 || bag >= 6) return;
    *reinterpret_cast<uint8_t*>(g_base + G_UIEQUIP_CUR_BAG_VMA) = static_cast<uint8_t>(bag);
    uint8_t** current_bag = reinterpret_cast<uint8_t**>(g_base + G_UIEQUIP_CUR_BAG_GOT_VMA);
    if (current_bag != nullptr && *current_bag != nullptr) **current_bag = static_cast<uint8_t>(bag);
    log_exit_trace_locked("write_set", 0, 0, 0);
}

bool original_got_bag_locked(int* bag) {
    if (bag == nullptr || g_base == 0) return false;
    uint8_t** current_bag = reinterpret_cast<uint8_t**>(g_base + G_UIEQUIP_CUR_BAG_GOT_VMA);
    if (current_bag == nullptr || *current_bag == nullptr || **current_bag >= kNoOriginalBagSelected) {
        return false;
    }
    *bag = **current_bag;
    return true;
}

void clear_original_bag_selection_locked() {
    if (g_base == 0) return;
    *reinterpret_cast<uint8_t*>(g_base + G_UIEQUIP_CUR_BAG_VMA) = kNoOriginalBagSelected;
    uint8_t** current_bag = reinterpret_cast<uint8_t**>(g_base + G_UIEQUIP_CUR_BAG_GOT_VMA);
    if (current_bag != nullptr && *current_bag != nullptr) **current_bag = kNoOriginalBagSelected;
    log_exit_trace_locked("write_clear", 0, 0, 0);
}

void clear_original_desc_locked() {
    reset_extension_desc_unequip_hook_locked();
    if (g_base == 0) return;
    *reinterpret_cast<uint8_t*>(g_base + G_UIEQUIP_DESC_TYPE_VMA) = 0;
    if (fn_ui_desc_set_off != nullptr) fn_ui_desc_set_off();
}

int original_bag_button_index(int64_t x, int64_t y) {
    // UIEquip original bag controls are children of nested parents. The final absolute
    // rect for bag 0 is (1116, 145, 57, 57); each subsequent original bag is 70px lower.
    constexpr int64_t kOriginalBagX = 0x45c;
    constexpr int64_t kOriginalBagWidth = 0x39;
    constexpr int64_t kOriginalBagY = 0x91;
    constexpr int64_t kOriginalBagHeight = 0x39;
    constexpr int64_t kOriginalBagStepY = 0x46;
    if (x < kOriginalBagX || x >= kOriginalBagX + kOriginalBagWidth || y < kOriginalBagY) {
        return -1;
    }
    const int64_t relative_y = y - kOriginalBagY;
    const int index = static_cast<int>(relative_y / kOriginalBagStepY);
    if (index < 0 || index >= 6 || relative_y % kOriginalBagStepY >= kOriginalBagHeight) return -1;
    return index;
}

bool bind_original_exit_display_bag_locked(int bag) {
    if (g_base == 0 || bag < 0 || bag >= 6 || fn_ui_equip_refresh_item_area == nullptr) return false;
    uint8_t* direct_bag = reinterpret_cast<uint8_t*>(g_base + G_UIEQUIP_CUR_BAG_VMA);
    uint8_t** current_bag = reinterpret_cast<uint8_t**>(g_base + G_UIEQUIP_CUR_BAG_GOT_VMA);
    if (current_bag == nullptr || *current_bag == nullptr) return false;
    const uint8_t saved_direct = *direct_bag;
    const uint8_t saved_got = **current_bag;
    *direct_bag = static_cast<uint8_t>(bag);
    **current_bag = static_cast<uint8_t>(bag);
    constexpr uint32_t kCapacityMask = (1u << 25) - 1u;
    const uint32_t* size_word = bag_size_word_locked(bag);
    const bool has_capacity = size_word != nullptr && (*size_word & kCapacityMask) != 0;
    if (has_capacity) fn_ui_equip_refresh_item_area();
    *direct_bag = saved_direct;
    **current_bag = saved_got;
    return has_capacity;
}

void* module_item_locked(int bag, int slot) {
    if (!virtual_bag::valid_index(bag) || slot < 0 || slot >= virtual_bag::kSlotCount) return nullptr;
    const virtual_bag::Item& descriptor = g_virtual_bag_state.items[bag][slot];
    if (descriptor.category <= 0 || descriptor.count <= 0) return nullptr;
    const uint32_t descriptor_hash = virtual_bag::payload_hash(descriptor);
    void* item = g_module_objects[bag][slot];
    if (item == nullptr || g_module_object_categories[bag][slot] != descriptor.category ||
        g_module_object_hashes[bag][slot] != descriptor_hash) {
        free_module_object_locked(bag, slot);
        item = materialize_module_item_locked(descriptor);
        if (item == nullptr) return nullptr;
        uint32_t handle = 0;
        if (ownership::allocate(&g_ownership_ledger, &handle) == ownership::Outcome::kOk) {
            g_module_object_handles[bag][slot] = handle;  // module-owned（G-13）
        }
        g_module_objects[bag][slot] = item;
        g_module_object_categories[bag][slot] = descriptor.category;
        g_module_object_hashes[bag][slot] = descriptor_hash;
    }
    return item;
}

// 保存门禁（P3 事故修复）：SAVE_SaveInventory 被游戏内部直调（存档面板/自动存档），
// 投影安装期间执行会把 INVEN 投影态写进存档（E-2026-08-29-02 污染事故根因）。
// 通过替换 SAVE_Save 内唯一 bl 调用点，保存前强制把投影写回快照。
typedef uint32_t (*SaveInventoryRawFn)(uint8_t* cursor);
uint32_t save_inventory_wrapper(uint8_t* cursor) {
    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        if (g_module_view_installed) {
            VIRTBAG_LOG("save gate: restoring projected view before SAVE_SaveInventory");
            restore_module_view_locked();
        }
    }
    const uintptr_t raw = g_base != 0
                              ? g_base + fn_resolve("F_SAVE_SAVE_INVENTORY_VMA",
                                                    F_SAVE_SAVE_INVENTORY_VMA)
                              : 0;
    if (raw == 0) return 0;
    return reinterpret_cast<SaveInventoryRawFn>(raw)(cursor);
}

// G-6 终极绘制门禁：ControlItem_Draw 的 bl ITEM_DrawPorting（0xaaf28）替换——
// 投影期间任何控件 data[0] 悬空（desc 残留/重建窗口/释放时序）都不再崩游戏：
// 物品指针必须命中已知集合（模块缓存/隔离区/INVEN_pItem）才放行绘制。
bool item_pointer_known_locked(void* item);

uint32_t item_draw_porting_gate(void* item, int32_t x, int32_t y, int32_t a, int32_t b, int32_t c) {
    if (g_module_view_installed && !item_pointer_known_locked(item)) {
        VIRTBAG_LOG("draw gate: suppressed unknown item=%p", item);
        return 0;
    }
    const uintptr_t raw = g_base != 0
                              ? g_base + fn_resolve("F_ITEM_DRAW_PORTING_VMA",
                                                    F_ITEM_DRAW_PORTING_VMA)
                              : 0;
    if (raw == 0) return 0;
    typedef uint32_t (*DrawPortingFn)(void*, int32_t, int32_t, int32_t, int32_t, int32_t);
    return reinterpret_cast<DrawPortingFn>(raw)(item, x, y, a, b, c);
}

// G-6/G-7 drop 门禁 → G-8 路由：bag proc event 0x04 落袋写入（b8cc0 bl）——
// 投影期间拖动投影物品松手到这里：路由到 ext→orig 事务（真实移动），借出对象
// 不经原版 SaveItemOnEmpty 写 INVEN。非投影拖动（installed=false）直通原版。
// G-8 源识别：drop 事件的物品指针与模块缓存比对，反查扩展袋/槽。
// 事件参数自带物品指针（SaveItemOnEmpty 的 item / 0x02 的 src 控件 data），
// 无需模块维护拖动状态机——TouchHandle 全权处理拖动建立与拖影。
bool module_slot_of_item_locked(void* item, int* out_bag, int* out_slot) {
    if (item == nullptr) return false;
    for (int bag = 0; bag < virtual_bag::kBagCount; ++bag) {
        for (int slot = 0; slot < virtual_bag::kSlotCount; ++slot) {
            if (g_module_objects[bag][slot] == item) {
                *out_bag = bag;
                *out_slot = slot;
                return true;
            }
        }
    }
    return false;
}

int32_t save_item_on_empty_gate(void* item, int32_t bag) {
    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        if (g_module_view_installed) {
            int src_bag = -1, src_slot = -1;
            if (!module_slot_of_item_locked(item, &src_bag, &src_slot)) {
                return reinterpret_cast<InvenSaveItemOnEmptyFn>(
                    g_base + fn_resolve("F_INVEN_SAVE_ITEM_ON_EMPTY_VMA",
                                        F_INVEN_SAVE_ITEM_ON_EMPTY_VMA))(item, bag);
            }
            const bool routed = virtual_bag::valid_original_transaction_bag(bag) &&
                                move_extension_to_original_locked(src_bag, src_slot, bag);
            if (routed) {
                persist_state_locked();
                refresh_projection_if_overwritten_locked();
                VIRTBAG_LOG("drop routed ext->orig bag=%d", bag);
                return 1;
            }
            VIRTBAG_LOG("drop gate: route failed bag=%d", bag);
            return 0;
        }
    }
    const uintptr_t raw = g_base != 0
                              ? g_base + fn_resolve("F_INVEN_SAVE_ITEM_ON_EMPTY_VMA",
                                                    F_INVEN_SAVE_ITEM_ON_EMPTY_VMA)
                              : 0;
    if (raw == 0) return 0;
    return reinterpret_cast<InvenSaveItemOnEmptyFn>(raw)(item, bag);
}

// 正式窗口投影（P3 方案 C，控件级）：只写控件不写 INVEN——
//   容量字（袋对象 +0x10）临时写扩展容量驱动 RefreshItemArea/DrawInvenBag 的容量语义；
//   RefreshItemArea 按容量字 SetActive/SetShow 容量外控件（隐藏）；
//   容量内控件 ControlItem_SetItem(借出对象) 显示扩展物品；
//   INVEN_pItem 全程真实（API/存档/捡取读数无污染），restore 仅写回容量字 + RefreshItemArea。
bool install_module_view_locked(int bag) {
    if (g_base == 0 || !virtual_bag::valid_index(bag)) return false;
    if (g_virtual_bag_state.capacities[bag] == 0) return false;
    if (fn_control_item_set_item == nullptr || fn_control_object_get_child == nullptr) return false;
    const int original_bag = original_bag_locked();
    if (!virtual_bag::valid_original_transaction_bag(original_bag)) {
        VIRTBAG_LOG("module view reject install: original window bag=%d (task bag reserved)",
                    original_bag);
        return false;
    }
    uint32_t* size_word = bag_size_word_locked(original_bag);
    if (size_word == nullptr) return false;

    if (g_module_view_installed) restore_module_view_locked();
    clear_original_desc_locked();  // 原版详情面板不跨视图残留

    g_original_bag_size_word = size_word;
    g_original_bag_size = *size_word;
    const int capacity = g_virtual_bag_state.capacities[bag];
    constexpr uint32_t kCapacityMask = (1u << 25) - 1u;
    *size_word = (*size_word & ~kCapacityMask) |
                 (static_cast<uint32_t>(capacity) & kCapacityMask);

    void* root = *reinterpret_cast<void**>(g_base + G_UIEQUIP_PANEL_CTRL_VMA);
    g_projected_item_root = root;
    if (root != nullptr && fn_ui_equip_refresh_item_area != nullptr) {
        fn_ui_equip_refresh_item_area();  // 按新容量禁用容量外控件 + 刷 INVEN 原版物品
        for (int slot = 0; slot < capacity; ++slot) {
            void* ctrl = valid_child_locked(root, slot);
            if (ctrl == nullptr) continue;
            void* item = module_item_locked(bag, slot);
            // 空槽也必须 SetItem(nullptr)：RefreshItemArea 刚把 INVEN 原版物品刷进控件，
            // 扩展袋空位不覆盖的话会残留原版物品显示。
            fn_control_item_set_item(ctrl, item);
            if (item != nullptr) {
                ownership::borrow_for_view(&g_ownership_ledger,
                                           g_module_object_handles[bag][slot]);
            }
        }
    }
    g_module_view_installed = true;
    g_module_view_index = bag;
    VIRTBAG_LOG("module view installed bag=%d window=%d capacity=%d", bag, original_bag, capacity);
    return true;
}

// 帧级自愈：RefreshItemArea 被游戏逻辑触发时会把控件刷回 INVEN 原版物品，
// installed 状态下每帧检测并重投影（draw wrapper 已持锁）。
void refresh_projection_if_overwritten_locked() {
    if (!g_module_view_installed || g_projected_item_root == nullptr ||
        fn_control_object_get_data == nullptr || fn_control_item_set_item == nullptr ||
        !virtual_bag::valid_index(g_module_view_index)) {
        return;
    }
    const int bag = g_module_view_index;
    const int capacity = g_virtual_bag_state.capacities[bag];
    static int heal_count = 0;
    for (int slot = 0; slot < capacity; ++slot) {
        void* item = g_module_objects[bag][slot];  // 空槽为 nullptr，同样需要清空控件
        void* ctrl = valid_child_locked(g_projected_item_root, slot);
        if (ctrl == nullptr) continue;
        void* data = fn_control_object_get_data(ctrl);
        void* current = data != nullptr ? *reinterpret_cast<void**>(data) : nullptr;
        if (current != item) {
            ++heal_count;
            fn_control_item_set_item(ctrl, item);
            VIRTBAG_LOG("projection heal #%d slot=%d current=%p item=%p",
                        heal_count, slot, current, item);
        }
    }
}

void restore_module_view_locked() {
    if (!g_module_view_installed || g_base == 0) return;
    // P4.3：视图借出逐槽归还（安装时借出的槽位必须成对归还，窗口外借用为零）。
    if (virtual_bag::valid_index(g_module_view_index)) {
        for (int slot = 0; slot < virtual_bag::kSlotCount; ++slot) {
            const uint32_t handle = g_module_object_handles[g_module_view_index][slot];
            if (ownership::live_state(g_ownership_ledger, handle,
                                      ownership::State::kBorrowedForView)) {
                ownership::return_from_view(&g_ownership_ledger, handle);
            }
        }
        log_ownership_audit_locked("restore_module_view");
    }
    clear_original_desc_locked();  // 关闭原版详情面板（二次点击 MakeDesc 打开）
    if (g_original_bag_size_word != nullptr) {
        constexpr uint32_t kCapacityMask = (1u << 25) - 1u;
        *g_original_bag_size_word =
            (*g_original_bag_size_word & ~kCapacityMask) |
            (g_original_bag_size & kCapacityMask);
    }
    if (fn_ui_equip_refresh_item_area != nullptr) {
        fn_ui_equip_refresh_item_area();  // 容量字已还原：原版逻辑自动恢复控件（INVEN 全程真实）
    }
    g_original_bag_size_word = nullptr;
    g_original_bag_size = 0;
    g_projected_item_root = nullptr;
    g_module_view_installed = false;
    g_module_view_index = -1;
    log_exit_trace_locked("write_restore", 0, 0, 0);
}

bool complete_original_exit_locked() {
    int selected_original_bag = 0;
    if (!original_got_bag_locked(&selected_original_bag)) {
        log_exit_trace_locked("complete_invalid_got", 0, 0, 0);
        return false;
    }
    set_original_bag_locked(selected_original_bag);
    if (fn_ui_equip_refresh_item_area != nullptr) fn_ui_equip_refresh_item_area();
    virtual_bag::enter_original(&g_virtual_bag_state, selected_original_bag);
    persist_state_locked();
    g_exit_display_bag = kNoOriginalBagSelected;
    log_exit_trace_locked("complete_success", 0, 0, 0);
    return true;
}

void cancel_original_exit_locked() {
    const int original_bag = g_original_current_got < kNoOriginalBagSelected ?
                             g_original_current_got : g_original_current_direct;
    set_original_bag_locked(original_bag);
    if (fn_ui_equip_refresh_item_area != nullptr) fn_ui_equip_refresh_item_area();
    virtual_bag::enter_original(&g_virtual_bag_state, original_bag);
    persist_state_locked();
    g_exit_display_bag = kNoOriginalBagSelected;
    log_exit_trace_locked("complete_cancel", 0, 0, 0);
}

void virtual_bag_f3_wrapper() {
    PopupNoArgFn original = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        ensure_state_loaded_locked();
        const bool was_exiting_module = g_virtual_bag_state.mode == virtual_bag::Mode::kExitingModule;
        if (g_module_view_installed) restore_module_view_locked();
        if (!was_exiting_module && g_virtual_bag_state.mode != virtual_bag::Mode::kOriginal) {
            virtual_bag::enter_original(&g_virtual_bag_state, original_bag_locked());
        }
        if (was_exiting_module) {
            cancel_original_exit_locked();
        }
        g_exit_display_bag = kNoOriginalBagSelected;
        g_inventory_frame_active = false;
        g_extension_touch_capture = false;
        g_extension_drag = {};
        disable_extension_tab_buttons_locked();
        log_exit_trace_locked("f3", 0, 0, 0);
        original = g_orig_f3;
    }
    if (original != nullptr) original();
}

void virtual_bag_inventory_enter_wrapper() {
    PopupNoArgFn original = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        ensure_state_loaded_locked();
        clear_original_desc_locked();
        restore_module_view_locked();
        set_original_bag_locked(0);
        virtual_bag::enter_original(&g_virtual_bag_state, 0);
        g_inventory_frame_active = true;
        g_extension_touch_capture = false;
        g_extension_drag = {};
        disable_extension_tab_buttons_locked();
        persist_state_locked();
        original = g_orig_enter;
    }
    if (original != nullptr) original();
    if (!g_virtual_bag_enabled.load()) return;
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    install_extension_tab_buttons_locked();
    if (fn_ui_equip_refresh_item_area != nullptr) fn_ui_equip_refresh_item_area();
}

void virtual_bag_save_panel_enter_wrapper() {
    if (game_in_world()) {
        virtual_bag_save_game();
    }
    if (g_orig_save_enter != nullptr) g_orig_save_enter();
}

void virtual_bag_draw_end_wrapper() {
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    if (g_state != nullptr && *reinterpret_cast<uint16_t*>(g_state) != 5) {
        prepare_main_menu_locked();
        if (fn_grpx_end != nullptr) fn_grpx_end();
        return;
    }
    if (g_gamestate != nullptr && *reinterpret_cast<uint32_t*>(g_gamestate) != 0) {
        restore_module_view_locked();
        if (fn_grpx_end != nullptr) fn_grpx_end();
        return;
    }
    if (!g_inventory_frame_active) {
        restore_module_view_locked();
        set_original_bag_locked(0);
        virtual_bag::enter_original(&g_virtual_bag_state, 0);
        g_inventory_frame_active = true;
    }
    if (!g_virtual_bag_enabled.load()) {
        restore_module_view_locked();
        if (fn_grpx_end != nullptr) fn_grpx_end();
        return;
    }
    ensure_state_loaded_locked();
    commit_pending_extension_tab_locked();
    // 投影常驻（P2/P3）：module 视图有效期间保持安装；仅在状态不再指向
    // 模块视图（退出/切档/异常）时兜底恢复，防止遗留投影污染原版窗口。
    const bool module_view_expected =
        g_virtual_bag_state.mode == virtual_bag::Mode::kModule &&
        virtual_bag::valid_index(g_virtual_bag_state.selected);
    if (g_module_view_installed && !module_view_expected) restore_module_view_locked();
    if (g_module_view_installed) {
        // TouchHandle owns the native drag shadow. Keep a verified projected control
        // alive across frames; all other stale moving controls are still cleared.
        if (g_base != 0) {
            uint8_t* touch_state = reinterpret_cast<uint8_t*>(g_base + G_TOUCH_STATE_VMA);
            void* moving = *reinterpret_cast<void**>(touch_state + TOUCH_STATE_MOVING_CTRL);
            bool projected_moving = false;
            if (moving != nullptr && virtual_bag::valid_index(g_module_view_index) &&
                g_projected_item_root != nullptr) {
                const int capacity = g_virtual_bag_state.capacities[g_module_view_index];
                for (int slot = 0; slot < capacity; ++slot) {
                    if (valid_child_locked(g_projected_item_root, slot) == moving) {
                        projected_moving = true;
                        break;
                    }
                }
            }
            if (!projected_moving) {
                *reinterpret_cast<void**>(touch_state + TOUCH_STATE_MOVING_CTRL) = nullptr;
            }
        }
        refresh_projection_if_overwritten_locked();
    }
    if (g_module_view_installed) {
        refresh_projection_if_overwritten_locked();
    }
    static int last_mode = -1;
    static int last_selected = -2;
    static int last_overlay = -1;
    static int last_overlay_index = -2;
    if (last_mode != static_cast<int>(g_virtual_bag_state.mode) ||
        last_selected != g_virtual_bag_state.selected ||
        last_overlay != static_cast<int>(g_module_view_installed) ||
        last_overlay_index != g_module_view_index) {
        log_exit_trace_locked("draw_end", 0, 0, 0);
        last_mode = static_cast<int>(g_virtual_bag_state.mode);
        last_selected = g_virtual_bag_state.selected;
        last_overlay = g_module_view_installed ? 1 : 0;
        last_overlay_index = g_module_view_index;
    }
    draw_cells_in_frame_locked();
    if (fn_grpx_end != nullptr) fn_grpx_end();
}

// thunk 页登记：每页 4096B 承载 28B thunk，剩余空间可复用（页必在 libgame
// 调用点 ±128MB 内，多挂钩共享免再扫地址空间；粗扫粒度下零星单页空洞会
// 耗尽——真机实证：第 7 个挂钩因分配失败拖垮整个注入链）。
std::array<void*, 8> g_thunk_pages{};
size_t g_thunk_page_count = 0;
size_t g_thunk_page_used = 0;  // 当前页已用槽位数（每槽 32B 对齐）

void* allocate_draw_thunk(uintptr_t call_addr, uintptr_t wrapper) {
#ifndef MAP_FIXED_NOREPLACE
    (void)call_addr;
    (void)wrapper;
    return nullptr;
#else
    constexpr size_t kPageSize = 4096;
    constexpr size_t kThunkSlot = 32;
    const auto emit_thunk = [](void* slot, uintptr_t wrapper_addr) {
        uint32_t code[] = {
            0xa9bf7bf0, // stp x16, x30, [sp, #-16]!
            0x58000090, // ldr x16, #16 (literal at thunk+20)
            0xd63f0200, // blr x16
            0xa8c17bf0, // ldp x16, x30, [sp], #16
            0xd65f03c0, // ret
        };
        memcpy(slot, code, sizeof(code));
        *reinterpret_cast<uintptr_t*>(reinterpret_cast<uint8_t*>(slot) + 20) = wrapper_addr;
        __builtin___clear_cache(reinterpret_cast<char*>(slot),
                                reinterpret_cast<char*>(reinterpret_cast<uint8_t*>(slot) + kThunkSlot));
    };
    // 1) 已有页 carving（校验对本调用点的 BL 可达性：页可能在别的调用点
    // ±128MB 边缘，libgame 跨度 ~20MB 时存在不可达组合）。
    if (g_thunk_page_count > 0) {
        void* page = g_thunk_pages[g_thunk_page_count - 1];
        void* slot = reinterpret_cast<uint8_t*>(page) + g_thunk_page_used * kThunkSlot;
        const int64_t reach = static_cast<int64_t>(reinterpret_cast<uintptr_t>(slot)) -
                              static_cast<int64_t>(call_addr);
        if ((g_thunk_page_used + 1) * kThunkSlot <= kPageSize && reach > -0x08000000LL &&
            reach < 0x08000000LL) {
            ++g_thunk_page_used;
            emit_thunk(slot, wrapper);
            return slot;
        }
    }
    // 2) 两级扫描新页：先 64KB 粗扫（快），失败后 4KB 细扫兜底。
    for (const int64_t coarse : {1, 0}) {
        const int64_t kStep = coarse ? 0x00010000 : 0x1000;
        const uintptr_t base = call_addr & ~(static_cast<uintptr_t>(kPageSize) - 1);
        for (int64_t distance = kStep; distance < 0x08000000; distance += kStep) {
            for (int sign : {1, -1}) {
                const int64_t candidate_signed = static_cast<int64_t>(base) + sign * distance;
                if (candidate_signed <= 0) continue;
                void* region = mmap(reinterpret_cast<void*>(static_cast<uintptr_t>(candidate_signed)), kPageSize,
                                    PROT_READ | PROT_WRITE | PROT_EXEC,
                                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
                if (region == MAP_FAILED) continue;
                emit_thunk(region, wrapper);
                if (g_thunk_page_count < g_thunk_pages.size()) {
                    g_thunk_pages[g_thunk_page_count++] = region;
                    g_thunk_page_used = 1;
                }
                return region;
            }
        }
    }
    return nullptr;
#endif
}

uintptr_t arm64_bl_target(uintptr_t call_addr, uint32_t instruction) {
    int64_t immediate = static_cast<int64_t>(instruction & 0x03ffffffu);
    if ((immediate & 0x02000000LL) != 0) immediate |= ~0x03ffffffLL;
    return static_cast<uintptr_t>(static_cast<int64_t>(call_addr) + (immediate << 2));
}

uint64_t virtual_bag_event(uint64_t event, uint64_t param, uint64_t param2) {
    if (p5_panel_observation_enabled()) {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        log_p5_observation_locked("panel-entry", nullptr, event, param2 != 0, param != 0,
                                  nullptr, false, 0);
    }
    const bool extension_enabled = g_virtual_bag_enabled.load();
    const uint32_t gamestate = g_gamestate != nullptr ? *reinterpret_cast<uint32_t*>(g_gamestate) : 0;
    if (!extension_enabled || gamestate != 0) {
        if (event == 0x17 || event == 0x18 || event == 0x19) {
            VIRTBAG_LOG("touch bypass event=0x%llx enabled=%d gamestate=%u base=0x%llx hooked=%d",
                        static_cast<unsigned long long>(event), extension_enabled ? 1 : 0,
                        gamestate, static_cast<unsigned long long>(g_base), g_state_entry != nullptr ? 1 : 0);
        }
        const uint64_t result = g_orig_event != nullptr ? g_orig_event(event, param, param2) : 0;
        if (p5_panel_observation_enabled()) {
            std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
            log_p5_observation_locked("panel-bypass-post", nullptr, event, param2 != 0,
                                      param != 0, nullptr, true, result);
        }
        return result;
    }
    int64_t x = 0;
    int64_t y = 0;
    bool exiting_to_original = false;
    if (event == 0x17 && param != 0) {
        x = *reinterpret_cast<const int64_t*>(param);
        y = *reinterpret_cast<const int64_t*>(param + 8);
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        ensure_state_loaded_locked();
        log_exit_trace_locked("event_enter", event, param, param2, x, y);
        const int64_t grid_width = 4 * kGridStep - (kGridStep - kGridCell);
        const int64_t grid_height = 4 * kGridStep - (kGridStep - kGridCell);
        VIRTBAG_LOG("touch bounds event=0x%llx x=%lld y=%lld grid=%lld,%lld %lldx%lld grid_hit=%d tab_origin=%lld,%lld mode=%d selected=%d overlay=%d root=%p root_gen=%llu state_gen=%llu",
                    static_cast<unsigned long long>(event), static_cast<long long>(x), static_cast<long long>(y),
                    static_cast<long long>(kGridX), static_cast<long long>(kGridY),
                    static_cast<long long>(grid_width), static_cast<long long>(grid_height),
                    extension_grid_hit(x, y) ? 1 : 0, static_cast<long long>(kCellX),
                    static_cast<long long>(kCellY), static_cast<int>(g_virtual_bag_state.mode),
                    g_virtual_bag_state.selected, g_module_view_installed ? 1 : 0,
                    g_extension_tab_root, static_cast<unsigned long long>(g_extension_tab_generation),
                    static_cast<unsigned long long>(g_inventory_generation));
        if (g_extension_touch_capture) {
            log_p5_observation_locked("panel-capture-press", nullptr, event, param2 != 0,
                                      param != 0, nullptr, true, 1, x, y);
            return 1;
        }
        if (extension_grid_hit(x, y)) {
            // G-6/G-7 零干预原则：投影常驻时网格触摸完全放行原版控件链
            // （TouchHandle 自带命中/选中动画/详情；操作按钮由 menu_gate 拦截；
            // 拖动 drop 由 SaveItemOnEmpty 门禁拦截）。模块不做任何状态清理——
            // press 时 reset/clear 会破坏 TouchHandle 的按压记录导致点击失效。
        }
        const int projected_slot = grid_slot_index(x, y, kOriginalGridX, kOriginalGridY);
        bool projected_item_press = false;
        if (g_virtual_bag_state.mode == virtual_bag::Mode::kModule &&
            g_module_view_installed && virtual_bag::valid_index(g_module_view_index) &&
            projected_slot >= 0 &&
            projected_slot < g_virtual_bag_state.capacities[g_module_view_index] &&
            g_projected_item_root != nullptr) {
            projected_item_press = valid_child_locked(g_projected_item_root, projected_slot) != nullptr;
        }
        if (g_virtual_bag_state.mode != virtual_bag::Mode::kOriginal &&
            projected_slot >= 0 && !projected_item_press) {
            // The original grid is hidden underneath the module grid layout.
            // Consume it even when no extension drag is active.
            g_extension_drag = {};
            reset_drag_state_locked(nullptr);
            g_extension_touch_capture = true;
            log_p5_observation_locked("panel-hidden-grid-press", nullptr, event, param2 != 0,
                                      param != 0, nullptr, true, 1, x, y);
            return 1;
        }
        const int target_original_bag = original_bag_button_index(x, y);
        if (!g_extension_touch_capture && g_virtual_bag_state.mode == virtual_bag::Mode::kModule &&
            target_original_bag >= 0) {
            restore_module_view_locked();
            virtual_bag::begin_exit_module(&g_virtual_bag_state);
            clear_original_bag_selection_locked();
            g_exit_display_bag = g_original_current_got < kNoOriginalBagSelected ?
                                 g_original_current_got : g_original_current_direct;
            if (bind_original_exit_display_bag_locked(target_original_bag)) {
                g_exit_display_bag = static_cast<uint8_t>(target_original_bag);
            }
            exiting_to_original = true;
            log_exit_trace_locked("original_press_pre_orig", event, param, param2, x, y);
            log_p5_observation_locked("panel-original-pre", nullptr, event, param2 != 0,
                                      param != 0, nullptr, false, 0, x, y);
        }
    }
    if (event == 0x18) {
        if (param != 0) {
            x = *reinterpret_cast<const int64_t*>(param);
            y = *reinterpret_cast<const int64_t*>(param + 8);
        } else if (g_base != 0) {
            const uint8_t* touch_state = reinterpret_cast<const uint8_t*>(g_base + G_TOUCH_STATE_VMA);
            x = *reinterpret_cast<const int64_t*>(touch_state + TOUCH_STATE_RELEASE_X);
            y = *reinterpret_cast<const int64_t*>(touch_state + TOUCH_STATE_RELEASE_Y);
        }
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        ensure_state_loaded_locked();
        log_exit_trace_locked("event_release", event, param, param2, x, y);
        if (g_extension_touch_capture) {
            if (param == 0 && x == 0 && y == 0 && g_extension_drag.active) {
                x = g_extension_drag.current_x;
                y = g_extension_drag.current_y;
            }
            if (param == 0 && x == 0 && y == 0) {
                g_extension_drag = {};
                g_extension_touch_capture = false;
                reset_drag_state_locked(nullptr);
                log_p5_observation_locked("panel-capture-release-empty", nullptr, event,
                                          param2 != 0, param != 0, nullptr, true, 1, x, y);
                return 1;
            }
            constexpr int64_t kTouchSlop = 24;
            const int64_t dx = x - g_extension_drag.press_x;
            const int64_t dy = y - g_extension_drag.press_y;
            const bool is_click = g_extension_drag.active &&
                                  dx >= -kTouchSlop && dx <= kTouchSlop &&
                                  dy >= -kTouchSlop && dy <= kTouchSlop;
            if (is_click) {
                g_virtual_bag_state.inspected = static_cast<int>(g_extension_drag.slot);
                const virtual_bag::Item& item =
                    g_virtual_bag_state.items[g_extension_drag.bag][g_extension_drag.slot];
                VIRTBAG_LOG("extension item click bag=%d slot=%d category=%d count=%d",
                            g_extension_drag.bag, g_extension_drag.slot, item.category,
                            item.count);
                log_exit_trace_locked("extension_item_click", event, param, param2, x, y);
            } else {
                const bool handled = g_extension_drag.active &&
                                     handle_bag_drop_release_locked(x, y);
                (void)handled;
            }
            reset_drag_state_locked(nullptr);
            g_extension_drag = {};
            g_extension_touch_capture = false;
            log_p5_observation_locked("panel-capture-release", nullptr, event, param2 != 0,
                                      param != 0, nullptr, true, 1, x, y);
            return 1;
        }
        if (g_virtual_bag_state.mode == virtual_bag::Mode::kOriginal &&
            handle_bag_drop_release_locked(x, y)) {
            log_p5_observation_locked("panel-original-release-handled", nullptr, event,
                                      param2 != 0, param != 0, nullptr, true, 1, x, y);
            return 1;
        }
        // G-10：tab 点击的坐标兜底已删除——tab 点击由控件回调
        // （extension_tab_button_clicked → queue_extension_tab_click）唯一处理，
        // 消除坐标常量与控件树的双路径分歧。
        // Phase one is deliberately read-only. Do not route release events to
        // the legacy projection/move transaction handlers.
    }
    if (event == 0x19) {
        if (param != 0) {
            x = *reinterpret_cast<const int64_t*>(param);
            y = *reinterpret_cast<const int64_t*>(param + 8);
        } else if (g_base != 0) {
            const uint8_t* touch_state = reinterpret_cast<const uint8_t*>(g_base + G_TOUCH_STATE_VMA);
            x = *reinterpret_cast<const int64_t*>(touch_state + TOUCH_STATE_RELEASE_X);
            y = *reinterpret_cast<const int64_t*>(touch_state + TOUCH_STATE_RELEASE_Y);
        }
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        ensure_state_loaded_locked();
        if (g_extension_touch_capture) {
            // 0x19 is the native MOVE event. Keep the source and capture
            // alive until 0x18 performs click-vs-drop classification.
            if (g_extension_drag.active) {
                g_extension_drag.current_x = x;
                g_extension_drag.current_y = y;
            }
            log_p5_observation_locked("panel-capture-move", nullptr, event, param2 != 0,
                                      param != 0, nullptr, true, 1, x, y);
            return 1;
        }
    }
    if (g_extension_touch_capture && event != 0x17 && event != 0x18 && event != 0x19) {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        g_extension_drag = {};
        g_extension_touch_capture = false;
        reset_drag_state_locked(nullptr);
        log_exit_trace_locked("event_cancel", event, param, param2, x, y);
        log_p5_observation_locked("panel-capture-cancel", nullptr, event, param2 != 0,
                                  param != 0, nullptr, true, 1, x, y);
        return 1;
    }
    if (g_extension_touch_capture) {
        // Consume every event in the captured sequence, not only release and cancel.
        if (p5_panel_observation_enabled()) {
            std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
            log_p5_observation_locked("panel-capture-consume", nullptr, event, param2 != 0,
                                      param != 0, nullptr, true, 1, x, y);
        }
        return 1;
    }
    if (event != 0x17) {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        ensure_state_loaded_locked();
        log_exit_trace_locked("event_enter", event, param, param2);
    }
    if (exiting_to_original) {
        const uint64_t original_result = g_orig_event != nullptr ? g_orig_event(event, param, param2) : 0;
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        ensure_state_loaded_locked();
        log_exit_trace_locked("original_press_post_orig", event, param, param2, x, y);
        log_p5_observation_locked("panel-original-post", nullptr, event, param2 != 0,
                                  param != 0, nullptr, true, original_result, x, y);
        return original_result;
    }
    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        ensure_state_loaded_locked();
        log_exit_trace_locked("delegate_pre_orig", event, param, param2, x, y);
        log_p5_observation_locked("panel-delegate-pre", nullptr, event, param2 != 0,
                                  param != 0, nullptr, false, 0, x, y);
    }
    const uint64_t result = g_orig_event != nullptr ? g_orig_event(event, param, param2) : 0;
    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        ensure_state_loaded_locked();
        log_exit_trace_locked("delegate_post_orig", event, param, param2, x, y);
        log_p5_observation_locked("panel-delegate-post", nullptr, event, param2 != 0,
                                  param != 0, nullptr, true, result, x, y);
        if (g_virtual_bag_state.mode == virtual_bag::Mode::kExitingModule) {
            if (event == 0x18 && !complete_original_exit_locked()) {
                cancel_original_exit_locked();
            }
            return result;
        }
        // 物品格点击也会产生 0x17；原版袋按钮已在上方单独处理。
        // 这里仅处理原版事件确实改写当前袋的兜底退出路径。
        const bool original_bag_changed = g_virtual_bag_state.mode == virtual_bag::Mode::kModule &&
                                          original_bag_locked() != g_original_current_got;
        if (event == 0x17 && g_virtual_bag_state.mode == virtual_bag::Mode::kModule &&
            original_bag_changed) {
            const int selected_original_bag = original_bag_locked();
            restore_module_view_locked();
            set_original_bag_locked(selected_original_bag);
            if (fn_ui_equip_refresh_item_area != nullptr) fn_ui_equip_refresh_item_area();
            virtual_bag::enter_original(&g_virtual_bag_state, selected_original_bag);
            persist_state_locked();
            g_exit_display_bag = kNoOriginalBagSelected;
        } else if (event == 0x17 || event == 0x18) {
            g_virtual_bag_state.original_selected = original_bag_locked();
        }
    }
    return result;
}

bool inject_locked() {
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    if (!g_virtual_bag_enabled.load()) return false;
    if (g_state_entry != nullptr) return true;
    uint8_t* entry = find_inventory_state_entry();
    if (entry == nullptr) return false;
    const uintptr_t expected_event = g_base + fn_resolve("F_SCENE_EVENT_EQUIP_VMA", F_SCENE_EVENT_EQUIP_VMA);
    if (*reinterpret_cast<uintptr_t*>(entry + 0x38) != expected_event) {
        VIRTBAG_LOG("inventory state callbacks differ from expected symbols");
        return false;
    }
    g_orig_event = reinterpret_cast<PopupEventFn>(*reinterpret_cast<uintptr_t*>(entry + 0x38));
    g_orig_f3 = reinterpret_cast<PopupNoArgFn>(*reinterpret_cast<uintptr_t*>(entry + 0x28));
    g_orig_enter = reinterpret_cast<PopupNoArgFn>(*reinterpret_cast<uintptr_t*>(entry + 0x10));

    const uintptr_t equip_draw = g_base + fn_resolve("F_UIEQUIP_DRAW_VMA", F_UIEQUIP_DRAW_VMA);
    const uintptr_t item_draw_call = equip_draw + 0x94;
    constexpr uint32_t kOriginalItemDrawCall = 0x97fffe33;
    constexpr size_t kPageSize = 4096;
    if (g_item_draw_patch_addr == 0) {
        const uintptr_t wrapper = reinterpret_cast<uintptr_t>(&virtual_bag_draw_inven_item_wrapper);
        const int64_t direct_delta = static_cast<int64_t>(wrapper) - static_cast<int64_t>(item_draw_call);
        uintptr_t branch_target = wrapper;
        if ((direct_delta & 0x3) != 0 || direct_delta <= -0x08000000LL || direct_delta >= 0x08000000LL) {
            g_item_draw_thunk = allocate_draw_thunk(item_draw_call, wrapper);
            if (g_item_draw_thunk == nullptr) {
                VIRTBAG_LOG("inventory item draw thunk allocation failed");
                return false;
            }
            branch_target = reinterpret_cast<uintptr_t>(g_item_draw_thunk);
        }
        const int64_t delta = static_cast<int64_t>(branch_target) - static_cast<int64_t>(item_draw_call);
        if ((delta & 0x3) != 0 || delta <= -0x08000000LL || delta >= 0x08000000LL) {
            VIRTBAG_LOG("inventory item draw target out of range");
            return false;
        }
        const uint32_t replacement = 0x94000000u | (static_cast<uint32_t>(delta >> 2) & 0x03ffffffu);
        const uintptr_t page = item_draw_call & ~(static_cast<uintptr_t>(kPageSize) - 1);
        if (mprotect(reinterpret_cast<void*>(page), kPageSize, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
            VIRTBAG_LOG("inventory item draw patch mprotect failed errno=%d", errno);
            return false;
        }
        const uint32_t current = *reinterpret_cast<uint32_t*>(item_draw_call);
        if (current != kOriginalItemDrawCall && current != replacement) {
            VIRTBAG_LOG("inventory item draw patch mismatch got=0x%08x", current);
            return false;
        }
        *reinterpret_cast<uint32_t*>(item_draw_call) = replacement;
        __builtin___clear_cache(reinterpret_cast<char*>(item_draw_call),
                                 reinterpret_cast<char*>(item_draw_call + sizeof(uint32_t)));
        g_item_draw_patch_addr = item_draw_call;
        VIRTBAG_LOG("inventory item draw hook patched replacement=0x%08x", replacement);
    }

    const uintptr_t bag_draw_call = equip_draw + 0x98;
    constexpr uint32_t kOriginalBagDrawCall = 0x97fffee8;
    if (g_bag_draw_patch_addr == 0) {
        const uintptr_t wrapper = reinterpret_cast<uintptr_t>(&virtual_bag_draw_original_bag_wrapper);
        const int64_t direct_delta = static_cast<int64_t>(wrapper) - static_cast<int64_t>(bag_draw_call);
        uintptr_t branch_target = wrapper;
        if ((direct_delta & 0x3) != 0 || direct_delta <= -0x08000000LL || direct_delta >= 0x08000000LL) {
            g_bag_draw_thunk = allocate_draw_thunk(bag_draw_call, wrapper);
            if (g_bag_draw_thunk == nullptr) {
                VIRTBAG_LOG("inventory bag draw thunk allocation failed");
                return false;
            }
            branch_target = reinterpret_cast<uintptr_t>(g_bag_draw_thunk);
        }
        const int64_t delta = static_cast<int64_t>(branch_target) - static_cast<int64_t>(bag_draw_call);
        if ((delta & 0x3) != 0 || delta <= -0x08000000LL || delta >= 0x08000000LL) {
            VIRTBAG_LOG("inventory bag draw target out of range");
            return false;
        }
        const uint32_t replacement = 0x94000000u | (static_cast<uint32_t>(delta >> 2) & 0x03ffffffu);
        const uintptr_t page = bag_draw_call & ~(static_cast<uintptr_t>(kPageSize) - 1);
        if (mprotect(reinterpret_cast<void*>(page), kPageSize, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
            VIRTBAG_LOG("inventory bag draw patch mprotect failed errno=%d", errno);
            return false;
        }
        const uint32_t current = *reinterpret_cast<uint32_t*>(bag_draw_call);
        if (current != kOriginalBagDrawCall && current != replacement) {
            VIRTBAG_LOG("inventory bag draw patch mismatch got=0x%08x", current);
            return false;
        }
        *reinterpret_cast<uint32_t*>(bag_draw_call) = replacement;
        __builtin___clear_cache(reinterpret_cast<char*>(bag_draw_call),
                                reinterpret_cast<char*>(bag_draw_call + sizeof(uint32_t)));
        g_bag_draw_patch_addr = bag_draw_call;
        VIRTBAG_LOG("inventory bag highlight hook patched replacement=0x%08x", replacement);
    }

    const uintptr_t call_addr = g_base + fn_resolve("F_SCENE_DRAW_EQUIP_VMA", F_SCENE_DRAW_EQUIP_VMA) + 0x210;
    constexpr uint32_t kOriginalEndCall = 0x97fd11d2;
    if (g_draw_patch_addr == 0) {
        const uintptr_t wrapper = reinterpret_cast<uintptr_t>(&virtual_bag_draw_end_wrapper);
        const int64_t direct_delta = static_cast<int64_t>(wrapper) - static_cast<int64_t>(call_addr);
        uintptr_t branch_target = wrapper;
        if ((direct_delta & 0x3) != 0 || direct_delta <= -0x08000000LL || direct_delta >= 0x08000000LL) {
            g_draw_thunk = allocate_draw_thunk(call_addr, wrapper);
            if (g_draw_thunk == nullptr) {
                VIRTBAG_LOG("inventory draw wrapper out of BL range and thunk allocation failed call=%p wrapper=%p",
                            reinterpret_cast<void*>(call_addr), reinterpret_cast<void*>(wrapper));
                return false;
            }
            branch_target = reinterpret_cast<uintptr_t>(g_draw_thunk);
        }
        const int64_t delta = static_cast<int64_t>(branch_target) - static_cast<int64_t>(call_addr);
        if ((delta & 0x3) != 0 || delta <= -0x08000000LL || delta >= 0x08000000LL) {
            VIRTBAG_LOG("inventory draw branch target out of BL range call=%p target=%p",
                        reinterpret_cast<void*>(call_addr), reinterpret_cast<void*>(branch_target));
            return false;
        }
        const uint32_t replacement = 0x94000000u | (static_cast<uint32_t>(delta >> 2) & 0x03ffffffu);
        const uintptr_t page = call_addr & ~(static_cast<uintptr_t>(kPageSize) - 1);
        if (mprotect(reinterpret_cast<void*>(page), kPageSize, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
            VIRTBAG_LOG("inventory draw patch mprotect failed errno=%d", errno);
            return false;
        }
        const uint32_t current = *reinterpret_cast<uint32_t*>(call_addr);
        if (current != kOriginalEndCall && current != replacement) {
            VIRTBAG_LOG("inventory draw patch mismatch got=0x%08x", current);
            return false;
        }
        *reinterpret_cast<uint32_t*>(call_addr) = replacement;
        __builtin___clear_cache(reinterpret_cast<char*>(call_addr), reinterpret_cast<char*>(call_addr + sizeof(uint32_t)));
        g_draw_patch_addr = call_addr;
        VIRTBAG_LOG("inventory draw restore hook patched call=%p replacement=0x%08x", reinterpret_cast<void*>(call_addr), replacement);
    }

    if (g_save_inventory_patch_addr == 0) {
        const uintptr_t call_addr =
            g_base + fn_resolve("F_SAVE_SAVE_INVENTORY_CALLSITE_VMA",
                                F_SAVE_SAVE_INVENTORY_CALLSITE_VMA);
        constexpr uint32_t kOriginalSaveCall = 0x97fff987;
        const uintptr_t wrapper = reinterpret_cast<uintptr_t>(&save_inventory_wrapper);
        const int64_t direct_delta = static_cast<int64_t>(wrapper) - static_cast<int64_t>(call_addr);
        uintptr_t branch_target = wrapper;
        if ((direct_delta & 0x3) != 0 || direct_delta <= -0x08000000LL || direct_delta >= 0x08000000LL) {
            g_save_inventory_thunk = allocate_draw_thunk(call_addr, wrapper);
            if (g_save_inventory_thunk == nullptr) {
                VIRTBAG_LOG("save gate thunk allocation failed call=%p wrapper=%p",
                            reinterpret_cast<void*>(call_addr), wrapper);
                return false;
            }
            branch_target = reinterpret_cast<uintptr_t>(g_save_inventory_thunk);
        }
        const int64_t delta = static_cast<int64_t>(branch_target) - static_cast<int64_t>(call_addr);
        if ((delta & 0x3) != 0 || delta <= -0x08000000LL || delta >= 0x08000000LL) {
            VIRTBAG_LOG("save gate branch target out of range call=%p target=%p",
                        reinterpret_cast<void*>(call_addr), branch_target);
            return false;
        }
        const uint32_t replacement = 0x94000000u | (static_cast<uint32_t>(delta >> 2) & 0x03ffffffu);
        const uintptr_t page = call_addr & ~(static_cast<uintptr_t>(kPageSize) - 1);
        if (mprotect(reinterpret_cast<void*>(page), kPageSize, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
            VIRTBAG_LOG("save gate patch mprotect failed errno=%d", errno);
            return false;
        }
        const uint32_t current = *reinterpret_cast<uint32_t*>(call_addr);
        if (current != kOriginalSaveCall && current != replacement) {
            VIRTBAG_LOG("save gate patch mismatch got=0x%08x", current);
            return false;
        }
        *reinterpret_cast<uint32_t*>(call_addr) = replacement;
        __builtin___clear_cache(reinterpret_cast<char*>(call_addr),
                                reinterpret_cast<char*>(call_addr + sizeof(uint32_t)));
        g_save_inventory_patch_addr = call_addr;
        VIRTBAG_LOG("save gate hook patched call=%p replacement=0x%08x",
                    reinterpret_cast<void*>(call_addr), replacement);
    }

    if (g_drop_gate_patch_addr == 0) {
        const uintptr_t call_addr = g_base + 0xb8cc0;
        constexpr uint32_t kOriginalDropCall = 0x94012fc8;
        const uintptr_t wrapper = reinterpret_cast<uintptr_t>(&save_item_on_empty_gate);
        const int64_t direct_delta = static_cast<int64_t>(wrapper) - static_cast<int64_t>(call_addr);
        uintptr_t branch_target = wrapper;
        if ((direct_delta & 0x3) != 0 || direct_delta <= -0x08000000LL || direct_delta >= 0x08000000LL) {
            g_drop_gate_thunk = allocate_draw_thunk(call_addr, wrapper);
            if (g_drop_gate_thunk == nullptr) {
                VIRTBAG_LOG("drop gate thunk allocation failed call=%p wrapper=%p",
                            reinterpret_cast<void*>(call_addr), wrapper);
                return false;
            }
            branch_target = reinterpret_cast<uintptr_t>(g_drop_gate_thunk);
        }
        const int64_t delta = static_cast<int64_t>(branch_target) - static_cast<int64_t>(call_addr);
        if ((delta & 0x3) != 0 || delta <= -0x08000000LL || delta >= 0x08000000LL) {
            VIRTBAG_LOG("drop gate branch target out of range call=%p target=%p",
                        reinterpret_cast<void*>(call_addr), branch_target);
            return false;
        }
        const uint32_t replacement = 0x94000000u | (static_cast<uint32_t>(delta >> 2) & 0x03ffffffu);
        const uintptr_t page = call_addr & ~(static_cast<uintptr_t>(kPageSize) - 1);
        if (mprotect(reinterpret_cast<void*>(page), kPageSize, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
            VIRTBAG_LOG("drop gate patch mprotect failed errno=%d", errno);
            return false;
        }
        const uint32_t current = *reinterpret_cast<uint32_t*>(call_addr);
        if (current != kOriginalDropCall && current != replacement) {
            VIRTBAG_LOG("drop gate patch mismatch got=0x%08x", current);
            return false;
        }
        *reinterpret_cast<uint32_t*>(call_addr) = replacement;
        __builtin___clear_cache(reinterpret_cast<char*>(call_addr),
                                reinterpret_cast<char*>(call_addr + sizeof(uint32_t)));
        g_drop_gate_patch_addr = call_addr;
        VIRTBAG_LOG("drop gate hook patched call=%p replacement=0x%08x",
                    reinterpret_cast<void*>(call_addr), replacement);
    }

    if (g_draw_gate_patch_addr == 0) {
        const uintptr_t call_addr = g_base + 0xaaf28;
        constexpr uint32_t kOriginalDrawCall = 0x94016d49;
        const uintptr_t wrapper = reinterpret_cast<uintptr_t>(&item_draw_porting_gate);
        const int64_t direct_delta = static_cast<int64_t>(wrapper) - static_cast<int64_t>(call_addr);
        uintptr_t branch_target = wrapper;
        if ((direct_delta & 0x3) != 0 || direct_delta <= -0x08000000LL || direct_delta >= 0x08000000LL) {
            g_draw_gate_thunk = allocate_draw_thunk(call_addr, wrapper);
            if (g_draw_gate_thunk == nullptr) {
                VIRTBAG_LOG("draw gate thunk allocation failed call=%p wrapper=%p",
                            reinterpret_cast<void*>(call_addr), wrapper);
                return false;
            }
            branch_target = reinterpret_cast<uintptr_t>(g_draw_gate_thunk);
        }
        const int64_t delta = static_cast<int64_t>(branch_target) - static_cast<int64_t>(call_addr);
        if ((delta & 0x3) != 0 || delta <= -0x08000000LL || delta >= 0x08000000LL) {
            VIRTBAG_LOG("draw gate branch target out of range call=%p target=%p",
                        reinterpret_cast<void*>(call_addr), branch_target);
            return false;
        }
        const uint32_t replacement = 0x94000000u | (static_cast<uint32_t>(delta >> 2) & 0x03ffffffu);
        const uintptr_t page = call_addr & ~(static_cast<uintptr_t>(kPageSize) - 1);
        if (mprotect(reinterpret_cast<void*>(page), kPageSize, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
            VIRTBAG_LOG("draw gate patch mprotect failed errno=%d", errno);
            return false;
        }
        const uint32_t current = *reinterpret_cast<uint32_t*>(call_addr);
        if (current != kOriginalDrawCall && current != replacement) {
            VIRTBAG_LOG("draw gate patch mismatch got=0x%08x", current);
            return false;
        }
        *reinterpret_cast<uint32_t*>(call_addr) = replacement;
        __builtin___clear_cache(reinterpret_cast<char*>(call_addr),
                                reinterpret_cast<char*>(call_addr + sizeof(uint32_t)));
        g_draw_gate_patch_addr = call_addr;
        VIRTBAG_LOG("draw gate hook patched call=%p replacement=0x%08x",
                    reinterpret_cast<void*>(call_addr), replacement);
    }

    if (g_desc_open_patch_addr == 0) {
        // InvenItemControlEventProc 事件 0x80（物品 desc 打开）调 MakeDesc 的
        // 唯一 bl：包装后在 desc 菜单上装装备按钮 hook（Path A 装备溢出）。
        const uintptr_t call_addr =
            g_base + fn_resolve("F_UIEQUIP_ITEM_DESC_MAKE_DESC_CALL_VMA",
                                F_UIEQUIP_ITEM_DESC_MAKE_DESC_CALL_VMA);
        constexpr uint32_t kOriginalDescOpenCall = 0x97fffdfe;  // bl 0xb8980（imm26=-0x202，真机核对）
        const uintptr_t wrapper = reinterpret_cast<uintptr_t>(&make_desc_equip_gate);
        const int64_t direct_delta = static_cast<int64_t>(wrapper) - static_cast<int64_t>(call_addr);
        uintptr_t branch_target = wrapper;
        if ((direct_delta & 0x3) != 0 || direct_delta <= -0x08000000LL || direct_delta >= 0x08000000LL) {
            g_desc_open_thunk = allocate_draw_thunk(call_addr, wrapper);
            if (g_desc_open_thunk == nullptr) {
                VIRTBAG_LOG("desc open gate thunk allocation failed call=%p wrapper=%p",
                            reinterpret_cast<void*>(call_addr), wrapper);
                return false;
            }
            branch_target = reinterpret_cast<uintptr_t>(g_desc_open_thunk);
        }
        const int64_t delta = static_cast<int64_t>(branch_target) - static_cast<int64_t>(call_addr);
        if ((delta & 0x3) != 0 || delta <= -0x08000000LL || delta >= 0x08000000LL) {
            VIRTBAG_LOG("desc open gate branch target out of range call=%p target=%p",
                        reinterpret_cast<void*>(call_addr), branch_target);
            return false;
        }
        const uint32_t replacement = 0x94000000u | (static_cast<uint32_t>(delta >> 2) & 0x03ffffffu);
        const uintptr_t page = call_addr & ~(static_cast<uintptr_t>(kPageSize) - 1);
        if (mprotect(reinterpret_cast<void*>(page), kPageSize, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
            VIRTBAG_LOG("desc open gate patch mprotect failed errno=%d", errno);
            return false;
        }
        const uint32_t current = *reinterpret_cast<uint32_t*>(call_addr);
        if (current != kOriginalDescOpenCall && current != replacement) {
            VIRTBAG_LOG("desc open gate patch mismatch got=0x%08x", current);
            return false;
        }
        *reinterpret_cast<uint32_t*>(call_addr) = replacement;
        __builtin___clear_cache(reinterpret_cast<char*>(call_addr),
                                reinterpret_cast<char*>(call_addr + sizeof(uint32_t)));
        g_desc_open_patch_addr = call_addr;
        VIRTBAG_LOG("desc open gate hook patched call=%p replacement=0x%08x",
                    reinterpret_cast<void*>(call_addr), replacement);
    }
    *reinterpret_cast<uintptr_t*>(entry + 0x28) = reinterpret_cast<uintptr_t>(&virtual_bag_f3_wrapper);
    *reinterpret_cast<uintptr_t*>(entry + 0x38) = reinterpret_cast<uintptr_t>(&virtual_bag_event);
    *reinterpret_cast<uintptr_t*>(entry + 0x10) = reinterpret_cast<uintptr_t>(&virtual_bag_inventory_enter_wrapper);
    uint8_t* save_entry = find_popup_state_entry(
        g_base + fn_resolve("F_PANEL_SAVE_SLOT_ENTER", F_PANEL_SAVE_SLOT_ENTER));
    if (save_entry != nullptr && save_entry != entry) {
        g_save_state_entry = save_entry;
        g_orig_save_enter = reinterpret_cast<PopupNoArgFn>(*reinterpret_cast<uintptr_t*>(save_entry + 0x10));
        *reinterpret_cast<uintptr_t*>(save_entry + 0x10) =
            reinterpret_cast<uintptr_t>(&virtual_bag_save_panel_enter_wrapper);
    }
    g_state_entry = entry;
    VIRTBAG_LOG("inventory event callback wrapped; original renderer is reused for module views");
    return true;
}

void ensure_inject_thread() {
    if (g_inject_thread_started.exchange(true)) return;
    std::thread([]() {
        while (g_state_entry == nullptr) {
            inject_locked();
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }
    }).detach();
}

void ensure_lifecycle_thread() {
    if (g_lifecycle_thread_started.exchange(true)) return;
    std::thread([]() {
        int last_state = -1;
        for (;;) {
            if (g_state != nullptr) {
                const int state = static_cast<int>(*reinterpret_cast<uint16_t*>(g_state));
                if (last_state == 5 && state != 5) {
                    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
                    prepare_main_menu_locked();
                }
                last_state = state;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
        }
    }).detach();
}

void prepare_main_menu_locked() {
    if (g_module_view_installed) restore_module_view_locked();
    clear_module_cache_locked();
    g_virtual_bag_state = {};
    g_loaded_slot = -2;
    g_item_state_dirty = false;
    g_inventory_frame_active = false;
    disable_extension_tab_buttons_locked();
    g_extension_tab_buttons.fill(nullptr);
}

}  // namespace

uint64_t virtual_bag_observe_item_proc_pre(void* control, uint64_t event, void* x2, void* param) {
    if (g_p5_observation_active || !p5_observation_enabled() ||
        !p5_should_begin_item_observation(event)) {
        return 0;
    }
    g_p5_observation_active = true;
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    const uint64_t observation_token =
        log_p5_observation_locked("item-pre", control, event, x2 != nullptr, param != nullptr,
                                  nullptr, false, 0);
    if (observation_token == 0) g_p5_observation_active = false;
    return observation_token;
}

void virtual_bag_observe_item_proc_source(uint64_t observation_token, void* control, uint64_t event,
                                          void* x2, void* param, void* source_control) {
    if (observation_token == 0) return;
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    log_p5_observation_locked("item-source", control, event, x2 != nullptr, param != nullptr,
                              source_control, false, 0, -1, -1, observation_token);
}

void virtual_bag_observe_item_proc_post(uint64_t observation_token, void* control, uint64_t event,
                                        void* x2, void* param, uint64_t result) {
    if (observation_token == 0) return;
    (void)control;
    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        // The original proc may have rebuilt the control tree. Do not dereference its prior control.
        log_p5_observation_locked("item-post", nullptr, event, x2 != nullptr, param != nullptr,
                                  nullptr, true, result, -1, -1, observation_token);
    }
    g_p5_observation_active = false;
}

bool virtual_bag_is_inventory_enter(uintptr_t enter) {
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    return g_state_entry != nullptr &&
           enter == *reinterpret_cast<uintptr_t*>(g_state_entry + 0x10);
}

int virtual_bag_inventory_state_id() {
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    if (g_state_entry == nullptr) return -1;
    return *reinterpret_cast<int32_t*>(g_state_entry);
}

// G-8：item proc 0x02（drop 到扩展格控件）路由——src=模块拖动状态（press 时
// projection_slot_at 记录），dst=落点控件索引。成功返回 true（wrapper 吞原版事件）。
bool virtual_bag_projection_drop_to_slot(void* dst_control, void* src_control) {
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    if (!g_module_view_installed || dst_control == nullptr ||
        src_control == nullptr || fn_ui_equip_get_item_slot_index == nullptr ||
        fn_control_object_get_data == nullptr) {
        return false;
    }
    void* src_data = fn_control_object_get_data(src_control);
    void* item = src_data != nullptr ? *reinterpret_cast<void**>(src_data) : nullptr;
    int src_bag = -1, src_slot = -1;
    if (item == nullptr || !module_slot_of_item_locked(item, &src_bag, &src_slot)) {
        return false;
    }
    const int dst_slot = fn_ui_equip_get_item_slot_index(dst_control);
    if (dst_slot < 0 || dst_slot >= g_virtual_bag_state.capacities[src_bag]) {
        return false;
    }
    const bool routed = move_extension_to_extension_locked(src_bag, src_slot, src_bag, dst_slot);
    if (routed) {
        persist_state_locked();
        refresh_projection_if_overwritten_locked();
        VIRTBAG_LOG("drop routed ext->ext dst_slot=%d", dst_slot);
    }
    return routed;
}

void virtual_bag_ui_start_auto_inject() {
    ensure_inject_thread();
    ensure_lifecycle_thread();
}

bool virtual_bag_module_view_installed() {
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    return g_module_view_installed;
}

bool virtual_bag_original_item_input_blocked() {
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    const bool blocked = g_virtual_bag_enabled.load() &&
                         (g_virtual_bag_state.mode == virtual_bag::Mode::kModule ||
                          g_virtual_bag_state.mode == virtual_bag::Mode::kExitingModule);
    if (blocked) {
        VIRTBAG_LOG("original item input blocked mode=%d selected=%d",
                    static_cast<int>(g_virtual_bag_state.mode), g_virtual_bag_state.selected);
    }
    return blocked;
}

bool set_virtual_bag_enabled(bool enabled) {
    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        g_virtual_bag_enabled.store(enabled);
        if (!enabled) {
            restore_module_view_locked();
            g_virtual_bag_state.mode = virtual_bag::Mode::kOriginal;
            g_virtual_bag_state.selected = -1;
            g_virtual_bag_state.inspected = -1;
        }
    }
    if (enabled) virtual_bag_ui_start_auto_inject();
    return true;
}

bool virtual_bag_enabled() {
    return g_virtual_bag_enabled.load();
}

void virtual_bag_prepare_save_slot_load() {
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    if (g_module_view_installed) restore_module_view_locked();
    clear_module_cache_locked();
    g_virtual_bag_state = {};
    g_loaded_slot = -2;
    disable_extension_tab_buttons_locked();
    g_extension_tab_buttons.fill(nullptr);
}

void virtual_bag_prepare_main_menu() {
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    prepare_main_menu_locked();
}

bool virtual_bag_save_game() {
    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        ensure_state_loaded_locked();
        if (g_module_view_installed) restore_module_view_locked();
        recover_pending_transaction_locked();
    }
    // fn_save 必须在锁外：内部 SAVE_SaveInventory 门禁 wrapper 需重新获锁做投影恢复，
    // 同线程重入 std::mutex 会自死锁（与 op_ok 锁内刷新同型）。
    g_explicit_save_in_progress = true;
    const int result = fn_save != nullptr ? fn_save() : 0;
    bool state_saved = false;
    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        if (result != 0) {
            state_saved = persist_state_locked(true);
            if (state_saved) {
                g_virtual_bag_state.pending = {};
                g_item_state_dirty = false;
            }
        }
    }
    g_explicit_save_in_progress = false;
    VIRTBAG_LOG("virtual bag save result native=%d sidecar=%d pending=%d",
                result != 0 ? 1 : 0, state_saved ? 1 : 0,
                g_virtual_bag_state.pending.valid ? 1 : 0);
    return state_saved && result != 0;
}

// G-14（方案 C）：原版物品操作后 RefreshItemArea 会以 INVEN 重刷控件，窗口袋投影需重写。
// 投影只占窗口袋；其他袋调用为 no-op。调用方（data_op_*）不持模块锁，此处安全获锁。
bool virtual_bag_sync_projected_slot(int display_bag, int slot) {
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    if (!g_module_view_installed || !virtual_bag::valid_index(g_module_view_index) ||
        g_projected_item_root == nullptr || fn_control_object_get_child == nullptr ||
        fn_control_item_set_item == nullptr) {
        return false;
    }
    if (!virtual_bag::valid_original_transaction_bag(display_bag) ||
        display_bag != original_bag_locked()) {
        return false;
    }
    if (slot < 0 || slot >= g_virtual_bag_state.capacities[g_module_view_index]) {
        return false;
    }
    void* live_root = g_base != 0 ? *reinterpret_cast<void**>(g_base + G_UIEQUIP_PANEL_CTRL_VMA)
                                  : nullptr;
    void* ctrl = valid_child_locked(live_root, slot);
    void* item = g_module_objects[g_module_view_index][slot];
    if (ctrl == nullptr) return false;
    fn_control_item_set_item(ctrl, item);
    return true;
}

bool virtual_bag_sync_projected_item_control(void* control) {
    if (control == nullptr) return false;
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    if (!g_module_view_installed || !virtual_bag::valid_index(g_module_view_index) ||
        g_projected_item_root == nullptr || fn_control_object_get_child == nullptr ||
        fn_control_item_set_item == nullptr) {
        return false;
    }
    const int capacity = g_virtual_bag_state.capacities[g_module_view_index];
    for (int slot = 0; slot < capacity; ++slot) {
        if (fn_control_object_get_child(g_projected_item_root, slot) == control) {
            fn_control_item_set_item(control, g_module_objects[g_module_view_index][slot]);
            return true;
        }
    }
    return false;
}

bool virtual_bag_sync_projected_bag() {
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    refresh_projection_if_overwritten_locked();
    return g_module_view_installed;
}

void virtual_bag_ui_register_bridge(JNIEnv* env, jclass bridge_class) {
    if (env == nullptr || bridge_class == nullptr) return;
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    if (g_virtual_bag_bridge_class != nullptr) env->DeleteGlobalRef(g_virtual_bag_bridge_class);
    g_virtual_bag_bridge_class = static_cast<jclass>(env->NewGlobalRef(bridge_class));
}

std::string data_virtual_bag_ui_status_json() {
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    if (!game_in_world()) {
        return "{\"injected\":false,\"state\":{}}";
    }
    ensure_state_loaded_locked();
    return "{\"injected\":" + std::string(g_state_entry != nullptr ? "true" : "false") +
           ",\"extension_tab_button\":" +
           std::string(g_extension_tab_buttons[0] != nullptr ? "true" : "false") +
           ",\"state\":" + virtual_bag::state_json(g_virtual_bag_state, true) + "}";
}

std::string data_virtual_bag_test_equip(int index, int bag_type) {
    bool updated = false;
    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        ensure_state_loaded_locked();
        updated = virtual_bag::set_test_equipped(&g_virtual_bag_state, index, bag_type);
        if (updated) g_item_state_dirty = true;
    }
    if (!updated) return op_err("bad virtual bag index or bag type");
    return op_ok();
}

std::string data_virtual_bag_test_item(int index, int slot, int category, int count) {
    bool updated = false;
    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        ensure_state_loaded_locked();
        updated = virtual_bag::set_item(&g_virtual_bag_state, index, slot, category, count);
        if (updated) g_item_state_dirty = true;
    }
    if (!updated) return op_err("bad virtual bag item");
    return op_ok();
}

namespace {

const char* extension_recovery_action_name_locked() {
    const virtual_bag::RecoveryAction action =
        virtual_bag::recovery_action(g_virtual_bag_state, g_virtual_bag_state.pending);
    if (action == virtual_bag::RecoveryAction::kComplete) return "complete";
    if (action == virtual_bag::RecoveryAction::kRollback) return "rollback";
    return "none";
}

std::string extension_bag_status_json_locked() {
    return "{\"enabled\":" + std::string(g_virtual_bag_enabled.load() ? "true" : "false") +
           ",\"injected\":" + std::string(g_state_entry != nullptr ? "true" : "false") +
           ",\"extension_tab_button\":" +
           std::string(g_extension_tab_buttons[0] != nullptr ? "true" : "false") +
           ",\"inventory_frame_active\":" +
           std::string(g_inventory_frame_active ? "true" : "false") +
           ",\"recovery_action\":\"" + extension_recovery_action_name_locked() + "\"" +
           ",\"state\":" + virtual_bag::state_json(g_virtual_bag_state, true) + "}";
}

std::string extension_bag_not_ready_error_locked() {
    if (!g_virtual_bag_enabled.load()) return op_err("extension bag disabled");
    return op_err("not in game");
}

bool extension_bag_ready_locked() {
    return g_virtual_bag_enabled.load() && game_in_world();
}

int extension_internal_bag(int logical_bag) {
    return virtual_bag::extension_internal_bag(logical_bag);
}

std::string extension_bag_view_result_json(bool ok, const char* error) {
    if (!ok) return op_err(error);
    return "{\"ok\":true,\"state\":" + extension_bag_status_json_locked() + "}";
}

enum class ExtensionBagUnequipResult {
    kOk,
    kNotEquipped,
    kNotEmpty,
    kNoSpace,
    kPersistFailed,
    kFailed,
};

// P3 袋解除（对齐原版 UIEquip_ButtonUnequipExe desc_type=1 语义）：
// 非空 → kNotEmpty（弹窗 7，b8084）；原版袋 0..4 全满 → kNoSpace（弹窗 6，
// b809c）；成功 = 按袋类型重建 count-1 背包物品入库（源袋优先、显式跳过
// 任务袋 5/ADR-006）→ 清装备态 → 切到接收袋（原版 b804c-b805c 双字节写）。
// 顺序铁律：先预检空袋再建对象（原版同序 b7f94→b7fa8），否则非空路径会把
// 已入库物品滞留 g_inven 造成复制；转移成功后仅 persist 失败需回滚
// （INVEN_RemoveItemDirect 返回值不可信 → 槽位重读确认后再真释放）。
ExtensionBagUnequipResult unequip_extension_bag_locked(int internal_bag) {
    ensure_state_loaded_locked();
    if (g_virtual_bag_state.capacities[internal_bag] == 0) {
        return ExtensionBagUnequipResult::kNotEquipped;
    }
    for (const virtual_bag::Item& item : g_virtual_bag_state.items[internal_bag]) {
        if (item.category > 0 || item.count > 0) {
            return ExtensionBagUnequipResult::kNotEmpty;
        }
    }
    if (fn_create_item == nullptr || fn_inven_save_item_on_empty == nullptr) {
        VIRTBAG_LOG("extension unequip symbols unavailable bag=%d", internal_bag);
        return ExtensionBagUnequipResult::kFailed;
    }
    void* item = fn_create_item(
        static_cast<int32_t>(g_virtual_bag_state.types[internal_bag]), 0, 0, 0);
    if (item == nullptr) {
        VIRTBAG_LOG("extension unequip create item failed bag=%d", internal_bag);
        return ExtensionBagUnequipResult::kFailed;
    }
    // P4.3：全新对象立即入账本；分配失败立即真释放（尚未暴露，安全）。
    uint32_t item_handle = 0;
    if (ownership::allocate(&g_ownership_ledger, &item_handle) != ownership::Outcome::kOk) {
        release_temporary_item_locked(item);
        VIRTBAG_LOG("extension unequip ledger exhausted bag=%d", internal_bag);
        return ExtensionBagUnequipResult::kFailed;
    }
    uint32_t count_flags =
        *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(item) + I_COUNT);
    *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(item) + I_COUNT) =
        stack_codec::write_count(count_flags, 1);

    int preferred = original_bag_locked();
    if (preferred < 0 || preferred > 4) preferred = g_virtual_bag_state.original_selected;
    if (preferred < 0 || preferred > 4) preferred = 0;
    int order[5];
    int order_count = 0;
    order[order_count++] = preferred;
    for (int bag = 0; bag < 5; ++bag) {
        if (bag != preferred) order[order_count++] = bag;
    }
    int receiving_bag = -1;
    for (int i = 0; i < order_count && receiving_bag < 0; ++i) {
        if (fn_inven_save_item_on_empty(item, order[i])) receiving_bag = order[i];
    }
    if (receiving_bag < 0) {
        // 全满：全新对象从未暴露给控件/TouchState，账本终止保管并真释放。
        release_tracked_item_locked(item_handle, item, "unequip no space");
        return ExtensionBagUnequipResult::kNoSpace;
    }

    // 入库成功后 g_inven 拥有对象，模块不得再释放；预检后 unequip_bag 不会
    // 因非空失败。UI 恢复仍由 virtual_bag_draw_end_wrapper 下一帧完成。
    const uint8_t saved_type = g_virtual_bag_state.types[internal_bag];
    virtual_bag::unequip_bag(&g_virtual_bag_state, internal_bag);
    virtual_bag::enter_original(&g_virtual_bag_state, receiving_bag);
    set_original_bag_locked(receiving_bag);
    g_item_state_dirty = true;
    if (persist_state_locked()) {
        // P4.3：persist 成功 = 提交点，此刻才移交原版库存（终态）。
        handover_tracked_item_locked(item_handle, item, "unequip handover");
        VIRTBAG_LOG("extension unequip ok bag=%d type=%d receiving=%d", internal_bag,
                    static_cast<int>(saved_type), receiving_bag);
        return ExtensionBagUnequipResult::kOk;
    }
    bool rolled_back = false;
    if (fn_remove_item_direct != nullptr) {
        for (int slot = 0; slot < virtual_bag::kSlotCount; ++slot) {
            void* slot_item = nullptr;
            if (!inventory_slot_locked(receiving_bag, slot, &slot_item) ||
                slot_item != item) {
                continue;
            }
            fn_remove_item_direct(receiving_bag, slot);
            void* after = item;
            if (inventory_slot_locked(receiving_bag, slot, &after) && after == nullptr) {
                rolled_back = true;
            }
            break;
        }
    }
    if (rolled_back) {
        // P4.3：回滚已确认（脱离 INVEN）→ 账本终止保管 + 真释放。
        release_tracked_item_locked(item_handle, item, "unequip rollback");
        g_virtual_bag_state.types[internal_bag] = saved_type;
        g_virtual_bag_state.isolation_now_ms = isolation_now_ms_locked();
        virtual_bag::normalize(&g_virtual_bag_state);
        persist_state_locked();
        VIRTBAG_LOG("extension unequip persist failed; rolled back bag=%d", internal_bag);
        return ExtensionBagUnequipResult::kPersistFailed;
    }
    // 回滚未确认：对象去向不明，按 handover 记账（inventory-owned 终态，审计可见）。
    handover_tracked_item_locked(item_handle, item, "unequip rollback uncertain");
    VIRTBAG_LOG("extension unequip rollback failed bag=%d receiving=%d", internal_bag,
                receiving_bag);
    return ExtensionBagUnequipResult::kFailed;
}

void show_extension_bag_not_empty_popup() {
    if (g_base == 0) return;
    const uintptr_t popup =
        g_base + fn_resolve("F_UI_POPUP_MSG_CREATE_OK_FROM_TEXT_DATA_VMA",
                            F_UI_POPUP_MSG_CREATE_OK_FROM_TEXT_DATA_VMA);
    if (popup == 0) return;
    reinterpret_cast<UiPopupMsgCreateOkFromTextDataFn>(popup)(7, 0, 0, 0);
}

void show_extension_bag_no_space_popup() {
    if (g_base == 0) return;
    const uintptr_t popup =
        g_base + fn_resolve("F_UI_POPUP_MSG_CREATE_OK_FROM_TEXT_DATA_VMA",
                            F_UI_POPUP_MSG_CREATE_OK_FROM_TEXT_DATA_VMA);
    if (popup == 0) return;
    reinterpret_cast<UiPopupMsgCreateOkFromTextDataFn>(popup)(6, 0, 0, 0);
}

// P3 真实装备（原版两装备入口的扩展对应物；拖放路径经扩展标签 drop，
// 装备按钮路径经 desc 按钮 hook 溢出）。事务顺序对齐解除侧的反向操作：
// 校验 → INVEN 定位源槽 → payload 备份 → RemoveItemDirect+重读确认
// （返回值不可信，control-plane v1.7）→ equip_bag 占位（容量由 normalize
// 派生）→ persist；失败回滚（清位 + 按备份 payload 重建入库）。
// P4.3：源物品在移除确认后入账本保管，提交/回滚均经 retire 终止保管
// （触摸窗口外真释放）；回滚重建对象经 tracked load + handover/release。
enum class ExtensionBagEquipResult {
    kOk,
    kReject,         // 前置不满足：非背包物品 / 扩展位已装备 / 不在原版库存
    kFailed,         // 移除或回滚失败（状态可能不一致，强日志）
    kPersistFailed,  // persist 失败且已完整回滚
};

ExtensionBagEquipResult equip_extension_bag_item_locked(int internal_bag, void* item) {
    ensure_state_loaded_locked();
    if (!virtual_bag::valid_index(internal_bag) || item == nullptr) {
        return ExtensionBagEquipResult::kReject;
    }
    if (g_virtual_bag_state.types[internal_bag] != 0) {
        return ExtensionBagEquipResult::kReject;
    }
    if (fn_get_bit == nullptr) return ExtensionBagEquipResult::kFailed;
    const uint16_t flags =
        *reinterpret_cast<uint16_t*>(reinterpret_cast<uint8_t*>(item) + I_TYPE);
    const int category = fn_get_bit(flags, 15, 6);
    if (!virtual_bag::valid_type(category) || category == 0) {
        return ExtensionBagEquipResult::kReject;
    }
    int src_bag = -1;
    int src_slot = -1;
    for (int bag = 0; bag < virtual_bag::kOriginalTransactionBagCount && src_bag < 0; ++bag) {
        for (int slot = 0; slot < virtual_bag::kSlotCount; ++slot) {
            void* current = nullptr;
            if (inventory_slot_locked(bag, slot, &current) && current == item) {
                src_bag = bag;
                src_slot = slot;
                break;
            }
        }
    }
    if (src_bag < 0) return ExtensionBagEquipResult::kReject;
    if (fn_remove_item_direct == nullptr || fn_inven_save_item_on_empty == nullptr ||
        fn_save_save_item == nullptr || fn_save_load_item == nullptr || fn_itempool_free == nullptr) {
        return ExtensionBagEquipResult::kFailed;
    }

    std::array<uint8_t, virtual_bag::kSerializedItemBuffer> payload{};
    int payload_size = 0;
    if (!serialize_item_payload_locked(item, &payload, &payload_size)) {
        VIRTBAG_LOG("extension equip reject reason=payload_serialize_failed src=%d/%d", src_bag,
                    src_slot);
        return ExtensionBagEquipResult::kFailed;
    }

    // P4.3：移除前先分配保管 handle（失败时物品仍在原版库存，无丢失风险）。
    uint32_t custody_handle = 0;
    if (ownership::allocate(&g_ownership_ledger, &custody_handle) != ownership::Outcome::kOk) {
        VIRTBAG_LOG("extension equip ledger exhausted src=%d/%d", src_bag, src_slot);
        return ExtensionBagEquipResult::kFailed;
    }

    fn_remove_item_direct(src_bag, src_slot);
    void* after = item;
    if (inventory_slot_locked(src_bag, src_slot, &after) && after != nullptr) {
        VIRTBAG_LOG("extension equip remove verify failed bag=%d slot=%d", src_bag, src_slot);
        // 物品仍被 INVEN 持有（remove 未生效）→ 不得释放，记 inventory 终态。
        handover_tracked_item_locked(custody_handle, item,
                                     "equip remove verify failed; retained in inventory");
        return ExtensionBagEquipResult::kFailed;
    }
    // 移除已确认：此后任何失败（state 拒绝 / persist 失败）都必须回插物品，
    // 否则真实丢失。equip_bag 前置已查 types==0，此处拒绝仅剩理论可能。
    const bool state_equipped =
        virtual_bag::equip_bag(&g_virtual_bag_state, internal_bag, category);
    if (state_equipped) {
        g_item_state_dirty = true;
        if (persist_state_locked()) {
            // P4.3：装备对象已被状态 payload 取代（原指针无人引用），终止保管；
            // drop 路径处于触摸窗口内 → 隔离区，API 路径窗口外 → 真释放。
            retire_custody_item_locked(custody_handle, item, "equip custody retired");
            VIRTBAG_LOG("extension equip ok bag=%d category=%d src=%d/%d", internal_bag,
                        category, src_bag, src_slot);
            return ExtensionBagEquipResult::kOk;
        }
        g_virtual_bag_state.types[internal_bag] = 0;
        g_virtual_bag_state.isolation_now_ms = isolation_now_ms_locked();
        virtual_bag::normalize(&g_virtual_bag_state);
    } else {
        VIRTBAG_LOG("extension equip state reject bag=%d category=%d", internal_bag, category);
    }
    bool restored = false;
    uint32_t rebuilt_handle = 0;
    void* rebuilt = load_item_payload_tracked_locked(payload.data(), payload_size,
                                                     "equip_rollback", &rebuilt_handle);
    if (rebuilt != nullptr) {
        if (fn_inven_save_item_on_empty(rebuilt, src_bag)) {
            handover_tracked_item_locked(rebuilt_handle, rebuilt, "equip rollback handover");
            restored = true;
        } else {
            release_tracked_item_locked(rebuilt_handle, rebuilt, "equip rollback insert failed");
        }
    }
    retire_custody_item_locked(custody_handle, item, "equip rollback custody retired");
    persist_state_locked();
    VIRTBAG_LOG("extension equip persist failed bag=%d restored=%d", internal_bag,
                restored ? 1 : 0);
    return restored ? ExtensionBagEquipResult::kPersistFailed : ExtensionBagEquipResult::kFailed;
}

// 装备成功后的 UI 收尾（游戏线程、持锁；先例：draw_end/enter 路径的锁内
// UI 调用）：标签物品对象物化 + 控件 data[0] 更新（绘制每帧读 capacities，
// 其余自愈）+ 物品区刷新（清消耗源槽）+ 复位拖动态。音效复用模块袋切换
// 音 0x11——原版按装备位查未注册全局（0x2f3418/0x2f5b60）从简。
void finish_extension_equip_ui_locked(int internal_bag, void* moving_control) {
    refresh_tab_bag_items_locked();
    void* tab = g_extension_tab_buttons[internal_bag];
    if (tab != nullptr && fn_control_object_get_data != nullptr) {
        void* data = fn_control_object_get_data(tab);
        if (data != nullptr) {
            *reinterpret_cast<void**>(data) = g_tab_bag_items[internal_bag];
        }
    }
    if (fn_ui_equip_refresh_item_area != nullptr) fn_ui_equip_refresh_item_area();
    if (moving_control != nullptr) reset_drag_state_locked(moving_control);
    play_extension_switch_sound();
}

bool try_equip_on_extension_tab_drop_locked(int index, void* moving_control) {
    if (!g_virtual_bag_enabled.load() || !game_in_world()) return false;
    if (fn_control_object_get_data == nullptr) return false;
    void* data = fn_control_object_get_data(moving_control);
    void* item = data != nullptr ? *reinterpret_cast<void**>(data) : nullptr;
    if (item == nullptr) return false;
    const ExtensionBagEquipResult result = equip_extension_bag_item_locked(index, item);
    if (result != ExtensionBagEquipResult::kOk) {
        VIRTBAG_LOG("extension tab drop equip result=%d index=%d", static_cast<int>(result),
                    index);
        return false;
    }
    finish_extension_equip_ui_locked(index, moving_control);
    VIRTBAG_LOG("extension tab drop equipped index=%d", index);
    return true;
}

// ---- Path A：装备按钮溢出（原版 ButtonEquipExe 背包分支的扩展对应物）----
// 原版全满判定 = 扫 INVEN_pBagSlot 1..4（b7d58-b7d70）→ 弹窗 6（b7d74）。
// 挂法：BL hook 0xb9188（物品 desc 打开调 MakeDesc）→ 调原函数后 PtrHook
// 装备按钮（panel+0x78，SetDescMenu desc_type=2 装备分支 b8668 创建）的
// execute proc；wrapper 仅在「背包物品 + 原版 1..4 全满 + 扩展有空位」时
// 接管，其余一律透传原 proc（原版行为不变）。
void extension_desc_equip_execute(void* button);

void reset_extension_desc_equip_hook_locked() {
    // 与解除 hook 同款：desc 菜单按钮为游戏所有，可能已销毁，只清模块记录。
    g_extension_desc_equip_hook = {};
    g_extension_desc_equip_item = nullptr;
}

void install_extension_desc_equip_hook_locked(void* item) {
    reset_extension_desc_equip_hook_locked();
    if (g_base == 0 || item == nullptr) return;
    void* button = *reinterpret_cast<void**>(g_base + G_UIEQUIP_PANEL_VMA + 0x78);
    if (button == nullptr) return;
    void* data = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(button) + CO_DATA);
    if (data == nullptr) return;
    void* execute_proc = reinterpret_cast<uint8_t*>(data) + CB_EXECUTE_PROC;
    if (!g_extension_desc_equip_hook.install_typed(execute_proc,
                                                   &extension_desc_equip_execute)) {
        VIRTBAG_LOG("extension desc equip hook install failed");
        return;
    }
    g_extension_desc_equip_item = item;
}

void extension_desc_equip_execute(void* button) {
    const uintptr_t raw = g_base != 0
        ? g_base + fn_resolve("F_UIEQUIP_BUTTON_EQUIP_EXE_VMA", F_UIEQUIP_BUTTON_EQUIP_EXE_VMA)
        : 0;
    bool takeover = false;
    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        void* item = g_extension_desc_equip_item;
        void* hooked_button =
            g_base != 0 ? *reinterpret_cast<void**>(g_base + G_UIEQUIP_PANEL_VMA + 0x78)
                        : nullptr;
        reset_extension_desc_equip_hook_locked();
        const bool current_button = button != nullptr && button == hooked_button;
        if (raw != 0 && current_button && g_virtual_bag_enabled.load() && game_in_world() &&
            item != nullptr && fn_get_bit != nullptr && fn_get_bag_size != nullptr) {
            const uint16_t flags =
                *reinterpret_cast<uint16_t*>(reinterpret_cast<uint8_t*>(item) + I_TYPE);
            const int category = fn_get_bit(flags, 15, 6);
            if (category >= 1 && category <= 4) {
                bool original_free = false;
                for (int bag = 1; bag <= 4; ++bag) {
                    if (fn_get_bag_size(bag) <= 0) {
                        original_free = true;
                        break;
                    }
                }
                int equip_index = -1;
                if (!original_free) {
                    for (int i = 0; i < virtual_bag::kBagCount; ++i) {
                        if (g_virtual_bag_state.types[i] == 0) {
                            equip_index = i;
                            break;
                        }
                    }
                }
                if (equip_index >= 0) {
                    const ExtensionBagEquipResult result =
                        equip_extension_bag_item_locked(equip_index, item);
                    takeover = result == ExtensionBagEquipResult::kOk;
                    if (takeover) {
                        // 对齐原版 ButtonEquipExe 入口行为（b7c34 UIDesc_SetOff
                        // 关详情）；clear_original_desc_locked 为锁内既有模式。
                        clear_original_desc_locked();
                        finish_extension_equip_ui_locked(equip_index, nullptr);
                    } else {
                        VIRTBAG_LOG("extension equip button overflow result=%d index=%d",
                                    static_cast<int>(result), equip_index);
                    }
                }
            }
        }
    }
    if (!takeover && raw != 0) {
        reinterpret_cast<void (*)(void*)>(raw)(button);
    }
}

// BL 门禁（0xb9188）：物品 desc 打开 → 调原 MakeDesc（同步生成含装备按钮的
// 菜单）→ 锁内重装装备按钮 hook。物品从触发控件 data[0] 取（MakeDesc 同源）。
void make_desc_equip_gate(void* ctrl, void* arg) {
    const uintptr_t raw = g_base != 0
        ? g_base + fn_resolve("F_UIEQUIP_MAKE_DESC_VMA", F_UIEQUIP_MAKE_DESC_VMA)
        : 0;
    if (raw == 0 || ctrl == nullptr) return;
    void* item = nullptr;
    if (fn_control_object_get_data != nullptr) {
        void* data = fn_control_object_get_data(ctrl);
        item = data != nullptr ? *reinterpret_cast<void**>(data) : nullptr;
    }
    reinterpret_cast<UiEquipMakeDescFn>(raw)(ctrl, arg);
    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        if (g_virtual_bag_enabled.load() && game_in_world()) {
            install_extension_desc_equip_hook_locked(item);
        } else {
            reset_extension_desc_equip_hook_locked();
        }
    }
}

void extension_desc_unequip_execute(void*) {
    ExtensionBagUnequipResult result = ExtensionBagUnequipResult::kNotEquipped;
    bool show_not_empty = false;
    bool show_no_space = false;
    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        const int internal_bag = g_extension_desc_unequip_bag;
        const bool owns_current_desc =
            virtual_bag::valid_index(internal_bag) &&
            g_virtual_bag_state.mode == virtual_bag::Mode::kModule &&
            g_virtual_bag_state.selected == internal_bag &&
            g_virtual_bag_state.info_bag == internal_bag;
        if (owns_current_desc) {
            result = unequip_extension_bag_locked(internal_bag);
            show_not_empty = result == ExtensionBagUnequipResult::kNotEmpty;
            show_no_space = result == ExtensionBagUnequipResult::kNoSpace;
            VIRTBAG_LOG("extension desc unequip bag=%d result=%d", internal_bag,
                        static_cast<int>(result));
        } else {
            VIRTBAG_LOG("extension desc unequip rejected: stale desc bag=%d", internal_bag);
        }
        reset_extension_desc_unequip_hook_locked();
    }

    // 锁内仅置标志，弹窗统一在解锁后调用（锁内零 UI 不变量）；两条弹窗均为
    // 原版 UIEquip_ButtonUnequipExe 对应分支的精确复刻（非空=7/b8084、
    // 无空位=6/b809c），且不在按钮回调栈内做 UIDesc_SetOff。
    if (show_not_empty) {
        show_extension_bag_not_empty_popup();
        return;
    }
    if (show_no_space) {
        show_extension_bag_no_space_popup();
        return;
    }
    // Successful removal is restored by virtual_bag_draw_end_wrapper on the
    // next game frame. Do not invoke original UI refresh from this callback or
    // from the HTTP/JNI operation path.
}

}  // namespace

std::string data_op_extension_bag_status_json() {
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    if (!game_in_world()) {
        return "{\"enabled\":" + std::string(g_virtual_bag_enabled.load() ? "true" : "false") +
               ",\"injected\":false,\"extension_tab_button\":false," +
               "\"inventory_frame_active\":false,\"recovery_action\":\"none\",\"state\":{}}";
    }
    ensure_state_loaded_locked();
    return extension_bag_status_json_locked();
}

std::string data_op_extension_bag_enter_view(int logical_bag) {
    if (!extension_bag_ready_locked()) return extension_bag_not_ready_error_locked();
    const int internal_bag = extension_internal_bag(logical_bag);
    if (internal_bag < 0) return op_err("bad extension bag (6-10)");
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    ensure_state_loaded_locked();
    // 面板门禁（P3 真机实证）：控件树仅在背包面板打开时有效，面板关闭时
    // enter_view 会投影到 stale 树（点击失效/重建后被 RefreshItemArea 冲掉）。
    if (!g_inventory_frame_active) return op_err("inventory panel not open");
    if (g_virtual_bag_state.mode == virtual_bag::Mode::kModule) {
        return op_err("already in extension view");
    }
    if (g_virtual_bag_state.mode == virtual_bag::Mode::kExitingModule) {
        return op_err("exit in progress");
    }
    handle_extension_tab_click_locked(internal_bag);
    const bool entered = g_virtual_bag_state.mode == virtual_bag::Mode::kModule;
    return extension_bag_view_result_json(entered, "enter extension view failed");
}

std::string data_op_extension_bag_exit_view() {
    if (!extension_bag_ready_locked()) return extension_bag_not_ready_error_locked();
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    ensure_state_loaded_locked();
    if (g_virtual_bag_state.mode == virtual_bag::Mode::kExitingModule) {
        return op_err("exit in progress");
    }
    if (g_virtual_bag_state.mode != virtual_bag::Mode::kModule || g_virtual_bag_state.selected < 0) {
        return op_err("not in extension view");
    }
    handle_extension_tab_click_locked(g_virtual_bag_state.selected);
    const bool exited = g_virtual_bag_state.mode == virtual_bag::Mode::kOriginal;
    return extension_bag_view_result_json(exited, "exit extension view failed");
}

std::string data_op_extension_bag_select_bag(int logical_bag) {
    if (!extension_bag_ready_locked()) return extension_bag_not_ready_error_locked();
    const int internal_bag = extension_internal_bag(logical_bag);
    if (internal_bag < 0) return op_err("bad extension bag (6-10)");
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    ensure_state_loaded_locked();
    if (g_virtual_bag_state.mode != virtual_bag::Mode::kModule) {
        return op_err("not in extension view");
    }
    if (g_virtual_bag_state.selected == internal_bag) return op_err("bag already selected");
    handle_extension_tab_click_locked(internal_bag);
    const bool selected = g_virtual_bag_state.selected == internal_bag;
    return extension_bag_view_result_json(selected, "select bag failed");
}

std::string data_op_extension_bag_click_item(int logical_bag, int slot) {
    if (!extension_bag_ready_locked()) return extension_bag_not_ready_error_locked();
    if (slot < 0 || slot >= virtual_bag::kSlotCount) return op_err("bad slot");
    const int internal_bag = extension_internal_bag(logical_bag);
    if (internal_bag < 0) return op_err("bad extension bag (6-10)");
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    ensure_state_loaded_locked();
    if (g_virtual_bag_state.mode != virtual_bag::Mode::kModule) {
        return op_err("not in extension view");
    }
    if (g_virtual_bag_state.selected != internal_bag) return op_err("bag not selected");
    if (slot >= g_virtual_bag_state.capacities[internal_bag]) {
        return op_err("slot beyond derived capacity");
    }
    const virtual_bag::Item& item = g_virtual_bag_state.items[internal_bag][slot];
    if (item.category <= 0 || item.count <= 0) {
        g_virtual_bag_state.inspected = -1;
        persist_state_locked();
        return "{\"ok\":true,\"item\":null}";
    }
    g_virtual_bag_state.inspected = slot;
    persist_state_locked();
    return "{\"ok\":true,\"item\":{\"bag\":" + std::to_string(logical_bag) +
           ",\"slot\":" + std::to_string(slot) +
           ",\"category\":" + std::to_string(item.category) +
           ",\"count\":" + std::to_string(item.count) +
           ",\"payload\":\"" +
           virtual_bag::base64_encode(item.payload.data(), item.payload_size) + "\"}}";
}

// P3 袋解除（原版语义）：非空拒绝、无空位拒绝、成功 = 按类型重建袋物品
// 入库 + 装备清零 + 切到接收袋。HTTP 路径不弹窗，仅返回错误码。
std::string data_op_extension_bag_unequip(int logical_bag) {
    if (!extension_bag_ready_locked()) return extension_bag_not_ready_error_locked();
    const int internal_bag = extension_internal_bag(logical_bag);
    if (internal_bag < 0) return op_err("bad extension bag (6-10)");
    ExtensionBagUnequipResult result = ExtensionBagUnequipResult::kNotEquipped;
    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        result = unequip_extension_bag_locked(internal_bag);
    }
    if (result == ExtensionBagUnequipResult::kOk) return op_ok();
    if (result == ExtensionBagUnequipResult::kNotEmpty) return op_err("bag not empty");
    if (result == ExtensionBagUnequipResult::kNoSpace) return op_err("no space");
    if (result == ExtensionBagUnequipResult::kPersistFailed) return op_err("persist failed");
    if (result == ExtensionBagUnequipResult::kFailed) return op_err("unequip failed");
    return op_err("bag not equipped");
}

std::string data_op_extension_bag_move_item(int from_bag, int from_slot, int to_bag, int to_slot) {
    if (!extension_bag_ready_locked()) return extension_bag_not_ready_error_locked();
    if (g_module_view_installed) return op_err("extension view open; movement disabled (P2)");
    if (from_bag == virtual_bag::kOriginalTaskBag ||
        to_bag == virtual_bag::kOriginalTaskBag) {
        return op_err("task bag excluded");
    }
    if (from_slot < 0 || from_slot >= virtual_bag::kSlotCount) return op_err("bad slot");
    const bool from_original = virtual_bag::valid_original_transaction_bag(from_bag);
    const bool to_original = virtual_bag::valid_original_transaction_bag(to_bag);
    const int from_internal = extension_internal_bag(from_bag);
    const int to_internal = extension_internal_bag(to_bag);
    const bool from_extension = from_internal >= 0;
    const bool to_extension = to_internal >= 0;
    if (!from_original && !from_extension) return op_err("bad from bag");
    if (!to_original && !to_extension) return op_err("bad to bag");
    if (from_original && to_original) return op_err("use /api/item/inventory/move_item");
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    ensure_state_loaded_locked();
    bool moved = false;
    if (from_original && to_extension) {
        moved = move_original_to_extension_slot_locked(from_bag, from_slot, to_internal);
    } else if (from_extension && to_original) {
        moved = move_extension_to_original_locked(from_internal, from_slot, to_bag);
    } else {
        moved = move_extension_to_extension_locked(from_internal, from_slot, to_internal, to_slot);
    }
    if (!moved) return op_err("move failed");
    return "{\"ok\":true,\"state\":" + extension_bag_status_json_locked() + "}";
}

std::string virtual_bag_inventory_bags_json() {
    if (!g_virtual_bag_enabled.load() || !game_in_world()) return "";
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    ensure_state_loaded_locked();
    std::string out;
    for (int bag = 0; bag < virtual_bag::kBagCount; ++bag) {
        const int capacity = g_virtual_bag_state.capacities[bag];
        int filled = 0;
        std::string items;
        for (int slot = 0; slot < virtual_bag::kSlotCount; ++slot) {
            const virtual_bag::Item& item = g_virtual_bag_state.items[bag][slot];
            if (item.category <= 0 || item.count <= 0) continue;
            if (filled > 0) items += ",";
            items += "{\"slot\":" + std::to_string(slot) +
                     ",\"category\":" + std::to_string(item.category) +
                     ",\"count\":" + std::to_string(item.count) + "}";
            ++filled;
        }
        out += ",{\"bag\":" + std::to_string(bag + 6) +
               ",\"items\":[" + items + "]" +
               ",\"capacity\":" + std::to_string(capacity) +
               ",\"slot_count\":" + std::to_string(filled) + "}";
    }
    return out;
}
