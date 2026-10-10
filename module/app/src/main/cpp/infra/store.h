#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>
#include "contracts/host_sdk.h"
#include "contracts/slot_operation.h"

// 模块 sidecar 容器层（infra/store）：MSAV 容器格式的 native 权威实现，
// 与 Kotlin ModuleSaveStore.kt 的字节格式逐字节兼容（B4-1 冻结核心件）。
//
// 职责：按槽（0..2）读写 module-saves/slot-<n>.module-save（primary）与 .last-good；
// 原子写（tmp + rename）、primary 无效时 last-good 回退、坏容器 quarantine 保留。
// 不解释 section 业务语义，不读写原版存档字节。
//
// 线程模型：单一全局 mutex；任意线程可调。持锁路径只做文件 IO 与内存操作，
// 不回调上层、不取其它锁（锁序：g_virtual_bag_mtx → store、g_sb_mtx → store）。
//
// 初始化：init(external_files_dir) 由 JNI 边界注入；未初始化或目录无效时
// available()=false，所有操作返回 kUnavailable。B4-2/B4-3 再收敛注入点。

namespace qol::infra::store {

enum class Status {
    kOk = 0,
    kNotFound,      // section/容器不存在
    kCorrupt,       // 容器字节非法（含 import 源容器无效）
    kIoError,       // 文件 IO/原子写失败
    kUnavailable,   // 未初始化或目录无效
    kBadSlot,       // slot 不在 0..2
    kBadVersion,    // section version <= 0
    kTooLarge,      // payload > 1MiB 或容器 > 4MiB 或 section 数 > 65535
    kBadName,       // section 名不符合 [a-z0-9._-]{1,64}
    kBusy,
    kBlocked,
};

// 稳定英文名，供诊断输出；未定义值返回 "unknown"。
const char* status_name(Status status);

// section 的防御性副本（version + payload）。
struct Section {
    uint16_t version = 0;
    std::vector<uint8_t> payload;
};

// replace_sections 的替换集合（保持插入序：新名追加到尾部，改名不改变既有位置）。
using ReplaceMap = std::vector<std::pair<std::string, Section>>;
// replace_sections 的删除集合。
using RemoveSet = std::vector<std::string>;

// 幂等初始化：external_files_dir 下创建/使用 module-saves/；空指针或目录不可建 → unavailable。
void init(const char* external_files_dir);

// 是否已初始化且 module-saves 目录可用。
bool available();

// 确保槽容器存在且可读；primary 无效时按 last-good 回退写回 primary（generation 保持）。
Status ensure_slot(int slot);

// 清空槽全部 section（删除 primary + last-good 后写空容器，generation=1）。
Status reset_slot(int slot);

// 删除槽 primary + last-good（文件不存在也算成功）。
Status delete_slot(int slot);

// 读取槽主容器原始字节（可按该槽解码时）；否则 kNotFound/kCorrupt。
Status read_container(int slot, std::vector<uint8_t>* out);

// 从外部容器字节导入（先按源容器头槽号校验，再重定槽并双写 primary + last-good）。
Status import_container(int slot, const uint8_t* data, std::size_t len);

// 读取 section 副本；未命中 kNotFound。
Status read_section(int slot, const char* name, std::vector<uint8_t>* out, uint16_t* version);

// 原子替换单个 section（其余 section 含未知项原样保留）；generation 递增。
Status write_section(int slot, const char* name, uint16_t version, const uint8_t* data,
                     std::size_t len);

// 删除 section；不存在返回 kNotFound（与原版 Game 侧无此语义差异，native 端口显式化）。
Status remove_section(int slot, const char* name);

// 单次原子提交：先删除 removals，再写入 replacements，其余 section 原样保留；generation 递增。
Status replace_sections(int slot, const ReplaceMap& replacements, const RemoveSet& removals);

Status inspect_slot(int slot, contracts::QolOwnerStoreSlotState* out);
Status preflight(int slot, const contracts::QolOwnerStoreCandidate* candidates,
                 uint32_t count, uint64_t* encoded_bytes);
Status reserve(uint64_t owner, int slot, const contracts::QolOwnerStoreCandidate* candidates,
               uint32_t count, contracts::QolOwnerStoreReservation* out);
Status release(uint64_t owner, const contracts::QolOwnerStoreReservation& reservation);
Status replace_reserved(uint64_t owner, int slot, contracts::QolOwnerStoreReservation* reservation,
                        const ReplaceMap& replacements, const RemoveSet& removals);
// owner 生命周期转接；pending 租约保留，恢复授权在 Host 重新签发时迁移。
void owner_stopped(uint64_t owner);
void owner_rebound(uint64_t old_owner, uint64_t new_owner);
bool owner_stopped_try(uint64_t owner);
bool owner_rebound_try(uint64_t old_owner, uint64_t new_owner);
Status set_referenced(uint64_t owner, int slot, contracts::QolOwnerStoreReservation* reservation,
                       uint32_t referenced);
// 以下 locked 查询与 gate 共用临界区，不自行取锁。
bool reservation_busy_locked(int slot);
bool operation_has_reservation_locked(uint64_t operation);
Status reserve_in_operation(uint64_t owner, const contracts::QolSlotOperationLease*, int slot,
                            const contracts::QolOwnerStoreCandidate*, uint32_t count,
                            contracts::QolOwnerStoreReservation*);
Status ensure_in_operation(uint64_t owner, const contracts::QolSlotOperationLease*, int slot);
Status reset_in_operation(uint64_t owner, const contracts::QolSlotOperationLease*, int slot);
Status import_in_operation(uint64_t owner, const contracts::QolSlotOperationLease*, int slot,
                           const uint8_t*, std::size_t, contracts::QolSlotOperationApply*);
Status delete_in_operation(uint64_t owner, const contracts::QolSlotOperationLease*, int slot);

#if defined(QOL_STORE_TEST_SEAMS)
// 仅 Host 测试构建提供确定性文件错误注入；生产始终调用系统 unlink。
void set_unlink_for_test(int (*unlink_fn)(const char*));
void with_store_lock_for_test(void (*held)(void*), void* context);
#endif

}  // namespace qol::infra::store
