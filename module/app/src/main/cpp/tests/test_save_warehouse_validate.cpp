// host 单测：wh4 相关纯逻辑层。
//   * v3 warehouseBlob 编解码（flags=0/1 混合、多文件、后缀校验）与 v2 旧编码兼容；
//   * parse_bundle 按 formatVersion 分派（v2 不得被误判为 v3）；
//   * warehouse_blob_decryptable：合法/非法容器、len<4、回调 0/1、回调缺失放行；
//   * warehouse_reencrypt_for_slot：补 slot + 重加密 + 往返自检的决策（no key / bad plain /
//     roundtrip failed / ok），全部用可注入假加解密函数。
#include "feature/save_backup/save_backup_bundle.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) ++g_pass; \
    else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

namespace {

using save_backup::WarehouseFile;

// ---------------- 假解密回调（warehouse_blob_decryptable） ----------------

struct DecryptStub {
    int ret = 1;
    int calls = 0;
    int last_len = -1;
    std::string last_key;
    std::vector<uint8_t> last_buf;
    uint8_t poke_value = 0xEE;
    int tail_byte = -1;  // buf[len]（紧邻解密长度之后的字节）——用于验证"完整容器"契约
};

DecryptStub g_stub;

int fake_decrypt(void* buf, int len, const char* key) {
    ++g_stub.calls;
    g_stub.last_len = len;
    g_stub.last_key = (key == nullptr) ? "" : key;
    auto* p = static_cast<uint8_t*>(buf);
    g_stub.last_buf.assign(p, p + (len > 0 ? len : 0));
    // 游戏 ENCRYPT_Process2(mode=1) 会读取 buf[len..len+3) 的尾部校验字节；这里读一个字节
    // 作为"容器未被截断"的可观测证据（调用方必须传完整容器）。
    g_stub.tail_byte = (len > 0) ? static_cast<int>(p[len]) : -1;
    if (len > 0) p[0] = g_stub.poke_value;  // 模拟就地解密
    return g_stub.ret;
}

constexpr const char* kKey = "device-key";

void reset_stub(int ret = 1) {
    g_stub = DecryptStub{};
    g_stub.ret = ret;
}

void test_blob_decryptable_too_short_or_null() {
    reset_stub();
    const uint8_t small[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    CHECK(!save_backup::warehouse_blob_decryptable(nullptr, 10, kKey, &fake_decrypt));
    CHECK(!save_backup::warehouse_blob_decryptable(small, 0, kKey, &fake_decrypt));
    CHECK(!save_backup::warehouse_blob_decryptable(small, 1, kKey, &fake_decrypt));
    CHECK(!save_backup::warehouse_blob_decryptable(small, 2, kKey, &fake_decrypt));
    CHECK(!save_backup::warehouse_blob_decryptable(small, 3, kKey, &fake_decrypt));
    CHECK(g_stub.calls == 0);
}

void test_blob_decryptable_ok() {
    reset_stub(/*ret=*/1);
    const std::vector<uint8_t> blob = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80};
    CHECK(save_backup::warehouse_blob_decryptable(blob.data(), blob.size(), kKey, &fake_decrypt));
    CHECK(g_stub.calls == 1);
    CHECK(g_stub.last_len == static_cast<int>(blob.size()) - 3);
    CHECK(g_stub.last_key == kKey);
    CHECK(g_stub.last_buf == std::vector<uint8_t>({0x10, 0x20, 0x30, 0x40, 0x50}));
    CHECK(g_stub.tail_byte == 0x60);  // 完整容器契约：解密长度之后仍有尾部 3 字节
    CHECK(blob[0] == 0x10);  // 原 buffer 未被就地写入
}

void test_blob_decryptable_minimum() {
    reset_stub(/*ret=*/1);
    const std::vector<uint8_t> blob = {0xAB, 0xCD, 0xEF, 0x01};
    CHECK(save_backup::warehouse_blob_decryptable(blob.data(), blob.size(), kKey, &fake_decrypt));
    CHECK(g_stub.calls == 1);
    CHECK(g_stub.last_len == 1);
    CHECK(g_stub.tail_byte == 0xCD);  // 4 字节容器（payload 1 + 尾部 3）也不得被截断
}

void test_blob_decryptable_reject_and_missing() {
    reset_stub(/*ret=*/0);
    const std::vector<uint8_t> blob = {1, 2, 3, 4, 5, 6};
    CHECK(!save_backup::warehouse_blob_decryptable(blob.data(), blob.size(), kKey, &fake_decrypt));
    CHECK(g_stub.calls == 1);

    reset_stub(/*ret=*/-1);
    CHECK(!save_backup::warehouse_blob_decryptable(blob.data(), blob.size(), kKey, &fake_decrypt));

    reset_stub();
    CHECK(save_backup::warehouse_blob_decryptable(blob.data(), blob.size(), kKey, nullptr));
    CHECK(save_backup::warehouse_blob_decryptable(blob.data(), blob.size(), nullptr, &fake_decrypt));
    CHECK(g_stub.calls == 0);
}

// ---------------- v2/v3 warehouseBlob 编解码 ----------------

void test_warehouse_v3_roundtrip() {
    namespace sb = save_backup;
    WarehouseFile cipher_item;
    cipher_item.suffix = ".wh4-000001a043a2bba1";
    cipher_item.flags = 0;
    cipher_item.data = {0xDE, 0xAD, 0xBE, 0xEF};
    WarehouseFile plain_item;
    plain_item.suffix = ".wh4-000001a043a2bba1.bak";
    plain_item.flags = sb::kWarehouseFlagPlain;
    plain_item.data = {0x01, 0x02, 0x03, 0x04, 0x05};
    const std::vector<WarehouseFile> files = {cipher_item, plain_item};

    CHECK(sb::encode_warehouse({}).empty());
    const std::vector<uint8_t> blob = sb::encode_warehouse(files);
    CHECK(!blob.empty());

    std::vector<WarehouseFile> decoded;
    CHECK(sb::decode_warehouse(blob.data(), blob.size(), decoded));
    CHECK(decoded.size() == static_cast<size_t>(2));
    CHECK(decoded[0].suffix == cipher_item.suffix);
    CHECK(decoded[0].flags == 0);
    CHECK(decoded[0].data == cipher_item.data);
    CHECK(decoded[1].suffix == plain_item.suffix);
    CHECK(decoded[1].flags == sb::kWarehouseFlagPlain);
    CHECK(decoded[1].data == plain_item.data);

    // 未知 flags 位 fail-closed：把第二项 flags 改成 0x02 后解码失败。
    // 偏移 = count(2) + [suffixLen(2)+suffix0+dataLen(4)+flags(1)+data0] + suffixLen(2)+suffix1+dataLen(4)。
    std::vector<uint8_t> bad_flags = blob;
    const size_t plain_flags_off = 2 + 2 + cipher_item.suffix.size() + 4 + 1 +
                                   cipher_item.data.size() + 2 + plain_item.suffix.size() + 4;
    CHECK(plain_flags_off < bad_flags.size());
    bad_flags[plain_flags_off] = 0x02;
    std::vector<WarehouseFile> bad;
    CHECK(!sb::decode_warehouse(bad_flags.data(), bad_flags.size(), bad));
}

void test_warehouse_v2_legacy_encoding() {
    namespace sb = save_backup;
    WarehouseFile f0;
    f0.suffix = ".wh4-000001a043a2bba1";
    f0.data = {0x11, 0x22, 0x33, 0x44};
    WarehouseFile f1;
    f1.suffix = ".wh4-000001a043a2bba1.bak";
    f1.flags = sb::kWarehouseFlagPlain;  // v2 无 flags 段：该字段被忽略
    f1.data = {0x55, 0x66};

    const std::vector<uint8_t> v2_blob = sb::encode_warehouse_v2({f0, f1});
    CHECK(!v2_blob.empty());

    std::vector<WarehouseFile> v2_decoded;
    CHECK(sb::decode_warehouse_v2(v2_blob.data(), v2_blob.size(), v2_decoded));
    CHECK(v2_decoded.size() == static_cast<size_t>(2));
    CHECK(v2_decoded[0].data == f0.data);
    CHECK(v2_decoded[0].flags == 0);
    CHECK(v2_decoded[1].data == f1.data);
    CHECK(v2_decoded[1].flags == 0);  // v2 无 flags：解码后恒 0

    // v2 blob 不得被 v3 解码器误读（v3 会多读 1 字节 flags 导致越界/错位）。
    std::vector<WarehouseFile> misread;
    CHECK(!save_backup::decode_warehouse(v2_blob.data(), v2_blob.size(), misread));
}

// ---------------- parse_bundle 版本分派 ----------------

void put_be16(std::vector<uint8_t>& o, uint16_t v) {
    o.push_back(static_cast<uint8_t>(v >> 8));
    o.push_back(static_cast<uint8_t>(v));
}

void put_be32(std::vector<uint8_t>& o, uint32_t v) {
    o.push_back(static_cast<uint8_t>(v >> 24));
    o.push_back(static_cast<uint8_t>(v >> 16));
    o.push_back(static_cast<uint8_t>(v >> 8));
    o.push_back(static_cast<uint8_t>(v));
}

std::vector<uint8_t> make_bundle(uint16_t version, const std::vector<uint8_t>& orig,
                                 const std::vector<uint8_t>& module,
                                 const std::vector<uint8_t>& wh_blob, const std::string& meta) {
    std::vector<uint8_t> b;
    put_be32(b, save_backup::kBundleMagic);
    put_be16(b, version);
    b.push_back(0);  // source_slot
    b.push_back(0);
    b.push_back(0);
    b.push_back(0);
    for (int i = 7; i >= 0; --i) b.push_back(0);  // exportTimeMs
    put_be32(b, static_cast<uint32_t>(orig.size()));
    b.insert(b.end(), orig.begin(), orig.end());
    put_be32(b, static_cast<uint32_t>(module.size()));
    b.insert(b.end(), module.begin(), module.end());
    put_be32(b, static_cast<uint32_t>(wh_blob.size()));
    b.insert(b.end(), wh_blob.begin(), wh_blob.end());
    put_be16(b, static_cast<uint16_t>(meta.size()));
    b.insert(b.end(), meta.begin(), meta.end());
    put_be32(b, save_backup::crc32_ieee(b.data(), b.size()));
    return b;
}

void test_parse_bundle_version_dispatch() {
    namespace sb = save_backup;
    WarehouseFile f0;
    f0.suffix = ".wh4-000001a043a2bba1";
    f0.flags = sb::kWarehouseFlagPlain;
    f0.data = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
    const std::vector<uint8_t> orig = {9, 8, 7};
    const std::string meta = "{\"checksum\":\"001122334455\"}";

    // v3 bundle：build_bundle 写出 kBundleVersion，flags 完整保留。
    std::vector<uint8_t> v3;
    sb::build_bundle(1, 123, orig, {}, sb::encode_warehouse({f0}), meta, v3);
    sb::ParsedBundle parsed_v3;
    CHECK(sb::parse_bundle(v3.data(), v3.size(), parsed_v3));
    CHECK(parsed_v3.has_warehouse);
    CHECK(parsed_v3.warehouse.size() == static_cast<size_t>(1));
    CHECK(parsed_v3.warehouse[0].flags == sb::kWarehouseFlagPlain);
    CHECK(parsed_v3.warehouse[0].data == f0.data);

    // v2 bundle：旧编码（无 flags），parse 成功且 flags 恒 0（不得走 v3 解码器）。
    std::vector<uint8_t> v2 =
        make_bundle(sb::kBundleVersionV2, orig, {}, sb::encode_warehouse_v2({f0}), meta);
    sb::ParsedBundle parsed_v2;
    CHECK(sb::parse_bundle(v2.data(), v2.size(), parsed_v2));
    CHECK(parsed_v2.has_warehouse);
    CHECK(parsed_v2.warehouse.size() == static_cast<size_t>(1));
    CHECK(parsed_v2.warehouse[0].flags == 0);
    CHECK(parsed_v2.warehouse[0].suffix == f0.suffix);
    CHECK(parsed_v2.warehouse[0].data == f0.data);
    CHECK(parsed_v2.orig_plain == orig);

    // v1 bundle：无仓库段，has_warehouse=false。
    std::vector<uint8_t> v1;
    put_be32(v1, sb::kBundleMagic);
    put_be16(v1, sb::kBundleVersionV1);
    v1.push_back(0);
    v1.push_back(0);
    v1.push_back(0);
    v1.push_back(0);
    for (int i = 7; i >= 0; --i) v1.push_back(0);
    put_be32(v1, static_cast<uint32_t>(orig.size()));
    v1.insert(v1.end(), orig.begin(), orig.end());
    put_be32(v1, 0);
    put_be16(v1, static_cast<uint16_t>(meta.size()));
    v1.insert(v1.end(), meta.begin(), meta.end());
    put_be32(v1, sb::crc32_ieee(v1.data(), v1.size()));
    sb::ParsedBundle parsed_v1;
    CHECK(sb::parse_bundle(v1.data(), v1.size(), parsed_v1));
    CHECK(!parsed_v1.has_warehouse);
    CHECK(parsed_v1.warehouse.empty());
}

// ---------------- warehouse_reencrypt_for_slot ----------------

enum class DecMode { kKeep, kFail, kBreakMagic, kBreakSlot, kBreakHash };
DecMode g_dec_mode = DecMode::kKeep;
int g_enc_calls = 0;
std::vector<uint8_t> g_enc_seen;  // 加密回调看到的 payload（含改写的 slot）

int reenc_encrypt(void* buf, int len, const char* key) {
    ++g_enc_calls;
    auto* p = static_cast<uint8_t*>(buf);
    g_enc_seen.assign(p, p + (len > 0 ? len : 0));
    p[len] = 0;
    p[len + 1] = 0;
    p[len + 2] = 0;
    return 1;
}

int reenc_decrypt(void* buf, int len, const char* key) {
    auto* p = static_cast<uint8_t*>(buf);
    if (g_dec_mode == DecMode::kFail) return 0;
    if (g_dec_mode == DecMode::kBreakMagic) p[0] = 0;
    if (g_dec_mode == DecMode::kBreakSlot) p[save_backup::kWh4PlainSlotOffset] = 0;
    if (g_dec_mode == DecMode::kBreakHash) p[save_backup::kWh4HashOffset] ^= 0x01;
    return 1;
}

std::vector<uint8_t> make_wh4_plain(uint32_t slot) {
    std::vector<uint8_t> p(0x40, 0);
    std::memcpy(p.data(), save_backup::kWh4PlainMagic, 8);
    p[save_backup::kWh4PlainSlotOffset] = static_cast<uint8_t>(slot);
    p[save_backup::kWh4PlainSlotOffset + 1] = 0;
    p[save_backup::kWh4PlainSlotOffset + 2] = 0;
    p[save_backup::kWh4PlainSlotOffset + 3] = 0;
    return p;
}

uint32_t read_le32(const std::vector<uint8_t>& v, size_t off) {
    return uint32_t(v[off]) | uint32_t(v[off + 1]) << 8 | uint32_t(v[off + 2]) << 16 |
           uint32_t(v[off + 3]) << 24;
}

uint64_t read_le64(const std::vector<uint8_t>& v, size_t off) {
    return uint64_t(read_le32(v, off)) | uint64_t(read_le32(v, off + 4)) << 32;
}

void put_le32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(static_cast<uint8_t>(x));
    v.push_back(static_cast<uint8_t>(x >> 8));
    v.push_back(static_cast<uint8_t>(x >> 16));
    v.push_back(static_cast<uint8_t>(x >> 24));
}

