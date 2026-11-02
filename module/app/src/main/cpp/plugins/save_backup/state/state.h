#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

#include "contracts/game_host.h"
#include "contracts/host_sdk.h"
#include "contracts/save_facts.h"
#include "contracts/world_facts.h"
#include "contracts/game_ports/save_call_port.h"
#include "plugins/save_backup/model/types.h"

// save_backup L1：运行期上下文与不可变查询快照。
// 写侧只允许主线程（install / RenderPre / 面板 enter）；读侧拿 shared_ptr 快照，
// 不直接触碰 live 状态。这样 HTTP/UI 查询不需要业务互斥量。

namespace save_backup {
namespace state {

struct Context {
    const qol::contracts::QolHostSdk* sdk = nullptr;
    const qol::contracts::SaveFactsApi* save_facts = nullptr;
    const qol::contracts::QolSaveCallPort* save_call = nullptr;
    const qol::contracts::QolGameHost* game = nullptr;
    const qol::contracts::WorldFactsApi* world_facts = nullptr;
    qol::contracts::QolOwnerToken owner = 0;
    std::string data_dir;
    std::string external_dir;
};

struct SlotMeta {
    bool valid = false;
    int hero_level = -1;
    int hero_index = -1;
    int class_idx = -1;
};

// 原子快照：bind/unbind 与目录更新只在装配/初始化阶段发生。
Context snapshot();
void bind(Context context);
void unbind();
void setDirs(const std::string& data_dir, const std::string& external_dir);

// map_id → 中文名。启动期一次写入，之后只读；未命中返回空串。
void setMapNames(const std::string& json);
std::string mapName(int map_id);
std::unordered_map<int, std::string> mapNamesSnapshot();

// 槽元数据快照：RenderPre 主线程发布，HTTP/绘制线程读取。
void publishSlotMeta(const std::array<SlotMeta, 3>& meta);
std::array<SlotMeta, 3> slotMeta();
// 单槽读取；无效槽返回全 -1。
SlotMeta slotMeta(int slot);
// 游戏主线程刷新 0..2 槽的 exists/hero_index/hero_level/hero_class 快照。
void refreshSlotMetaOnGameThread();

// 职业索引 0-5 → 中文名；非法/未知返回 nullptr。
const char* className(int class_idx);

}  // namespace state
}  // namespace save_backup
