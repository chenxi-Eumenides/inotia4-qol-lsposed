#pragma once

#include <cstdint>

namespace qol::extension_bag {

// 入口重入策略：
// - kReadSnapshot：重入时调用方应返回最近提交的快照/回退值，不进入半事务状态。
// - kRejectWrite：重入时拒绝本次写入并记日志，避免事务中间态被二次修改。
enum class ReentryPolicy {
    kReadSnapshot,
    kRejectWrite,
};

// 真实 pthread 主线程判定，来自 host 的 QolThreadApi；不可用时返回 true 并只记录一次。
bool extension_bag_on_main_thread();

// 当前线程是否已有 EB 入口在执行（同线程重入）。
bool extension_bag_entry_active();

// 非递归入口守卫：替代以 std::mutex::try_lock 探测重入的做法。
// 所有会读写 EB 状态的入口在取得业务状态前构造；entered() 为 false 时
// 调用方必须按策略返回快照/回退或拒绝。
class EntryGuard {
public:
    EntryGuard(const char* op, ReentryPolicy policy);
    ~EntryGuard();
    EntryGuard(const EntryGuard&) = delete;
    EntryGuard& operator=(const EntryGuard&) = delete;

    bool entered() const { return entered_; }

private:
    const char* op_ = nullptr;
    ReentryPolicy policy_ = ReentryPolicy::kRejectWrite;
    bool entered_ = false;
};

// 观测计数（主线程读写，单线程无锁）。
struct EntryGuardStats {
    uint64_t read_reentries = 0;
    uint64_t write_reentries = 0;
    uint64_t main_thread_violations = 0;
};
EntryGuardStats extension_bag_entry_stats();

}  // namespace qol::extension_bag
