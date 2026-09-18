#pragma once

namespace save_backup {

// 导入事务的还原结果：每个持久化副作用单独报告，调用方据此决定是否把错误升级为
// 「原始操作失败 + rollback failed」。
struct RollbackResult {
    bool dat_restored = true;
    bool sidecar_restored = true;
    bool warehouse_restored = true;
    bool evidence_preserved = true;

    bool complete() const {
        return dat_restored && sidecar_restored && warehouse_restored && evidence_preserved;
    }
};

}  // namespace save_backup
