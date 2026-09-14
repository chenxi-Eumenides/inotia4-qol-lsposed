#pragma once

// 世界传送 4 选项：拦截地图名横幅尾部，改为原生 UICHOICE 面板。
// 安装必须在 libgame.so 映射完成且 bridge_ready() 后调用；失败保持原版执行流。
bool world_teleport_install_if_ready();
