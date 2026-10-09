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
bool g_pending_title = false;
uint64_t g_dialog_sequence = 0;

const contracts::QolChoicePort* choice() { return choice_port(); }

uint64_t clear_active_locked() {
    const uint64_t generation = g_generation;
    g_active = false;
    g_active_spec = {};
    g_pending_title = false;
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

void call_original(ChoiceButtonExecuteFn original, void* control) {
    if (original != nullptr) original(control);
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
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        original = g_init_original;
    }
    if (original != nullptr) original(control);
    const char* title = nullptr;
    uint64_t generation = 0;
    const contracts::QolChoicePort* port = choice();
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_active || !g_pending_title || g_active_spec.title == nullptr || port == nullptr ||
            port->write.write_title == nullptr) {
            return;
        }
        // 游戏保存标题指针；地址必须在选择框存活期间保持有效。
        title = g_title.data();
        generation = g_generation;
    }
    (void)port->write.write_title(title);
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_active && g_generation == generation) g_pending_title = false;
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
    if (!handle_module_choice || choice() == nullptr || choice()->read.selected_index == nullptr ||
        choice()->read.text_owns == nullptr || choice()->write.write_focus == nullptr ||
        choice()->write.close_popup == nullptr) {
        call_original(original, control);
        return;
    }

    int32_t index = -1;
    int32_t owned = 0;
    if (!contracts::succeeded(choice()->read.selected_index(&index)) ||
        !contracts::succeeded(choice()->read.text_owns(
            spec.items, static_cast<uint32_t>(spec.count), &owned)) ||
        owned == 0 || index < 0 || index >= spec.count ||
        !contracts::succeeded(choice()->write.write_focus(index))) {
        uint64_t cleared_generation = 0;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_active && g_generation == generation) {
                cleared_generation = clear_active_locked();
            }
        }
        if (cleared_generation != 0) publish_dialog_clear(cleared_generation);
        call_original(original, control);
        return;
    }

    if (native_choice_detail::is_close_index(index, spec.close_index)) {
        uint64_t cleared_generation = 0;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_active && g_generation == generation) {
                cleared_generation = clear_active_locked();
            }
        }
        if (cleared_generation != 0) publish_dialog_clear(cleared_generation);
        call_original(original, control);
        return;
    }

    uint64_t cleared_generation = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_active && g_generation == generation) {
            cleared_generation = clear_active_locked();
        }
    }
    if (cleared_generation != 0) publish_dialog_clear(cleared_generation);
    if (choice()->write.play_sound != nullptr) {
        (void)choice()->write.play_sound(2);
    }
    (void)choice()->write.close_popup();
    if (spec.on_select != nullptr) spec.on_select(index, spec.user);
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
        g_pending_title = g_active_spec.title != nullptr;
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
