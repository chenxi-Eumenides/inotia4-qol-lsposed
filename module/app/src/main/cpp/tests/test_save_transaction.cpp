#include "contracts/save_route.h"
#include "game/access/save_transaction.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>

namespace contracts = qol::contracts;
using namespace qol::contracts;

namespace {
int g_pass = 0, g_fail = 0;
#define CHECK(c) \
    do { \
        if (c) ++g_pass; \
        else { \
            ++g_fail; \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
        } \
    } while (0)

constexpr uintptr_t kEntry = 0x1000;
constexpr uintptr_t kSaveEntry = 0x2000;
constexpr uintptr_t kSpan = 0x100;

struct ScanBuffer {
    std::vector<uint8_t> bytes;
    uintptr_t base = kEntry;
    bool accessible_fail = false;
};

bool scan_read(void* ctx, uintptr_t site, uint32_t* out_word) {
    auto* buffer = static_cast<ScanBuffer*>(ctx);
    if (buffer->accessible_fail) return false;
    if (site < buffer->base || site + 4 > buffer->base + buffer->bytes.size()) return false;
    std::memcpy(out_word, buffer->bytes.data() + (site - buffer->base), 4);
    return true;
}

uint32_t bl_word(uintptr_t site, uintptr_t target) {
    const int64_t offset = static_cast<int64_t>(target) - static_cast<int64_t>(site);
    const int64_t immediate = offset / 4;
    return 0x94000000u | static_cast<uint32_t>(immediate & 0x03FFFFFF);
}


int g_current_slot = 0;
int fake_current_slot() { return g_current_slot; }

void fake_get_name(int32_t slot, char* out) {
    std::snprintf(out, 64, "save%d.dat", slot);
}
void fake_get_name_directory(int32_t, char* out) {
    std::snprintf(out, 256, "fcea920f7412b5da7be0cf42b8c93759/save0.dat");
}
void fake_get_name_absolute(int32_t, char* out) {
    std::snprintf(out, 256, "/data/data/com.example/files/save0.dat");
}
void fake_get_name_outside_absolute(int32_t, char* out) {
    std::snprintf(out, 256, "/data/data/other.app/save0.dat");
}
void fake_get_name_broken(int32_t, char* out) {
    std::snprintf(out, 256, "fcea920f7412b5da7be0cf42b8c93759/../../etc/passwd");
}
void fake_get_name_empty(int32_t, char* out) { out[0] = '\0'; }
void fake_get_name_too_long(int32_t, char* out) { std::memset(out, 'x', 256); }

int g_trampoline_calls = 0;
int g_trampoline_result = -42;
int fake_trampoline() {
    ++g_trampoline_calls;
    return g_trampoline_result;
}

int g_resume_calls = 0;
int g_resume_result = 1;
int fake_resume(int32_t) {
    ++g_resume_calls;
    return g_resume_result;
}
}  // namespace

namespace save_tx = qol::game;

