#pragma once

#include <cstdint>
#include <string>

// 系统聚合域（parse 域，唯一允许 include 其他域头的聚合域）：
// 帧计数 + 初始化报告 + 事件流（snapshot diff）。

// 快照结构：events 轮询 diff + build_snapshot_json 共用
struct Snapshot {
    int64_t money;
    int16_t x, y;
    int32_t hp[3], mp[3], level[3];
    int64_t exp[3];
    int inv_count;
};

int64_t data_frame_count();
std::string data_init_report();
std::string data_events_json();

// 数据读取（read 域拆分）：游戏状态/快照 JSON 构造。
std::string build_gamestate_json();
std::string build_snapshot_json();

// core.system owner 用的富化快照：与迁移前 Kotlin NameInjector.withItemNames 对
// build_snapshot_json 输出的加工逐字节一致（party 成员注入 type_name/class_name 并按
// restructureRole 重排）。原始 build_snapshot_json 保留给 /api/system/game。
std::string build_snapshot_enriched_json();
