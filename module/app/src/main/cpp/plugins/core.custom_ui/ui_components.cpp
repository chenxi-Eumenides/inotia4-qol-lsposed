#include "ui_components.h"

#include "../../contracts/ui_theme.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "panels/stack.h"

namespace qol::features::core_ui {
namespace {

enum class ComponentKind : uint8_t {
    kPanel,
    kButton,
    kLabel,
    kToggle,
    kSelect,
    kScrollView,
    kScrollList,
    kHelpPage,
    kNativeToggle,
    kTextButton,
    kNavButton,
};

struct Component;
struct ScrollRow {
    contracts::QolUiHandle native = nullptr;
    Component* list = nullptr;
    uint32_t index = 0;
};

struct Component {
    contracts::QolUiHandle native = nullptr;
    ComponentKind kind = ComponentKind::kPanel;
    UiRect rect{};
    std::string text;
    uint32_t style = 0;
    int32_t value = 0;
    int32_t selected = 0;
    int32_t content_extent = 0;
    int32_t offset = 0;
    int32_t page_size = 0;
    contracts::QolUiClickFn on_click = nullptr;
    contracts::QolUiChangedFn on_changed = nullptr;
    contracts::QolUiScrollFn on_scroll = nullptr;
    void* user = nullptr;
    std::vector<std::string> options;
    Component* parent = nullptr;
    std::vector<std::unique_ptr<Component>> children;
    std::vector<ScrollRow> rows;
    contracts::QolUiListItemDrawFn draw_item = nullptr;
    contracts::QolUiListSelectFn on_select = nullptr;
    std::vector<contracts::UiHelpOption> help_options;
    UiRect left_viewport{}, right_viewport{};
    contracts::QolUiHandle help_list = nullptr;
    QolResult draw_result = contracts::ok();
    bool pending_destroy = false;
    bool native_deleted = false;
    bool enabled = true;
    bool selection_enabled = true;
};

const contracts::QolUiAdapter* g_adapter = nullptr;
std::unordered_map<contracts::QolUiHandle, Component*> g_components;
std::vector<std::unique_ptr<Component>> g_roots;
uint32_t g_callback_depth = 0;

struct CallbackScope {
    CallbackScope() { ++g_callback_depth; }
    ~CallbackScope() { --g_callback_depth; }
};

QolResult destroy(contracts::QolUiHandle handle);
bool native_list_ready();
bool rect_within_int32(UiRect rect);
bool valid_utf8(const char* text);
QolResult require_list(contracts::QolUiHandle handle, Component** out);
void request_host_back();

constexpr const char* kInvalidArgument = "invalid argument";
constexpr const char* kNotReady = "ui primitive not ready";
constexpr const char* kNotFound = "ui component not found";
constexpr const char* kUnsupported = "ui component unsupported";
constexpr int32_t kFrameImageUnit = 0x0f;
constexpr uint16_t kUiAdapterBaseMinor = 13;
constexpr int64_t kNativeTextButtonMinimumWidth = 204;
constexpr int64_t kNativeTextButtonMinimumHeight = 48;

QolResult invalid_argument() {
    return contracts::error(kQolErrInvalidArgument, kInvalidArgument, 16);
}

QolResult not_ready() {
    return contracts::error(kQolErrNotReady, kNotReady, 22);
}

QolResult not_found() {
    return contracts::error(kQolErrNotFound, kNotFound, 20);
}

QolResult unsupported() {
    return contracts::error(kQolErrUnsupported, kUnsupported, 26);
}

Component* find_component(contracts::QolUiHandle handle) {
    const auto it = g_components.find(handle);
    return it == g_components.end() ? nullptr : it->second;
}

bool adapter_ready() {
    return g_adapter != nullptr && g_adapter->major == contracts::kUiContractMajor &&
        g_adapter->minor >= contracts::kUiContractMinor;
}

bool on_main_thread() {
    return adapter_ready() && g_adapter->is_main_thread != nullptr &&
        g_adapter->is_main_thread() != 0;
}

QolResult require_component(contracts::QolUiHandle handle, Component** out) {
    if (handle == nullptr || out == nullptr) return invalid_argument();
    *out = find_component(handle);
    return *out == nullptr ? not_found() : contracts::ok();
}

QolResult create_component(ComponentKind kind, uint32_t native_kind, UiRect rect,
                           contracts::QolUiHandle parent, const char* text, uint32_t style,
                           contracts::QolUiClickFn on_click, void* click_user,
                           contracts::QolUiDrawFn draw_proc, contracts::QolUiHandle* out,
                           Component* borrowed_row_owner = nullptr,
                           bool allow_foreign_native_parent = false) {
    if (out == nullptr) return invalid_argument();
    *out = nullptr;
    if (g_callback_depth != 0) return not_ready();
    if (!adapter_ready() || g_adapter->create_control == nullptr || g_adapter->set_rect == nullptr) {
        return not_ready();
    }
    if ((kind == ComponentKind::kPanel && parent != nullptr) ||
        (kind != ComponentKind::kPanel && parent == nullptr)) {
        return invalid_argument();
    }
    Component* parent_component = parent == nullptr ? nullptr : find_component(parent);
    if (parent_component == nullptr && borrowed_row_owner != nullptr) {
        parent_component = borrowed_row_owner;
    }
    // allow_foreign_native_parent：允许 parent 是未注册进 g_components 的裸原生控件
    // （如 settings 页自建 g_root）。此时 parent_component 保持空，组件挂 g_roots，native
    // 仍以传入 parent 为父；仅 nav 需要该借用语义，其它组件保持 not_found。
    if (parent != nullptr && parent_component == nullptr && !allow_foreign_native_parent) {
        return not_found();
    }
    if (parent_component != nullptr && parent_component->kind == ComponentKind::kScrollList &&
        (borrowed_row_owner == nullptr || kind != ComponentKind::kTextButton)) {
        return unsupported();  // 列表拥有固定 native 子树，不允许追加独立组件。
    }

    contracts::QolUiHandle native = nullptr;
    const QolResult create_result = g_adapter->create_control(native_kind, parent, &native);
    if (!contracts::succeeded(create_result)) return create_result;
    if (native == nullptr) return not_ready();

    std::unique_ptr<Component> component(new Component());
    component->native = native;
    component->kind = kind;
    component->rect = rect;
    component->text = text == nullptr ? "" : text;
    component->style = style;
    component->on_click = on_click;
    component->user = click_user;
    Component* component_ptr = component.get();

    QolResult result = g_adapter->set_rect(native, rect);
    if (!contracts::succeeded(result)) return result;
    if (g_adapter->set_text != nullptr && text != nullptr) {
        result = g_adapter->set_text(native, component->text.c_str());
        if (!contracts::succeeded(result)) return result;
    }
    if (draw_proc != nullptr) {
        if (g_adapter->set_draw_proc == nullptr) return not_ready();
        result = g_adapter->set_draw_proc(native, draw_proc);
        if (!contracts::succeeded(result)) return result;
    }
    if (on_click != nullptr) {
        if (g_adapter->set_click_proc == nullptr) return not_ready();
        result = g_adapter->set_click_proc(native, on_click, click_user);
        if (!contracts::succeeded(result)) return result;
    }

    if (parent_component != nullptr) {
        component->parent = parent_component;
        parent_component->children.push_back(std::move(component));
    } else {
        component->parent = nullptr;
        g_roots.push_back(std::move(component));
    }
    g_components.emplace(native, component_ptr);
    *out = native;
    return contracts::ok();
}

QolResult panel_create(const contracts::UiPanelDesc* desc, contracts::QolUiHandle parent,
                       contracts::QolUiHandle* out) {
    if (desc == nullptr) return invalid_argument();
    return create_component(ComponentKind::kPanel, 0, desc->rect, parent, nullptr, desc->flags,
                            nullptr, nullptr, nullptr, out);
}

QolResult button_create(const contracts::UiButtonDesc* desc, contracts::QolUiHandle parent,
                        contracts::QolUiHandle* out) {
    if (desc == nullptr) return invalid_argument();
    return create_component(ComponentKind::kButton, 1, desc->rect, parent, desc->text, desc->style,
                            desc->on_click, desc->user, nullptr, out);
}

QolResult label_create(const contracts::UiLabelDesc* desc, contracts::QolUiHandle parent,
                       contracts::QolUiHandle* out) {
    if (desc == nullptr) return invalid_argument();
    return create_component(ComponentKind::kLabel, 2, desc->rect, parent, desc->text, desc->color,
                            nullptr, nullptr, nullptr, out);
}

QolResult toggle_create(const contracts::UiToggleDesc* desc, contracts::QolUiHandle parent,
                        contracts::QolUiHandle* out) {
    if (desc == nullptr) return invalid_argument();
    const QolResult result = create_component(ComponentKind::kToggle, 1, desc->rect, parent,
                                              desc->text, desc->style, nullptr, nullptr, nullptr, out);
    if (!contracts::succeeded(result)) return result;
    Component* component = find_component(*out);
    component->value = desc->value;
    component->on_changed = desc->on_changed;
    component->user = desc->user;
    return contracts::ok();
}

QolResult select_create(const contracts::UiSelectDesc* desc, contracts::QolUiHandle parent,
                        contracts::QolUiHandle* out) {
    if (desc == nullptr || (desc->option_count > 0 && desc->options == nullptr) ||
        desc->selected < 0 ||
        (desc->option_count > 0 && static_cast<uint32_t>(desc->selected) >= desc->option_count)) {
        return invalid_argument();
    }
    const char* text = desc->option_count == 0 ? "" : desc->options[desc->selected];
    if (text == nullptr) return invalid_argument();
    const QolResult result = create_component(ComponentKind::kSelect, 1, desc->rect, parent, text,
                                              desc->style, nullptr, nullptr, nullptr, out);
    if (!contracts::succeeded(result)) return result;
    Component* component = find_component(*out);
    component->selected = desc->selected;
    component->on_changed = desc->on_changed;
    component->user = desc->user;
    component->options.reserve(desc->option_count);
    for (uint32_t i = 0; i < desc->option_count; ++i) {
        if (desc->options[i] == nullptr) return invalid_argument();
        component->options.emplace_back(desc->options[i]);
    }
    return contracts::ok();
}

QolResult scroll_view_create(const contracts::UiScrollViewDesc* desc, contracts::QolUiHandle parent,
                             contracts::QolUiHandle* out) {
    if (desc == nullptr || desc->content_extent < 0 || desc->page_size < 0) return invalid_argument();
    const QolResult result = create_component(ComponentKind::kScrollView, 1, desc->viewport, parent,
                                              nullptr, desc->flags, nullptr, nullptr, nullptr, out);
    if (!contracts::succeeded(result)) return result;
    Component* component = find_component(*out);
    component->content_extent = desc->content_extent;
    component->page_size = desc->page_size;
    component->offset = desc->offset;
    component->on_scroll = desc->on_scroll;
    component->user = desc->user;
    return contracts::ok();
}

QolResult set_rect(contracts::QolUiHandle handle, UiRect rect) {
    Component* component = nullptr;
    const QolResult lookup = require_component(handle, &component);
    if (!contracts::succeeded(lookup)) return lookup;
    if (component->kind == ComponentKind::kScrollList ||
        component->kind == ComponentKind::kHelpPage) return unsupported();
    if (!adapter_ready() || g_adapter->set_rect == nullptr) return not_ready();
    const QolResult result = g_adapter->set_rect(component->native, rect);
    if (contracts::succeeded(result)) component->rect = rect;
    return result;
}

QolResult set_text(contracts::QolUiHandle handle, const char* text, ComponentKind expected) {
    if (text == nullptr) return invalid_argument();
    Component* component = nullptr;
    const QolResult lookup = require_component(handle, &component);
    if (!contracts::succeeded(lookup)) return lookup;
    if (component->kind == ComponentKind::kTextButton && expected == ComponentKind::kButton) {
        if (!valid_utf8(text)) return invalid_argument();
        component->text = text;
        return contracts::ok();
    }
    if (component->kind != expected) return unsupported();
    if (!adapter_ready() || g_adapter->set_text == nullptr) return not_ready();
    const QolResult result = g_adapter->set_text(component->native, text);
    if (contracts::succeeded(result)) component->text = text;
    return result;
}

QolResult button_set_text(contracts::QolUiHandle handle, const char* text) {
    return set_text(handle, text, ComponentKind::kButton);
}

QolResult label_set_text(contracts::QolUiHandle handle, const char* text) {
    return set_text(handle, text, ComponentKind::kLabel);
}

QolResult toggle_set_value(contracts::QolUiHandle handle, int32_t value) {
    Component* component = nullptr;
    const QolResult lookup = require_component(handle, &component);
    if (!contracts::succeeded(lookup)) return lookup;
    if (component->kind != ComponentKind::kToggle) return unsupported();
    component->value = value;
    return contracts::ok();
}

QolResult select_set_selected(contracts::QolUiHandle handle, int32_t selected) {
    Component* component = nullptr;
    const QolResult lookup = require_component(handle, &component);
    if (!contracts::succeeded(lookup)) return lookup;
    if (component->kind != ComponentKind::kSelect) return unsupported();
    if (selected < 0 || static_cast<size_t>(selected) >= component->options.size()) {
        return invalid_argument();
    }
    component->selected = selected;
    return contracts::ok();
}

QolResult scroll_set_offset(contracts::QolUiHandle handle, int32_t offset) {
    Component* component = nullptr;
    const QolResult lookup = require_component(handle, &component);
    if (!contracts::succeeded(lookup)) return lookup;
    if (component->kind == ComponentKind::kScrollList) {
        if (!native_list_ready() || component->pending_destroy || component->native_deleted ||
            g_adapter->scroll_set_offset == nullptr) return not_ready();
        return g_adapter->scroll_set_offset(handle, offset);
    }
    if (component->kind != ComponentKind::kScrollView) return unsupported();
    const int32_t max_offset = component->content_extent > component->page_size
        ? component->content_extent - component->page_size : 0;
    if (offset < 0 || offset > max_offset) return invalid_argument();
    component->offset = offset;
    return contracts::ok();
}

bool hit_component(Component* component, int64_t x, int64_t y) {
    if (g_adapter == nullptr || g_adapter->hit_test == nullptr) return false;
    return g_adapter->hit_test(component->native, x, y,
                               UiRect{0, 0, component->rect.w, component->rect.h}) > 0;
}

Component* find_hit(Component* root, int64_t x, int64_t y) {
    for (auto it = root->children.rbegin(); it != root->children.rend(); ++it) {
        Component* child = it->get();
        if (!hit_component(child, x, y)) continue;
        Component* nested = find_hit(child, x, y);
        return nested == nullptr ? child : nested;
    }
    return hit_component(root, x, y) ? root : nullptr;
}

QolResult dispatch_touch(contracts::QolUiHandle root_handle, uint32_t phase, int64_t x, int64_t y) {
    Component* root = nullptr;
    const QolResult lookup = require_component(root_handle, &root);
    if (!contracts::succeeded(lookup)) return lookup;
    if (!adapter_ready() || g_adapter->hit_test == nullptr) return not_ready();
    Component* target = find_hit(root, x, y);
    if (target == nullptr) return contracts::ok();
    if (phase != 1) return contracts::ok();

    switch (target->kind) {
        case ComponentKind::kButton:
            if (target->on_click != nullptr) target->on_click(target->user);
            break;
        case ComponentKind::kToggle:
            target->value = target->value == 0 ? 1 : 0;
            if (target->on_changed != nullptr) target->on_changed(target->user, target->value);
            break;
        case ComponentKind::kSelect:
            if (!target->options.empty()) {
                target->selected = (target->selected + 1) % static_cast<int32_t>(target->options.size());
                if (target->on_changed != nullptr) {
                    target->on_changed(target->user, target->selected);
                }
            }
            break;
        case ComponentKind::kScrollView:
            if (target->on_scroll != nullptr) target->on_scroll(target->user, target->offset);
            break;
        case ComponentKind::kScrollList:
        case ComponentKind::kHelpPage:
        case ComponentKind::kNativeToggle:
            return unsupported();
        case ComponentKind::kTextButton:
        case ComponentKind::kNavButton:
            break;
        case ComponentKind::kPanel:
        case ComponentKind::kLabel:
            break;
    }
    return contracts::ok();
}

void erase_component(Component* component) {
    g_components.erase(component->native);
    for (auto& child : component->children) erase_component(child.get());
}

bool native_list_ready() {
    return g_adapter != nullptr && g_adapter->major == contracts::kUiAdapterMajor &&
        g_adapter->minor >= kUiAdapterBaseMinor && g_adapter->is_main_thread != nullptr &&
        g_adapter->is_main_thread() != 0 && g_adapter->delete_control != nullptr;
}

QolResult delete_owned_lists(Component* component) {
    // 组件拥有的独立 native 按钮（含导航按钮）：destroy 时删除本控件一次。
    if (component->kind == ComponentKind::kTextButton ||
        component->kind == ComponentKind::kNavButton) {
        if (component->native_deleted) return contracts::ok();
        if (!native_list_ready()) return not_ready();
        const QolResult result = g_adapter->delete_control(component->native);
        if (contracts::succeeded(result)) component->native_deleted = true;
        return result;
    }
    if (component->kind == ComponentKind::kScrollList ||
        component->kind == ComponentKind::kHelpPage) {
        if (component->native_deleted) return contracts::ok();
        if (!native_list_ready()) return not_ready();
        // owned page/list 仅删除根一次；Adapter 先撤销绑定，再递归释放所有子控件。
        const QolResult result = g_adapter->delete_control(component->native);
        if (contracts::succeeded(result)) component->native_deleted = true;
        return result;
    }
    for (auto& child : component->children) {
        const QolResult result = delete_owned_lists(child.get());
        if (!contracts::succeeded(result)) return result;
    }
    return contracts::ok();
}

QolResult destroy(contracts::QolUiHandle handle) {
    Component* component = nullptr;
    const QolResult lookup = require_component(handle, &component);
    if (!contracts::succeeded(lookup)) return lookup;
    if (g_callback_depth != 0) {
        component->pending_destroy = true;
        return contracts::ok();
    }
    if (component->kind == ComponentKind::kNativeToggle) {
        if (!native_list_ready()) return not_ready();
        const QolResult native_delete = g_adapter->delete_control(component->native);
        if (!contracts::succeeded(native_delete)) return native_delete;
    }
    const QolResult deleted = delete_owned_lists(component);
    if (!contracts::succeeded(deleted)) return deleted;
    Component* parent = component->parent;
    erase_component(component);
    if (parent == nullptr) {
        const auto it = std::find_if(g_roots.begin(), g_roots.end(),
                                     [component](const std::unique_ptr<Component>& root) {
                                         return root.get() == component;
                                     });
        if (it != g_roots.end()) g_roots.erase(it);
        return contracts::ok();
    }
    const auto it = std::find_if(parent->children.begin(), parent->children.end(),
                                 [component](const std::unique_ptr<Component>& child) {
                                     return child.get() == component;
                                 });
    if (it != parent->children.end()) parent->children.erase(it);
    return contracts::ok();
}

QolResult flush_pending_destroy() {
    std::vector<contracts::QolUiHandle> pending;
    for (const auto& entry : g_components) {
        if (entry.second->pending_destroy) pending.push_back(entry.first);
    }
    for (auto handle : pending) {
        if (find_component(handle) == nullptr) continue;
        const QolResult result = destroy(handle);
        if (!contracts::succeeded(result)) return result;
    }
    return contracts::ok();
}

void scroll_row_execute(void* user) {
    auto* row = static_cast<ScrollRow*>(user);
    if (row == nullptr || row->list->pending_destroy || row->list->native_deleted) return;
    CallbackScope scope;
    Component* list = row->list;
    if (list->selection_enabled) {
        list->selected = static_cast<int32_t>(row->index);
        if (list->on_select != nullptr) list->on_select(list->user, row->index);
    }
}

void native_toggle_execute(void* user) {
    auto* toggle = static_cast<Component*>(user);
    if (toggle == nullptr || toggle->pending_destroy || toggle->native_deleted ||
        !toggle->enabled || toggle->on_changed == nullptr) return;
    CallbackScope scope;
    toggle->on_changed(toggle->user, toggle->value == 0 ? 1 : 0);
}

void text_button_execute(void* user) {
    auto* button = static_cast<Component*>(user);
    if (button == nullptr || button->pending_destroy || button->native_deleted ||
        !button->enabled || button->on_click == nullptr) return;
    CallbackScope scope;
    button->on_click(button->user);
}

// 自适应导航按钮：点击一律延迟请求弹面板栈顶（帧安全点执行，栈顶仅一层即退出本页）；
// on_click 仅作消费者附加通知，在 back 请求之后调用。
void nav_button_execute(void* user) {
    auto* button = static_cast<Component*>(user);
    if (button == nullptr || button->pending_destroy || button->native_deleted ||
        !button->enabled) return;
    CallbackScope scope;
    request_host_back();
    if (button->on_click != nullptr) button->on_click(button->user);
}

bool valid_utf8(const char* text) {
    if (text == nullptr) return false;
    const size_t size = std::strlen(text);
    for (size_t pos = 0; pos < size;) {
        const unsigned char lead = static_cast<unsigned char>(text[pos]);
        const size_t length = lead < 0x80 ? 1 : (lead & 0xe0) == 0xc0 ? 2 :
                              (lead & 0xf0) == 0xe0 ? 3 : (lead & 0xf8) == 0xf0 ? 4 : 0;
        if (length == 0 || pos + length > size || (length == 2 && lead < 0xc2) ||
            (length == 4 && lead > 0xf4)) return false;
        for (size_t i = 1; i < length; ++i) {
            if ((static_cast<unsigned char>(text[pos + i]) & 0xc0) != 0x80) return false;
        }
        if (length == 3 && ((lead == 0xe0 &&
             static_cast<unsigned char>(text[pos + 1]) < 0xa0) ||
            (lead == 0xed && static_cast<unsigned char>(text[pos + 1]) >= 0xa0))) return false;
        if (length == 4 && ((lead == 0xf0 &&
             static_cast<unsigned char>(text[pos + 1]) < 0x90) ||
            (lead == 0xf4 && static_cast<unsigned char>(text[pos + 1]) > 0x8f))) return false;
        pos += length;
    }
    return true;
}

QolResult text_button_create(const contracts::UiButtonDesc* desc,
                             contracts::QolUiHandle parent, contracts::QolUiHandle* out) {
    if (out == nullptr) return invalid_argument();
    *out = nullptr;
    if (desc == nullptr || !valid_utf8(desc->text) || desc->on_click == nullptr ||
        desc->rect.w < kNativeTextButtonMinimumWidth ||
        desc->rect.h < kNativeTextButtonMinimumHeight || !rect_within_int32(desc->rect) ||
        (desc->style != contracts::kUiTextButtonFontDefault &&
         desc->style != contracts::kUiTextButtonFontLarger)) {
        return invalid_argument();
    }
    constexpr size_t kTextButtonAdapterSize =
        offsetof(contracts::QolUiAdapter, draw_native_text_button_label) +
        sizeof(((contracts::QolUiAdapter*)nullptr)->draw_native_text_button_label);
    if (g_adapter == nullptr || g_adapter->minor < contracts::kUiAdapterMinor ||
        g_adapter->size < kTextButtonAdapterSize ||
        g_adapter->draw_native_text_button_background == nullptr ||
        g_adapter->set_button_enabled == nullptr ||
        g_adapter->draw_native_text_button_label == nullptr) return not_ready();
    Component* borrowed_row_owner = nullptr;
    if (find_component(parent) == nullptr) {
        for (const auto& entry : g_components) {
            Component* candidate = entry.second;
            if (candidate->kind != ComponentKind::kScrollList) continue;
            for (const ScrollRow& row : candidate->rows) {
                if (row.native == parent) {
                    borrowed_row_owner = candidate;
                    break;
                }
            }
            if (borrowed_row_owner != nullptr) break;
        }
    }
    const QolResult created = create_component(ComponentKind::kTextButton, 1, desc->rect,
                                                parent, nullptr, desc->style, nullptr, nullptr,
                                                nullptr, out, borrowed_row_owner);
    if (!contracts::succeeded(created)) return created;
    Component* button = find_component(*out);
    if (button == nullptr || g_adapter->set_click_proc == nullptr) {
        if (button != nullptr) {
            const QolResult cleanup = destroy(*out);
            if (!contracts::succeeded(cleanup)) return cleanup;
        }
        *out = nullptr;
        return not_ready();
    }
    button->text = desc->text;
    button->on_click = desc->on_click;
    button->user = desc->user;
    QolResult bound = contracts::ok();
    if (borrowed_row_owner != nullptr) {
        if (g_adapter->button_set_scroll == nullptr) bound = not_ready();
        else bound = g_adapter->button_set_scroll(button->native, borrowed_row_owner->native);
    }
    if (contracts::succeeded(bound)) {
        if (g_adapter->set_click_proc == nullptr) bound = not_ready();
        else bound = g_adapter->set_click_proc(button->native, &text_button_execute, button);
    }
    if (!contracts::succeeded(bound)) {
        const QolResult cleanup = destroy(button->native);
        *out = nullptr;
        return contracts::succeeded(cleanup) ? bound : cleanup;
    }
    return contracts::ok();
}

QolResult text_button_set_enabled(contracts::QolUiHandle handle, uint8_t enabled) {
    if (enabled > 1) return invalid_argument();
    Component* component = nullptr;
    const QolResult lookup = require_component(handle, &component);
    if (!contracts::succeeded(lookup)) return lookup;
    if (component->kind != ComponentKind::kTextButton) return unsupported();
    if (g_adapter->set_button_enabled == nullptr) return not_ready();
    const QolResult result = g_adapter->set_button_enabled(handle, enabled);
    if (!contracts::succeeded(result)) return result;
    component->enabled = enabled != 0;
    return contracts::ok();
}

QolResult text_button_draw(contracts::QolUiHandle handle) {
    if (!on_main_thread()) return not_ready();
    Component* button = nullptr;
    const QolResult lookup = require_component(handle, &button);
    if (!contracts::succeeded(lookup)) return lookup;
    if (button->kind != ComponentKind::kTextButton) return unsupported();
    if (g_adapter->draw_native_text_button_background == nullptr ||
        g_adapter->control_absolute_origin == nullptr ||
        g_adapter->control_pressed == nullptr ||
        g_adapter->draw_native_text_button_label == nullptr) return not_ready();

    int32_t pressed = 0;
    int64_t x = 0;
    int64_t y = 0;
    QolResult result = g_adapter->control_pressed(handle, &pressed);
    if (!contracts::succeeded(result)) return result;
    result = g_adapter->control_absolute_origin(handle, &x, &y);
    if (!contracts::succeeded(result)) return result;
    result = g_adapter->draw_native_text_button_background(
        {x, y, button->rect.w, button->rect.h}, pressed != 0 ? 1 : 0);
    if (!contracts::succeeded(result)) return result;

    const uint32_t color = !button->enabled ? contracts::ui_theme::kTextDisabled
        : pressed != 0 ? contracts::ui_theme::kTextNormal : contracts::ui_theme::kTextGold;
    return g_adapter->draw_native_text_button_label(
        {x, y, button->rect.w, button->rect.h}, button->text.c_str(), color,
        static_cast<int32_t>(button->style));
}

QolResult nav_button_create(const contracts::UiNavButtonDesc* desc,
                            contracts::QolUiHandle parent, contracts::QolUiHandle* out) {
    if (out == nullptr) return invalid_argument();
    *out = nullptr;
    if (desc == nullptr || desc->rect.w <= 0 || desc->rect.h <= 0 ||
        !rect_within_int32(desc->rect) ||
        (desc->mode != contracts::kUiNavButtonAuto &&
         desc->mode != contracts::kUiNavButtonForceClose &&
         desc->mode != contracts::kUiNavButtonForceBack)) {
        return invalid_argument();
    }
    constexpr size_t kNavButtonAdapterSize =
        offsetof(contracts::QolUiAdapter, draw_native_close_button) +
        sizeof(((contracts::QolUiAdapter*)nullptr)->draw_native_close_button);
    if (g_adapter == nullptr || g_adapter->minor < contracts::kUiAdapterMinor ||
        g_adapter->size < kNavButtonAdapterSize ||
        g_adapter->draw_native_close_button == nullptr ||
        g_adapter->draw_back_button == nullptr || g_adapter->set_button_enabled == nullptr) {
        return not_ready();
    }
    // 返回箭头分支画共享 option 图集（unit 0x59），只有 load_option_images 会确保其加载。
    // 与 back_button_create 同规格：借入，任何路径都不卸载。
    if (g_adapter->load_option_images != nullptr) {
        (void)g_adapter->load_option_images();
    }
    // 允许 parent 为裸原生父节点（settings 页自建 g_root）：组件挂 g_roots，native 以 parent 为父。
    const QolResult created = create_component(ComponentKind::kNavButton, 1, desc->rect, parent,
                                               nullptr, desc->mode, nullptr, nullptr, nullptr,
                                               out, nullptr, true);
    if (!contracts::succeeded(created)) return created;
    Component* button = find_component(*out);
    if (button == nullptr || g_adapter->set_click_proc == nullptr) {
        if (button != nullptr) {
            const QolResult cleanup = destroy(*out);
            if (!contracts::succeeded(cleanup)) return cleanup;
        }
        *out = nullptr;
        return not_ready();
    }
    button->on_click = desc->on_click;
    button->user = desc->user;
    const QolResult bound = g_adapter->set_click_proc(button->native, &nav_button_execute,
                                                      button);
    if (!contracts::succeeded(bound)) {
        const QolResult cleanup = destroy(button->native);
        *out = nullptr;
        return contracts::succeeded(cleanup) ? bound : cleanup;
    }
    return contracts::ok();
}

QolResult nav_button_set_enabled(contracts::QolUiHandle handle, uint8_t enabled) {
    if (enabled > 1) return invalid_argument();
    Component* component = nullptr;
    const QolResult lookup = require_component(handle, &component);
    if (!contracts::succeeded(lookup)) return lookup;
    if (component->kind != ComponentKind::kNavButton) return unsupported();
    if (g_adapter->set_button_enabled == nullptr) return not_ready();
    const QolResult result = g_adapter->set_button_enabled(handle, enabled);
    if (!contracts::succeeded(result)) return result;
    component->enabled = enabled != 0;
    return contracts::ok();
}

QolResult nav_button_draw(contracts::QolUiHandle handle) {
    if (!on_main_thread()) return not_ready();
    Component* button = nullptr;
    const QolResult lookup = require_component(handle, &button);
    if (!contracts::succeeded(lookup)) return lookup;
    if (button->kind != ComponentKind::kNavButton) return unsupported();
    if (g_adapter->draw_native_close_button == nullptr || g_adapter->draw_back_button == nullptr ||
        g_adapter->control_pressed == nullptr ||
        g_adapter->control_absolute_origin == nullptr) {
        return not_ready();
    }
    int32_t pressed = 0;
    QolResult result = g_adapter->control_pressed(handle, &pressed);
    if (!contracts::succeeded(result)) return result;
    bool show_back;
    if (button->style == contracts::kUiNavButtonForceBack) show_back = true;
    else if (button->style == contracts::kUiNavButtonForceClose) show_back = false;
    else show_back = ui_panels_depth() > 1;
    if (show_back) {
        // 返回箭头沿用原版绘制路径（unit 0x59 loc 3 常态 / loc 2 按下叠加），按控件位置绘制。
        return g_adapter->draw_back_button(handle, pressed != 0 ? 1 : 0);
    }
    int64_t x = 0;
    int64_t y = 0;
    result = g_adapter->control_absolute_origin(handle, &x, &y);
    if (!contracts::succeeded(result)) return result;
    return g_adapter->draw_native_close_button({x, y, button->rect.w, button->rect.h},
                                               pressed != 0 ? 1 : 0);
}

QolResult scroll_list_get_row(contracts::QolUiHandle handle, uint32_t index,
                              contracts::QolUiHandle* out) {
    if (out == nullptr) return invalid_argument();
    *out = nullptr;
    Component* list = nullptr;
    const QolResult lookup = require_list(handle, &list);
    if (!contracts::succeeded(lookup)) return lookup;
    if (index >= list->rows.size()) return invalid_argument();
    *out = list->rows[index].native;
    return contracts::ok();
}

QolResult scroll_list_set_selection_enabled(contracts::QolUiHandle handle, uint8_t enabled) {
    Component* list = nullptr;
    const QolResult lookup = require_list(handle, &list);
    if (!contracts::succeeded(lookup)) return lookup;
    list->selection_enabled = enabled != 0;
    if (!list->selection_enabled) list->selected = -1;
    return contracts::ok();
}

QolResult scroll_row_button_create(contracts::QolUiHandle handle, uint32_t index, UiRect rect,
                                   contracts::QolUiClickFn on_click, void* user,
                                   contracts::QolUiHandle* out) {
    if (out == nullptr || on_click == nullptr || rect.w <= 0 || rect.h <= 0) {
        return invalid_argument();
    }
    *out = nullptr;
    Component* list = nullptr;
    const QolResult lookup = require_list(handle, &list);
    if (!contracts::succeeded(lookup)) return lookup;
    if (index >= list->rows.size()) return invalid_argument();
    if (g_callback_depth != 0 || g_adapter->create_control == nullptr ||
        g_adapter->set_rect == nullptr || g_adapter->set_click_proc == nullptr ||
        g_adapter->button_set_scroll == nullptr) return not_ready();

    contracts::QolUiHandle native = nullptr;
    QolResult result = g_adapter->create_control(1, list->rows[index].native, &native);
    if (!contracts::succeeded(result)) return result;
    if (native == nullptr) return not_ready();
    result = g_adapter->set_rect(native, rect);
    if (contracts::succeeded(result)) result = g_adapter->set_click_proc(native, on_click, user);
    if (contracts::succeeded(result)) result = g_adapter->button_set_scroll(native, list->native);
    if (!contracts::succeeded(result)) {
        if (g_adapter->delete_control != nullptr) (void)g_adapter->delete_control(native);
        return result;
    }
    *out = native;
    return contracts::ok();
}

QolResult native_toggle_create(const contracts::UiNativeToggleDesc* desc,
                               contracts::QolUiHandle parent,
                               contracts::QolUiHandle* out) {
    if (desc == nullptr || out == nullptr || parent == nullptr || desc->rect.w <= 0 ||
        desc->rect.h <= 0 || desc->on_changed == nullptr) return invalid_argument();
    *out = nullptr;
    if (g_callback_depth != 0 || !native_list_ready() || g_adapter->create_control == nullptr ||
        g_adapter->set_rect == nullptr || g_adapter->set_click_proc == nullptr ||
        g_adapter->control_absolute_origin == nullptr || g_adapter->draw_native_toggle == nullptr ||
        g_adapter->control_pressed == nullptr) return not_ready();

    Component* owner = find_component(parent);
    Component* list_owner = nullptr;
    contracts::QolUiHandle native_parent = parent;
    if (owner != nullptr && owner->kind == ComponentKind::kScrollList) return unsupported();
    if (owner == nullptr) {
        for (const auto& entry : g_components) {
            Component* candidate = entry.second;
            if (candidate->kind != ComponentKind::kScrollList) continue;
            for (const auto& row : candidate->rows) {
                if (row.native == parent) {
                    list_owner = candidate;
                    break;
                }
            }
            if (list_owner != nullptr) break;
        }
        if (list_owner == nullptr) return not_found();
    } else return unsupported();

    contracts::QolUiHandle native = nullptr;
    QolResult result = g_adapter->create_control(1, native_parent, &native);
    if (!contracts::succeeded(result)) return result;
    if (native == nullptr) return not_ready();
    std::unique_ptr<Component> component(new Component());
    component->native = native;
    component->kind = ComponentKind::kNativeToggle;
    component->rect = desc->rect;
    component->value = desc->value != 0 ? 1 : 0;
    component->enabled = desc->enabled != 0;
    component->on_changed = desc->on_changed;
    component->user = desc->user;
    Component* raw = component.get();
    result = g_adapter->set_rect(native, desc->rect);
    if (contracts::succeeded(result)) {
        result = g_adapter->set_click_proc(native, &native_toggle_execute, raw);
    }
    if (contracts::succeeded(result) && list_owner->kind == ComponentKind::kScrollList) {
        if (g_adapter->button_set_scroll == nullptr) result = not_ready();
        else result = g_adapter->button_set_scroll(native, list_owner->native);
    }
    if (!contracts::succeeded(result)) {
        (void)g_adapter->delete_control(native);
        return result;
    }
    raw->parent = list_owner;
    list_owner->children.push_back(std::move(component));
    g_components.emplace(native, raw);
    *out = native;
    return contracts::ok();
}

QolResult native_toggle_set_value(contracts::QolUiHandle handle, int32_t value) {
    Component* component = nullptr;
    const QolResult lookup = require_component(handle, &component);
    if (!contracts::succeeded(lookup)) return lookup;
    if (component->kind != ComponentKind::kNativeToggle) return unsupported();
    component->value = value != 0 ? 1 : 0;
    return contracts::ok();
}

QolResult native_toggle_get_value(contracts::QolUiHandle handle, int32_t* out) {
    if (out == nullptr) return invalid_argument();
    Component* component = nullptr;
    const QolResult lookup = require_component(handle, &component);
    if (!contracts::succeeded(lookup)) return lookup;
    if (component->kind != ComponentKind::kNativeToggle) return unsupported();
    *out = component->value;
    return contracts::ok();
}

QolResult native_toggle_set_enabled(contracts::QolUiHandle handle, uint8_t enabled) {
    Component* component = nullptr;
    const QolResult lookup = require_component(handle, &component);
    if (!contracts::succeeded(lookup)) return lookup;
    if (component->kind != ComponentKind::kNativeToggle) return unsupported();
    component->enabled = enabled != 0;
    return contracts::ok();
}

QolResult native_toggle_draw(contracts::QolUiHandle handle) {
    if (!on_main_thread()) return not_ready();
    Component* component = nullptr;
    const QolResult lookup = require_component(handle, &component);
    if (!contracts::succeeded(lookup)) return lookup;
    if (component->kind != ComponentKind::kNativeToggle) return unsupported();
    int64_t x = 0;
    int64_t y = 0;
    int32_t pressed = 0;
    QolResult result = g_adapter->control_absolute_origin(handle, &x, &y);
    if (!contracts::succeeded(result)) return result;
    result = g_adapter->control_pressed(handle, &pressed);
    if (!contracts::succeeded(result)) return result;
    return g_adapter->draw_native_toggle({x, y, component->rect.w, component->rect.h},
                                         component->value, pressed != 0 ? 1 : 0);
}

void scroll_content_draw(contracts::QolUiHandle handle, int64_t) {
    Component* list = find_component(handle);
    if (list == nullptr || list->pending_destroy || list->draw_item == nullptr) return;
    CallbackScope scope;
    // 原生已完成内容平移及 clip；直接回调消费者自绘每一行，不经原生 draw_control，
    // 以免原生按钮绘制读取/写入内联文本缓冲。
    for (const auto& row : list->rows) {
        if (list->pending_destroy) break;
        list->draw_item(list->user, row.index, row.native,
                        list->selected == static_cast<int32_t>(row.index) ? 1 : 0);
    }
}

QolResult scroll_list_create(const contracts::UiScrollListDesc* desc,
                            contracts::QolUiHandle parent, contracts::QolUiHandle* out) {
    if (out == nullptr) return invalid_argument();
    *out = nullptr;
    if (desc == nullptr || parent == nullptr || desc->viewport.w <= 0 || desc->viewport.h <= 0 ||
        desc->item_height <= 0 || desc->item_count > 1024 || desc->initial_selected < -1 ||
        (desc->initial_selected >= 0 &&
         static_cast<uint32_t>(desc->initial_selected) >= desc->item_count) ||
        (desc->item_count != 0 && desc->draw_item == nullptr) ||
        desc->item_count > static_cast<uint32_t>(INT32_MAX / desc->item_height) ||
        desc->viewport.x < INT32_MIN || desc->viewport.y < INT32_MIN ||
        desc->viewport.w > INT32_MAX || desc->viewport.h > INT32_MAX ||
        desc->viewport.x > INT32_MAX - desc->viewport.w ||
        desc->viewport.y > INT32_MAX - desc->viewport.h) return invalid_argument();
    if (g_callback_depth != 0 || !native_list_ready() || g_adapter->scroll_create == nullptr ||
        g_adapter->scroll_set_target == nullptr || g_adapter->button_set_scroll == nullptr ||
        g_adapter->create_control == nullptr || g_adapter->set_rect == nullptr ||
        g_adapter->set_click_proc == nullptr) return not_ready();
    Component* parent_component = find_component(parent);
    for (Component* ancestor = parent_component; ancestor != nullptr; ancestor = ancestor->parent) {
        if (ancestor->kind == ComponentKind::kScrollList) return unsupported();
    }
    auto component = std::make_unique<Component>();
    component->kind = ComponentKind::kScrollList;
    component->rect = desc->viewport;
    component->selected = desc->initial_selected;
    component->draw_item = desc->draw_item;
    component->on_select = desc->on_select;
    component->user = desc->user;
    component->rows.resize(desc->item_count);
    const int64_t extent = static_cast<int64_t>(desc->item_count) * desc->item_height;
    const contracts::UiNativeScrollDesc native_desc{
        desc->viewport, extent, desc->item_height, 12, static_cast<uint8_t>(desc->viewport.h >= 4)};
    QolResult result = g_adapter->scroll_create(parent, &native_desc, &scroll_content_draw,
                                               &component->native);
    if (!contracts::succeeded(result)) return result;
    if (component->native == nullptr) return not_ready();
    Component* list = component.get();
    list->parent = parent_component;
    g_components.emplace(list->native, list);
    if (parent_component != nullptr) parent_component->children.push_back(std::move(component));
    else g_roots.push_back(std::move(component));  // 借用 native parent，只拥有列表子树。
    const auto fail = [list](QolResult failure) {
        list->pending_destroy = true;
        const QolResult cleanup = destroy(list->native);
        return contracts::succeeded(cleanup) ? failure : cleanup;
    };
    contracts::QolUiHandle group = nullptr;
    result = g_adapter->create_control(3, list->native, &group);
    if (!contracts::succeeded(result)) return fail(result);
    if (group == nullptr) return fail(not_ready());
    result = g_adapter->set_rect(group, {0, 0, desc->viewport.w, extent});
    if (!contracts::succeeded(result)) return fail(result);
    result = g_adapter->scroll_set_target(list->native, group);
    if (!contracts::succeeded(result)) return fail(result);
    for (uint32_t i = 0; i < desc->item_count; ++i) {
        ScrollRow& row = list->rows[i];
        row.list = list;
        row.index = i;
        result = g_adapter->create_control(1, group, &row.native);
        if (!contracts::succeeded(result)) return fail(result);
        if (row.native == nullptr) return fail(not_ready());
        result = g_adapter->set_rect(row.native,
            {0, static_cast<int64_t>(i) * desc->item_height, desc->viewport.w, desc->item_height});
        if (!contracts::succeeded(result)) return fail(result);
        // 行控件只承担触摸命中与 ExecuteProc；绘制由消费者 draw_item 自绘，不写内联文本。
        result = g_adapter->set_click_proc(row.native, &scroll_row_execute, &row);
        if (!contracts::succeeded(result)) return fail(result);
        result = g_adapter->button_set_scroll(row.native, list->native);
        if (!contracts::succeeded(result)) return fail(result);
    }
    *out = list->native;
    return contracts::ok();
}

QolResult require_list(contracts::QolUiHandle handle, Component** out) {
    const QolResult lookup = require_component(handle, out);
    if (!contracts::succeeded(lookup)) return lookup;
    if ((*out)->kind != ComponentKind::kScrollList) return unsupported();
    if ((*out)->native_deleted || (*out)->pending_destroy) return not_ready();
    return native_list_ready() ? contracts::ok() : not_ready();
}

QolResult scroll_list_process(contracts::QolUiHandle handle) {
    if (!native_list_ready() || g_callback_depth != 0) return not_ready();
    const QolResult pending = flush_pending_destroy();
    if (!contracts::succeeded(pending)) return pending;
    Component* list = nullptr;
    const QolResult lookup = require_list(handle, &list);
    if (!contracts::succeeded(lookup)) return lookup;
    return g_adapter->scroll_process == nullptr ? not_ready() : g_adapter->scroll_process(handle);
}

QolResult scroll_list_draw(contracts::QolUiHandle handle) {
    Component* list = nullptr;
    const QolResult lookup = require_list(handle, &list);
    if (!contracts::succeeded(lookup)) return lookup;
    if (g_callback_depth != 0 || g_adapter->scroll_draw == nullptr) return not_ready();
    list->draw_result = contracts::ok();
    CallbackScope scope;
    const QolResult result = g_adapter->scroll_draw(handle);
    return contracts::succeeded(result) ? list->draw_result : result;
}

QolResult scroll_list_get_offset(contracts::QolUiHandle handle, int64_t* out) {
    if (out == nullptr) return invalid_argument();
    *out = 0;
    Component* list = nullptr;
    const QolResult lookup = require_list(handle, &list);
    if (!contracts::succeeded(lookup)) return lookup;
    return g_adapter->scroll_get_offset == nullptr ? not_ready() :
        g_adapter->scroll_get_offset(handle, out);
}

bool native_event_safe(const Component* component) {
    if (component->pending_destroy || component->native_deleted) return false;
    if (component->kind == ComponentKind::kToggle || component->kind == ComponentKind::kSelect ||
        component->kind == ComponentKind::kScrollView ||
        (component->kind == ComponentKind::kButton && component->on_click == nullptr)) return false;
    for (const auto& child : component->children) {
        if (!native_event_safe(child.get())) return false;
    }
    return true;
}

QolResult dispatch_native_event(contracts::QolUiHandle root, uint64_t event, uint64_t param,
                                uint64_t param2, uint32_t* consumed) {
    if (root == nullptr || consumed == nullptr) return invalid_argument();
    *consumed = 0;
    if (!native_list_ready() || g_callback_depth != 0 || g_adapter->control_tree_event == nullptr) {
        return not_ready();
    }
    const Component* component = find_component(root);
    if (component != nullptr && !native_event_safe(component)) return unsupported();
    CallbackScope scope;
    return g_adapter->control_tree_event(root, event, param, param2, consumed);
}

QolResult draw_container(UiRect rect) {
    if (rect.w < 48 || rect.h < 48 ||
        rect.x < INT32_MIN || rect.x > INT32_MAX || rect.y < INT32_MIN || rect.y > INT32_MAX ||
        rect.w > INT32_MAX || rect.h > INT32_MAX ||
        rect.x > static_cast<int64_t>(INT32_MAX) - rect.w ||
        rect.y > static_cast<int64_t>(INT32_MAX) - rect.h) {
        return invalid_argument();
    }
    if (!adapter_ready() || g_adapter->draw_border_rect == nullptr) return not_ready();
    return g_adapter->draw_border_rect(static_cast<int32_t>(rect.x),
                                       static_cast<int32_t>(rect.y),
                                       static_cast<int32_t>(rect.w),
                                       static_cast<int32_t>(rect.h));
}

QolResult draw_container_ornamented(UiRect rect) {
    if (rect.w < 48 || rect.h < 48 ||
        rect.x < static_cast<int64_t>(INT32_MIN) + 2 || rect.x > INT32_MAX ||
        rect.y < static_cast<int64_t>(INT32_MIN) + 2 || rect.y > INT32_MAX ||
        rect.w > INT32_MAX || rect.h > INT32_MAX ||
        rect.x > static_cast<int64_t>(INT32_MAX) - rect.w ||
        rect.y > static_cast<int64_t>(INT32_MAX) - rect.h) {
        return invalid_argument();
    }
    if (!adapter_ready() || g_adapter->draw_border_rect == nullptr ||
        g_adapter->image_group == nullptr || g_adapter->image_loc == nullptr ||
        g_adapter->image_loc_size == nullptr || g_adapter->draw_part == nullptr) {
        return not_ready();
    }

    contracts::QolUiHandle group = nullptr;
    QolResult result = g_adapter->image_group(kFrameImageUnit, &group);
    if (!contracts::succeeded(result)) return result;

    constexpr int32_t kLocations[] = {0x30, 0x31, 0x35, 0x36};
    contracts::QolUiHandle parts[4]{};
    int32_t widths[4]{};
    int32_t heights[4]{};
    for (size_t i = 0; i < 4; ++i) {
        result = g_adapter->image_loc(kFrameImageUnit, kLocations[i], &parts[i]);
        if (!contracts::succeeded(result)) return result;
        result = g_adapter->image_loc_size(kFrameImageUnit, kLocations[i], &widths[i], &heights[i]);
        if (!contracts::succeeded(result)) return result;
        if (widths[i] <= 0 || heights[i] <= 0) return not_ready();
    }

    result = g_adapter->draw_border_rect(static_cast<int32_t>(rect.x),
                                         static_cast<int32_t>(rect.y),
                                         static_cast<int32_t>(rect.w),
                                         static_cast<int32_t>(rect.h));
    if (!contracts::succeeded(result)) return result;

    const int64_t positions_x[] = {rect.x - 2, rect.x + rect.w - widths[1], rect.x - 2,
                                   rect.x + rect.w - widths[3]};
    const int64_t positions_y[] = {rect.y - 2, rect.y - 2, rect.y + rect.h - heights[2],
                                   rect.y + rect.h - heights[3]};
    int32_t xs[4]{};
    int32_t ys[4]{};
    for (size_t i = 0; i < 4; ++i) {
        if (positions_x[i] < INT32_MIN || positions_x[i] > INT32_MAX ||
            positions_y[i] < INT32_MIN || positions_y[i] > INT32_MAX) {
            return invalid_argument();
        }
        xs[i] = static_cast<int32_t>(positions_x[i]);
        ys[i] = static_cast<int32_t>(positions_y[i]);
    }
    for (size_t i = 0; i < 4; ++i) {
        result = g_adapter->draw_part(group, xs[i], ys[i], parts[i], 0, 1);
        if (!contracts::succeeded(result)) return result;
    }
    return contracts::ok();
}

// 不经控件 data 的文本绘制原语：按控件绝对原点绘制传入字符串，颜色沿用控件文本口径。
QolResult draw_control_text(contracts::QolUiHandle control, const char* text, int32_t x_offset,
                            int32_t y_offset, uint32_t color, int32_t align, int32_t font) {
    if (control == nullptr || text == nullptr) return invalid_argument();
    if (!adapter_ready() || g_adapter->control_absolute_origin == nullptr ||
        g_adapter->set_font_color_rgb == nullptr || g_adapter->draw_string_with_font == nullptr) {
        return not_ready();
    }
    int64_t x = 0;
    int64_t y = 0;
    const QolResult origin = g_adapter->control_absolute_origin(control, &x, &y);
    if (!contracts::succeeded(origin)) return origin;
    const int64_t draw_x = x + x_offset;
    const int64_t draw_y = y + y_offset;
    if (draw_x < INT32_MIN || draw_x > INT32_MAX || draw_y < INT32_MIN || draw_y > INT32_MAX) {
        return invalid_argument();
    }
    const QolResult color_result = g_adapter->set_font_color_rgb(
        static_cast<int32_t>(color & 0xff), static_cast<int32_t>((color >> 8) & 0xff),
        static_cast<int32_t>((color >> 16) & 0xff));
    if (!contracts::succeeded(color_result)) return color_result;
    return g_adapter->draw_string_with_font(text, static_cast<int32_t>(draw_x),
                                            static_cast<int32_t>(draw_y), align, font);
}

bool rect_within_int32(UiRect rect) {
    return rect.x >= INT32_MIN && rect.y >= INT32_MIN && rect.x <= INT32_MAX &&
        rect.y <= INT32_MAX && rect.w > 0 && rect.h > 0 && rect.w <= INT32_MAX &&
        rect.h <= INT32_MAX && rect.x <= static_cast<int64_t>(INT32_MAX) - rect.w &&
        rect.y <= static_cast<int64_t>(INT32_MAX) - rect.h;
}

// 可缩放开关：矩形轨道 + 左右滑块，全部用 ABGR 实色矩形自绘，不依赖原生控件或共享图集。
// 颜色取统一 token；开=亮金轨 + 右滑块，关=暗轨 + 左滑块。尺寸不足以容纳滑块时只画轨道。
QolResult switch_draw(UiRect rect, int32_t value) {
    if (!rect_within_int32(rect)) return invalid_argument();
    if (!on_main_thread() || g_adapter->fill_rect == nullptr) return not_ready();
    const bool on = value != 0;
    const int64_t thickness = rect.h >= 16 ? 2 : 1;
    const uint32_t track = on ? contracts::ui_theme::kSwitchTrackOn
                              : contracts::ui_theme::kSwitchTrackOff;
    const uint32_t border = on ? contracts::ui_theme::kFrameGold
                               : contracts::ui_theme::kTextDisabled;
    QolResult result = g_adapter->fill_rect(rect, track);
    if (!contracts::succeeded(result)) return result;
    const UiRect edges[4] = {
        {rect.x, rect.y, rect.w, thickness},
        {rect.x, rect.y + rect.h - thickness, rect.w, thickness},
        {rect.x, rect.y, thickness, rect.h},
        {rect.x + rect.w - thickness, rect.y, thickness, rect.h},
    };
    for (const UiRect& edge : edges) {
        result = g_adapter->fill_rect(edge, border);
        if (!contracts::succeeded(result)) return result;
    }
    const int64_t inset = thickness + 2;
    const int64_t thumb = rect.h - inset * 2;
    if (thumb >= 4 && rect.w >= thumb + inset * 2 + 4) {
        const UiRect knob{on ? rect.x + rect.w - inset - thumb : rect.x + inset,
                          rect.y + inset, thumb, thumb};
        result = g_adapter->fill_rect(
            knob, on ? contracts::ui_theme::kSwitchThumbOn : contracts::ui_theme::kSwitchThumbOff);
        if (!contracts::succeeded(result)) return result;
    }
    return contracts::ok();
}

// 命中只按 rect 自身判断，不触碰父行其它区域；按下（phase 1）即翻转，与组件 kToggle 一致。
QolResult switch_touch(UiRect rect, uint32_t phase, int64_t x, int64_t y, int32_t* inout_value) {
    if (inout_value == nullptr) return invalid_argument();
    if (!rect_within_int32(rect)) return invalid_argument();
    if (!on_main_thread()) return not_ready();
    if (phase != 1) return contracts::ok();
    const bool inside = x >= rect.x && x < rect.x + rect.w && y >= rect.y && y < rect.y + rect.h;
    if (inside) *inout_value = *inout_value == 0 ? 1 : 0;
    return contracts::ok();
}

UiRect help_column_frame(UiRect viewport, bool left) {
    return {viewport.x - 8, viewport.y - 8, viewport.w + (left ? 32 : 16), viewport.h + 16};
}

bool help_viewport_valid(UiRect viewport, UiRect panel, bool left) {
    // 先限范围，避免 int64 极值在扩框/加坐标时溢出。
    if (viewport.x < 8 || viewport.y < 8 || viewport.w < (left ? 16 : 32) ||
        viewport.h < 32 || viewport.x > panel.w || viewport.y > panel.h ||
        viewport.w > panel.w || viewport.h > panel.h) return false;
    const UiRect frame = help_column_frame(viewport, left);
    return frame.x + frame.w <= panel.w && frame.y + frame.h <= panel.h;
}

void help_item_draw(void* user, uint32_t index, contracts::QolUiHandle row, uint8_t selected) {
    auto* page = static_cast<Component*>(user);
    if (!page->pending_destroy && page->draw_item != nullptr) {
        page->draw_item(page->user, index, row, selected);
    }
}

void help_select(void* user, uint32_t index) {
    auto* page = static_cast<Component*>(user);
    if (!page->pending_destroy) page->selected = static_cast<int32_t>(index);
}

QolResult help_page_create(const contracts::UiHelpPageDesc* desc,
                          contracts::QolUiHandle parent, contracts::QolUiHandle* out) {
    if (out == nullptr) return invalid_argument();
    *out = nullptr;
    if (desc == nullptr || desc->panel_rect.w < 48 || desc->panel_rect.h < 48 ||
        desc->panel_rect.w > INT32_MAX || desc->panel_rect.h > INT32_MAX ||
        desc->panel_rect.x < INT32_MIN || desc->panel_rect.y < INT32_MIN ||
        desc->panel_rect.x > INT32_MAX - desc->panel_rect.w ||
        desc->panel_rect.y > INT32_MAX - desc->panel_rect.h ||
        desc->item_height <= 0 || desc->option_count > 1024 ||
        desc->option_count > static_cast<uint32_t>(INT32_MAX / desc->item_height) ||
        desc->initial_selected < -1 || (desc->initial_selected >= 0 &&
        static_cast<uint32_t>(desc->initial_selected) >= desc->option_count) ||
        (desc->option_count != 0 && (desc->options == nullptr || desc->draw_item == nullptr)) ||
        !help_viewport_valid(desc->left_viewport, desc->panel_rect, true) ||
        !help_viewport_valid(desc->right_viewport, desc->panel_rect, false)) return invalid_argument();
    const UiRect left = help_column_frame(desc->left_viewport, true);
    const UiRect right = help_column_frame(desc->right_viewport, false);
    if (left.x + left.w > right.x) return invalid_argument();
    for (uint32_t i = 0; i < desc->option_count; ++i) {
        if (desc->options[i].draw_detail == nullptr) return invalid_argument();
    }
    if (!native_list_ready() || g_callback_depth != 0 || g_adapter->create_control == nullptr ||
        g_adapter->set_rect == nullptr || g_adapter->control_absolute_origin == nullptr) {
        return not_ready();
    }
    Component* parent_component = find_component(parent);
    for (auto* ancestor = parent_component; ancestor != nullptr; ancestor = ancestor->parent) {
        if (ancestor->kind == ComponentKind::kScrollList ||
            ancestor->kind == ComponentKind::kHelpPage) return unsupported();
    }
    auto component = std::make_unique<Component>();
    component->kind = ComponentKind::kHelpPage;
    component->rect = desc->panel_rect;
    component->left_viewport = desc->left_viewport;
    component->right_viewport = desc->right_viewport;
    component->selected = desc->initial_selected;
    component->draw_item = desc->draw_item;
    component->user = desc->user;
    if (desc->option_count != 0) {
        component->help_options.assign(desc->options, desc->options + desc->option_count);
    }
    QolResult result = g_adapter->create_control(parent == nullptr ? 0 : 3, parent, &component->native);
    if (!contracts::succeeded(result)) return result;
    if (component->native == nullptr) return not_ready();
    Component* page = component.get();
    page->parent = parent_component;
    g_components.emplace(page->native, page);
    if (parent_component != nullptr) parent_component->children.push_back(std::move(component));
    else g_roots.push_back(std::move(component));  // parent 可借用；只拥有 page 子树。
    const auto fail = [page](QolResult failure) {
        page->pending_destroy = true;
        const QolResult cleanup = destroy(page->native);
        return contracts::succeeded(cleanup) ? failure : cleanup;
    };
    result = g_adapter->set_rect(page->native, desc->panel_rect);
    if (!contracts::succeeded(result)) return fail(result);
    const contracts::UiScrollListDesc list_desc{desc->left_viewport, desc->item_height,
        desc->option_count, desc->initial_selected, &help_item_draw, &help_select, page};
    result = scroll_list_create(&list_desc, page->native, &page->help_list);
    if (!contracts::succeeded(result)) return fail(result);
    *out = page->native;
    return contracts::ok();
}

QolResult require_help_page(contracts::QolUiHandle handle, Component** out) {
    if (!native_list_ready()) return not_ready();
    const QolResult lookup = require_component(handle, out);
    if (!contracts::succeeded(lookup)) return lookup;
    if ((*out)->kind != ComponentKind::kHelpPage) return unsupported();
    return (*out)->pending_destroy || (*out)->native_deleted ? not_ready() : contracts::ok();
}

QolResult help_page_process(contracts::QolUiHandle handle) {
    if (!native_list_ready() || g_callback_depth != 0) return not_ready();
    const QolResult pending = flush_pending_destroy();
    if (!contracts::succeeded(pending)) return pending;
    Component* page = nullptr;
    const QolResult lookup = require_help_page(handle, &page);
    if (!contracts::succeeded(lookup)) return lookup;
    return scroll_list_process(page->help_list);
}

QolResult help_page_draw(contracts::QolUiHandle handle) {
    Component* page = nullptr;
    QolResult result = require_help_page(handle, &page);
    if (!contracts::succeeded(result)) return result;
    if (g_callback_depth != 0) return not_ready();
    int64_t x = 0, y = 0;
    result = g_adapter->control_absolute_origin(handle, &x, &y);
    if (!contracts::succeeded(result)) return result;
    if (x < static_cast<int64_t>(INT32_MIN) + 2 || y < static_cast<int64_t>(INT32_MIN) + 2 ||
        x > INT32_MAX - page->rect.w || y > INT32_MAX - page->rect.h) return invalid_argument();
    auto absolute = [x, y](UiRect rect) { rect.x += x; rect.y += y; return rect; };
    result = draw_container(absolute({0, 0, page->rect.w, page->rect.h}));
    if (!contracts::succeeded(result)) return result;
    result = draw_container_ornamented(absolute(help_column_frame(page->left_viewport, true)));
    if (!contracts::succeeded(result)) return result;
    result = draw_container(absolute(help_column_frame(page->right_viewport, false)));
    if (!contracts::succeeded(result)) return result;
    result = scroll_list_draw(page->help_list);
    if (!contracts::succeeded(result)) return result;
    // ControlScroll_Draw 已复原整屏裁剪，右栏不处于左列表 clip 内。
    if (!page->pending_destroy && page->selected >= 0) {
        const auto option = page->help_options[static_cast<size_t>(page->selected)];
        CallbackScope scope;
        option.draw_detail(option.user, absolute(page->right_viewport));
    }
    return contracts::ok();
}

QolResult help_page_set_selected(contracts::QolUiHandle handle, int32_t index) {
    Component* page = nullptr;
    const QolResult lookup = require_help_page(handle, &page);
    if (!contracts::succeeded(lookup)) return lookup;
    if (index < -1 || (index >= 0 && static_cast<size_t>(index) >= page->help_options.size())) {
        return invalid_argument();
    }
    if (g_callback_depth != 0) return not_ready();
    Component* list = nullptr;
    const QolResult found = require_list(page->help_list, &list);
    if (!contracts::succeeded(found)) return found;
    page->selected = index;
    list->selected = index;
    return contracts::ok();
}

QolResult help_page_get_selected(contracts::QolUiHandle handle, int32_t* out) {
    if (out == nullptr) return invalid_argument();
    *out = -1;
    Component* page = nullptr;
    const QolResult lookup = require_help_page(handle, &page);
    if (contracts::succeeded(lookup)) *out = page->selected;
    return lookup;
}

void request_host_back() { ui_panels_request_back(); }

void back_button_activated(void*) { request_host_back(); }

QolResult back_button_create(contracts::QolUiHandle parent, UiRect rect,
                             contracts::QolUiHandle* out) {
    if (out == nullptr) return invalid_argument();
    *out = nullptr;
    if (g_callback_depth != 0) return not_ready();
    if (!adapter_ready()) return not_ready();
    // 共享图集 0x59 由助手确保加载并借用，页面不再各自加载；任何路径都不卸载。
    if (g_adapter->load_option_images != nullptr) {
        (void)g_adapter->load_option_images();
    }
    // 组件树（手工分派）：创建组件按钮，激活由 back_button_touch 命中。不绑定点击，
    // 避免 dispatch_touch 按下即触发与 back_button_touch 抬起触发重复。
    if (parent != nullptr && find_component(parent) != nullptr) {
        const contracts::UiButtonDesc desc{rect, "返回", 0, nullptr, nullptr};
        return button_create(&desc, parent, out);
    }
    // 原生事件树：创建原生控件并绑定 ExecuteProc → 宿主 back（释放落于按钮内才触发）。
    if (g_adapter->create_control == nullptr || g_adapter->set_rect == nullptr ||
        g_adapter->set_click_proc == nullptr) {
        return not_ready();
    }
    contracts::QolUiHandle native = nullptr;
    QolResult result = g_adapter->create_control(1, parent, &native);
    if (!contracts::succeeded(result) || native == nullptr) return not_ready();
    const auto fail = [native](QolResult failure) {
        if (g_adapter->delete_control != nullptr) (void)g_adapter->delete_control(native);
        return failure;
    };
    result = g_adapter->set_rect(native, rect);
    if (!contracts::succeeded(result)) return fail(result);
    if (g_adapter->set_text != nullptr) {
        result = g_adapter->set_text(native, "返回");
        if (!contracts::succeeded(result)) return fail(result);
    }
    result = g_adapter->set_click_proc(native, &back_button_activated, nullptr);
    if (!contracts::succeeded(result)) return fail(result);
    *out = native;
    return contracts::ok();
}

QolResult back_button_draw(contracts::QolUiHandle button) {
    if (button == nullptr) return invalid_argument();
    if (!adapter_ready()) return not_ready();
    uint8_t pressed = 0;
    UiRect rect{};
    Component* component = find_component(button);
    if (component != nullptr) {
        pressed = component->value != 0 ? 1 : 0;
        rect = component->rect;
    } else {
        if (g_adapter->control_pressed != nullptr) {
            int32_t value = 0;
            if (contracts::succeeded(g_adapter->control_pressed(button, &value))) {
                pressed = value != 0 ? 1 : 0;
            }
        }
        if (g_adapter->get_rect == nullptr ||
            !contracts::succeeded(g_adapter->get_rect(button, &rect))) {
            return not_ready();
        }
    }
    if (g_adapter->draw_back_button != nullptr &&
        contracts::succeeded(g_adapter->draw_back_button(button, pressed))) {
        return contracts::ok();
    }
    if (g_adapter->draw_custom_button == nullptr) return not_ready();
    return g_adapter->draw_custom_button(button, {0, 0, rect.w, rect.h}, 0xFF241C18u, 0xFF00B4D7u,
                                         2, "返回", 0xFFCB9EE2u, 8, 0x18);
}

QolResult back_button_touch(contracts::QolUiHandle button, uint32_t phase, int64_t x, int64_t y) {
    if (button == nullptr) return invalid_argument();
    if (!adapter_ready() || g_adapter->hit_test == nullptr) return not_ready();
    Component* component = find_component(button);
    if (component == nullptr || component->kind != ComponentKind::kButton) {
        // 原生事件树按钮由 ExecuteProc 处理，不经手工命中入口。
        return unsupported();
    }
    if (phase != 1 && phase != 2) return contracts::ok();
    const bool inside =
        g_adapter->hit_test(button, x, y, {0, 0, component->rect.w, component->rect.h}) > 0;
    if (phase == 1) {
        if (inside) component->value = 1;
        return contracts::ok();
    }
    const bool pressed = component->value != 0;
    component->value = 0;
    if (pressed && inside) request_host_back();
    return contracts::ok();
}

QolResult back_request() {
    request_host_back();
    return contracts::ok();
}

const contracts::QolUiComponents kUiComponents{
    sizeof(contracts::QolUiComponents),
    contracts::kUiContractMajor,
    contracts::kUiContractMinor,
    &panel_create,
    &button_create,
    &label_create,
    &toggle_create,
    &select_create,
    &scroll_view_create,
    &set_rect,
    &button_set_text,
    &label_set_text,
    &toggle_set_value,
    &select_set_selected,
    &scroll_set_offset,
    &dispatch_touch,
    &destroy,
    &draw_container,
    &draw_container_ornamented,
    &scroll_list_create,
    &scroll_list_process,
    &scroll_list_draw,
    &scroll_list_get_offset,
    &dispatch_native_event,
    &help_page_create,
    &help_page_process,
    &help_page_draw,
    &help_page_set_selected,
    &help_page_get_selected,
    &back_button_create,
    &back_button_draw,
    &back_button_touch,
    &back_request,
    &draw_control_text,
    &switch_draw,
    &switch_touch,
    &native_toggle_create,
    &native_toggle_set_value,
    &native_toggle_get_value,
    &native_toggle_set_enabled,
    &native_toggle_draw,
    &scroll_list_get_row,
    &scroll_list_set_selection_enabled,
    &scroll_row_button_create,
    &text_button_create,
    &text_button_set_enabled,
    &text_button_draw,
    &nav_button_create,
    &nav_button_set_enabled,
    &nav_button_draw,
};

}  // namespace

bool ui_components_bind(const contracts::QolUiAdapter* adapter) {
    constexpr size_t kBaseAdapterSize = offsetof(contracts::QolUiAdapter, item_proc_source_control) +
        sizeof(((contracts::QolUiAdapter*)nullptr)->item_proc_source_control);
    if (adapter == nullptr || adapter->size < kBaseAdapterSize ||
        adapter->major != contracts::kUiAdapterMajor || adapter->minor < kUiAdapterBaseMinor) {
        return false;
    }
    g_adapter = adapter;
    return true;
}

void ui_components_unbind() {
    // owned 列表必须先归还 native 控件，再清空回调上下文。
    if (g_callback_depth != 0) return;
    for (const auto& root : g_roots) {
        if (!contracts::succeeded(delete_owned_lists(root.get()))) return;
    }
    g_adapter = nullptr;
    g_components.clear();
    g_roots.clear();
}

const contracts::QolUiComponents* ui_components_api() { return &kUiComponents; }

// ui_components.h 声明的适配表访问器：返回 ui_components_bind 注入的宿主 UI 表。
// 定义归组件插件自身（debug 面板等组件消费者需要），不再由页面侧提供。
const contracts::QolUiAdapter* ui_adapter() { return g_adapter; }


}  // namespace qol::features::core_ui
