// 原生 UICHOICE 选择框插件业务层。

#include "feature.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "plugin.h"
#include "../../contracts/ui.h"

namespace qol::features::core_ui {
namespace {

constexpr size_t kTextBufferSize = 256;
constexpr size_t kDialogTypeIdBufferSize = 128;

using UiChoiceInitFn = void (*)(void* control);
using ChoiceButtonExecuteFn = void (*)(void* control);

std::mutex g_mutex;
UiChoiceInitFn g_init_original = nullptr;
ChoiceButtonExecuteFn g_button_original = nullptr;
bool g_installed = false;
bool g_active = false;
bool g_open_inflight = false;
uint64_t g_generation = 0;
contracts::UiChoiceSpec g_active_spec{};
std::array<std::array<char, kTextBufferSize>, native_choice_detail::kMaxItems> g_item_text{};
std::array<const char*, native_choice_detail::kMaxItems> g_item_ptrs{};
std::array<char, kTextBufferSize> g_title{};
std::array<char, kDialogTypeIdBufferSize> g_dialog_type_id{};
// Init 之后还需把模块已持有的 items/count(+标题) 重写回游戏态；open 为**所有** spec 置位
// （无标题 spec 同样必须恢复 items），clear/消费后复位，不逐帧重试。
bool g_pending_init = false;
uint64_t g_dialog_sequence = 0;

const contracts::QolChoicePort* choice() { return choice_port(); }

uint64_t clear_active_locked() {
    const uint64_t generation = g_generation;
    g_active = false;
    g_active_spec = {};
    g_pending_init = false;
    ++g_generation;
    return generation;
}

void copy_text(char* destination, size_t destination_size, const char* source) {
    if (destination == nullptr || destination_size == 0) return;
    std::snprintf(destination, destination_size, "%s", source == nullptr ? "" : source);
}

bool valid_dialog_type_id(const char* id) {
    if (id == nullptr || id[0] == '\0') return false;
    size_t length = 0;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(id); *p != 0; ++p) {
        if (++length >= kDialogTypeIdBufferSize) return false;
        const unsigned char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) return false;
    }
    return true;
}

// 轻量 SDK 日志：只为失败分支可观测，不新建公共诊断 API 或日志框架。
// 只记录枚举化原因与必要计数（code/index/count/owned/backup），不记录玩家文本或指针内容。
void log_line(int32_t level, const char* message) {
    const contracts::QolHostSdk* sdk = host_sdk();
    if (sdk == nullptr || sdk->log.write == nullptr || message == nullptr) return;
    const size_t length = std::strlen(message);
    if (length == 0) return;
    (void)sdk->log.write(level, 1, "plugins/core.custom_ui/feature.cpp", message,
                         static_cast<uint32_t>(length));
}

// module owned 但本轮无法完成：只记录原因并保留模块状态，绝不回落原生（区别于 foreign 旁路）。
void log_blocked(const char* reason, int32_t code, int32_t index, int32_t owned, int count,
                 bool backup_ready) {
    char message[192]{};
    std::snprintf(message, sizeof(message),
                  "choice button module blocked reason=%s index=%d count=%d owned=%d code=%d "
                  "backup=%d",
                  reason, index, count, owned, code, backup_ready ? 1 : 0);
    log_line(2, message);
}

// 文本不属于本模块选择框：清本地状态后透传原函数（不写 items/focus、不关原生 popup、不调模块回调）。
void log_foreign_passthrough(bool backup_ready) {
    char message[128]{};
    std::snprintf(message, sizeof(message),
                  "choice button foreign passthrough text_owns=0 backup=%d (native original)",
                  backup_ready ? 1 : 0);
    log_line(2, message);
}

void log_module_select(int32_t index, bool is_close) {
    char message[96]{};
    std::snprintf(message, sizeof(message), "choice button module %s index=%d",
                  is_close ? "close" : "select", index);
    log_line(0, message);
}

void call_original(ChoiceButtonExecuteFn original, void* control) {
    if (original == nullptr) {
        // backup 缺失 → 原 ExecuteProc 不会被调用（触屏将无任何结果）：记一条 warn，
        // 便于区分「PtrSlot backup 断链」与「模块自身拒绝/关闭」。不记录控件指针内容。
        log_line(2, "choice button original=null (button ptr slot backup missing)");
        return;
    }
    original(control);
}