// ---- wh4 完整性校验值（FNV-1a 64）----

void test_integrity_hash_known_vectors() {
    namespace sb = save_backup;
    // 向量 A：len=0x40，header [0..0x38)=i，无尾段。
    std::vector<uint8_t> a(0x40);
    for (size_t i = 0; i < a.size(); ++i) a[i] = static_cast<uint8_t>(i);
    CHECK(sb::warehouse_plain_integrity_hash(a) == 0xa17f99d97962286du);

    // 向量 B：len=0x50，header=i，+0x38..0x40 = 0xBB，尾 [0x40..0x50)=0xAA。
    std::vector<uint8_t> b(0x50);
    for (size_t i = 0; i < b.size(); ++i) b[i] = static_cast<uint8_t>(i);
    for (size_t i = 0x38; i < 0x40; ++i) b[i] = 0xBB;
    for (size_t i = 0x40; i < 0x50; ++i) b[i] = 0xAA;
    CHECK(sb::warehouse_plain_integrity_hash(b) == 0x6f4de6e335a653edu);

    // 向量 C：len=0x40 全零。
    std::vector<uint8_t> c(0x40, 0);
    CHECK(sb::warehouse_plain_integrity_hash(c) == 0x8ac123d6f7dce585u);

    // 长度不足 → 0。
    std::vector<uint8_t> short_plain(0x3f, 0);
    CHECK(sb::warehouse_plain_integrity_hash(short_plain) == 0);
}

