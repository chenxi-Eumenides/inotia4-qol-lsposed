#pragma once

#include <cstdint>

// 人物属性总览面板（panel_id=`character_panel.overview`，owner=`character_panel`）。
//
// 由原版人物属性面板内容区的点击经 `charinfo_watch.*` 触发，走 core.custom_ui 的
// `ui_panels` 能力打开：PopupState 占用、入栈、process_info 与帧封装仍由共享 PopupHost
// 负责，本文件只提供 provider 判定与页面回调。
//
// 页面数据只在面板打开（enter）时读取一次：角色指针优先 `menu_character`（原版面板
// 同源 PARTY_GetMenuCharacter），失败/空指针回退 `party_member(0)`；主属性/面板派生值/
// attr 全量经 `QolCharacterReadApi` 读取。任一行读取失败只让该行显示 `--`。
//
// 布局只用 ui_components 的组件与文本绘制接口；分页用标准导航盒 + 左右箭头按钮，
// 翻页只改页码并重绘，不重建控件树。

namespace qol::contracts {
struct QolGameHost;
}  // namespace qol::contracts

namespace qol::features::character_panel {

// 面板 id（`ui_panels` 打开与 provider 注册共用同一字面量）。
constexpr const char* kOverviewPanelId = "character_panel.overview";

// 安装期绑定 core.custom_ui 的 ui_components 能力；deactivate 传 nullptr 解绑。
void overview_bind_components(const void* ui_components);

// 安装期绑定 Game Host 与 CHARACTER_EXTRAS 端口（角色选择/队伍槽解析）；deactivate 传 nullptr 解绑。
void overview_bind_context(const qol::contracts::QolGameHost* game,
                           const void* character_extras_port);

// 安装期绑定 character.attributes 能力；deactivate 传 nullptr 解绑。
void overview_bind_attributes(const void* attributes);

// 选中角色 → 队伍槽 0..2（menu_character → party_index → party_member 指针比对 → 0）。
int32_t overview_resolve_party_role();

// enter 时刷新一次属性快照（生产路径）。
void overview_refresh_snapshot();

// 测试/诊断：读取当前快照的某一行（section 为 kSections 下标）。
struct OverviewRowProbe {
    int32_t value;
    uint8_t ok;
    uint8_t learned;
};
bool overview_probe_row(uint32_t section, uint32_t row, OverviewRowProbe* out);

// 用户开关 `character_panel.overview` 的运行时门禁：关闭后原版面板点击不再打开总览。
void set_overview_enabled(bool enabled);
bool overview_enabled();

// ---- PanelProviderSpec 实现 ----
bool overview_provider_open();
void overview_provider_close();
bool overview_provider_active();
const char* overview_provider_block_reason();

// ---- PanelCallbacks（core.custom_ui PopupHost 托管）----
void overview_panel_enter();
void overview_panel_process();
void overview_panel_f3();
void overview_panel_f4();
uint32_t overview_panel_event(uint64_t event, uint64_t param, uint64_t param2);

}  // namespace qol::features::character_panel
