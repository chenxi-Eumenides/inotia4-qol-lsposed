// 原生 UICHOICE 选择框插件业务层。

#include "plugins/native_choice/feature.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "plugins/native_choice/plugin.h"

namespace qol::features::native_choice {
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

const contracts::QolGameHost* game() {
    return game_host();
}

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
    if (!g_active || !g_pending_title || g_active_spec.title == nullptr || game() == nullptr ||
        game()->extra.choice_call.write_title == nullptr) {
        return;
    }
    (void)game()->extra.choice_call.write_title(g_title.data());
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
    if (!handle_module_choice || game() == nullptr || game()->extra.choice.selected_index == nullptr ||
        game()->extra.choice.text_owns == nullptr ||
        game()->extra.choice_call.write_focus == nullptr ||
        game()->extra.choice_call.close_popup == nullptr) {
        call_original(original, control);
        return;
    }

    int32_t index = -1;
    int32_t owned = 0;
    if (!contracts::succeeded(game()->extra.choice.selected_index(&index)) ||
        !contracts::succeeded(game()->extra.choice.text_owns(
            g_active_spec.items, static_cast<uint32_t>(g_active_spec.count), &owned)) ||
        owned == 0 || index < 0 || index >= spec.count ||
        !contracts::succeeded(game()->extra.choice_call.write_focus(index))) {
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
    if (game()->extra.choice_call.play_sound != nullptr) {
        (void)game()->extra.choice_call.play_sound(2);
    }
    (void)game()->extra.choice_call.close_popup();
    if (spec.on_select != nullptr) spec.on_select(index, spec.user);
}

bool native_choice_open(const contracts::UiChoiceSpec& spec) {
    if (!native_choice_detail::valid_spec(spec)) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_installed || g_active) return false;
    if (game() == nullptr || game()->extra.choice.state_id == nullptr ||
        game()->extra.choice_call.write_items == nullptr ||
        game()->extra.choice_call.write_focus == nullptr ||
        game()->extra.choice_call.push == nullptr) return false;
    int32_t state_id = -1;
    if (!contracts::succeeded(game()->extra.choice.state_id(&state_id)) || state_id < 0) {
        return false;
    }
    for (int i = 0; i < spec.count; ++i) {
        copy_text(g_item_text[static_cast<size_t>(i)].data(), kTextBufferSize, spec.items[i]);
    }
    g_item_ptrs.fill(nullptr);
    for (int i = 0; i < spec.count; ++i) {
        g_item_ptrs[static_cast<size_t>(i)] = g_item_text[i].data();
    }
    if (!contracts::succeeded(game()->extra.choice_call.write_items(
            g_item_ptrs.data(), static_cast<uint32_t>(spec.count))) ||
        !contracts::succeeded(game()->extra.choice_call.write_focus(0))) {
        return false;
    }
    copy_text(g_title.data(), g_title.size(), spec.title);
    g_active_spec = spec;
    g_active_spec.items = g_item_ptrs.data();
    g_active_spec.title = spec.title == nullptr || spec.title[0] == '\0' ? nullptr : g_title.data();
    g_active = true;
    g_pending_title = g_active_spec.title != nullptr;
    const uint64_t generation = ++g_generation;
    if (!contracts::succeeded(game()->extra.choice_call.push())) {
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
    if (close_popup && game() != nullptr && game()->extra.choice_call.close_popup != nullptr) {
        (void)game()->extra.choice_call.close_popup();
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

}  // namespace qol::features::native_choice