void test_integrity_hash_scope_and_write() {
    namespace sb = save_backup;
    std::vector<uint8_t> plain(0x50, 0x11);
    std::vector<uint8_t> base = plain;
    const uint64_t h = sb::warehouse_plain_integrity_hash(base);

    // 写 +0x38 后，存储值 == 重算值（校验字段自身不参与）。
    sb::warehouse_plain_write_integrity_hash(plain);
    CHECK(read_le64(plain, sb::kWh4HashOffset) == h);
    CHECK(sb::warehouse_plain_integrity_hash(plain) == h);

    // 改 +0x00..0x38 或 +0x40.. 会改变 hash；改 +0x38..0x40 不影响。
    std::vector<uint8_t> head = plain;
    head[0x14] ^= 0x01;
    CHECK(sb::warehouse_plain_integrity_hash(head) != h);
    std::vector<uint8_t> tail = plain;
    tail[0x41] ^= 0x01;
    CHECK(sb::warehouse_plain_integrity_hash(tail) != h);
    std::vector<uint8_t> hashfield = plain;
    hashfield[0x39] = 0xFF;
    CHECK(sb::warehouse_plain_integrity_hash(hashfield) == h);
}

// ---- warehouse_reencrypt_for_slot（跨槽补 slot + 重算 hash）----

