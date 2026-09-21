#include "feature/extension_bag/extension_bag_retry_log.h"
#include "feature/patch/game_patch_move_merge_log.h"

#include <cstdio>

namespace {

int g_pass = 0;
int g_fail = 0;

#define CHECK(condition)                                                            \
    do {                                                                            \
        if (condition) {                                                            \
            ++g_pass;                                                              \
        } else {                                                                    \
            ++g_fail;                                                              \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);       \
        }                                                                           \
    } while (false)

using extension_bag_retry::FailureKey;
using extension_bag_retry::FailureKind;

void test_retry_log_state_transitions() {
    const FailureKey not_loaded{FailureKind::kNativeHookUnavailable, 0x1000};
    const FailureKey changed_target{FailureKind::kNativeHookUnavailable, 0x2000};
    const FailureKey changed_kind{FailureKind::kSaveHookFailed, 0x1000};

    extension_bag_retry::RetryLogState state;
    CHECK(state.observe_failure(not_loaded));
    CHECK(!state.observe_failure(not_loaded));
    CHECK(state.observe_failure(changed_target));
    CHECK(state.observe_failure(changed_kind));

    state.reset();
    CHECK(state.observe_failure(not_loaded));
}

void test_partial_injection_does_not_reset() {
    const FailureKey save_panel{FailureKind::kSavePanelUnavailable, 0x1000};
    extension_bag_retry::RetryLogState state;

    CHECK(!extension_bag_retry::injection_complete({true, false, true, true}));
    CHECK(state.observe_failure(save_panel));
    CHECK(!state.observe_failure(save_panel));
    CHECK(extension_bag_retry::injection_complete({true, true, true, true}));
    state.reset();
    CHECK(state.observe_failure(save_panel));
}

void test_retry_log_key_details() {
    const FailureKey base{FailureKind::kItemDrawPatchMismatch, 0x1000, 0, 0, 0, 0x1234};
    const FailureKey changed_errno{FailureKind::kItemDrawPatchMismatch, 0x1000, 0, 0, 13, 0x1234};
    const FailureKey changed_word{FailureKind::kItemDrawPatchMismatch, 0x1000, 0, 0, 0, 0x5678};

    CHECK(!extension_bag_retry::retry_log_should_emit(true, base, base));
    CHECK(extension_bag_retry::retry_log_should_emit(true, base, changed_errno));
    CHECK(extension_bag_retry::retry_log_should_emit(true, base, changed_word));
}

void test_move_merge_log_counts() {
    CHECK(move_merge_log::expected_info_line_count(false, false, true) == 0);
    CHECK(move_merge_log::expected_info_line_count(true, false, true) == 1);
    CHECK(move_merge_log::expected_info_line_count(true, true, true) == 1);
    CHECK(move_merge_log::expected_info_line_count(false, true, true) == 1);
    CHECK(move_merge_log::expected_info_line_count(true, false, false) == 0);
    CHECK(move_merge_log::setter_info_should_emit(true, false, true));
    CHECK(!move_merge_log::setter_info_should_emit(true, true, true));
    CHECK(!move_merge_log::setter_info_should_emit(false, false, true));
}

}  // namespace

int main() {
    test_retry_log_state_transitions();
    test_partial_injection_does_not_reset();
    test_retry_log_key_details();
    test_move_merge_log_counts();
    std::printf("extension_bag_retry_log: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
