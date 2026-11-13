#include "../plugin.h"
#include "../../../contracts/ui.h"
#include "../../../contracts/ui_theme.h"
#include <atomic>

namespace qol::features::item_lab {
namespace {
contracts::QolUiAnchoredButton g_entry{};
const contracts::QolUiComponents* components() {
    static const contracts::QolUiComponents* cached=nullptr;
    if(cached||!host_sdk()||!host_sdk()->capabilities.require)return cached;
    const void* out=nullptr;
    if(contracts::succeeded(host_sdk()->capabilities.require(contracts::kUiComponentsCapabilityId,
        {contracts::kUiContractMajor,contracts::kUiContractMinor},&out)))cached=static_cast<const contracts::QolUiComponents*>(out);
    return cached;
}
void clicked(void*) { item_lab_construct_all(); }
QolResult layout(void* parent,contracts::UiRect* out,void*) {
    const auto* ui=host_sdk()?host_sdk()->ui:nullptr;
    if(!parent||!out||!ui||!ui->control_absolute_origin||!ui->screen_inset_width||!ui->screen_inset_height)return contracts::error(kQolErrNotReady,"item_lab layout",0);
    int64_t px=0,py=0;int32_t inset_w=0,inset_h=0;
    if(!contracts::succeeded(ui->control_absolute_origin(parent,&px,&py))||
       !contracts::succeeded(ui->screen_inset_width(&inset_w))||
       !contracts::succeeded(ui->screen_inset_height(&inset_h)))return contracts::error(kQolErrNotReady,"item_lab layout",0);
    // 与自动出售入口同一逻辑屏宽口径：设计基准 960x640 + 两侧 inset。
    int64_t logical_w=0x3c0;
    const int64_t screen_w=0x3c0+static_cast<int64_t>(inset_w)*2;
    const int64_t screen_h=0x280+static_cast<int64_t>(inset_h)*2;
    if(screen_w>0&&screen_h>0)logical_w=screen_w*0x280/screen_h;
    constexpr int64_t size=68;
    const int64_t xabs=logical_w-qol::contracts::ui_theme::kNavBoxAbsX-
        qol::contracts::ui_theme::kNavBoxW+
        (qol::contracts::ui_theme::kNavBoxW-size)/2-(size+8);
    const int64_t yabs=qol::contracts::ui_theme::kNavBoxAbsY+
        (qol::contracts::ui_theme::kNavBoxH-size)/2;
    *out={xabs-px,yabs-py,size,size};return contracts::ok();
}
void draw_button() {
    const auto* c=components(); if(!c)return;
    if(!item_lab_enabled()){if(c->anchored_button_set_visible)(void)c->anchored_button_set_visible(&g_entry,0);return;}
    if(!c->anchored_button_ensure||!c->anchored_button_draw)return;
    const contracts::UiIconButtonDesc button{{0,0,68,68},0x16,0x00,1,1,"造",&clicked,nullptr,0};
    const contracts::UiAnchoredButtonDesc desc{static_cast<uint32_t>(contracts::QolUiRootKind::kEquipBagContainer),button};
    uint8_t created=0;
    if(contracts::succeeded(c->anchored_button_ensure(&g_entry,&desc,&layout,nullptr,&created)))
        (void)c->anchored_button_draw(&g_entry);
}
}
void item_lab_equip_roll_dice_draw_wrapper() {
    const auto* sdk=host_sdk();
    if(sdk&&sdk->ui&&sdk->ui->roll_dice_draw)(void)sdk->ui->roll_dice_draw();
    draw_button();
}
}