const contracts::UiStateApi* ui_state_api() {
    const contracts::QolHostSdk* sdk = host_sdk();
    if (sdk == nullptr || sdk->capabilities.require == nullptr) return nullptr;
    const void* iface = nullptr;
    const QolResult result = sdk->capabilities.require(
        contracts::kUiStateCapabilityId,
        {contracts::kUiStateVersionMajor, contracts::kUiStateVersionMinor}, &iface);
    return contracts::succeeded(result) && iface != nullptr
               ? static_cast<const contracts::UiStateApi*>(iface)
               : nullptr;
}

struct ChoiceDialogSnapshotCopy {
    uint64_t sequence = 0;
    uint64_t generation = 0;
    std::array<char, kDialogTypeIdBufferSize> type_id{};
    std::array<char, kTextBufferSize> title{};
    bool has_title = false;
    std::array<std::array<char, kTextBufferSize>, native_choice_detail::kMaxItems> labels{};
    std::array<const char*, native_choice_detail::kMaxItems> label_ptrs{};
    uint32_t count = 0;
};

void publish_dialog_snapshot(const ChoiceDialogSnapshotCopy& copy) {
    const contracts::UiStateApi* api = ui_state_api();
    if (api == nullptr || api->publish_custom_dialog == nullptr) return;
    const contracts::UiCustomDialogSnapshot snapshot{
        copy.sequence, copy.generation, copy.type_id.data(),
        copy.has_title ? copy.title.data() : nullptr, copy.label_ptrs.data(), copy.count};
    (void)api->publish_custom_dialog(&snapshot);
}

void publish_dialog_clear(uint64_t generation) {
    const contracts::UiStateApi* api = ui_state_api();
    uint64_t sequence = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        sequence = ++g_dialog_sequence;
    }
    if (api == nullptr || api->clear_custom_dialog == nullptr) return;
    (void)api->clear_custom_dialog(generation, sequence);
}

void copy_active_dialog_locked(uint64_t generation, ChoiceDialogSnapshotCopy* out) {
    if (out == nullptr) return;
    out->sequence = ++g_dialog_sequence;
    out->generation = generation;
    copy_text(out->type_id.data(), out->type_id.size(), g_active_spec.dialog_type_id);
    out->has_title = g_active_spec.title != nullptr;
    if (out->has_title) copy_text(out->title.data(), out->title.size(), g_active_spec.title);
    out->count = static_cast<uint32_t>(g_active_spec.count);
    for (int i = 0; i < g_active_spec.count; ++i) {
        copy_text(out->labels[static_cast<size_t>(i)].data(), kTextBufferSize,
                  g_active_spec.items[i]);
        out->label_ptrs[static_cast<size_t>(i)] = out->labels[static_cast<size_t>(i)].data();
    }
}

}  // namespace

void choice_init_wrapper(void* control) {
    UiChoiceInitFn original = nullptr;
    // 只对**进入本包装前已存在**的 pending module generation 恢复数据；原 Init 执行期间才产生的
    // 新请求不属于本次 Init，由 original 后的同 generation 重查挡掉，避免错写别的选择框。
    contracts::UiChoiceSpec spec{};
    const char* title = nullptr;
    uint64_t generation = 0;
    bool pending = false;
    const contracts::QolChoicePort* port = choice();
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        original = g_init_original;
        if (g_active && g_pending_init && port != nullptr &&
            port->write.write_items != nullptr && port->write.write_focus != nullptr) {
            spec = g_active_spec;
            generation = g_generation;
            // 游戏保存标题指针；地址必须在选择框存活期间保持有效。
            if (spec.title != nullptr) title = g_title.data();
            pending = true;
        }
    }
    // 非 module（无 pending）：仅透传原 Init，零 module 写。
    if (original != nullptr) original(control);
    if (!pending) return;

    // 原版 Scene_Init_POPUP_SC_CHOICE 顺序：先 UIChoice_Init（重置 pItemText[]/count/focus），
    // 再 UIChoice_CreateControl（只消费 items/count/title）。本包装正处于两者之间的窗口：
    // 用模块自有静态缓冲重写 open 时已持有的 items/count 与 focus(0)（无标题 spec 也一样），
    // 使随后的 CreateControl 拿到正确数据。port 调用一律在锁外，缓冲为静态（非栈字符串）。
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_active || !g_pending_init || g_generation != generation) return;
    }

    QolResult items_result = contracts::ok();
    QolResult focus_result = contracts::ok();
    QolResult title_result = contracts::ok();
    if (spec.count > 0) {
        items_result = port->write.write_items(g_item_ptrs.data(),
                                              static_cast<uint32_t>(spec.count));
    }
    if (contracts::succeeded(items_result)) focus_result = port->write.write_focus(0);
    if (title != nullptr && port->write.write_title != nullptr) {
        title_result = port->write.write_title(title);
    }
    const bool reassert_ok = contracts::succeeded(items_result) &&
                             contracts::succeeded(focus_result) &&
                             (title == nullptr || contracts::succeeded(title_result));
    if (!reassert_ok) {
        // 重写失败 → 本次打开不可用：不假报模块 ready，按同 generation 清 active + 发布 clear，
        // 并用现成的 close_popup 结束这次失败打开（不新增回滚事务/调度器）。
        int32_t fail_code = contracts::succeeded(items_result)
                                ? (contracts::succeeded(focus_result) ? title_result.code
                                                                    : focus_result.code)
                                : items_result.code;
        char message[160]{};
        std::snprintf(message, sizeof(message),
                      "choice init reassert failed stage=%s code=%d count=%d backup=%d",
                      contracts::succeeded(items_result)
                          ? (contracts::succeeded(focus_result) ? "title" : "focus")
                          : "items",
                      fail_code, spec.count, port->write.close_popup != nullptr ? 1 : 0);
        log_line(2, message);
        uint64_t cleared_generation = 0;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_active && g_generation == generation) {
                cleared_generation = clear_active_locked();
            }
        }
        if (cleared_generation != 0) publish_dialog_clear(cleared_generation);
        if (port->write.close_popup != nullptr) (void)port->write.close_popup();
        return;
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_active && g_generation == generation) g_pending_init = false;
}

