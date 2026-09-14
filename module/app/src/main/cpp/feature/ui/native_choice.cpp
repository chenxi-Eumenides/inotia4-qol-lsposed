#include "feature/ui/native_choice.h"

#include "core/native/qol_log.h"
#include "feature/patch/native_inventory_hook.h"
#include "game_access.h"
#include "game_ptr_hook.h"
#include "game_symbols.h"

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <sys/mman.h>
#include <unistd.h>

namespace {

constexpr size_t kTextBufferSize = 256;

using ChoiceButtonExecuteFn = void (*)(void* control);

std::mutex g_mutex;
PtrHook g_button_hook;
UiChoiceInitFn g_init_original = nullptr;
bool g_init_hook_installed = false;
bool g_installed = false;
bool g_active = false;
uint64_t g_generation = 0;
NativeChoiceSpec g_active_spec{};
std::array<std::array<char, kTextBufferSize>, native_choice_detail::kMaxItems> g_item_text{};
std::array<char, kTextBufferSize> g_title{};
bool g_pending_title = false;
bool g_popup_closed = false;

bool choice_symbols_ready() {
    return g_uichoice_itemtext != nullptr && g_uichoice_count != nullptr &&
           g_uichoice_focus != nullptr && g_uichoice_main_text != nullptr &&
           g_uichoice_button_list_exe_got != nullptr && g_uichoice_control_got != nullptr &&
           fn_uichoice_button_list_exe != nullptr && fn_uichoice_init != nullptr &&
           fn_control_object_get_cursor_index != nullptr &&
           fn_ui_set_popup_process_info != nullptr && fn_popupstate_push != nullptr;
}

void clear_active_locked() {
    g_active = false;
    g_active_spec = {};
    g_pending_title = false;
    g_popup_closed = false;
    ++g_generation;
}

void copy_text(char* destination, size_t destination_size, const char* source) {
    if (destination == nullptr || destination_size == 0) return;
    if (source == nullptr) {
        destination[0] = '\0';
        return;
    }
    std::snprintf(destination, destination_size, "%s", source);
}

bool component_text_owned_locked() {
    if (g_uichoice_itemtext == nullptr ||
        !game_memory_accessible(g_uichoice_itemtext, sizeof(char*), 'r')) {
        return false;
    }
    auto** item_text = static_cast<char**>(g_uichoice_itemtext);
    return native_choice_detail::address_in_range(
        reinterpret_cast<uintptr_t>(item_text[0]),
        reinterpret_cast<uintptr_t>(g_item_text.data()), sizeof(g_item_text));
}

ChoiceButtonExecuteFn original_button_locked() {
    return reinterpret_cast<ChoiceButtonExecuteFn>(g_button_hook.orig);
}

void call_original(ChoiceButtonExecuteFn original, void* control) {
    if (original != nullptr) {
        original(control);
    } else {
        QOL_LOG_ERROR(QolDomain::kUi, "native choice: original ExecuteProc unavailable");
    }
}

int choice_state_id_locked() {
    if (g_base == 0) return -1;
    auto* list_slot = reinterpret_cast<void**>(g_base + G_POPUP_STATE_LIST_GOT_VMA);
    if (!game_memory_accessible(list_slot, sizeof(void*), 'r') || *list_slot == nullptr) {
        return -1;
    }
    auto* list = static_cast<uint8_t*>(*list_slot);
    if (!game_memory_accessible(list, POPUP_STATE_COUNT * POPUP_ENTRY_SIZE, 'r')) {
        return -1;
    }
    for (size_t index = 0; index < POPUP_STATE_COUNT; ++index) {
        auto* entry = list + index * POPUP_ENTRY_SIZE;
        const uintptr_t enter = *reinterpret_cast<uintptr_t*>(entry + POPUP_ENTRY_ENTER);
        if (enter == g_base + F_PANEL_CHOICE_ENTER) {
            return static_cast<int>(*reinterpret_cast<uint32_t*>(entry));
        }
    }
    return -1;
}

void choice_init_wrapper(void* control) {
    const UiChoiceInitFn original = g_init_original;
    if (original != nullptr) original(control);

    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_active || !g_pending_title || g_active_spec.title == nullptr) return;
    if (!game_memory_accessible(g_uichoice_main_text, sizeof(char*), 'w')) {
        QOL_LOG_ERROR(QolDomain::kUi, "native choice: main title symbol unavailable");
        g_pending_title = false;
        return;
    }
    *reinterpret_cast<char**>(g_uichoice_main_text) = g_title.data();
    g_pending_title = false;
}

