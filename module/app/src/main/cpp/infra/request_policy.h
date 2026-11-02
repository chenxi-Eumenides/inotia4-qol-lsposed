#pragma once

#include <atomic>

namespace qol::infra {

struct RequestPolicy {
    bool requires_op = false;
};

using RequestOpAuthorizer = bool (*)();

namespace request_policy_detail {
inline thread_local bool t_requires_op = false;
inline std::atomic<RequestOpAuthorizer> g_op_authorizer{nullptr};
}  // namespace request_policy_detail

inline RequestPolicy current_request_policy() noexcept {
    return RequestPolicy{request_policy_detail::t_requires_op};
}

class ScopedRequestPolicy {
public:
    explicit ScopedRequestPolicy(RequestPolicy policy) noexcept
        : previous_requires_op_(request_policy_detail::t_requires_op) {
        request_policy_detail::t_requires_op = previous_requires_op_ || policy.requires_op;
    }

    ScopedRequestPolicy(const ScopedRequestPolicy&) = delete;
    ScopedRequestPolicy& operator=(const ScopedRequestPolicy&) = delete;

    ~ScopedRequestPolicy() {
        request_policy_detail::t_requires_op = previous_requires_op_;
    }

private:
    bool previous_requires_op_;
};

inline void set_request_op_authorizer(RequestOpAuthorizer authorizer) noexcept {
    request_policy_detail::g_op_authorizer.store(authorizer, std::memory_order_release);
}

inline bool request_policy_authorized(RequestPolicy policy) {
    if (!policy.requires_op) return true;
    const RequestOpAuthorizer authorizer =
        request_policy_detail::g_op_authorizer.load(std::memory_order_acquire);
    return authorizer != nullptr && authorizer();
}

}  // namespace qol::infra
