#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "contracts/ui.h"
#include "fake_shared_atlases.h"
#include "plugins/core.custom_ui/ui_components.h"
#include "plugins/core.custom_ui/panels/debug_help.h"

using namespace qol::contracts;
namespace {
struct Control {
    uint32_t kind = 0;
    Control* parent = nullptr;
    std::vector<std::unique_ptr<Control>> children;
    UiRect rect{};
    QolUiDrawFn draw = nullptr;
    QolUiNativeScrollDrawFn content = nullptr;
    QolUiClickFn click = nullptr;
    void* user = nullptr;
    const char* text = nullptr;
};
std::vector<std::unique_ptr<Control>> g_roots;
bool g_main = true, g_fail_delete = false, g_clip = false, g_back_requested = false;
int g_live = 0, g_deletes = 0, g_calls = 0, g_fail_at = -1, g_processes = 0, g_details = 0;
int g_opens = 0, g_removed = 0;
int32_t g_detail_index = -1;
int g_users[] = {0, 1, 2, 3, 4, 5, 6, 7};
std::vector<uint8_t> g_highlights;
std::vector<UiRect> g_frames;
std::vector<std::string> g_draw_order;
UiRect g_detail_rect{};
Control* g_execute = nullptr;
QolUiHandle g_destroy_on_draw = nullptr, g_page = nullptr;
bool g_destroy_in_detail = false;
UiHelpPageDesc g_recreate_desc{};
const QolUiComponents* g_api = nullptr;
const PanelProviderSpec* g_provider = nullptr;
QolFrameHandler g_frame = nullptr;
QolResult fail() { return error(kQolErrNotReady, "injected", 8); }
QolResult step() { return ++g_calls == g_fail_at ? fail() : ok(); }
int32_t main_thread() { return g_main ? 1 : 0; }
QolResult create(uint32_t kind, QolUiHandle parent, QolUiHandle* out) {
    assert(g_main);
    const auto result = step();
    if (!succeeded(result)) return result;
    auto node = std::make_unique<Control>();
    node->kind = kind; node->parent = static_cast<Control*>(parent); *out = node.get();
    if (parent != nullptr) static_cast<Control*>(parent)->children.push_back(std::move(node));
    else g_roots.push_back(std::move(node));
    ++g_live; return ok();
}
QolResult set_rect(QolUiHandle handle, UiRect rect) {
    const auto result = step();
    if (succeeded(result)) static_cast<Control*>(handle)->rect = rect;
    return result;
}
QolResult get_rect(QolUiHandle handle, UiRect* out) {
    *out = static_cast<Control*>(handle)->rect;
    return ok();
}
QolResult origin(QolUiHandle handle, int64_t* x, int64_t* y) {
    *x = *y = 0;
    for (auto* node = static_cast<Control*>(handle); node != nullptr; node = node->parent) {
        *x += node->rect.x; *y += node->rect.y;
    }
    return ok();
}
QolResult set_text(QolUiHandle handle, const char* text) {
    auto* node = static_cast<Control*>(handle);
    assert(node->kind == 1 || node->kind == 2);
    node->text = text; return ok();
}
QolResult set_click(QolUiHandle handle, QolUiClickFn fn, void* user) {
    assert(fn != nullptr);
    const auto result = step();
    if (succeeded(result)) {
        auto* node = static_cast<Control*>(handle); node->click = fn; node->user = user;
    }
    return result;
}
QolResult set_draw(QolUiHandle handle, QolUiDrawFn draw) {
    const auto result = step();
    if (succeeded(result)) static_cast<Control*>(handle)->draw = draw;
    return result;
}
void revoke(Control* node) {
    node->click = nullptr; node->draw = nullptr; node->content = nullptr; node->user = nullptr;
    --g_live;
    for (const auto& child : node->children) revoke(child.get());
}
QolResult remove_control(QolUiHandle handle) {
    assert(g_main && !g_clip);
    if (g_fail_delete) return fail();
    auto* node = static_cast<Control*>(handle);
    auto& siblings = node->parent == nullptr ? g_roots : node->parent->children;
    auto it = std::find_if(siblings.begin(), siblings.end(), [node](const auto& p) { return p.get() == node; });
    assert(it != siblings.end());
    revoke(node); siblings.erase(it); ++g_deletes; return ok();
}
QolResult create_scroll(QolUiHandle parent, const UiNativeScrollDesc* desc,
                        QolUiNativeScrollDrawFn draw, QolUiHandle* out) {
    const auto result = create(4, parent, out);
    if (succeeded(result)) {
        auto* node = static_cast<Control*>(*out); node->rect = desc->viewport; node->content = draw;
    }
    return result;
}
QolResult connect(QolUiHandle, QolUiHandle) { return step(); }
QolResult process(QolUiHandle) { assert(g_main); ++g_processes; return ok(); }
QolResult draw_control(QolUiHandle handle) {
    auto* node = static_cast<Control*>(handle);
    assert(node->kind == 1 && node->draw != nullptr && g_clip);
    node->draw(handle); return ok();
}
QolResult draw_scroll(QolUiHandle handle) {
    assert(!g_clip);
    g_draw_order.emplace_back("scroll"); g_clip = true;
    static_cast<Control*>(handle)->content(handle, 0);
    g_clip = false; return ok();
}
QolResult event(QolUiHandle, uint64_t evt, uint64_t param, uint64_t param2, uint32_t* consumed) {
    assert(g_main && evt == 0x18 && param == 10 && param2 == 20);
    if (g_execute != nullptr) g_execute->click(g_execute->user);
    *consumed = 7; return ok();
}
QolResult container(int32_t x, int32_t y, int32_t w, int32_t h) {
    assert(!g_clip); g_frames.push_back({x, y, w, h}); g_draw_order.emplace_back("container"); return ok();
}
// 游戏 IMGSYS fake：UnitLoad 幂等（已加载不重载、不增引用），UnitUnload 无引用计数、
// 直接置失效并记录物理卸载次数。0x0f 边框图集由游戏共享，人物信息页等原生消费者在
// 模块页关闭后仍会绘制（SGL_DrawTexturePartEx 读 0x0f）；模块页任何关闭/回滚路径都
// 不得卸载，物理卸载次数必须为 0。
constexpr int32_t kFrameImageUnit = 0x0f;
qol::tests::FakeSharedAtlases g_atlases;
// 模拟仍存活的原生消费者：记录其依赖的图集加载代次，模块页关闭后校验仍可绘制。
const auto g_native_frame = g_atlases.native_consumer({kFrameImageUnit});
QolResult load(int32_t unit) {
    assert(unit == kFrameImageUnit);
    g_atlases.load(unit);
    return ok();
}
QolResult unload(int32_t unit) {
    assert(unit == kFrameImageUnit);
    g_atlases.unload(unit);
    return ok();
}
QolResult image_group(int32_t unit, QolUiHandle* out) {
    assert(unit == kFrameImageUnit); *out = &g_atlases; return ok();
}
QolResult image_loc(int32_t unit, int32_t, QolUiHandle* out) { return image_group(unit, out); }
QolResult image_size(int32_t unit, int32_t, int32_t* w, int32_t* h) {
    assert(unit == kFrameImageUnit); *w = *h = 24; return ok();
}
QolResult part(QolUiHandle, int32_t, int32_t, QolUiHandle, int32_t, int32_t) { return ok(); }
QolResult no_op() { return ok(); }
QolResult inset(int32_t* out) { *out = 10; return ok(); }
QolResult draw_text(QolUiHandle handle, int32_t, int32_t, uint32_t) {
    assert(static_cast<Control*>(handle)->text != nullptr); return ok();
}
QolResult color(uint32_t) { return ok(); }
QolResult color_rgb(int32_t, int32_t, int32_t) { return ok(); }
QolResult string(const char* text, int32_t, int32_t, int32_t, int32_t) {
    // 行文本经 draw_control_text 在左栏 clip 内绘制；详情文本在 clip 外绘制（detail 另行校验）。
    assert(text != nullptr); return ok();
}
QolResult fill(UiRect, uint32_t) { assert(!g_clip); return ok(); }
QolResult button(QolUiHandle handle, UiRect, uint32_t, uint32_t, int32_t, const char* text,
                 uint32_t, int32_t, int32_t) {
    if (text != nullptr) assert(std::strcmp(static_cast<Control*>(handle)->text, text) == 0);
    return ok();
}
QolUiAdapter adapter() {
    QolUiAdapter ui{};
    ui.size = sizeof(ui); ui.major = kUiAdapterMajor; ui.minor = kUiAdapterMinor;
    ui.is_main_thread = &main_thread; ui.create_control = &create; ui.set_rect = &set_rect;
    ui.get_rect = &get_rect;
    ui.control_absolute_origin = &origin; ui.set_text = &set_text;
    ui.set_click_proc = &set_click; ui.set_draw_proc = &set_draw; ui.delete_control = &remove_control;
    ui.scroll_create = &create_scroll; ui.scroll_set_target = &connect; ui.button_set_scroll = &connect;
    ui.scroll_process = &process; ui.scroll_draw = &draw_scroll; ui.draw_control = &draw_control;
    ui.control_tree_event = &event; ui.draw_border_rect = &container; ui.image_group = &image_group;
    ui.image_loc = &image_loc; ui.image_loc_size = &image_size; ui.draw_part = &part;
    ui.begin_frame = &no_op; ui.end_frame = &no_op;
    ui.screen_inset_width = &inset; ui.screen_inset_height = &inset;
    ui.draw_text = &draw_text; ui.set_font_color = &color; ui.set_font_color_rgb = &color_rgb;
    ui.draw_string_with_font = &string;
    ui.fill_rect = &fill; ui.draw_custom_button = &button;
    ui.image_unit_load = &load; ui.image_unit_unload = &unload;
    return ui;
}
void draw_item(void* user, uint32_t, QolUiHandle, uint8_t selected) {
    assert(user == &g_live && g_clip); g_highlights.push_back(selected);
    if (g_destroy_on_draw != nullptr && !g_destroy_in_detail) {
        const int deletes = g_deletes;
        assert(succeeded(g_api->destroy(g_destroy_on_draw)) && deletes == g_deletes);
    }
}
void detail(void* user, UiRect rect) {
    assert(!g_clip); ++g_details; g_detail_index = *static_cast<int*>(user); g_detail_rect = rect;
    g_draw_order.emplace_back("detail");
    if (g_destroy_on_draw != nullptr && g_destroy_in_detail) {
        const int deletes = g_deletes;
        assert(succeeded(g_api->destroy(g_destroy_on_draw)) && deletes == g_deletes);
        QolUiHandle created = reinterpret_cast<void*>(1);
        assert(g_api->help_page_create(&g_recreate_desc, nullptr, &created).code == kQolErrNotReady);
        assert(created == nullptr);
    }
}
UiHelpPageDesc desc(UiHelpOption* options) {
    return {{24, 100, 912, 408}, {20, 24, 244, 352}, {320, 24, 568, 352}, 64,
            options, 8, 2, &draw_item, &g_live};
}
QolResult register_provider(const PanelProviderSpec* spec) { g_provider = spec; return ok(); }
QolResult add_task(int32_t point, QolFrameHandler fn, void*, int32_t interval, int32_t count, uint64_t* out) {
    assert(point == 1 && interval == 1 && count == 0); g_frame = fn; *out = 42; return ok();
}
QolResult remove_task(uint64_t id) { assert(id == 42); ++g_removed; return ok(); }
int send(const char* body) {
    uint8_t buffer[256]{};
    HttpReq req{}; req.body = reinterpret_cast<const uint8_t*>(body); req.body_len = std::strlen(body);
    HttpRes res{}; res.body = buffer; res.body_capacity = sizeof(buffer);
    assert(succeeded(qol::features::core_ui::debug_help_http_command(&req, &res, nullptr)));
    return res.status;
}
Control* row(QolUiHandle page, uint32_t index) {
    return static_cast<Control*>(page)->children[0]->children[0]->children[index].get();
}
}  // namespace
namespace qol::features::core_ui {
bool ui_panels_open(const char* id) {
    assert(g_main && std::strcmp(id, "debug_help") == 0); ++g_opens;
    if (!g_provider->open()) return false;
    g_provider->callbacks->enter(); return true;
}
void ui_panels_close(const char* id) {
    assert(g_main && std::strcmp(id, "debug_help") == 0);
    if (g_provider != nullptr && g_provider->active()) g_provider->close();
}
// 宿主 back 请求：真实宿主经帧任务在弹窗回调之外执行。测试只置标志，
// 由 main 在回调返回后调用 flush_back_request() 应用（模拟帧边界）。
void ui_panels_request_back() { g_back_requested = true; }
int ui_panels_depth() { return 1; }
}  // namespace qol::features::core_ui