void choice_button_execute(void* control) {
    ChoiceButtonExecuteFn original = nullptr;
    contracts::UiChoiceSpec spec{};
    uint64_t generation = 0;
    bool handle_module_choice = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        original = g_button_original;
        if (g_active) {
            spec = g_active_spec;
            generation = g_generation;
            handle_module_choice = true;
        }
    }
    if (!handle_module_choice) {
        // 无 module active：直接原样透传原函数，不触碰 choice port（读/写/关闭）也不调模块回调。
        call_original(original, control);
        return;
    }

    const contracts::QolChoicePort* port = choice();
    // ownership 读面不可用：不猜 owner、不写/关 Game、不调任何 callback，保留模块状态。
    if (port == nullptr || port->read.text_owns == nullptr) {
        log_blocked("owner_unavailable", kQolOk, -1, 0, spec.count, original != nullptr);
        return;
    }
    int32_t owned = 0;
    const QolResult owns =
        port->read.text_owns(spec.items, static_cast<uint32_t>(spec.count), &owned);
    if (!contracts::succeeded(owns)) {
        log_blocked("owner_read_failed", owns.code, -1, owned, spec.count, original != nullptr);
        return;
    }
    if (owned == 0) {
        // 文本不属于本模块选择框：清本地模块状态/快照后透传原函数；
        // 不写原生 items/focus、不关原生 popup、不调模块回调。
        uint64_t cleared_generation = 0;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_active && g_generation == generation) cleared_generation = clear_active_locked();
        }
        if (cleared_generation != 0) publish_dialog_clear(cleared_generation);
        log_foreign_passthrough(original != nullptr);
        call_original(original, control);
        return;
    }

    // 已确认 module owned：以下全部走模块路径，任何失败都不调用原版
    // UIChoice_ButtonListExe / EVTSYSTEM 剧情结果，只记录原因并保留模块状态供下一次点击。
    if (port->read.selected_index == nullptr || port->write.write_focus == nullptr ||
        port->write.close_popup == nullptr) {
        log_blocked("missing_module_fn", kQolOk, -1, owned, spec.count, original != nullptr);
        return;
    }
    int32_t index = -1;
    const QolResult selected = port->read.selected_index(&index);
    if (!contracts::succeeded(selected)) {
        log_blocked("selected_index", selected.code, index, owned, spec.count, original != nullptr);
        return;
    }
    if (index < 0 || index >= spec.count) {
        log_blocked("index", kQolOk, index, owned, spec.count, original != nullptr);
        return;
    }
    const QolResult focused = port->write.write_focus(index);
    if (!contracts::succeeded(focused)) {
        log_blocked("focus", focused.code, index, owned, spec.count, original != nullptr);
        return;
    }

    const bool is_close = native_choice_detail::is_close_index(index, spec.close_index);
    uint64_t cleared_generation = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_active && g_generation == generation) cleared_generation = clear_active_locked();
    }
    if (cleared_generation != 0) publish_dialog_clear(cleared_generation);
    log_module_select(index, is_close);  // debug：确认该次点击走了模块路径（每次点击至多一行）
    if (port->write.play_sound != nullptr) {
        (void)port->write.play_sound(2);
    }
    (void)port->write.close_popup();
    // 模块 close：只清自身状态 + sound/close，不 on_select、不调用原生原函数。
    if (!is_close && spec.on_select != nullptr) spec.on_select(index, spec.user);
}