int main() {
    // ---- BL 扫描：唯一命中有效；零命中/多命中/读取失败 fail-closed ----
    ScanBuffer buffer;
    buffer.bytes.assign(kSpan, 0);
    const uint32_t first_bl = bl_word(kEntry + 0x20, kSaveEntry);
    std::memcpy(buffer.bytes.data() + 0x20, &first_bl, 4);
    const auto single = save_tx::save_bypass_scan(kEntry, kSpan, kSaveEntry, &scan_read, &buffer);
    CHECK(single.valid && single.callsite == kEntry + 0x20 && single.save_entry == kSaveEntry);

    const uint32_t second_bl = bl_word(kEntry + 0x40, kSaveEntry);
    std::memcpy(buffer.bytes.data() + 0x40, &second_bl, 4);
    const auto doubled = save_tx::save_bypass_scan(kEntry, kSpan, kSaveEntry, &scan_read, &buffer);
    CHECK(!doubled.valid && doubled.callsite == 0);

    buffer.bytes.assign(kSpan, 0);
    const auto none = save_tx::save_bypass_scan(kEntry, kSpan, kSaveEntry, &scan_read, &buffer);
    CHECK(!none.valid);

    buffer.bytes.assign(kSpan, 0);
    std::memcpy(buffer.bytes.data() + 0x20, &first_bl, 4);
    buffer.accessible_fail = true;
    const auto unreadable =
        save_tx::save_bypass_scan(kEntry, kSpan, kSaveEntry, &scan_read, &buffer);
    CHECK(!unreadable.valid);

    const auto bad_span =
        save_tx::save_bypass_scan(kEntry, 0x102, kSaveEntry, &scan_read, &buffer);
    CHECK(!bad_span.valid);

    // ---- invocation 生命周期：仅当前回调有效 ----
    const uint64_t first = save_tx::save_invocation_begin(3, 1);
    CHECK(first != 0);
    CHECK(save_tx::save_invocation_valid(first, 3, 1));
    CHECK(!save_tx::save_invocation_valid(first, 4, 1));
    CHECK(!save_tx::save_invocation_valid(first, 3, 2));
    const uint64_t second = save_tx::save_invocation_begin(4, 2);
    CHECK(!save_tx::save_invocation_valid(first, 3, 1));  // 新 invocation 覆盖旧的
    CHECK(save_tx::save_invocation_valid(second, 4, 2));
    save_tx::save_invocation_end(second);
    CHECK(!save_tx::save_invocation_valid(second, 4, 2));
    save_tx::save_invocation_end(second);  // 幂等

    // ---- loading 旁路判定矩阵 ----
    save_tx::SaveLoadingBypassInput input{};
    input.on_game_main_thread = true;
    input.invocation_verified = true;
    input.has_unready_load_transition = true;
    input.transition_target = 0;
    input.current_slot = 0;
    input.lease_covers_slot = true;
    input.joint_save_in_flight = false;
    input.session_ready = false;
    CHECK(save_tx::save_admit_loading_bypass(input).ok == kQolOk);
    // 翻转单字段构造非法组合；每次翻转后恢复，保持其余字段合法。
    const auto blocked_case = [&input](bool* flag) {
        *flag = !*flag;
        const auto copy = input;
        *flag = !*flag;
        return save_tx::save_admit_loading_bypass(copy).code;
    };
    CHECK(blocked_case(&input.on_game_main_thread) == kQolErrBlocked);
    CHECK(blocked_case(&input.invocation_verified) == kQolErrBlocked);
    CHECK(blocked_case(&input.has_unready_load_transition) == kQolErrBlocked);
    input.transition_target = 1;  // target != current
    CHECK(save_tx::save_admit_loading_bypass(input).code == kQolErrBlocked);
    input.transition_target = 0;
    CHECK(blocked_case(&input.lease_covers_slot) == kQolErrBlocked);
    CHECK(blocked_case(&input.joint_save_in_flight) == kQolErrBlocked);
    input.session_ready = true;  // session ready → 普通路径放行
    CHECK(save_tx::save_admit_loading_bypass(input).ok == kQolOk);
    input.session_ready = false;

    // ---- transition 状态 ----
    int32_t target = -1;
    CHECK(!save_tx::save_has_unready_load_transition(&target));
    save_tx::save_set_pending_transition(99, 2, false);
    CHECK(save_tx::save_has_unready_load_transition(&target) && target == 2);
    save_tx::save_set_pending_transition(99, 2, true);
    CHECK(!save_tx::save_has_unready_load_transition(&target));
    save_tx::save_clear_pending_transition();
    CHECK(!save_tx::save_has_unready_load_transition(&target));
    const uint64_t scoped = save_tx::save_load_bypass_begin(100, 0, 1);
    CHECK(scoped != 0);
    CHECK(save_tx::save_load_bypass_begin(101, 1, 1) == 0);  // R9：禁止 scope 嵌套。
    save_tx::save_load_bypass_end(scoped + 1);
    CHECK(save_tx::save_has_unready_load_transition(&target) && target == 0);
    save_tx::save_load_bypass_end(scoped);
    CHECK(!save_tx::save_has_unready_load_transition(&target));

    // ---- P3c 生产仲裁组装（save_loading_bypass_admit）：Game 层事实 + gate 凭据 ----
    {
        save_tx::save_transaction_reset_for_test();
        save_tx::SaveTransactionGate admit_gate{};
        admit_gate.operation_admits = +[](void*, QolOwnerToken, const QolSlotOperationLease*,
                                          int32_t) { return true; };
        save_tx::save_transaction_bind_gate(&admit_gate);
        save_tx::save_transaction_set_current_slot_fn(&fake_current_slot);
        g_current_slot = 0;
        QolSlotOperationLease lease{};
        lease.size = sizeof(lease);
        lease.owner = 7;
        lease.slot_mask = 1u;
        // 全条件满足：kResume invocation + 未就绪 transition(target==current) + gate 放行。
        (void)save_tx::save_invocation_begin(
            static_cast<uint32_t>(qol::contracts::QolSaveCallKind::kResume), 0);
        save_tx::save_set_pending_transition(11, 0, false);
        CHECK(save_tx::save_loading_bypass_admit(&lease, 0).ok == kQolOk);
        // 无 invocation（不在读档窗口）：阻断。
        save_tx::save_invocation_end(
            0);  // 未知 id：不清当前 invocation；用新 invocation 覆盖为非 kResume。
        (void)save_tx::save_invocation_begin(9, 0);
        CHECK(save_tx::save_loading_bypass_admit(&lease, 0).code == kQolErrBlocked);
        // 转换已 ready（无未就绪 transition）：阻断。
        save_tx::save_invocation_end(0);
        (void)save_tx::save_invocation_begin(
            static_cast<uint32_t>(qol::contracts::QolSaveCallKind::kResume), 0);
        save_tx::save_set_pending_transition(11, 0, true);
        CHECK(save_tx::save_loading_bypass_admit(&lease, 0).code == kQolErrBlocked);
        save_tx::save_clear_pending_transition();
        save_tx::save_transaction_reset_for_test();
    }

    // ---- call_original_save：未绑定 NotReady；绑定后 gate+槽校验+结果透传 ----
    save_tx::save_transaction_reset_for_test();
    QolSlotOperationLease lease{};
    lease.size = sizeof(lease);
    lease.owner = 7;
    lease.id = 5;
    lease.slot_mask = 1u;
    lease.revision = 1;
    save_tx::SaveTransactionGate gate{};
    gate.operation_admits = +[](void*, QolOwnerToken, const QolSlotOperationLease*, int32_t) {
        return true;
    };
    save_tx::save_transaction_bind_gate(&gate);
    save_tx::save_transaction_set_current_slot_fn(&fake_current_slot);
    g_current_slot = 0;
    QolSaveOriginalResult original{};
    original.size = sizeof(original);
    CHECK(save_tx::save_transaction_call_original_save(&lease, 0, &original).code == kQolErrNotReady);
    CHECK(original.invoked == 0 && original.result_valid == 0);
    CHECK(save_tx::save_transaction_bind_backup(0, reinterpret_cast<void*>(&fake_trampoline)).ok == kQolOk);
    CHECK(save_tx::save_transaction_call_original_save(nullptr, 0, &original).code ==
          kQolErrInvalidArgument);
    CHECK(save_tx::save_transaction_call_original_save(&lease, 3, &original).code ==
          kQolErrInvalidArgument);
    g_current_slot = 1;  // 槽漂移
    CHECK(save_tx::save_transaction_call_original_save(&lease, 0, &original).code == kQolErrBusy);
    g_current_slot = 0;
    CHECK(save_tx::save_transaction_call_original_save(&lease, 0, &original).ok == kQolOk);
    CHECK(original.invoked == 1 && original.result_valid == 1 &&
          original.original_result == -42 && g_trampoline_calls == 1);

    // gate 拒绝 → Blocked，不执行原版。
    save_tx::SaveTransactionGate rejecting{};
    rejecting.operation_admits = +[](void*, QolOwnerToken, const QolSlotOperationLease*, int32_t) {
        return false;
    };
    save_tx::save_transaction_bind_gate(&rejecting);
    CHECK(save_tx::save_transaction_call_original_save(&lease, 0, &original).code == kQolErrBlocked);
    CHECK(g_trampoline_calls == 1);
    save_tx::save_transaction_bind_gate(&gate);

    // ---- call_transition：凭据校验后按注入的原版入口执行（B7 转换桥）----
    QolSaveTransitionCall transition{};
    transition.size = sizeof(transition);
    transition.target_slot = 0;
    // 未注入 resume 入口：NotReady，不执行原版。
    CHECK(save_tx::save_transaction_call_transition(&lease, &transition, &original).code ==
          kQolErrNotReady);
    CHECK(original.invoked == 0);
    save_tx::save_transaction_bind_gate(&rejecting);
    CHECK(save_tx::save_transaction_call_transition(&lease, &transition, &original).code ==
          kQolErrBlocked);
    save_tx::save_transaction_bind_gate(&gate);
    // 注入 resume 入口：真实执行并按作用域释放旁路上下文（pending transition + invocation）。
    save_tx::save_transaction_set_resume_fn(&fake_resume);
    g_resume_calls = 0;
    g_resume_result = 1;
    transition.transition = 777;
    CHECK(save_tx::save_transaction_call_transition(&lease, &transition, &original).ok == kQolOk);
    CHECK(g_resume_calls == 1 && original.invoked == 1 && original.result_valid == 1 &&
          original.original_result == 1);
    // 作用域结束后旁路上下文已清除：无未就绪读档转换。
    int32_t leftover_target = -1;
    CHECK(!save_tx::save_has_unready_load_transition(&leftover_target));
    const uint64_t occupied_scope = save_tx::save_load_bypass_begin(778, 0, 1);
    CHECK(occupied_scope != 0);
    transition.transition = 779;
    const int resume_before_nested = g_resume_calls;
    CHECK(save_tx::save_transaction_call_transition(&lease, &transition, &original).code ==
          kQolErrBusy);
    CHECK(original.invoked == 0 && g_resume_calls == resume_before_nested);
    save_tx::save_load_bypass_end(occupied_scope);
    transition.transition = 777;
    // 原版 resume 返回失败：invoked 保留事实、结果码为失败。
    g_resume_result = 0;
    CHECK(save_tx::save_transaction_call_transition(&lease, &transition, &original).code ==
          kQolErrInternal);
    CHECK(original.invoked == 1 && original.original_result == 0);
    save_tx::save_transaction_set_resume_fn(nullptr);
    transition.kind = static_cast<uint32_t>(QolSaveTransitionKind::kBeginNew);
    CHECK(save_tx::save_transaction_call_transition(&lease, &transition, &original).code ==
          kQolErrUnsupported);  // R8：无可靠 begin_new 端口，不得谎报已调用。
    CHECK(original.invoked == 0);

    // ---- FS 返回名路径解析：相对/绝对同根允许，路径逃逸 fail-closed ----
    const char* app_dir = "/data/data/com.example/files";
    char resolved_path[1024]{};
    bool was_absolute = false;
    CHECK(save_tx::save_transaction_resolve_dat_path(
        app_dir, "fcea920f7412b5da7be0cf42b8c93759/save0.dat", resolved_path,
        sizeof(resolved_path), &was_absolute));
    CHECK(!was_absolute && std::string(resolved_path) ==
          "/data/data/com.example/files/fcea920f7412b5da7be0cf42b8c93759/save0.dat");
    CHECK(save_tx::save_transaction_resolve_dat_path(app_dir, "save0.dat", resolved_path,
                                                      sizeof(resolved_path), &was_absolute));
    CHECK(!was_absolute && std::string(resolved_path) ==
          "/data/data/com.example/files/save0.dat");
    CHECK(save_tx::save_transaction_resolve_dat_path(
        app_dir, "/data/data/com.example/files/save0.dat", resolved_path,
        sizeof(resolved_path), &was_absolute) && was_absolute);
    CHECK(!save_tx::save_transaction_resolve_dat_path(
        app_dir, "/data/data/other.app/save0.dat", resolved_path,
        sizeof(resolved_path), &was_absolute));
    CHECK(!save_tx::save_transaction_resolve_dat_path(
        app_dir, "fcea/../../etc/passwd", resolved_path, sizeof(resolved_path), &was_absolute));
    CHECK(!save_tx::save_transaction_resolve_dat_path(
        app_dir, "dir/../save0.dat", resolved_path, sizeof(resolved_path), &was_absolute));
    CHECK(!save_tx::save_transaction_resolve_dat_path(
        app_dir, "dir//save0.dat", resolved_path, sizeof(resolved_path), &was_absolute));
    CHECK(!save_tx::save_transaction_resolve_dat_path(
        app_dir, "dir/./save0.dat", resolved_path, sizeof(resolved_path), &was_absolute));
    CHECK(!save_tx::save_transaction_resolve_dat_path(
        app_dir, "", resolved_path, sizeof(resolved_path), &was_absolute));
    std::string long_name(300, 'a');
    CHECK(!save_tx::save_transaction_resolve_dat_path(
        app_dir, long_name.c_str(), resolved_path, sizeof(resolved_path), &was_absolute));

    // ---- read_evidence：路径上下文/gate/文件三重条件 ----
    QolSaveFileEvidence evidence{};
    CHECK(save_tx::save_transaction_read_evidence(&lease, 0, &evidence).code == kQolErrNotReady);
    save_tx::save_transaction_set_platform_dir("/nonexistent-root");
    CHECK(save_tx::save_transaction_read_evidence(&lease, 0, &evidence).code == kQolErrNotReady);
    save_tx::save_transaction_set_platform_dir(".");
    CHECK(save_tx::save_transaction_read_evidence(&lease, 3, &evidence).code == kQolErrInvalidArgument);
    CHECK(save_tx::save_transaction_read_evidence(nullptr, 0, &evidence).code == kQolErrInvalidArgument);
    // gate 拒绝 → Blocked（fail-closed，无凭据不读证据；路径上下文需先就绪）。
    save_tx::save_transaction_set_get_name_fn(&fake_get_name);
    save_tx::save_transaction_bind_gate(&rejecting);
    CHECK(save_tx::save_transaction_read_evidence(&lease, 0, &evidence).code == kQolErrBlocked);
    save_tx::save_transaction_set_platform_dir(app_dir);
    // 名字解析不可靠 → InvalidArgument，不扫描兜底。
    save_tx::save_transaction_set_get_name_fn(&fake_get_name_broken);
    save_tx::save_transaction_bind_gate(&gate);
    CHECK(save_tx::save_transaction_read_evidence(&lease, 0, &evidence).code == kQolErrInvalidArgument);
    save_tx::save_transaction_set_get_name_fn(&fake_get_name_empty);
    CHECK(save_tx::save_transaction_read_evidence(&lease, 0, &evidence).code == kQolErrInvalidArgument);
    save_tx::save_transaction_set_get_name_fn(&fake_get_name_too_long);
    CHECK(save_tx::save_transaction_read_evidence(&lease, 0, &evidence).code == kQolErrInvalidArgument);
    save_tx::save_transaction_set_get_name_fn(&fake_get_name_directory);
    CHECK(save_tx::save_transaction_read_evidence(&lease, 0, &evidence).code == kQolErrNotFound);
    save_tx::save_transaction_set_get_name_fn(&fake_get_name_absolute);
    save_tx::save_transaction_set_platform_dir(app_dir);
    CHECK(save_tx::save_transaction_read_evidence(&lease, 0, &evidence).code == kQolErrNotFound);
    save_tx::save_transaction_set_get_name_fn(&fake_get_name_outside_absolute);
    CHECK(save_tx::save_transaction_read_evidence(&lease, 0, &evidence).code == kQolErrInvalidArgument);
    save_tx::save_transaction_set_get_name_fn(nullptr);
    CHECK(save_tx::save_transaction_read_evidence(&lease, 0, &evidence).code == kQolErrNotReady);
    save_tx::save_transaction_set_get_name_fn(&fake_get_name);
    save_tx::save_transaction_set_platform_dir("");
    CHECK(save_tx::save_transaction_read_evidence(&lease, 0, &evidence).code == kQolErrNotReady);
    // 文件缺失 → NotFound。
    save_tx::save_transaction_set_platform_dir("/nonexistent-root-xyz");
    CHECK(save_tx::save_transaction_read_evidence(&lease, 0, &evidence).code == kQolErrNotFound);
    // 真实文件：evidence 内容与字节一致。
    char working_dir[512]{};
    CHECK(::getcwd(working_dir, sizeof(working_dir)) != nullptr);
    save_tx::save_transaction_set_platform_dir(working_dir);
    {
        FILE* file = std::fopen("save0.dat", "wb");
        CHECK(file != nullptr);
        const uint8_t payload[5] = {1, 2, 3, 4, 5};
        std::fwrite(payload, 1, sizeof(payload), file);
        std::fclose(file);
    }
    CHECK(save_tx::save_transaction_read_evidence(&lease, 0, &evidence).ok == kQolOk);
    CHECK(evidence.file_length == 5 && evidence.sha256[0] != 0);
    std::remove("save0.dat");

    save_tx::save_transaction_reset_for_test();
    std::printf("save_transaction_tests: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