void test_reencrypt_cross_slot_ok() {
    namespace sb = save_backup;
    g_dec_mode = DecMode::kKeep;
    g_enc_calls = 0;
    std::vector<uint8_t> plain = make_wh4_plain(0);  // 源槽 0 → 目标槽 1
    sb::warehouse_plain_write_integrity_hash(plain);
    const uint64_t src_hash = read_le64(plain, sb::kWh4HashOffset);
    std::vector<uint8_t> cipher;
    const auto status = sb::warehouse_reencrypt_for_slot(plain, 1, kKey, &reenc_encrypt,
                                                         &reenc_decrypt, cipher);
    CHECK(status == sb::WarehouseReencryptStatus::kOk);
    CHECK(g_enc_calls == 1);
    CHECK(cipher.size() == plain.size() + 3);
    // 加密回调看到 +0x14 已写为目标槽 1，且 +0x38 已重算（与源槽 hash 不同）。
    CHECK(read_le32(g_enc_seen, sb::kWh4PlainSlotOffset) == 1u);
    CHECK(read_le64(g_enc_seen, sb::kWh4HashOffset) != src_hash);
    // 密文（fake 不变换 payload）自洽：magic/slot/hash 均正确（hash 只覆盖明文部分）。
    CHECK(std::memcmp(cipher.data(), sb::kWh4PlainMagic, 8) == 0);
    CHECK(read_le32(cipher, sb::kWh4PlainSlotOffset) == 1u);
    const std::vector<uint8_t> plain_view(cipher.begin(), cipher.begin() + plain.size());
    CHECK(sb::warehouse_plain_integrity_hash(plain_view) == read_le64(cipher, sb::kWh4HashOffset));
    // 原 plain 未被修改。
    CHECK(read_le32(plain, sb::kWh4PlainSlotOffset) == 0u);
    CHECK(read_le64(plain, sb::kWh4HashOffset) == src_hash);
}