bool native_choice_open(const contracts::UiChoiceSpec& spec) {
    if (!native_choice_detail::valid_spec(spec) || !valid_dialog_type_id(spec.dialog_type_id)) {
        return false;
    }
    const contracts::QolChoicePort* port = choice();
    uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_installed || g_active || g_open_inflight || port == nullptr ||
            port->read.state_id == nullptr ||
            port->write.write_items == nullptr || port->write.write_focus == nullptr ||
            port->write.push == nullptr) return false;
        for (int i = 0; i < spec.count; ++i) {
            copy_text(g_item_text[static_cast<size_t>(i)].data(), kTextBufferSize, spec.items[i]);
        }
        g_item_ptrs.fill(nullptr);
        for (int i = 0; i < spec.count; ++i) {
            g_item_ptrs[static_cast<size_t>(i)] = g_item_text[i].data();
        }
        copy_text(g_title.data(), g_title.size(), spec.title);
        copy_text(g_dialog_type_id.data(), g_dialog_type_id.size(), spec.dialog_type_id);
        g_active_spec = spec;
        g_active_spec.items = g_item_ptrs.data();
        g_active_spec.title = spec.title == nullptr || spec.title[0] == '\0' ? nullptr : g_title.data();
        g_active_spec.dialog_type_id = g_dialog_type_id.data();
        g_active = true;
        g_open_inflight = true;
        g_pending_init = true;
        generation = ++g_generation;
    }

    int32_t state_id = -1;
    if (!contracts::succeeded(port->read.state_id(&state_id)) || state_id < 0 ||
        !contracts::succeeded(port->write.write_items(
            g_item_ptrs.data(), static_cast<uint32_t>(spec.count))) ||
        !contracts::succeeded(port->write.write_focus(0))) {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_active && g_generation == generation) clear_active_locked();
        g_open_inflight = false;
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_installed || !g_active || g_generation != generation) {
            g_open_inflight = false;
            return false;
        }
    }
    if (!contracts::succeeded(port->write.push())) {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_active && g_generation == generation) clear_active_locked();
        g_open_inflight = false;
        return false;
    }
    ChoiceDialogSnapshotCopy dialog_copy{};
    bool opened = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        opened = g_active && g_generation == generation;
        if (opened) copy_active_dialog_locked(generation, &dialog_copy);
        g_open_inflight = false;
    }
    if (opened) publish_dialog_snapshot(dialog_copy);
    return opened;
}

bool native_choice_active() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_active;
}

void native_choice_close() {
    bool close_popup = false;
    uint64_t cleared_generation = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        close_popup = g_active;
        if (close_popup) cleared_generation = clear_active_locked();
    }
    if (cleared_generation != 0) publish_dialog_clear(cleared_generation);
    if (close_popup && choice() != nullptr && choice()->write.close_popup != nullptr) {
        (void)choice()->write.close_popup();
    }
}

int32_t on_resource_ready(contracts::PluginContext& ctx, const char* resource_id, void* backup) {
    (void)ctx;
    if (resource_id == nullptr) return 1;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (std::strcmp(resource_id, "hook:UIChoice_Init") == 0) {
        g_init_original = reinterpret_cast<UiChoiceInitFn>(backup);
        return 0;
    }
    if (std::strcmp(resource_id, "ptrslot:g_uichoice_button_list_exe_got") == 0) {
        g_button_original = reinterpret_cast<ChoiceButtonExecuteFn>(backup);
        return 0;
    }
    return 1;
}

void native_choice_set_installed(bool installed) {
    uint64_t cleared_generation = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_installed = installed;
        if (!installed && g_active) cleared_generation = clear_active_locked();
    }
    if (cleared_generation != 0) publish_dialog_clear(cleared_generation);
}

}  // namespace qol::features::core_ui
