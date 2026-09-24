#include "charinfo_scope.h"

#include "feature.h"
#include "plugin.h"

namespace qol::features::character_panel_i18n {
namespace {

// 原函数 ABI（Scene_Draw_POPUP_SC_CHARACTER_INFO；与 feature.cpp 同规则，不引入符号头）。
using SceneDrawCharinfoFn = void (*)();

// LSPosed native hook 写入的原始函数指针 backup（宿主经 on_resource_ready 交还）。
SceneDrawCharinfoFn g_backup_scene_draw_charinfo = nullptr;

}  // namespace

// 面板每帧绘制入口：进出各写一次作用域（守卫析构时恢复上一层），把整个绘制窗口标记为
// kCharacterPanel。文案与 id 表由 text_layer 持有 —— 这里只负责「画的是哪个窗口」。
void scene_draw_charinfo_wrapper() {
    const contracts::QolHostSdk* sdk = host_sdk();
    const uint8_t previous_scope = active_scope_value();
    const bool pushed = sdk != nullptr && sdk->text_scope.push != nullptr &&
        contracts::succeeded(sdk->text_scope.push(1));
    if (pushed) set_active_scope_value(1);
    if (g_backup_scene_draw_charinfo != nullptr) g_backup_scene_draw_charinfo();
    if (pushed) set_active_scope_value(previous_scope);
    if (pushed && sdk != nullptr && sdk->text_scope.pop != nullptr) {
        (void)sdk->text_scope.pop();
    }
}

int32_t charinfo_scope_on_resource_ready(void* backup) {
    g_backup_scene_draw_charinfo = reinterpret_cast<SceneDrawCharinfoFn>(backup);
    return 0;
}

}  // namespace qol::features::character_panel_i18n
