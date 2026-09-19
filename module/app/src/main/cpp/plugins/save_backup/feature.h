#pragma once

#include <cstdint>
#include <string>

#include "contracts/game_host.h"
#include "contracts/host_sdk.h"

// 存档管理器备份 feature（save-backup）：
// .qol_save bundle 的导出/导入/列表/删除全部收口本层（文件 IO + 游戏加解密 + 事务回滚），
// 返回值即 HTTP 响应 JSON（{"ok":true,...} / {"ok":false,"error":"..."}）。
// 依赖方向：feature → core（op_err 信封）+ Host SDK（sidecar 容器）+ Game Host
// （存档域原语）；不依赖 api/native。插件清单位于 plugins/save_backup/plugin.cpp。

// 幂等初始化：记录 dataDir（原版存档根目录）与 externalFilesDir（备份/sidecar 根目录）。
void save_backup_init(const char* data_dir, const char* external_files_dir);

// 插件装配期注入 Host SDK / Game Host；业务回调通过该上下文访问宿主适配面。
void save_backup_bind_context(const qol::contracts::QolHostSdk* sdk,
                              const qol::contracts::QolGameHost* game);
const qol::contracts::QolHostSdk* save_backup_host_sdk();
const qol::contracts::QolGameHost* save_backup_game_host();

// 启动期 Kotlin 一次性下发 MAPINFOBASE 编译产物（map_id → 中文名 JSON）。
// 内部按需替换内存表，entry_json 输出会携带 map_name；map_name 缺省空串。
// 空串视为清空（用于解绑/卸载场景）；非法 JSON 容错为空表。
void save_backup_set_map_names(const char* json);

// map_id → 中文名查询（上表内存）；未命中返回空串。
// 供 UI 层显示存档槽/备份条目地图名，避免上层复制一份映射表。
std::string save_backup_map_name(int map_id);

// class_idx（0-5）→ 职业中文名；非法/未知返回空串。表为编译期常量（无锁、无共享状态），
// 可在持有任意锁的 UI DrawProc/后台解析路径安全调用。名称口径与游戏 CHARCLASSBASE 文本一致
// （0=黑暗骑士 1=忍者 2=黑魔导 3=祭司 4=暗影猎手 5=狂战士）。
const char* save_backup_class_name(int class_idx);

// GET /api/system/backup/list 响应体。
std::string save_backup_list_json();

// POST /api/system/backup/export 响应体（去重命中时额外带 "deduplicated":true）。
std::string save_backup_export_json(int slot);

// POST /api/system/backup/import 响应体（按 checksum 定位，还原 save{slot}.dat + sidecar）。
std::string save_backup_import_json(const char* checksum, int slot);

// POST /api/system/backup/delete 响应体。
std::string save_backup_delete_json(const char* checksum);

// 游戏删档回调（SaveSlot_Delete）PtrHook wrapper 的 sidecar 同步体：
// 游戏内存档面板删档确认后，按槽删除模块 sidecar（slot-{n}.module-save + .last-good）并标记槽区 stale。
// 由插件删档 PtrHook wrapper 在原函数返回后调用（锁序 g_sb_mtx → store）；slot 越界直接返回。
void save_backup_slot_deleted_sync(int32_t slot);

// 删除指定槽的游戏存档（原版 save{slot}.dat + 模块 sidecar slot-{slot}.module-save/.last-good）。
// 只作用于该 slot 的精确文件名，不触碰其它槽；未找到任何目标文件返回结构化错误。
// 与 save_backup_delete_json 同风格（HTTP 风格 {"ok":true,...}/{"ok":false,"error":"..."}）。
std::string save_backup_delete_slot_json(int slot);
