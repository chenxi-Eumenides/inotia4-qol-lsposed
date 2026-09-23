// 原生 UICHOICE 选择框插件业务层。

#include "feature.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "plugin.h"

namespace qol::features::core_ui_state {
namespace {

constexpr size_t kTextBufferSize = 256;

using UiChoiceInitFn = void (*)(void* control);
using ChoiceButtonExecuteFn = void (*)(void* control);

std::mutex g_mutex;
UiChoiceInitFn g_init_original = nullptr;
ChoiceButtonExecuteFn g_button_original = nullptr;
bool g_installed = false;
bool g_active = false;
uint64_t g_generation = 0;
contracts::UiChoiceSpec g_active_spec{};
std::array<std::array<char, kTextBufferSize>, native_choice_detail::kMaxItems> g_item_text{};
std::array<const char*, native_choice_detail::kMaxItems> g_item_ptrs{};
std::array<char, kTextBufferSize> g_title{};
bool g_pending_title = false;

const contracts::QolChoicePort* choice() { return choice_port(); }

void clear_active_locked() {
    g_active = false;
    g_active_spec = {};
    g_pending_title = false;
    ++g_generation;
}

void copy_text(char* destination, size_t destination_size, const char* source) {
    if (destination == nullptr || destination_size == 0) return;
    std::snprintf(destination, destination_size, "%s", source == nullptr ? "" : source);
}

void call_original(ChoiceButtonExecuteFn original, void* control) {
    if (original != nullptr) original(control);
}

}  // namespace

void choice_init_wrapper(void* control) {
    UiChoiceInitFn original = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        original = g_init_original;
    }
    if (original != nullptr) original(control);
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_active || !g_pending_title || g_active_spec.title == nullptr || choice() == nullptr ||
        choice()->write.write_title == nullptr) {
        return;
    }
    (void)choice()->write.write_title(g_title.data());
    g_pending_title = false;
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
            g_active_spec.items, static_cast<uint32_t>(g_active_spec.count), &owned)) ||
        owned == 0 || index < 0 || index >= spec.count ||
        !contracts::succeeded(choice()->write.write_focus(index))) {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_active && g_generation == generation) clear_active_locked();
        call_original(original, control);
        return;
    }

    if (native_choice_detail::is_close_index(index, spec.close_index)) {
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_active && g_generation == generation) clear_active_locked();
        }
        call_original(original, control);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_active && g_generation == generation) clear_active_locked();
    }
    if (choice()->write.play_sound != nullptr) {
        (void)choice()->write.play_sound(2);
    }
    (void)choice()->write.close_popup();
    if (spec.on_select != nullptr) spec.on_select(index, spec.user);
}

bool native_choice_open(const contracts::UiChoiceSpec& spec) {
    if (!native_choice_detail::valid_spec(spec)) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_installed || g_active) return false;
    if (choice() == nullptr || choice()->read.state_id == nullptr ||
        choice()->write.write_items == nullptr || choice()->write.write_focus == nullptr ||
        choice()->write.push == nullptr) return false;
    int32_t state_id = -1;
    if (!contracts::succeeded(choice()->read.state_id(&state_id)) || state_id < 0) {
        return false;
    }
    for (int i = 0; i < spec.count; ++i) {
        copy_text(g_item_text[static_cast<size_t>(i)].data(), kTextBufferSize, spec.items[i]);
    }
    g_item_ptrs.fill(nullptr);
    for (int i = 0; i < spec.count; ++i) {
        g_item_ptrs[static_cast<size_t>(i)] = g_item_text[i].data();
    }
    if (!contracts::succeeded(choice()->write.write_items(
            g_item_ptrs.data(), static_cast<uint32_t>(spec.count))) ||
        !contracts::succeeded(choice()->write.write_focus(0))) {
        return false;
    }
    copy_text(g_title.data(), g_title.size(), spec.title);
    g_active_spec = spec;
    g_active_spec.items = g_item_ptrs.data();
    g_active_spec.title = spec.title == nullptr || spec.title[0] == '\0' ? nullptr : g_title.data();
    g_active = true;
    g_pending_title = g_active_spec.title != nullptr;
    const uint64_t generation = ++g_generation;
    if (!contracts::succeeded(choice()->write.push())) {
        if (g_active && g_generation == generation) clear_active_locked();
        return false;
    }
    return true;
}

bool native_choice_active() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_active;
}

void native_choice_close() {
    bool close_popup = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        close_popup = g_active;
        if (close_popup) clear_active_locked();
    }
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
    std::lock_guard<std::mutex> lock(g_mutex);
    g_installed = installed;
    if (!installed) g_active = false;
}

}  // namespace qol::features::core_ui_state
