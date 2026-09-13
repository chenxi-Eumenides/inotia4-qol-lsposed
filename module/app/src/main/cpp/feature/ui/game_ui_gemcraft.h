#pragma once

// 合成器宝石合成操作优化（gem-craft-optimization）阶段1 native 宿主：
//   功能 A 放料解绑：覆盖 desc 放入按钮 ExecuteProc（UIMix_ButtonInvenItemSelectExe），
//            放料前把 stuffList[0]（配方材料档位）临时改写为当前选中宝石的 category，
//            绕过原版档位比较；非宝石/重复/未选中/UIDesc_SetOff 等其余行为仍由原函数执行。
//            调用后比较前后已填格数，增加则自动选中下一空格（全满 -1）。
//   功能 B 自动选中格（全部为「调用链终点挂钩」，无逐帧 tick）：
//            - 进入视图：覆盖菜单按钮 ExecuteProc（UIMix_ButtonMenuListExe）与配方按钮
//              ExecuteProc（UIMix_ButtonRecipeExe）——二者尾部本写 [+0x128]=-1，wrapper 调
//              原函数后改为选中第一个空格。
//            - 合成清空：BL patch UIMix_StartMix 内 bl UIMix_ResetStuffItemControl 调用点，
//              复刻清空后选中第一个空格。
//            - 手动点击填入格只走 UIMix_StuffItemControlEventProc，不触发本模块逻辑。
//   阶段2 合成行为：覆盖合成按钮 ExecuteProc（UIMix_ButtonMixingExe）——填满且三格同档
//            28..31 时前置改写 mixType=12+(category-28) 并调 UIMix_InitMixingState 重算
//            stuffList/费用，再转调原函数（原版确认/扣料/产物语义不变）；填满但混档/混沌/
//            非宝石段时用原生提示框（文本项 0x62）拦截，不弹确认、不扣料、不产物；未填满
//            则转调原函数。
// 安装：4 个按钮 ExecuteProc 初值 GOT 槽（desc/menu/recipe/craft）一次性 PtrHook + 1 个
// craft 清空 BL patch，全部成功才就绪；失败可重试。禁用时不回滚指针，wrapper 由
// g_gemcraft_enabled 门控 no-op；启用时对当前已存在的按钮补挂一次。
bool set_gemcraft_enabled(bool enabled);
bool gemcraft_enabled();
bool gemcraft_install_if_ready();