void test_reencrypt_roundtrip_hash_mismatch() {
    namespace sb = save_backup;
    g_dec_mode = DecMode::kBreakHash;  // 解密后 +0x38 被动过 → 自检失败
    const std::vector<uint8_t> plain = make_wh4_plain(0);
    std::vector<uint8_t> cipher;
    CHECK(sb::warehouse_reencrypt_for_slot(plain, 1, kKey, &reenc_encrypt, &reenc_decrypt, cipher) ==
          sb::WarehouseReencryptStatus::kRoundtripFailed);
    CHECK(cipher.empty());
}

void test_reencrypt_no_key() {
    namespace sb = save_backup;
    const std::vector<uint8_t> plain = make_wh4_plain(0);
    std::vector<uint8_t> cipher;
    CHECK(sb::warehouse_reencrypt_for_slot(plain, 1, nullptr, &reenc_encrypt, &reenc_decrypt,
                                           cipher) == sb::WarehouseReencryptStatus::kNoKey);
    CHECK(sb::warehouse_reencrypt_for_slot(plain, 1, kKey, nullptr, &reenc_decrypt, cipher) ==
          sb::WarehouseReencryptStatus::kNoKey);
    CHECK(sb::warehouse_reencrypt_for_slot(plain, 1, kKey, &reenc_encrypt, nullptr, cipher) ==
          sb::WarehouseReencryptStatus::kNoKey);
    CHECK(cipher.empty());
}

