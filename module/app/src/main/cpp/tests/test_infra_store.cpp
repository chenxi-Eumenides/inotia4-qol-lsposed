// infra/store host 单测：MSAV 容器字节格式（金样）、generation 递增、last-good 回退、
// quarantine、命名/容量/版本/越界错误码、replace_sections 原子性、import 重定槽、
// section 覆盖/删除/空 payload。格式以 Kotlin ModuleSaveStore.kt 为权威。

#include "infra/store.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

using qol::infra::store::ReplaceMap;
using qol::infra::store::RemoveSet;
using qol::infra::store::Section;
using qol::infra::store::Status;

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) ++g_pass; \
    else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

namespace {

std::string make_temp_dir() {
    char tmpl[] = "infra_store_test_XXXXXX";
    char* dir = ::mkdtemp(tmpl);
    return dir != nullptr ? std::string(dir) : std::string();
}

void remove_tree_recursive(const std::string& path) {
    struct stat st {};
    if (::lstat(path.c_str(), &st) != 0) return;
    if (S_ISDIR(st.st_mode)) {
        ::chmod(path.c_str(), 0755);
        DIR* d = ::opendir(path.c_str());
        if (d != nullptr) {
            while (dirent* e = ::readdir(d)) {
                const std::string name = e->d_name;
                if (name == "." || name == "..") continue;
                remove_tree_recursive(path + "/" + name);
            }
            ::closedir(d);
        }
        ::rmdir(path.c_str());
        return;
    }
    ::chmod(path.c_str(), 0755);
    ::unlink(path.c_str());
}

void remove_tree(const std::string& dir) {
    remove_tree_recursive(dir);
}

std::string primary_path(const std::string& dir, int slot) {
    return dir + "/module-saves/slot-" + std::to_string(slot) + ".module-save";
}
std::string last_good_path(const std::string& dir, int slot) {
    return primary_path(dir, slot) + ".last-good";
}

std::vector<uint8_t> read_file(const std::string& path) {
    std::vector<uint8_t> out;
    std::ifstream in(path, std::ios::binary);
    char c = 0;
    while (in.get(c)) out.push_back(static_cast<uint8_t>(c));
    return out;
}

bool file_exists(const std::string& path) {
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool dir_has_corrupt(const std::string& dir) {
    DIR* d = ::opendir(dir.c_str());
    if (d == nullptr) return false;
    bool found = false;
    while (dirent* e = ::readdir(d)) {
        const std::string name = e->d_name;
        if (name.find("slot-0.module-save.corrupt.") == 0) found = true;
    }
    ::closedir(d);
    return found;
}

int64_t generation_of(const std::vector<uint8_t>& bytes) {
    int64_t gen = 0;
    for (int i = 0; i < 8; ++i) {
        gen = (gen << 8) | static_cast<int64_t>(bytes[7 + i]);
    }
    return gen;
}

std::vector<uint8_t> bytes(std::initializer_list<int> values) {
    std::vector<uint8_t> out;
    for (int v : values) out.push_back(static_cast<uint8_t>(v));
    return out;
}

// 金样（由 Kotlin 格式 + 独立 CRC32 参考实现生成）：
// reset_slot(0) → 空容器 generation=1。
const std::vector<uint8_t> kGoldenEmptySlot0Gen1 = bytes({
    0x4d, 0x53, 0x41, 0x56, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x51, 0x28, 0x4d, 0x2b,
});
// 空槽首次 write_section("a", v1, {01,02,03}) → generation=1、单 section。
const std::vector<uint8_t> kGoldenSectionA = bytes({
    0x4d, 0x53, 0x41, 0x56, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x01, 0x61, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x03, 0x01, 0x02, 0x03, 0x55, 0xbc, 0x80, 0x1d,
    0xee, 0x64, 0x21, 0x65,
});

void test_unavailable() {
    qol::infra::store::init(nullptr);
    CHECK(!qol::infra::store::available());
    CHECK(qol::infra::store::ensure_slot(0) == Status::kUnavailable);
    CHECK(qol::infra::store::read_container(0, nullptr) == Status::kBadSlot);
    std::vector<uint8_t> out;
    uint16_t version = 0;
    CHECK(qol::infra::store::read_section(0, "a", &out, &version) == Status::kUnavailable);
    qol::infra::store::init("");
    CHECK(!qol::infra::store::available());
}

void test_golden_bytes(const std::string& dir) {
    qol::infra::store::init(dir.c_str());
    CHECK(qol::infra::store::available());

    CHECK(qol::infra::store::reset_slot(0) == Status::kOk);
    CHECK(read_file(primary_path(dir, 0)) == kGoldenEmptySlot0Gen1);

    // 空槽（无 primary）首次写：generation 0 → 1。
    CHECK(qol::infra::store::delete_slot(0) == Status::kOk);
    CHECK(qol::infra::store::write_section(0, "a", 1, bytes({1, 2, 3}).data(), 3) ==
          Status::kOk);
    CHECK(read_file(primary_path(dir, 0)) == kGoldenSectionA);
}

void test_roundtrip_and_generation(const std::string& dir) {
    qol::infra::store::init(dir.c_str());
    CHECK(qol::infra::store::write_section(0, "a", 1, bytes({9}).data(), 1) == Status::kOk);
    CHECK(generation_of(read_file(primary_path(dir, 0))) == 1);
    CHECK(qol::infra::store::write_section(0, "a", 2, bytes({9, 8}).data(), 2) == Status::kOk);
    CHECK(generation_of(read_file(primary_path(dir, 0))) == 2);

    std::vector<uint8_t> out;
    uint16_t version = 0;
    CHECK(qol::infra::store::read_section(0, "a", &out, &version) == Status::kOk);
    CHECK(version == 2 && out == bytes({9, 8}));

    // 多 section 插入序与未知 section 保留。
    CHECK(qol::infra::store::write_section(0, "b", 1, nullptr, 0) == Status::kOk);
    CHECK(qol::infra::store::write_section(0, "c", 1, bytes({7}).data(), 1) == Status::kOk);
    CHECK(qol::infra::store::read_section(0, "b", &out, &version) == Status::kOk);
    CHECK(version == 1 && out.empty());
    // 覆盖 a 不改变顺序；容器仍可解码且三 section 齐全。
    CHECK(qol::infra::store::write_section(0, "a", 3, bytes({1}).data(), 1) == Status::kOk);
    CHECK(qol::infra::store::read_section(0, "a", &out, &version) == Status::kOk);
    CHECK(version == 3 && out == bytes({1}));
    CHECK(qol::infra::store::read_section(0, "c", &out, &version) == Status::kOk);
}

void test_last_good_and_quarantine(const std::string& dir) {
    qol::infra::store::init(dir.c_str());
    CHECK(qol::infra::store::write_section(0, "a", 1, bytes({1}).data(), 1) == Status::kOk);
    CHECK(qol::infra::store::write_section(0, "a", 2, bytes({2}).data(), 1) == Status::kOk);
    CHECK(file_exists(last_good_path(dir, 0)));

    // 截断 primary（CRC 坏）→ 读回退 last-good（gen1 payload=1），primary 被 quarantine。
    std::vector<uint8_t> raw = read_file(primary_path(dir, 0));
    raw.resize(raw.size() - 4);
    std::ofstream out(primary_path(dir, 0), std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(raw.data()), static_cast<std::streamsize>(raw.size()));
    out.close();

    std::vector<uint8_t> payload;
    uint16_t version = 0;
    CHECK(qol::infra::store::read_section(0, "a", &payload, &version) == Status::kOk);
    CHECK(version == 1 && payload == bytes({1}));
    CHECK(!file_exists(primary_path(dir, 0)));       // quarantine 是 rename，不保留原文件
    CHECK(dir_has_corrupt(dir + "/module-saves"));   // 不删除，改名保留

    // 下一次写入把恢复数据写回 primary（preserve_current：last-good 更新）。
    CHECK(qol::infra::store::write_section(0, "a", 3, bytes({3}).data(), 1) == Status::kOk);
    CHECK(qol::infra::store::read_section(0, "a", &payload, &version) == Status::kOk);
    CHECK(version == 3 && payload == bytes({3}));
}

void test_errors(const std::string& dir) {
    qol::infra::store::init(dir.c_str());
    std::vector<uint8_t> out;
    uint16_t version = 0;

    const std::vector<uint8_t> one = bytes({1});
    CHECK(qol::infra::store::write_section(0, "A", 1, one.data(), 1) == Status::kBadName);
    CHECK(qol::infra::store::write_section(0, "", 1, one.data(), 1) == Status::kBadName);
    std::string long_name(65, 'a');
    CHECK(qol::infra::store::write_section(0, long_name.c_str(), 1, one.data(), 1) ==
          Status::kBadName);
    CHECK(qol::infra::store::write_section(0, "ok", 0, one.data(), 1) == Status::kBadVersion);
    CHECK(qol::infra::store::write_section(0, "ok", 1, nullptr, 0) == Status::kOk);

    std::vector<uint8_t> big(1024 * 1024 + 1, 0x5A);
    CHECK(qol::infra::store::write_section(0, "big", 1, big.data(), big.size()) ==
          Status::kTooLarge);

    CHECK(qol::infra::store::ensure_slot(3) == Status::kBadSlot);
    CHECK(qol::infra::store::ensure_slot(-1) == Status::kBadSlot);
    CHECK(qol::infra::store::read_section(0, "missing", &out, &version) == Status::kNotFound);
    CHECK(qol::infra::store::remove_section(0, "missing") == Status::kNotFound);
    CHECK(qol::infra::store::read_container(2, &out) == Status::kNotFound);

    // 手工写坏 primary → read_container kCorrupt。
    qol::infra::store::ensure_slot(1);
    std::ofstream bad(primary_path(dir, 1), std::ios::binary | std::ios::trunc);
    bad << "BADBADBAD";
    bad.close();
    CHECK(qol::infra::store::read_container(1, &out) == Status::kCorrupt);

    // 容器总量 > 4MiB：4 个 1MiB section 超限。
    ReplaceMap too_big;
    for (int i = 0; i < 4; ++i) {
        Section s;
        s.version = 1;
        s.payload.assign(1024 * 1024, static_cast<uint8_t>(i));
        too_big.emplace_back("s" + std::to_string(i), std::move(s));
    }
    CHECK(qol::infra::store::replace_sections(0, too_big, RemoveSet{}) == Status::kTooLarge);
    // 失败后容器未被破坏：ok section 仍在。
    CHECK(qol::infra::store::read_section(0, "ok", &out, &version) == Status::kOk);
}

void test_replace_atomic(const std::string& dir) {
    qol::infra::store::init(dir.c_str());
    CHECK(qol::infra::store::write_section(0, "a", 1, bytes({1}).data(), 1) == Status::kOk);
    CHECK(qol::infra::store::write_section(0, "b", 1, bytes({2}).data(), 1) == Status::kOk);

    // 只读目录注入 IO 失败：原子写第一步失败，primary 保持原状。
    const std::string module_dir = dir + "/module-saves";
    CHECK(::chmod(module_dir.c_str(), 0555) == 0);
    ReplaceMap repl;
    Section c;
    c.version = 1;
    c.payload = bytes({3});
    repl.emplace_back("c", std::move(c));
    CHECK(qol::infra::store::replace_sections(0, repl, RemoveSet{"a"}) == Status::kIoError);
    CHECK(::chmod(module_dir.c_str(), 0755) == 0);

    std::vector<uint8_t> out;
    uint16_t version = 0;
    CHECK(qol::infra::store::read_section(0, "a", &out, &version) == Status::kOk);
    CHECK(qol::infra::store::read_section(0, "b", &out, &version) == Status::kOk);
    CHECK(qol::infra::store::read_section(0, "c", &out, &version) == Status::kNotFound);

    // 正常提交：删除 a、新增 c，b 保留。
    Section c2;
    c2.version = 2;
    c2.payload = bytes({3, 3});
    ReplaceMap repl2;
    repl2.emplace_back("c", std::move(c2));
    CHECK(qol::infra::store::replace_sections(0, repl2, RemoveSet{"a"}) == Status::kOk);
    CHECK(qol::infra::store::read_section(0, "a", &out, &version) == Status::kNotFound);
    CHECK(qol::infra::store::read_section(0, "b", &out, &version) == Status::kOk);
    CHECK(qol::infra::store::read_section(0, "c", &out, &version) == Status::kOk);
    CHECK(version == 2 && out == bytes({3, 3}));

    // 覆盖已存在 section 不改变插入序（b 仍在 c 前）。
    CHECK(qol::infra::store::write_section(0, "b", 4, bytes({4}).data(), 1) == Status::kOk);
    CHECK(qol::infra::store::read_section(0, "b", &out, &version) == Status::kOk);
    CHECK(version == 4 && out == bytes({4}));
}

void test_import_container(const std::string& dir) {
    qol::infra::store::init(dir.c_str());
    CHECK(qol::infra::store::write_section(0, "a", 1, bytes({1, 2}).data(), 2) == Status::kOk);
    CHECK(qol::infra::store::write_section(0, "b", 1, bytes({5}).data(), 1) == Status::kOk);
    std::vector<uint8_t> source;
    CHECK(qol::infra::store::read_container(0, &source) == Status::kOk);

    CHECK(qol::infra::store::import_container(2, source.data(), source.size()) == Status::kOk);
    std::vector<uint8_t> target;
    CHECK(qol::infra::store::read_container(2, &target) == Status::kOk);
    CHECK(target.size() == source.size());
    CHECK(target[6] == 2);                                        // 重定槽
    CHECK(generation_of(target) == generation_of(source) + 1);    // generation 递增

    std::vector<uint8_t> payload;
    uint16_t version = 0;
    CHECK(qol::infra::store::read_section(2, "a", &payload, &version) == Status::kOk);
    CHECK(payload == bytes({1, 2}));
    CHECK(file_exists(last_good_path(dir, 2)));  // 双写

    // 源容器无效 → kCorrupt；目标保持原状。
    const std::string garbage = "not-a-msav-container";
    CHECK(qol::infra::store::import_container(2, reinterpret_cast<const uint8_t*>(garbage.data()),
                                              garbage.size()) == Status::kCorrupt);
    CHECK(qol::infra::store::read_section(2, "a", &payload, &version) == Status::kOk);

    // 源容器槽号越界（第 6 字节 = 5）→ kCorrupt。
    std::vector<uint8_t> bad_slot = source;
    bad_slot[6] = 5;
    CHECK(qol::infra::store::import_container(2, bad_slot.data(), bad_slot.size()) ==
          Status::kCorrupt);

    // 源容器 header 尾 bodyCRC 坏 → kCorrupt（目标保持原状）。
    std::vector<uint8_t> corrupt_crc = source;
    corrupt_crc[corrupt_crc.size() - 1] ^= 0xFF;
    CHECK(qol::infra::store::import_container(2, corrupt_crc.data(), corrupt_crc.size()) ==
          Status::kCorrupt);
    CHECK(qol::infra::store::read_section(2, "a", &payload, &version) == Status::kOk);
}

void test_delete_and_reset(const std::string& dir) {
    qol::infra::store::init(dir.c_str());
    CHECK(qol::infra::store::write_section(0, "a", 1, bytes({1}).data(), 1) == Status::kOk);
    CHECK(qol::infra::store::delete_slot(0) == Status::kOk);
    std::vector<uint8_t> out;
    CHECK(qol::infra::store::read_container(0, &out) == Status::kNotFound);
    CHECK(!file_exists(primary_path(dir, 0)));
    CHECK(!file_exists(last_good_path(dir, 0)));

    CHECK(qol::infra::store::ensure_slot(0) == Status::kOk);
    CHECK(file_exists(primary_path(dir, 0)));
    uint16_t version = 0;
    CHECK(qol::infra::store::read_section(0, "a", &out, &version) == Status::kNotFound);

    CHECK(qol::infra::store::write_section(0, "a", 1, bytes({1}).data(), 1) == Status::kOk);
    CHECK(qol::infra::store::reset_slot(0) == Status::kOk);
    CHECK(qol::infra::store::read_section(0, "a", &out, &version) == Status::kNotFound);
    CHECK(read_file(primary_path(dir, 0)) == kGoldenEmptySlot0Gen1);
}

void test_status_name() {
    CHECK(std::strcmp(qol::infra::store::status_name(Status::kOk), "ok") == 0);
    CHECK(std::strcmp(qol::infra::store::status_name(Status::kNotFound), "not_found") == 0);
    CHECK(std::strcmp(qol::infra::store::status_name(Status::kCorrupt), "corrupt") == 0);
    CHECK(std::strcmp(qol::infra::store::status_name(Status::kUnavailable), "unavailable") == 0);
}

void run_case(void (*fn)(const std::string&)) {
    const std::string dir = make_temp_dir();
    CHECK(!dir.empty());
    fn(dir);
    remove_tree(dir);
}

}  // namespace

int main() {
    test_unavailable();
    test_status_name();
    run_case(test_golden_bytes);
    run_case(test_roundtrip_and_generation);
    run_case(test_last_good_and_quarantine);
    run_case(test_errors);
    run_case(test_replace_atomic);
    run_case(test_import_container);
    run_case(test_delete_and_reset);
    std::printf("infra_store_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
