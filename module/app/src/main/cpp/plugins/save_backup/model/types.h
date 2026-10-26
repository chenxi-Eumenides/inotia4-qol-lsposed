#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "plugins/save_backup/bundle.h"

// save_backup L0：跨层共享的请求/结果/错误类型。
// 规则：本层不依赖 Android、游戏、UI、文件系统或 JSON；L4 只返回这里的类型。

namespace save_backup {

enum class BackupError : uint8_t {
    kOk = 0,
    kInvalidArgument,
    kNotFound,
    kUnreadable,
    kChecksumMismatch,
    kSlotBusy,
    kNotMainMenu,
    kEncryptFailed,
    kWriteFailed,
    kImportSidecarFailed,
    kImportRollbackFailed,
    kNameCollision,
    kInternal,
};

struct ErrorInfo {
    BackupError code = BackupError::kOk;
    std::string message;
};

template <class T>
struct Outcome {
    bool ok = false;
    T value{};
    ErrorInfo error{};

    static Outcome success(T v) {
        Outcome out;
        out.ok = true;
        out.value = std::move(v);
        return out;
    }
    static Outcome failure(BackupError code, std::string message) {
        Outcome out;
        out.error.code = code;
        out.error.message = std::move(message);
        return out;
    }
};

struct BackupFile {
    std::string name;
    uint64_t size = 0;
    int64_t mtime_ms = 0;
};

struct BackupFilesResult {
    std::vector<BackupFile> files;
};

struct ExportResult {
    Entry entry;
    bool deduplicated = false;
};

struct ListResult {
    std::vector<Entry> entries;
};

struct EntryResult {
    Entry entry;
};

struct ImportResult {
    Entry entry;
    int target_slot = -1;
};

struct DeleteResult {
    bool deleted = false;
};

struct SlotSummary {
    bool exists = false;
    int state = 0;  // 0=空、1=损坏、2=有效
    int map_id = -1;
    int hero_level = -1;
    int hero_index = -1;
    int class_idx = -1;
};

struct SlotSummaryList {
    std::array<SlotSummary, 3> slots{};
};

}  // namespace save_backup