namespace {
void flush_back_request() {
    if (!g_back_requested) return;
    g_back_requested = false;
    if (g_main && g_provider != nullptr && g_provider->active()) g_provider->close();
}
}  // namespace

int main() {
    using namespace qol::features::core_ui;
    int checks = 0;
#define CHECK(expr) do { assert(expr); ++checks; } while (false)
    static_assert(offsetof(QolUiComponents, help_page_create) ==
        offsetof(QolUiComponents, dispatch_native_event) + sizeof(void*));
    auto ui = adapter();
    CHECK(ui_components_bind(&ui)); g_api = ui_components_api();
    CHECK(g_api->minor == qol::contracts::kUiContractMinor && kUiAdapterMinor == 21);
    UiHelpOption options[8]{};
    for (int i = 0; i < 8; ++i) options[i] = {&g_users[i], &detail};
    auto page_desc = desc(options);
    QolUiHandle borrowed = nullptr;
    CHECK(succeeded(create(0, nullptr, &borrowed)));
    CHECK(succeeded(set_rect(borrowed, {10, 12, 960, 640})));
    CHECK(succeeded(g_api->help_page_create(&page_desc, borrowed, &g_page)));
    CHECK(g_live == 12 && static_cast<Control*>(g_page)->kind == 3);
    CHECK(static_cast<Control*>(g_page)->parent == borrowed && row(g_page, 7)->click != nullptr);
    int32_t selected = -1;
    CHECK(succeeded(g_api->help_page_get_selected(g_page, &selected)) && selected == 2);
    CHECK(succeeded(g_api->help_page_process(g_page)) && g_processes == 1);
    // 描述符与 options 数组均无需跨创建存活；回调/user 仍归消费者。
    options[2] = {nullptr, nullptr};
    CHECK(succeeded(g_api->help_page_draw(g_page)) && g_detail_index == 2);
    CHECK(g_frames.size() == 3 && g_frames[0].x == 34 && g_frames[0].y == 112);
    CHECK(g_frames[1].x == 46 && g_frames[1].y == 128 && g_frames[1].w == 276);
    CHECK(g_frames[2].x == 346 && g_frames[2].w == 584);
    CHECK(g_detail_rect.x == 354 && g_detail_rect.y == 136 && g_detail_rect.w == 568);
    CHECK(g_draw_order[g_draw_order.size() - 2] == "scroll" && g_draw_order.back() == "detail");
    CHECK(g_highlights.size() == 8 && g_highlights[2] == 1);
    CHECK(succeeded(g_api->help_page_set_selected(g_page, 6)));
    g_highlights.clear(); CHECK(succeeded(g_api->help_page_draw(g_page)));
    CHECK(g_detail_index == 6 && g_highlights[6] == 1 && g_highlights[2] == 0);
    uint32_t consumed = 0;
    g_execute = row(g_page, 7);
    CHECK(succeeded(g_api->dispatch_native_event(borrowed, 0x18, 10, 20, &consumed)) && consumed == 7);
    CHECK(succeeded(g_api->help_page_get_selected(g_page, &selected)) && selected == 7);
    CHECK(succeeded(g_api->help_page_draw(g_page)) && g_detail_index == 7);
    g_execute = nullptr;
    CHECK(succeeded(g_api->help_page_set_selected(g_page, -1)));
    const int details = g_details;
    CHECK(succeeded(g_api->help_page_draw(g_page)) && g_details == details);
    CHECK(g_api->help_page_set_selected(g_page, -2).code == kQolErrInvalidArgument);
    CHECK(g_api->help_page_set_selected(g_page, 8).code == kQolErrInvalidArgument);
    CHECK(g_api->help_page_get_selected(g_page, nullptr).code == kQolErrInvalidArgument);
    CHECK(g_api->set_rect(g_page, {}).code == kQolErrUnsupported);
    QolUiHandle nested = nullptr;
    options[2] = {&g_users[2], &detail};
    CHECK(g_api->help_page_create(&page_desc, g_page, &nested).code == kQolErrUnsupported);
    CHECK(g_api->help_page_create(&page_desc, static_cast<Control*>(g_page)->children[0].get(),
                                 &nested).code == kQolErrUnsupported);
    g_main = false;
    CHECK(g_api->help_page_create(&page_desc, borrowed, &nested).code == kQolErrNotReady);
    CHECK(g_api->help_page_process(g_page).code == kQolErrNotReady);
    CHECK(g_api->help_page_draw(g_page).code == kQolErrNotReady);
    CHECK(g_api->help_page_set_selected(g_page, 0).code == kQolErrNotReady);
    CHECK(g_api->help_page_get_selected(g_page, &selected).code == kQolErrNotReady);
    CHECK(g_api->destroy(g_page).code == kQolErrNotReady);
    g_main = true;
    int deletes = g_deletes;
    CHECK(succeeded(g_api->destroy(g_page)) && g_live == 1 && g_deletes == deletes + 1);
    CHECK(static_cast<Control*>(borrowed)->children.empty());
    // 每一步接线失败都释放 owned 子树；借用根不删除。
    // 接线步数：建 page/set_rect(2) + scroll_list 16 = 18（行不再绑定原生 draw_proc）。
    page_desc.option_count = 3;
    for (int failure = 1; failure <= 18; ++failure) {
        g_calls = 0; g_fail_at = failure;
        CHECK(!succeeded(g_api->help_page_create(&page_desc, borrowed, &g_page)) && g_page == nullptr);
        CHECK(g_live == 1 && static_cast<Control*>(borrowed)->children.empty());
    }
    g_fail_at = -1;
    g_calls = 0; g_fail_at = 4; g_fail_delete = true;
    CHECK(!succeeded(g_api->help_page_create(&page_desc, borrowed, &g_page)) && g_page == nullptr);
    g_fail_at = -1; g_fail_delete = false;
    CHECK(succeeded(g_api->help_page_create(&page_desc, borrowed, &g_page)));
    CHECK(succeeded(g_api->help_page_process(g_page)) && g_live == 7);
    g_fail_delete = true;
    CHECK(g_api->destroy(g_page).code == kQolErrNotReady && g_live == 7);
    g_fail_delete = false;
    CHECK(succeeded(g_api->destroy(g_page)) && g_live == 1);
    auto invalid = page_desc;
    invalid.option_count = 0; invalid.options = nullptr; invalid.initial_selected = -1; invalid.draw_item = nullptr;
    CHECK(succeeded(g_api->help_page_create(&invalid, nullptr, &g_page)));
    CHECK(static_cast<Control*>(g_page)->kind == 0);
    CHECK(succeeded(g_api->help_page_draw(g_page)));
    CHECK(succeeded(g_api->destroy(g_page)) && g_live == 1);
    invalid = page_desc; invalid.options = nullptr;
    CHECK(g_api->help_page_create(&invalid, borrowed, &g_page).code == kQolErrInvalidArgument);
    invalid = page_desc; invalid.initial_selected = 3;
    CHECK(g_api->help_page_create(&invalid, borrowed, &g_page).code == kQolErrInvalidArgument);
    invalid = page_desc; invalid.left_viewport.x = INT64_MAX;
    CHECK(g_api->help_page_create(&invalid, borrowed, &g_page).code == kQolErrInvalidArgument);
    invalid = page_desc; invalid.right_viewport.x = 30;
    CHECK(g_api->help_page_create(&invalid, borrowed, &g_page).code == kQolErrInvalidArgument);
    invalid = page_desc; invalid.option_count = 1025;
    CHECK(g_api->help_page_create(&invalid, borrowed, &g_page).code == kQolErrInvalidArgument);
    invalid = page_desc; invalid.item_height = INT32_MAX;
    CHECK(g_api->help_page_create(&invalid, borrowed, &g_page).code == kQolErrInvalidArgument);
    options[0].draw_detail = nullptr;
    CHECK(g_api->help_page_create(&page_desc, borrowed, &g_page).code == kQolErrInvalidArgument);
    options[0].draw_detail = &detail;
    // 左行回调排队 destroy(page)：不再画右内容，下一 process 一次递归释放。
    CHECK(succeeded(g_api->help_page_create(&page_desc, borrowed, &g_page)));
    g_destroy_on_draw = g_page; deletes = g_deletes;
    CHECK(succeeded(g_api->help_page_draw(g_page)) && g_deletes == deletes);
    g_destroy_on_draw = nullptr;
    CHECK(g_api->help_page_process(g_page).code == kQolErrNotFound && g_live == 1);
    // detail 回调同样不得同步删除；禁止有效描述符的重入创建。
    page_desc = desc(options);
    CHECK(succeeded(g_api->help_page_create(&page_desc, borrowed, &g_page)));
    g_destroy_on_draw = g_page; g_destroy_in_detail = true; g_recreate_desc = page_desc;
    deletes = g_deletes;
    CHECK(succeeded(g_api->help_page_draw(g_page)) && g_deletes == deletes);
    g_destroy_on_draw = nullptr; g_destroy_in_detail = false;
    CHECK(g_api->help_page_process(g_page).code == kQolErrNotFound && g_live == 1);
    CHECK(succeeded(remove_control(borrowed)) && g_live == 0);

    QolHostSdk sdk{}; sdk.ui = &ui; sdk.panels.register_provider = &register_provider;
    sdk.scheduler.add = &add_task; sdk.scheduler.remove = &remove_task;
    CHECK(debug_help_register(&sdk));
    CHECK(std::strcmp(g_provider->panel_id, "debug_help") == 0);
    CHECK(send("{\"open\":true}") == 503);
    debug_help_set_enabled(true);
    CHECK(send("{\"open\":1}") == 400);
    CHECK(send("{\"open\":truejunk}") == 400);
    CHECK(send("{\"open\":true,\"other\":false}") == 400);
    g_main = false;
    CHECK(send("{ \"open\" : true }") == 200 && g_live == 0 && g_opens == 0);
    CHECK(g_frame(1, nullptr) == 1 && g_opens == 0);
    g_main = true;
    CHECK(g_frame(2, nullptr) == 1 && g_provider->active() && g_live == 16 &&
          g_atlases.unit(kFrameImageUnit).loaded);
    const int frame_generation = static_cast<int>(g_atlases.unit(kFrameImageUnit).generation);
    g_provider->callbacks->process();
    auto* popup = g_roots.front().get();
    g_execute = row(popup->children[0].get(), 7);
    CHECK(g_provider->callbacks->event(0x18, 10, 20) == 7);
    g_execute = nullptr; g_provider->callbacks->process();
    // 返回按钮 ExecuteProc → 宿主 back（测试桩同步关闭）。
    g_execute = popup->children.back().get();
    CHECK(g_provider->callbacks->event(0x18, 10, 20) == 7);
    g_execute = nullptr;
    flush_back_request();  // 帧边界应用延迟关闭
    // 关闭后 0x0f 仍加载、未被物理卸载：仍存活的原生消费者可继续绘制。
    CHECK(!g_provider->active() && g_live == 0 && g_atlases.native_draw(g_native_frame));
    CHECK(send("{\"open\":true}") == 200); g_frame(3, nullptr);
    // f3 → 宿主 back（帧边界应用）。
    g_provider->callbacks->f3();
    flush_back_request();
    // UnitLoad 幂等：反复打开页面不重复加载、不增引用。
    CHECK(g_live == 0 && !g_provider->active());
    CHECK(g_atlases.unit(kFrameImageUnit).generation == frame_generation);
    CHECK(send("{\"open\":true}") == 200); g_frame(4, nullptr);
    g_provider->callbacks->f4();
    flush_back_request();
    CHECK(g_live == 0 && !g_provider->active());
    CHECK(send("{\"open\":true}") == 200 && send("{\"open\":false}") == 200);
    g_frame(5, nullptr); CHECK(g_live == 0);
    CHECK(send("{\"open\":true}") == 200); g_frame(6, nullptr);
    debug_help_set_enabled(false); CHECK(g_live == 16);
    g_frame(7, nullptr);
    CHECK(g_live == 0 && g_atlases.native_draw(g_native_frame));
    debug_help_unregister(&sdk); CHECK(g_removed == 1);
    // 模块整个生命周期对共享图集的物理卸载次数为 0。
    CHECK(g_atlases.protected_unloads() == 0);
    CHECK(succeeded(create(0, nullptr, &borrowed)));
    CHECK(succeeded(g_api->help_page_create(&page_desc, borrowed, &g_page)));
    ui_components_unbind(); CHECK(g_live == 1);
    CHECK(succeeded(remove_control(borrowed)) && g_live == 0);
    std::printf("help_page checks: %d\n", checks);
    return 0;
#undef CHECK
}
