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
// v2 在 module 与 metaJson 之间插入个人仓库段：
//   ... | u32 warehouseLen | warehouseBlob | u16 metaLen | metaJson | u32 crc32
// v3（当前）把 warehouseBlob 的每项编码扩展为带 flags（明文/密文标志），其余分段不变：
//   item = u16 suffixLen | suffix | u32 dataLen | u8 flags | data
// parse_bundle 接受 v1（无仓库段）/v2（仓库段无 flags）/v3（仓库段带 flags）。

namespace save_backup {

constexpr uint32_t kBundleMagic = 0x51534231u;  // "QSB1"
constexpr uint16_t kBundleVersion = 3;          // 当前版本（仓库段带 flags，支持跨设备重加密）
constexpr uint16_t kBundleVersionV2 = 2;        // 旧备份（有仓库段、无 flags），parse 仍接受
constexpr uint16_t kBundleVersionV1 = 1;        // 更旧备份（无仓库段），parse 仍接受
// 头(18) + origLen(4) + moduleLen(4) + metaLen(2) + crc(4)；v1 最小值。
// v2/v3 最小值更大（多 u32 warehouseLen，v3 还多每项 flags），由逐段边界检查自然拒绝过小输入。
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
// flags 为 v3 才有的每项标志（v1/v2 解析结果恒为 0）。
struct WarehouseFile {
    std::string suffix;
    // v3 flags：bit0=1 表示 data 是**明文**（导出时已用源设备密钥解密，导入时需补 slot 后重加密）；
    // bit0=0 表示 data 是原始容器密文（源端无法解密时的兜底，导入沿用「可解密才写」）。
    uint8_t flags = 0;
    std::vector<uint8_t> data;
};

// v3 warehouseBlob flags 位定义。
constexpr uint8_t kWarehouseFlagPlain = 1u << 0;

// bundle 解析产物（逐段边界 + CRC 校验通过后）。
struct ParsedBundle {
    int source_slot = 0;
    long long export_time_ms = 0;
    std::vector<uint8_t> orig_plain;
    std::vector<uint8_t> module;
    // 个人仓库：v2/v3 bundle 才置 has_warehouse=true（即使仓库列表为空）；
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

// v3 编码为 warehouseBlob：u16 count + 每项（u16 suffixLen | suffix | u32 dataLen | u8 flags | data）。
// **空列表编码为空 vector（size 0）**：保证无仓库时 bundle 的 checksum 与旧版逐字节一致，
// 且 warehouse 段长度字段为 0（不引入任何额外摘要输入）。
std::vector<uint8_t> encode_warehouse(const std::vector<WarehouseFile>& files);
// v3 解码：size==0 成功且 out 为空；否则 size>=2，逐项校验 suffixLen/dataLen/flags/边界与后缀
// 合法性，任何越界、未知 flags 位或不合法返回 false（fail-closed）。
bool decode_warehouse(const uint8_t* data, size_t size, std::vector<WarehouseFile>& out);
// v2 兼容编码/解码（无 flags 字节）；仅用于构造/解析旧 v2 备份，解码结果 flags 恒为 0。
std::vector<uint8_t> encode_warehouse_v2(const std::vector<WarehouseFile>& files);
bool decode_warehouse_v2(const uint8_t* data, size_t size, std::vector<WarehouseFile>& out);
// 合法后缀：长度 5..64、`.wh4-` 开头、字符仅 ASCII 字母数字与 `.`/`-`、不含 `..`。
// （`.wh4-` 与 `.bak` 等固定字面量含 hex 之外的 w/h/k，故白名单放宽到字母数字，仍拒绝
// 路径分隔符与遍历序列。）
bool valid_warehouse_suffix(const std::string& s);

// wh4 容器解密回调：buf 为可写副本（长度 len）、key 为设备密钥；返回 1 表示双校验和通过
// （本机密钥可解），0 表示不可用。与游戏 ENCRYPT_Process2(buf, len, mode=1, key) 语义一致。
using WarehouseDecryptFn = int (*)(void* buf, int len, const char* key);
// wh4 容器加密回调：mode=0，就地写入 len+3 字节（写入需 len+3 容量）。返回 1 成功。
using WarehouseEncryptFn = int (*)(void* buf, int len, const char* key);

// 纯逻辑：判断一个 wh4 容器在本机是否可用（可解密）。
//   * data==nullptr 或 len<4 → false（解密长度 = len-3，低于 4 无有效负载）；
//   * key==nullptr 或 decrypt==nullptr → true（无法判定，放行以保持既有行为）；
//   * 否则把**完整容器**（len 字节，含尾部 3 字节 sum/seed/sum_plain）复制一份后调用
//     decrypt(copy, len-3, key) —— 回调等价于游戏 `ENCRYPT_Process2(buf, len-3, mode=1, key)`，
//     它必须能读到 buf[len-3 .. len) 的尾部字节做双重校验；**截断缓冲会导致必然失败**。
//     返回是否 == 1；原 buffer 不被修改。
bool warehouse_blob_decryptable(const uint8_t* data, size_t len, const char* key,
                                WarehouseDecryptFn decrypt);

// ---- wh4 明文 → 目标槽重加密（v3 flags=1 导入路径）----

// wh4 明文布局（docs/reference/game/save.md §1.1）：+0x00 "WH4JRN01"、+0x14 u32 槽位号（小端）、
// +0x38 u64 FNV-1a 64 完整性校验值（小端）。+0x14 可改写，但**必须同步重算 +0x38**，
// 否则游戏校验失败被拒（真机 A/B 实证：只改槽不重算 → 进档 SIGSEGV）。
constexpr char kWh4PlainMagic[] = "WH4JRN01";       // 含结尾 NUL，有效 8 字节
constexpr size_t kWh4PlainSlotOffset = 0x14;
constexpr size_t kWh4HashOffset = 0x38;        // 完整性校验值所在（u64 小端）
constexpr size_t kWh4HashBodyStart = 0x40;     // 校验覆盖：+0x00..0x38 与 +0x40..len（跳 +0x38..0x40 自身）
constexpr size_t kWh4PlainMinBytes = kWh4HashBodyStart;  // 至少覆盖到校验字段之后

// FNV-1a 64 参数（反汇编证据：隐藏段 0x40a4-0x410c 立即数 + 两段循环 + cmp/FAIL）。
constexpr uint64_t kWh4FnvOffsetBasis = 0xCBF29CE484222325ull;
constexpr uint64_t kWh4FnvPrime = 0x100000001B3ull;

// 纯逻辑：计算 wh4 明文完整性校验值（FNV-1a 64，覆盖 +0x00..0x38 与 +0x40..len）。
// plain.size() < 0x40 时返回 0（调用方判为 kBadPlain）。
uint64_t warehouse_plain_integrity_hash(const std::vector<uint8_t>& plain);
// 把校验值写入 plain[0x38..0x40)（小端）；plain.size() < 0x40 时不动作。
void warehouse_plain_write_integrity_hash(std::vector<uint8_t>& plain);

enum class WarehouseReencryptStatus : uint8_t {
    kOk = 0,
    kNoKey,            // 缺 key 或缺加密/解密函数 → 失败
    kBadPlain,         // 明文长度不足/槽位非法 → 失败
    kRoundtripFailed,  // 加密后再解密自检失败 → 失败
};

const char* warehouse_reencrypt_status_name(WarehouseReencryptStatus s);

// 纯逻辑（host 可测）：改写明文 +0x14 为目标 slot（小端 u32）→ 重算并写 +0x38 →
// encrypt(mode=0)（就地，需 len+3 容量）→ decrypt(mode=1) 往返自检：返回 1 且 +0x00 为 magic、
// +0x14 == slot、重算 hash == 存储于 +0x38 的值。成功时 out_cipher 为 len+3 密文；
// 失败时不写盘（out_cipher 保持为空）。明文长度 < 0x40 → kBadPlain。
WarehouseReencryptStatus warehouse_reencrypt_for_slot(
    const std::vector<uint8_t>& plain, int slot, const char* key,
    WarehouseEncryptFn encrypt, WarehouseDecryptFn decrypt,
    std::vector<uint8_t>& out_cipher);

// ---- 导出统一分离：剥离存档 block3 内嵌的 WH96v002 仓库段 ----

// 段头为 ASCII "WH96v002"（8 字节），紧随其后 u32 段长（小端）；段总长 = 8 + 段长。
constexpr char kInlineWarehouseMagic[] = "WH96v002";

enum class InlineWarehouseStrip : uint8_t {
    kStripped = 0,   // 已剥离并重建块表
    kNoSegment,      // block3 内未找到 WH96v002 段
    kNotAtEnd,       // 找到段但未恰好结束于 block3 末尾（保守：不剥离）
    kMalformed,      // 块表/边界非法（保守：不剥离）
};

const char* inline_warehouse_strip_name(InlineWarehouseStrip s);

// 纯逻辑（host 可测）：在存档明文 block3 内定位 **最后一个** WH96v002 段，仅当该段恰好
// 结束于 block3 末尾时剥离；剥离后把各块按索引顺序重排为连续布局、重写块表项（offset_rel/len），
// plain 总长缩短。不满足保守规则时 plain 保持原样。
InlineWarehouseStrip warehouse_strip_inline_segment(std::vector<uint8_t>& plain);

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
