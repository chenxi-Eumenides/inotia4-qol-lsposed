// P3c：扩展背包 save.slot_data 消费面（extension_bag_persistence.cpp）host 测试。
// 覆盖：注册（session ready 时 catch_up 激活 → SessionBound load 自动触发）、load 回调
// codec（missing 默认状态 / present 解析 / 准入放行）、capture 单次序列化、committed 清
// dirty、会话准入（未加载拒绝）、dirty 排空 request_write 的放行与拒绝分支、codec bytes
// 等价（state_json → parse → state_json 幂等往返）。
#include "contracts/game_host.h"
#include "contracts/slot_data.h"
#include "plugins/core.save/slot_data_provider.h"
#include "plugins/extension_bag/extension_bag_context.h"

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

// ---- 测试提供的 EB 环境（persistence.cpp 的 extern / context 依赖）----
std::mutex g_virtual_bag_mtx;
bool g_item_state_dirty = false;
uint64_t g_item_revision = 0;
void extension_bag_mark_dirty_locked() {
    g_item_state_dirty = true;
    ++g_item_revision;
}
uint64_t extension_bag_item_revision_locked() { return g_item_revision; }
void extension_bag_slot_data_complete_for_test(const qol::contracts::QolSlotDataContext*, uint32_t);

namespace qol::features::extension_bag {
const qol::contracts::QolHostSdk* extension_bag_host_sdk() { return nullptr; }
}  // namespace qol::features::extension_bag

namespace {

using namespace qol::contracts;
static int g_pass = 0, g_fail = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; \
    std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

virtual_bag::State g_state{};
int32_t is_main() { return 1; }
QolResult verify_ok(QolOwnerToken token) { return token == 7 ? ok() : error(kQolErrBlocked, "no", 2); }

QolResult store_ensure(QolOwnerToken, int32_t) { return ok(); }
QolResult store_reset(QolOwnerToken, int32_t) { return ok(); }
struct StoredSection { uint16_t version; std::vector<uint8_t> payload; };
std::vector<std::pair<std::string, StoredSection>> g_sections;

StoredSection* find_section(const char* name) {
    for (auto& entry : g_sections)
        if (entry.first == name) return &entry.second;
    return nullptr;
}
QolResult store_read(QolOwnerToken, int32_t, const char* name, uint8_t* out, uint64_t cap,
                     uint64_t* size, uint16_t* version) {
    const auto* section = find_section(name);
    if (section == nullptr) return error(kQolErrNotFound, "missing", 7);
    if (section->payload.size() > cap) return error(kQolErrInvalidArgument, "large", 5);
    if (!section->payload.empty()) std::memcpy(out, section->payload.data(), section->payload.size());
    *size = section->payload.size();
    *version = section->version;
    return ok();
}
QolResult store_write(QolOwnerToken, int32_t, const char* name, uint16_t version,
                      const uint8_t* bytes, uint64_t size) {
    auto* section = find_section(name);
    if (section == nullptr) {
        g_sections.emplace_back(name, StoredSection{});
        section = &g_sections.back().second;
    }
    section->version = version;
    section->payload.assign(bytes, bytes + size);
    return ok();
}
QolResult store_remove(QolOwnerToken, int32_t, const char* name) {
    for (auto it = g_sections.begin(); it != g_sections.end(); ++it) {
        if (it->first == name) {
            g_sections.erase(it);
            return ok();
        }
    }
    return ok();
}
QolResult store_replace(QolOwnerToken, int32_t, const QolStoreSection* sections, uint32_t count,
                        const char* const* removals, uint32_t removal_count) {
    for (uint32_t i = 0; i < count; ++i) {
        auto* section = find_section(sections[i].name);
        if (section == nullptr) {
            g_sections.emplace_back(sections[i].name, StoredSection{});
            section = &g_sections.back().second;
        }
        section->version = sections[i].version;
        section->payload.assign(sections[i].data, sections[i].data + sections[i].size);
    }
    for (uint32_t i = 0; i < removal_count; ++i) (void)store_remove(0, 0, removals[i]);
    return ok();
}

const QolOwnerStoreApi kBackend{
    {sizeof(QolOwnerStoreApi), 1, 0}, &store_ensure, &store_reset, &store_read, &store_write,
    &store_remove, &store_replace, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, nullptr};

// ---- slot_data request_write 排空面（fake api；注册/卸载走真 provider api）----
int g_request_write_calls = 0;
QolResult api_request_write(QolSlotDataHandle, uint64_t, QolSlotDataRequest* out) {
    ++g_request_write_calls;
    if (out != nullptr) *out = 99;
    return ok();
}

std::string section_json(const char* name) {
    const auto* section = find_section(name);
    return section == nullptr
               ? std::string()
               : std::string(section->payload.begin(), section->payload.end());
}

}  // namespace