void test_reencrypt_bad_plain() {
    namespace sb = save_backup;
    std::vector<uint8_t> cipher;
    const std::vector<uint8_t> too_short(0x10, 0);
    CHECK(sb::warehouse_reencrypt_for_slot(too_short, 1, kKey, &reenc_encrypt, &reenc_decrypt,
                                           cipher) == sb::WarehouseReencryptStatus::kBadPlain);
    const std::vector<uint8_t> ok = make_wh4_plain(0);
    CHECK(sb::warehouse_reencrypt_for_slot(ok, -1, kKey, &reenc_encrypt, &reenc_decrypt, cipher) ==
          sb::WarehouseReencryptStatus::kBadPlain);
    CHECK(sb::warehouse_reencrypt_for_slot(ok, 300, kKey, &reenc_encrypt, &reenc_decrypt, cipher) ==
          sb::WarehouseReencryptStatus::kBadPlain);
    CHECK(cipher.empty());
}

void test_reencrypt_roundtrip_failures() {
    namespace sb = save_backup;
    const std::vector<uint8_t> plain = make_wh4_plain(0);
    std::vector<uint8_t> cipher;

    g_dec_mode = DecMode::kFail;
    CHECK(sb::warehouse_reencrypt_for_slot(plain, 1, kKey, &reenc_encrypt, &reenc_decrypt, cipher) ==
          sb::WarehouseReencryptStatus::kRoundtripFailed);
    CHECK(cipher.empty());

    g_dec_mode = DecMode::kBreakMagic;
    CHECK(sb::warehouse_reencrypt_for_slot(plain, 1, kKey, &reenc_encrypt, &reenc_decrypt, cipher) ==
          sb::WarehouseReencryptStatus::kRoundtripFailed);

    g_dec_mode = DecMode::kBreakSlot;
    CHECK(sb::warehouse_reencrypt_for_slot(plain, 1, kKey, &reenc_encrypt, &reenc_decrypt, cipher) ==
          sb::WarehouseReencryptStatus::kRoundtripFailed);
}

