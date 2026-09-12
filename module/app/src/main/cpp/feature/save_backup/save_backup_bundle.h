#pragma once

#include <cstdint>
#include <string>
#include <vector>

// save-backup 纯逻辑层：.qsb bundle 编解码、摘要/校验原语、极简 metaJson 字段提取。
// 零游戏/Android 依赖（纯 STL，编入 host 单测）；文件 IO 与游戏函数调用在 save_backup.cpp。
//
// bundle 布局 v1（大端，与 docs/development/features/save-backup.md §3 一致）：
//   u32 magic=0x51534231 | u16 ver=1 | u8 sourceSlot | u8[3] rsvd |
//   u64 exportTimeMs | u32 origLen | origPlain | u32 moduleLen | module |
//   u16 metaLen | metaJson | u32 crc32（覆盖其前全部字节）

namespace save_backup {

constexpr uint32_t kBundleMagic = 0x51534231u;  // "QSB1"
constexpr uint16_t kBundleVersion = 1;
// 头(18) + origLen(4) + moduleLen(4) + metaLen(2) + crc(4)
constexpr size_t kMinBundleBytes = 32;

// 备份元信息（HTTP BackupMeta 逐字段对应；JSON 组装见 entry_json）。
struct Entry {
    std::string file_name;
    long long size_bytes = 0;
    int source_slot = 0;
    long long export_time_ms = 0;
    int map_id = 0;
    // 地图名称（Kotlin 启动期下发的 MAPINFOBASE 表 map_id→text_0；启动前/未命中为空串）。
    // 仅 entry_json 输出；bundle 字节布局未引入新字段，向后兼容旧 .qsb。
    std::string map_name;
    int hero_level = -1;
    int hero_index = -1;
    int save_version = 0;
    long long save_time = 0;
    std::string original_sha256;
    std::string module_sha256;
    std::string checksum;
};

// bundle 解析产物（逐段边界 + CRC 校验通过后）。
struct ParsedBundle {
    int source_slot = 0;
    long long export_time_ms = 0;
    std::vector<uint8_t> orig_plain;
    std::vector<uint8_t> module;
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

// ---- bundle 组装与解析 ----

// metaJson（字段顺序与 Kotlin 历史实现一致，扁平数字/字符串）。
std::string build_meta_json(int source_slot, long long export_time_ms, int map_id, int hero_level,
                            int hero_index, int save_version, long long save_time,
                            const std::string& original_sha256, const std::string& module_sha256,
                            const std::string& checksum);

void build_bundle(int source_slot, long long export_time_ms, const std::vector<uint8_t>& plain,
                  const std::vector<uint8_t>& module, const std::string& meta_json,
                  std::vector<uint8_t>& out);

// 逐段边界校验 + crc32 验证；失败返回 false（调用方跳过/报错）。
bool parse_bundle(const uint8_t* bytes, size_t size, ParsedBundle& out);

// BackupMeta JSON（字段顺序与 Kotlin BackupMeta.toJson 一致；值均为 ASCII，无转义需求）。
std::string entry_json(const Entry& e);

// 从解析结果取 meta：bundle 头提供 source_slot/export_time/size；
// metaJson 提供其余字段（缺省值与 Kotlin 一致）；checksum 以 metaJson 为权威、
// 文件名 `_([0-9a-f]{12}).qsb` 后缀兜底。
void entry_from_bundle(const std::string& file_name, const ParsedBundle& p, Entry& out);

// ---- 极简扁平 JSON 字段提取器（值只有数字与带引号字符串）----

bool json_find_int(const std::string& json, const char* key, long long& out);
bool json_find_string(const std::string& json, const char* key, std::string& out);

// ^[0-9a-f]{12}$
bool valid_checksum(const std::string& value);
// 从文件名后缀 `_([0-9a-f]{12}).qsb` 提取 checksum。
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
