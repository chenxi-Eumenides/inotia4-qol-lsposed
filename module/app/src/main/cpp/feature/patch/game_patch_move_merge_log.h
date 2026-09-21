#pragma once

namespace move_merge_log {

constexpr bool setter_info_should_emit(bool request_changed, bool hook_changed, bool update_succeeded) {
    return update_succeeded && request_changed && !hook_changed;
}

constexpr unsigned expected_info_line_count(bool request_changed, bool hook_changed, bool update_succeeded) {
    if (!update_succeeded) return 0;
    return (hook_changed || request_changed) ? 1u : 0u;
}

}  // namespace move_merge_log