void choice_button_execute(void* control) {
    ChoiceButtonExecuteFn original = nullptr;
    NativeChoiceSpec spec{};
    bool handle_module_choice = false;
    bool ownership_failed = false;
    uint64_t generation = 0;

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        original = original_button_locked();
        if (!g_active) {
            // 非模块选择框必须逐字节透传原版 ExecuteProc。
        } else if (!component_text_owned_locked()) {
            clear_active_locked();
            ownership_failed = true;
        } else {
            spec = g_active_spec;
            handle_module_choice = true;
            generation = g_generation;
        }
    }

    if (!handle_module_choice) {
        if (ownership_failed) {
            QOL_LOG_WARN(QolDomain::kUi,
                         "native choice: active ownership check failed; falling back to original");
        }
        call_original(original, control);
        return;
    }

    if (fn_sound_system_play != nullptr) fn_sound_system_play(2);
    fn_ui_set_popup_process_info(3, 0);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_active && g_generation == generation) g_popup_closed = true;
    }

    if (!game_memory_accessible(g_uichoice_control_got, sizeof(void*), 'r') ||
        !game_memory_accessible(g_uichoice_focus, sizeof(uint8_t), 'w')) {
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            clear_active_locked();
        }
        QOL_LOG_ERROR(QolDomain::kUi, "native choice: control or focus symbol unavailable");
        call_original(original, control);
        return;
    }
    void* choice_control = *reinterpret_cast<void**>(g_uichoice_control_got);
    if (choice_control == nullptr) {
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            clear_active_locked();
        }
        QOL_LOG_ERROR(QolDomain::kUi, "native choice: choice control unavailable");
        call_original(original, control);
        return;
    }

    const int index = fn_control_object_get_cursor_index(choice_control);
    if (index < 0 || index >= spec.count) {
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            clear_active_locked();
        }
        QOL_LOG_ERROR(QolDomain::kUi, "native choice: invalid choice index=%d", index);
        call_original(original, control);
        return;
    }
    *reinterpret_cast<uint8_t*>(g_uichoice_focus) = static_cast<uint8_t>(index);

    if (native_choice_detail::is_close_index(index, spec.close_index)) {
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            clear_active_locked();
        }
        // 关闭项必须走原版完整清理链，而不是只改 popup 状态。
        call_original(original, control);
        return;
    }

    if (spec.on_select != nullptr) spec.on_select(index, spec.user);
}

bool install_button_hook_locked() {
    if (g_button_hook.installed()) return true;
    if (g_uichoice_button_list_exe_got == nullptr || fn_uichoice_button_list_exe == nullptr) {
        return false;
    }
    auto* slot = static_cast<void**>(g_uichoice_button_list_exe_got);
    if (!game_memory_accessible(slot, sizeof(void*), 'r')) return false;
    if (*slot != reinterpret_cast<void*>(fn_uichoice_button_list_exe)) {
        QOL_LOG_ERROR(QolDomain::kUi,
                      "native choice: ExecuteProc slot mismatch slot=%p got=%p expected=%p",
                      slot, *slot, reinterpret_cast<void*>(fn_uichoice_button_list_exe));
        return false;
    }
    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) page_size = 0x1000;
    const uintptr_t page = reinterpret_cast<uintptr_t>(slot) &
                           ~(static_cast<uintptr_t>(page_size) - 1);
    if (mprotect(reinterpret_cast<void*>(page), static_cast<size_t>(page_size),
                 PROT_READ | PROT_WRITE) != 0) {
        QOL_LOG_ERROR(QolDomain::kUi, "native choice: ExecuteProc mprotect failed errno=%d", errno);
        return false;
    }
    return g_button_hook.install_typed(slot, &choice_button_execute);
}