// context.h 的访问器与 game_ui_virtbag.h 的准入依赖（persistence.cpp 引用）。
virtual_bag::State* extension_bag_state() { return &g_state; }
uint64_t extension_bag_isolation_now_ms() { return 1234; }
bool virtual_bag_prepare_save_slot_load() { return true; }
bool virtual_bag_module_save_validate_locked() { return true; }  // R-66 校验的测试替身。
qol::contracts::QolItemCountEncoding virtual_bag_category_uses_stack_count(int) {
    // count-encoded 判定：round-trip 测试需要 category 参与数量编码（kUnknown 会被
    // state_json fail-closed 拒绝，R-40）。
    return qol::contracts::QolItemCountEncoding::kEncoded;
}

int main() {
    using namespace qol::contracts;
    using namespace qol::features::core_save;
    CHECK(succeeded(slot_data_initialize(7, &verify_ok, &kBackend, &is_main)));
    slot_data_set_available(true);
    // session ready：epoch=1、slot=0。
    CHECK(succeeded(slot_data_begin_transition(static_cast<uint32_t>(QolSlotDataReason::kEnter), 0)));
    CHECK(succeeded(slot_data_confirm_transition(0)));
    QolSlotSession session{-1, 0, 0};
    CHECK(succeeded(slot_data_api().session(&session)));
    CHECK(session.state == static_cast<uint32_t>(QolSlotSessionState::kReady) && session.epoch == 1);

    // ---- 注册：session ready → catch_up 激活（SessionBound load 自动触发）----
    CHECK(!extension_bag_session_admitted_locked());  // 注册前未加载：准入拒绝。
    const QolSlotDataApi flush_api{{sizeof(QolSlotDataApi), 1, 0}, nullptr, nullptr,
                                   &api_request_write, nullptr, nullptr, nullptr};
    CHECK(extension_bag_slot_data_register(7, &slot_data_api()));
    slot_data_pump();
    // load（missing）→ 默认状态 → 准入放行；session_ready 通知登记 epoch。
    CHECK(extension_bag_session_admitted_locked());

    // ---- dirty 排空：admitted 时 request_write 恰一次；非 admitted 不排空 ----
    g_request_write_calls = 0;
    extension_bag_mark_dirty_locked();
    extension_bag_flush_dirty_staging(&flush_api);
    CHECK(g_request_write_calls == 1);
    extension_bag_flush_dirty_staging(&flush_api);
    extension_bag_flush_dirty_staging(&flush_api);
    CHECK(g_request_write_calls == 1);  // 多帧无新 revision 只通知一次。
    g_item_state_dirty = false;
    extension_bag_flush_dirty_staging(&flush_api);
    CHECK(g_request_write_calls == 1);  // 非 dirty：不排空。

    // ---- SlotBound 写请求：handle=0 非法拒绝（真实 handle 由注册期分配）----
    QolSlotDataRequest write_request = 0;
    CHECK(slot_data_api().request_write(0, session.epoch, &write_request).code ==
          kQolErrNotReady);
    CHECK(g_request_write_calls == 1);  // 拒绝路径不经排空面。

    // ---- codec bytes 等价：state_json → parse → state_json 幂等往返 ----
    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        virtual_bag::State roundtrip{};
        const std::string first = virtual_bag::state_json(g_state);
        CHECK(virtual_bag::parse_state_json_at(first.c_str(), &roundtrip, 0,
                                               virtual_bag_category_uses_stack_count));
        CHECK(virtual_bag::state_json(roundtrip) == first);  // 空状态往返字节等价。
        g_state.types[0] = 1;
        g_state.items[0][0] = virtual_bag::Item{321, 7};
        virtual_bag::normalize(&g_state);  // capacities 是派生字段：先归一再序列化。
        const std::string nonempty = virtual_bag::state_json(g_state);
        CHECK(virtual_bag::parse_state_json_at(nonempty.c_str(), &roundtrip, 0,
                                               virtual_bag_category_uses_stack_count));
        CHECK(virtual_bag::state_json(roundtrip) == nonempty);  // 非空状态往返字节等价。
    }

    // ---- B10：complete(kCommitted) 只清匹配的捕获 revision ----
    {
        SlotDataSaveView view{};
        extension_bag_mark_dirty_locked();
        CHECK(succeeded(slot_data_save_begin(100, &view)));
        const char* section = nullptr;
        uint16_t version = 0;
        std::vector<uint8_t> buffer(65536);
        uint64_t size = 0;
        QolOwnerToken captured_owner = 0;
        CHECK(succeeded(slot_data_save_capture_next(100, &section, &version, buffer.data(),
                                                    buffer.size(), &size, &captured_owner)));
        CHECK(captured_owner == 7);
        slot_data_save_mark_pending(100);
        CHECK(!extension_bag_session_admitted_locked()); // pending 立即关闭依赖该槽的操作门。
        slot_data_save_finish(100, static_cast<uint32_t>(QolSlotDataOutcome::kCommitted));
        CHECK(extension_bag_session_admitted_locked());
        CHECK(!g_item_state_dirty);  // 捕获后无修改：清 dirty。
    }
    {
        SlotDataSaveView view{};
        extension_bag_mark_dirty_locked();
        CHECK(succeeded(slot_data_save_begin(101, &view)));
        const char* section = nullptr;
        uint16_t version = 0;
        std::vector<uint8_t> buffer(65536);
        uint64_t size = 0;
        QolOwnerToken captured_owner = 0;
        CHECK(succeeded(slot_data_save_capture_next(101, &section, &version, buffer.data(),
                                                    buffer.size(), &size, &captured_owner)));
        // 捕获后产生新修改（state_json 变化）→ committed 不得清 dirty。
        {
            std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
            g_state.items[1][0] = virtual_bag::Item{123, 1};
            virtual_bag::normalize(&g_state);
            extension_bag_mark_dirty_locked();
        }
        slot_data_save_finish(101, static_cast<uint32_t>(QolSlotDataOutcome::kCommitted));
        CHECK(g_item_state_dirty);  // 捕获后的新修改：保留 dirty。
    }
    {
        SlotDataSaveView view{};
        extension_bag_mark_dirty_locked();
        CHECK(succeeded(slot_data_save_begin(102, &view)));
        const char* section = nullptr;
        uint16_t version = 0;
        std::vector<uint8_t> buffer(65536);
        uint64_t size = 0;
        CHECK(succeeded(slot_data_save_capture_next(102, &section, &version, buffer.data(),
                                                    buffer.size(), &size)));
        {
            std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
            g_state.pending.valid = true;
            extension_bag_mark_dirty_locked();
        }
        QolSlotDataContext wrong_context{session, static_cast<uint32_t>(QolSlotDataReason::kSave),
                                         999};
        extension_bag_slot_data_complete_for_test(
            &wrong_context, static_cast<uint32_t>(QolSlotDataOutcome::kCommitted));
        CHECK(g_item_state_dirty && g_state.pending.valid); // 不匹配 tx 不清 pending/dirty。
        slot_data_save_finish(102, static_cast<uint32_t>(QolSlotDataOutcome::kCommitted));
        CHECK(g_item_state_dirty && g_state.pending.valid); // 捕获后 revision 改变仍保留。
    }

    // ---- B9：prepare_detach 开始即撤销本地准入（detach 窗口拒绝扫描/物品操作）----
    CHECK(extension_bag_session_admitted_locked());
    CHECK(succeeded(slot_data_begin_transition(
        static_cast<uint32_t>(QolSlotDataReason::kEnter), 0)));
    CHECK(!extension_bag_session_admitted_locked());
    CHECK(succeeded(slot_data_abandon_transition()));

    // ---- 注销：先关准入（未加载/降级拒绝），注册请求由 query 收敛 ----
    const QolSlotDataRequest unregistered = extension_bag_slot_data_unregister(&slot_data_api());
    CHECK(unregistered != 0);
    slot_data_pump();
    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        CHECK(!extension_bag_session_admitted_locked());
    }
    g_item_state_dirty = true;
    extension_bag_flush_dirty_staging(&flush_api);
    CHECK(g_request_write_calls == 1);  // 注销后不排空。

    slot_data_set_available(false);
    std::printf("extension_bag_slot_data: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
