#include "ui_components.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace qol::features::core_ui {
namespace {

enum class ComponentKind : uint8_t {
    kPanel,
    kButton,
    kLabel,
    kToggle,
    kSelect,
    kScrollView,
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
};

const contracts::QolUiAdapter* g_adapter = nullptr;
std::unordered_map<contracts::QolUiHandle, Component*> g_components;
std::vector<std::unique_ptr<Component>> g_roots;

constexpr const char* kInvalidArgument = "invalid argument";
constexpr const char* kNotReady = "ui primitive not ready";
constexpr const char* kNotFound = "ui component not found";
constexpr const char* kUnsupported = "ui component unsupported";

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

QolResult require_component(contracts::QolUiHandle handle, Component** out) {
    if (handle == nullptr || out == nullptr) return invalid_argument();
    *out = find_component(handle);
    return *out == nullptr ? not_found() : contracts::ok();
}

QolResult create_component(ComponentKind kind, uint32_t native_kind, UiRect rect,
                           contracts::QolUiHandle parent, const char* text, uint32_t style,
                           contracts::QolUiClickFn on_click, void* click_user,
                           contracts::QolUiDrawFn draw_proc, contracts::QolUiHandle* out) {
    if (out == nullptr) return invalid_argument();
    *out = nullptr;
    if (!adapter_ready() || g_adapter->create_control == nullptr || g_adapter->set_rect == nullptr) {
        return not_ready();
    }
    if ((kind == ComponentKind::kPanel && parent != nullptr) ||
        (kind != ComponentKind::kPanel && parent == nullptr)) {
        return invalid_argument();
    }
    Component* parent_component = parent == nullptr ? nullptr : find_component(parent);
    if (parent != nullptr && parent_component == nullptr) return not_found();

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

QolResult destroy(contracts::QolUiHandle handle) {
    Component* component = nullptr;
    const QolResult lookup = require_component(handle, &component);
    if (!contracts::succeeded(lookup)) return lookup;
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
};

}  // namespace

bool ui_components_bind(const contracts::QolUiAdapter* adapter) {
    if (adapter == nullptr || adapter->size < sizeof(contracts::QolUiAdapter) ||
        adapter->major != contracts::kUiContractMajor || adapter->minor < contracts::kUiContractMinor) {
        return false;
    }
    g_adapter = adapter;
    return true;
}

void ui_components_unbind() {
    g_adapter = nullptr;
    g_components.clear();
    g_roots.clear();
}

const contracts::QolUiComponents* ui_components_api() { return &kUiComponents; }


}  // namespace qol::features::core_ui