void test_reencrypt_status_names() {
    using sb = save_backup::WarehouseReencryptStatus;
    CHECK(std::strcmp(save_backup::warehouse_reencrypt_status_name(sb::kOk), "ok") == 0);
    CHECK(std::strcmp(save_backup::warehouse_reencrypt_status_name(sb::kNoKey), "no key") == 0);
    CHECK(std::strcmp(save_backup::warehouse_reencrypt_status_name(sb::kBadPlain), "bad plain") == 0);
    CHECK(std::strcmp(save_backup::warehouse_reencrypt_status_name(sb::kRoundtripFailed),
                      "roundtrip failed") == 0);
}

// ---- 导出分离：剥离 block3 内嵌 WH96v002 段 ----

void put_le16(std::vector<uint8_t>& o, uint16_t v) {
    o.push_back(static_cast<uint8_t>(v));
    o.push_back(static_cast<uint8_t>(v >> 8));
}

// 构造 n 块连续存档明文：header(8) + 块表(n*4) + 各块数据。block3 内容由调用方给定。
std::vector<uint8_t> build_save_plain(const std::vector<uint8_t>& block3) {
    constexpr size_t kN = 5;
    std::vector<std::vector<uint8_t>> blocks = {
        {0x01, 0x02, 0x03, 0x04},
        {0x11, 0x12, 0x13, 0x14},
        {0x21, 0x22, 0x23, 0x24},
        block3,
        {0x41, 0x42, 0x43, 0x44},
    };
    std::vector<uint8_t> p(8, 0xEE);
    size_t off = kN * 4;
    for (const auto& b : blocks) {
        put_le16(p, static_cast<uint16_t>(off));
        put_le16(p, static_cast<uint16_t>(b.size()));
        off += b.size();
    }
    for (const auto& b : blocks) p.insert(p.end(), b.begin(), b.end());
    return p;
}

std::vector<uint8_t> make_inline_segment(const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> s;
    const char magic[] = "WH96v002";
    s.insert(s.end(), magic, magic + 8);
    put_le32(s, static_cast<uint32_t>(4 + payload.size()));  // 段长字段值 = 4(len) + payload
    s.insert(s.end(), payload.begin(), payload.end());
    return s;
}

std::vector<uint8_t> block_at(const std::vector<uint8_t>& plain, size_t idx) {
    const uint16_t off = static_cast<uint16_t>(plain[8 + idx * 4] | plain[8 + idx * 4 + 1] << 8);
    const uint16_t len = static_cast<uint16_t>(plain[8 + idx * 4 + 2] | plain[8 + idx * 4 + 3] << 8);
    return std::vector<uint8_t>(plain.begin() + 8 + off, plain.begin() + 8 + off + len);
}

void test_strip_inline_segment_ok() {
    namespace sb = save_backup;
    const std::vector<uint8_t> records = {0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5};
    std::vector<uint8_t> b3 = records;
    const std::vector<uint8_t> seg = make_inline_segment({0xAA, 0xBB, 0xCC});
    b3.insert(b3.end(), seg.begin(), seg.end());
    std::vector<uint8_t> plain = build_save_plain(b3);
    const size_t old_size = plain.size();

    CHECK(sb::warehouse_strip_inline_segment(plain) == sb::InlineWarehouseStrip::kStripped);
    CHECK(plain.size() == old_size - seg.size());
    // block3 只剩记录区；块表重排为连续布局。
    CHECK(block_at(plain, 3) == records);
    CHECK(block_at(plain, 0) == std::vector<uint8_t>({0x01, 0x02, 0x03, 0x04}));
    CHECK(block_at(plain, 4) == std::vector<uint8_t>({0x41, 0x42, 0x43, 0x44}));
    size_t expect = 5 * 4;
    for (size_t i = 0; i < 5; ++i) {
        const uint16_t off = static_cast<uint16_t>(plain[8 + i * 4] | plain[8 + i * 4 + 1] << 8);
        const uint16_t len = static_cast<uint16_t>(plain[8 + i * 4 + 2] | plain[8 + i * 4 + 3] << 8);
        CHECK(off == expect);
        expect += len;
    }
    CHECK(8 + expect == plain.size());
}

