#pragma once

namespace qol::extension_bag {

// 单线程模型下的业务锁替代：只记录同线程嵌套深度，不做跨线程互斥。
// 所有 EB 入口最终都在游戏主线程执行（HTTP 写命令经 commands 编组、
// 查询读已发布快照），因此原来的 std::mutex 只承担“同线程重入检测”。
// - ReentryLock：进入即增加深度，允许正常嵌套。
// - TryReentryLock：仅当当前线程不在任何 EB 临界区时取得；用于原生回调
//   （页签 proc、延迟释放 tick、详情动作）判断是否由 EB 自身调用栈重入。
inline thread_local int g_reentry_depth = 0;

inline int reentry_depth() { return g_reentry_depth; }

class ReentryLock {
public:
    ReentryLock() { acquire(); }
    ~ReentryLock() { unlock(); }
    ReentryLock(const ReentryLock&) = delete;
    ReentryLock& operator=(const ReentryLock&) = delete;

    void lock() {
        if (owned_) return;
        acquire();
    }
    void unlock() {
        if (!owned_) return;
        --g_reentry_depth;
        owned_ = false;
    }
    bool owns_lock() const { return owned_; }

protected:
    struct TryTag {};
    explicit ReentryLock(TryTag) {}

    void acquire() {
        ++g_reentry_depth;
        owned_ = true;
    }

    bool owned_ = false;
};

class TryReentryLock : public ReentryLock {
public:
    TryReentryLock() : ReentryLock(TryTag{}) {
        if (g_reentry_depth == 0) acquire();
    }
    TryReentryLock(const TryReentryLock&) = delete;
    TryReentryLock& operator=(const TryReentryLock&) = delete;
};

}  // namespace qol::extension_bag
