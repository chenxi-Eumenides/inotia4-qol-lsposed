#pragma once

#include <cstdint>
#include <string>
#include <vector>

// save-backup 纯逻辑层：.qol_save bundle 编解码、摘要/校验原语、极简 metaJson 字段提取。
// 零游戏/Android 依赖（纯 STL，编入 host 单测）；文件 IO 与游戏函数调用在 save_backup.cpp。
//
// bundle 布局（大端，与 docs/development/features/save-backup.md §3 一致）：
// v1：
//   u32 magic=0x51534231 | u16 ver=1 | u8 sourceSlot | u8[3] rsvd |
//   u64 exportTimeMs | u32 origLen | origPlain | u32 moduleLen | module |
//   u16 metaLen | metaJson | u32 crc32（覆盖其前全部字节）
// v2（当前）在 module 与 metaJson 之间插入个人仓库段：
//   ... | u32 warehouseLen | warehouseBlob | u16 metaLen | metaJson | u32 crc32
// parse_bundle 同时接受 v1（无仓库段，has_warehouse=false）与 v2。

namespace save_backup {

constexpr uint32_t kBundleMagic = 0x51534231u;  // "QSB1"
constexpr uint16_t kBundleVersion = 2;          // 当前版本（含个人仓库段）
constexpr uint16_t kBundleVersionV1 = 1;        // 旧备份（无仓库段），parse 仍接受
// 头(18) + origLen(4) + moduleLen(4) + metaLen(2) + crc(4)；v1 最小值。
// v2 最小值为 36（多 u32 warehouseLen），由逐段边界检查自然拒绝过小输入。
constexpr size_t kMinBundleBytes = 32;

// ---- 个人仓库（monster 改版 save{slot}.dat.wh4-*）序列化上限 ----
constexpr size_t kMaxWarehouseFiles = 64;
constexpr size_t kMaxWarehouseSuffixBytes = 64;
constexpr size_t kMaxWarehouseDataBytes = 4 * 1024 * 1024;
constexpr size_t kMaxWarehouseBlobBytes = 8 * 1024 * 1024;

// 备份元信息（HTTP BackupMeta 逐字段对应；JSON 组装见 entry_json）。
struct Entry {
    std::string file_name;
    long long size_bytes = 0;
    int source_slot = 0;
    long long export_time_ms = 0;
    int map_id = 0;
    // 地图名称（Kotlin 启动期下发的 MAPINFOBASE 表 map_id→text_0；启动前/未命中为空串）。
    // 仅 entry_json 输出；bundle 字节布局未引入新字段，向后兼容旧 .qol_save。
    std::string map_name;
    int hero_level = -1;
    int hero_index = -1;
    // 主角职业：class_idx 0-5（-1=未知/旧备份）。class_name 为中文名（UI 展示用），
    // 由 save_backup.cpp 的职业表填充；bundle 解析只取 metaJson，旧备份缺失时保持 -1/空。
    int class_idx = -1;
    std::string class_name;
    int save_version = 0;
    long long save_time = 0;
    std::string original_sha256;
    std::string module_sha256;
    std::string checksum;
};

// 个人仓库伴生文件：suffix 为相对 `save{slot}.dat` 的后缀（含前导点），
// 如 ".wh4-000001a043a2bba1" 或 ".wh4-000001a043a2bba1.bak"；data 为原始文件字节。
struct WarehouseFile {
    std::string suffix;
    std::vector<uint8_t> data;
};

// bundle 解析产物（逐段边界 + CRC 校验通过后）。
struct ParsedBundle {
    int source_slot = 0;
    long long export_time_ms = 0;
    std::vector<uint8_t> orig_plain;
    std::vector<uint8_t> module;
    // 个人仓库：v2 bundle 才置 has_warehouse=true（即使仓库列表为空）；
    // v1 旧备份 has_warehouse=false、warehouse 为空——导入侧据此完全跳过仓库写回。
    std::vector<WarehouseFile> warehouse;
    bool has_warehouse = false;
    std::string meta_json;
    long long size_bytes = 0;
};

// ---- 摘要与编码原语 ----

// sha256(a ‖ b) 的小写 hex（b 允许为空）。
std::string sha256_hex(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b);
// md5 的小写 hex（存档目录名 hex(MD5(key)) 用；与 java MessageDigest MD5 一致）。
std::string md5_hex(const uint8_t* data, size_t len);
// CRC32（IEEE，多项式 0xEDB88320 反射；与 java.util.zip.CRC32 一致）。
uint32_t crc32_ieee(const uint8_t* data, size_t len);
// RFC4648 base64（带 '=' 填充）。
std::string base64_encode(const uint8_t* data, size_t size);
// 严格解码：长度为 4 的倍数，'=' 仅允许结尾 0-2 个，非法字符拒绝。
bool base64_decode(const std::string& in, std::vector<uint8_t>& out);

// ---- 个人仓库段编解码 ----

// 编码为 warehouseBlob：u16 count + 每项（u16 suffixLen | suffix | u32 dataLen | data）。
// **空列表编码为空 vector（size 0）**：保证无仓库时 bundle 的 checksum 与旧版逐字节一致，
// 且 v2 的 warehouse 段长度字段为 0（不引入任何额外摘要输入）。
std::vector<uint8_t> encode_warehouse(const std::vector<WarehouseFile>& files);
// 解码：size==0 成功且 out 为空；否则 size>=2，逐项校验 suffixLen/dataLen/边界与后缀合法性，
// 任何越界或不合法返回 false（fail-closed）。
bool decode_warehouse(const uint8_t* data, size_t size, std::vector<WarehouseFile>& out);
// 合法后缀：长度 5..64、`.wh4-` 开头、字符仅 ASCII 字母数字与 `.`/`-`、不含 `..`。
// （`.wh4-` 与 `.bak` 等固定字面量含 hex 之外的 w/h/k，故白名单放宽到字母数字，仍拒绝
// 路径分隔符与遍历序列。）
bool valid_warehouse_suffix(const std::string& s);

// ---- bundle 组装与解析 ----

// metaJson（字段顺序遵循 Kotlin 历史实现，扁平数字/字符串；class_idx/class_name 追加，
// warehouse_count/warehouse_sha256 再追加在末尾；旧备份缺失时走默认值，不影响既有解析）。
std::string build_meta_json(int source_slot, long long export_time_ms, int map_id, int hero_level,
                            int hero_index, int save_version, long long save_time,
                            const std::string& original_sha256, const std::string& module_sha256,
                            const std::string& checksum, int class_idx = -1,
                            const std::string& class_name = std::string(),
                            int warehouse_count = 0,
                            const std::string& warehouse_sha256 = std::string());

// v2 组装：warehouse_blob 位于 module 与 meta_json 之间（空 blob 表示无仓库）。
void build_bundle(int source_slot, long long export_time_ms, const std::vector<uint8_t>& plain,
                  const std::vector<uint8_t>& module,
                  const std::vector<uint8_t>& warehouse_blob, const std::string& meta_json,
                  std::vector<uint8_t>& out);

// 逐段边界校验 + crc32 验证；失败返回 false（调用方跳过/报错）。
bool parse_bundle(const uint8_t* bytes, size_t size, ParsedBundle& out);

// BackupMeta JSON（字段顺序与 Kotlin BackupMeta.toJson 一致；值均为 ASCII，无转义需求）。
std::string entry_json(const Entry& e);

// 从解析结果取 meta：bundle 头提供 source_slot/export_time/size；
// metaJson 提供其余字段（缺省值与 Kotlin 一致）；checksum 以 metaJson 为权威、
// 文件名 `_([0-9a-f]{12}).qol_save` 后缀兜底。
void entry_from_bundle(const std::string& file_name, const ParsedBundle& p, Entry& out);

// ---- 极简扁平 JSON 字段提取器（值只有数字与带引号字符串）----

bool json_find_int(const std::string& json, const char* key, long long& out);
bool json_find_string(const std::string& json, const char* key, std::string& out);

// ^[0-9a-f]{12}$
bool valid_checksum(const std::string& value);
// 从文件名后缀 `_([0-9a-f]{12}).qol_save` 提取 checksum。
bool extract_checksum_from_name(const std::string& file_name, std::string& out);

// ---- 模块 sidecar 容器（MSAV）最小操作（不解析 section）----
//
// 容器布局（大端）：u32 magic "MSAV" | u16 ver | u8 slot | u64 generation |
// u16 sectionCount | [sections...] | u32 crc32(body = 除末 4 字节)。

// 最小校验：大小/上限 + magic + 尾部 CRC32。
bool module_container_valid(const std::vector<uint8_t>& bytes);
// 导入重定槽：源容器最小校验通过后，把第 6 字节写为目标槽并重算尾部 CRC32。
bool module_container_reslot(std::vector<uint8_t>& bytes, int slot);

}  // namespace save_backup