void test_strip_inline_segment_not_at_end() {
    namespace sb = save_backup;
    std::vector<uint8_t> b3 = {0xA0, 0xA1};
    const std::vector<uint8_t> seg = make_inline_segment({0xAA, 0xBB});
    b3.insert(b3.end(), seg.begin(), seg.end());
    b3.push_back(0xFF);  // 段后仍有字节 → 不剥离
    std::vector<uint8_t> plain = build_save_plain(b3);
    const std::vector<uint8_t> before = plain;
    CHECK(sb::warehouse_strip_inline_segment(plain) == sb::InlineWarehouseStrip::kNotAtEnd);
    CHECK(plain == before);
}

void test_strip_inline_segment_zero_trailer() {
    namespace sb = save_backup;
    const std::vector<uint8_t> records = {0xB0, 0xB1, 0xB2};
    std::vector<uint8_t> b3 = records;
    const std::vector<uint8_t> seg = make_inline_segment({0xAA, 0xBB});
    b3.insert(b3.end(), seg.begin(), seg.end());
    b3.insert(b3.end(), {0x00, 0x00, 0x00});  // 段后仅零填充 → 允许剥离（零填充随段一并截断）
    std::vector<uint8_t> plain = build_save_plain(b3);

    CHECK(sb::warehouse_strip_inline_segment(plain) == sb::InlineWarehouseStrip::kStripped);
    CHECK(block_at(plain, 3) == records);
    size_t total = 5 * 4;
    for (size_t i = 0; i < 5; ++i) {
        const uint16_t off = static_cast<uint16_t>(plain[8 + i * 4] | plain[8 + i * 4 + 1] << 8);
        const uint16_t len = static_cast<uint16_t>(plain[8 + i * 4 + 2] | plain[8 + i * 4 + 3] << 8);
        CHECK(off == total);
        total += len;
    }
    CHECK(8 + total == plain.size());
}

void test_strip_inline_segment_absent_or_malformed() {
    namespace sb = save_backup;
    std::vector<uint8_t> plain = build_save_plain({0x51, 0x52, 0x53});
    const std::vector<uint8_t> before = plain;
    CHECK(sb::warehouse_strip_inline_segment(plain) == sb::InlineWarehouseStrip::kNoSegment);
    CHECK(plain == before);

    std::vector<uint8_t> bad = {1, 2, 3};  // 过短 / 块表非法
    CHECK(sb::warehouse_strip_inline_segment(bad) == sb::InlineWarehouseStrip::kMalformed);
    CHECK(bad == std::vector<uint8_t>({1, 2, 3}));
}

void test_inline_strip_names() {
    using sb = save_backup::InlineWarehouseStrip;
    CHECK(std::strcmp(save_backup::inline_warehouse_strip_name(sb::kStripped), "stripped") == 0);
    CHECK(std::strcmp(save_backup::inline_warehouse_strip_name(sb::kNoSegment), "no segment") == 0);
    CHECK(std::strcmp(save_backup::inline_warehouse_strip_name(sb::kNotAtEnd),
                      "not at block end") == 0);
    CHECK(std::strcmp(save_backup::inline_warehouse_strip_name(sb::kMalformed),
                      "malformed block table") == 0);
}

}  // namespace

int main() {
    test_blob_decryptable_too_short_or_null();
    test_blob_decryptable_ok();
    test_blob_decryptable_minimum();
    test_blob_decryptable_reject_and_missing();
    test_warehouse_v3_roundtrip();
    test_warehouse_v2_legacy_encoding();
    test_parse_bundle_version_dispatch();
    test_integrity_hash_known_vectors();
    test_integrity_hash_scope_and_write();
    test_reencrypt_cross_slot_ok();
    test_reencrypt_roundtrip_hash_mismatch();
    test_reencrypt_no_key();
    test_reencrypt_bad_plain();
    test_reencrypt_roundtrip_failures();
    test_reencrypt_status_names();
    test_strip_inline_segment_ok();
    test_strip_inline_segment_zero_trailer();
    test_strip_inline_segment_not_at_end();
    test_strip_inline_segment_absent_or_malformed();
    test_inline_strip_names();
    std::printf("save_warehouse_validate_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
