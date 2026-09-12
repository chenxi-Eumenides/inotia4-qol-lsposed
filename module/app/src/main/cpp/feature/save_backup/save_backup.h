#pragma once

#include <string>

// 存档管理器备份 feature（save-backup）：
// .qsb bundle 的导出/导入/列表/删除全部收口本层（文件 IO + 游戏加解密 + 事务回滚），
// 返回值即 HTTP 响应 JSON（{"ok":true,...} / {"ok":false,"error":"..."}）。
// 依赖方向：feature → core（op_err 信封）+ data（game_access fn 指针）；不依赖 api/native。

// 幂等初始化：记录 dataDir（原版存档根目录）与 externalFilesDir（备份/sidecar 根目录）。
void save_backup_init(const char* data_dir, const char* external_files_dir);

// 启动期 Kotlin 一次性下发 MAPINFOBASE 编译产物（map_id → 中文名 JSON）。
// 内部按需替换内存表，entry_json 输出会携带 map_name；map_name 缺省空串。
// 空串视为清空（用于解绑/卸载场景）；非法 JSON 容错为空表。
void save_backup_set_map_names(const char* json);

// GET /api/system/backup/list 响应体。
std::string save_backup_list_json();

// POST /api/system/backup/export 响应体（去重命中时额外带 "deduplicated":true）。
std::string save_backup_export_json(int slot);

// POST /api/system/backup/import 响应体（按 checksum 定位，还原 save{slot}.dat + sidecar）。
std::string save_backup_import_json(const char* checksum, int slot);

// POST /api/system/backup/delete 响应体。
std::string save_backup_delete_json(const char* checksum);
