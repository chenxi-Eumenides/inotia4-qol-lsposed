#include "plugins/text_layer/charinfo_scope.h"

#include "infra/text_scope.h"

namespace qol::features::text_layer {
namespace {

// 原函数 ABI（Scene_Draw_POPUP_SC_CHARACTER_INFO；与 feature.cpp 同规则，不引入符号头）。
using SceneDrawCharinfoFn = void (*)();

// LSPosed native hook 写入的原始函数指针 backup（宿主经 on_resource_ready 交还）。
SceneDrawCharinfoFn g_backup_scene_draw_charinfo = nullptr;

}  // namespace

// 面板每帧绘制入口：进出各写一次作用域（守卫析构时恢复上一层），把整个绘制窗口标记为
// kCharacterPanel。文案与 id 表由 text_layer 持有 —— 这里只负责「画的是哪个窗口」。
void scene_draw_charinfo_wrapper() {
    qol::infra::TextScopeGuard scope(qol::infra::TextScope::kCharacterPanel);
    if (g_backup_scene_draw_charinfo != nullptr) g_backup_scene_draw_charinfo();
}

int32_t charinfo_scope_on_resource_ready(void* backup) {
    g_backup_scene_draw_charinfo = reinterpret_cast<SceneDrawCharinfoFn>(backup);
    return 0;
}

}  // namespace qol::features::text_layer