bool install_init_hook_locked() {
    if (g_init_hook_installed) return true;
    const NativeHookFunType hook = native_hook_func();
    if (hook == nullptr || fn_uichoice_init == nullptr) return false;
    void* backup = nullptr;
    const int rc = hook(reinterpret_cast<void*>(fn_uichoice_init),
                        reinterpret_cast<void*>(&choice_init_wrapper), &backup);
    if (rc != 0 || backup == nullptr) {
        QOL_LOG_ERROR(QolDomain::kUi, "native choice: UIChoice_Init hook failed rc=%d", rc);
        return false;
    }
    g_init_original = reinterpret_cast<UiChoiceInitFn>(backup);
    g_init_hook_installed = true;
    return true;
}

}  // namespace

bool native_choice_install_if_ready() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_installed) return true;
    if (!bridge_ready() || g_base == 0 || !choice_symbols_ready()) return false;

    if (!install_init_hook_locked()) return false;
    if (!install_button_hook_locked()) {
        const NativeUnhookFunType unhook = native_unhook_func();
        if (unhook != nullptr && g_init_hook_installed) {
            unhook(reinterpret_cast<void*>(fn_uichoice_init));
        }
        g_init_original = nullptr;
        g_init_hook_installed = false;
        return false;
    }
    g_installed = true;
    QOL_LOG_INFO(QolDomain::kUi, "native choice: hooks installed");
    return true;
}

bool native_choice_open(const NativeChoiceSpec& spec) {
    uint64_t generation = 0;
    int state_id = -1;

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_installed || !native_choice_detail::can_open(g_active, spec)) return false;
        if (!choice_symbols_ready() || choice_state_id_locked() < 0 ||
            !game_memory_accessible(g_uichoice_itemtext,
                                    sizeof(char*) * static_cast<size_t>(spec.count), 'w') ||
            !game_memory_accessible(g_uichoice_count, sizeof(uint8_t), 'w') ||
            !game_memory_accessible(g_uichoice_focus, sizeof(uint8_t), 'w')) {
            return false;
        }

        for (int i = 0; i < spec.count; ++i) {
            copy_text(g_item_text[static_cast<size_t>(i)].data(), kTextBufferSize, spec.items[i]);
        }
        auto** item_text = static_cast<char**>(g_uichoice_itemtext);
        for (int i = 0; i < spec.count; ++i) {
            item_text[i] = g_item_text[static_cast<size_t>(i)].data();
        }
        *reinterpret_cast<uint8_t*>(g_uichoice_count) = static_cast<uint8_t>(spec.count);
        *reinterpret_cast<uint8_t*>(g_uichoice_focus) = 0;

        copy_text(g_title.data(), g_title.size(), spec.title);
        g_active_spec = spec;
        g_active_spec.items = nullptr;
        g_active_spec.title = spec.title == nullptr || spec.title[0] == '\0' ? nullptr : g_title.data();
        g_active = true;
        g_pending_title = g_active_spec.title != nullptr;
        g_popup_closed = false;
        generation = ++g_generation;
        state_id = choice_state_id_locked();
    }

    if (state_id < 0 || fn_popupstate_push(static_cast<uint32_t>(state_id)) == 0) {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_active && g_generation == generation) clear_active_locked();
        QOL_LOG_ERROR(QolDomain::kUi, "native choice: choice push failed state=%d", state_id);
        return false;
    }
    return true;
}

bool native_choice_active() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_active;
}

void native_choice_close() {
    bool was_active = false;
    bool close_popup = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        was_active = g_active;
        close_popup = was_active && !g_popup_closed;
        if (was_active) clear_active_locked();
    }
    if (close_popup && fn_ui_set_popup_process_info != nullptr) {
        fn_ui_set_popup_process_info(3, 0);
    }
}
