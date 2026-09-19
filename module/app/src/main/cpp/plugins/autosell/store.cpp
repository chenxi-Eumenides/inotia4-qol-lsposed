// autosell_store.cpp —— 自动出售按存档配置持久化（sidecar section `autosell`，直写）。
//
// 读写经 infra/store（模块 sidecar 容器 MSAV 单写者）：section 名 `autosell`、version 2，
// slot 由调用方按 current_save_slot() 传入。纯序列化/解析在 store.h（inline，可 host 测）；
// 本文件只做 section 读写与运行时加载状态。与扩展背包 section/journal 完全隔离，不注册 participant。

#include "plugins/autosell/store.h"

#include "plugins/autosell/config.h"
#include "plugins/autosell/log.h"
#include "plugins/autosell/plugin.h"

#include <mutex>
#include <string>
#include <vector>

namespace {

// section 名/版本是 sidecar 契约的一部分（跨版本读档稳定性），不得变更。
constexpr const char* kSectionName = "autosell";
constexpr uint16_t kSectionVersion = 2;
constexpr int kSlotCount = 3;  // ModuleSaveStore SLOT_COUNT

std::mutex g_store_mtx;
int g_loaded_slot = -1;          // 运行时配置当前对应的存档槽
bool g_last_persist_ok = false;  // 最近一次持久化结果

bool valid_slot(int slot) { return slot >= 0 && slot < kSlotCount; }

enum class ReadOutcome { kOk, kUnavailable, kOther };

ReadOutcome read_section(int slot, std::vector<uint8_t>* payload, uint16_t* version) {
    const qol::contracts::QolHostSdk* sdk = qol::features::autosell::host_sdk();
    if (sdk == nullptr || sdk->store.read == nullptr || payload == nullptr || version == nullptr) {
        return ReadOutcome::kUnavailable;
    }

    payload->resize(4096);
    for (int attempt = 0; attempt < 2; ++attempt) {
        qol::contracts::QolStoreBuffer buffer{
            payload->empty() ? nullptr : payload->data(),
            0,
            static_cast<uint32_t>(payload->size()),
        };
        const QolResult result = sdk->store.read(slot, kSectionName, &buffer, version);
        if (result.ok == kQolOk) {
            payload->resize(buffer.size);
            return ReadOutcome::kOk;
        }
        if (result.ok == kQolErrInvalidArgument && buffer.size > buffer.capacity) {
            payload->resize(buffer.size);
            continue;
        }
        if (result.ok == kQolErrNotReady) return ReadOutcome::kUnavailable;
        return ReadOutcome::kOther;
    }
    return ReadOutcome::kOther;
}

}  // namespace

void autosell_store_ensure_loaded(int slot) {
    if (!valid_slot(slot)) return;

    int loaded = -1;
    bool persisted = false;
    bool needs_load = false;
    {
        std::lock_guard<std::mutex> lock(g_store_mtx);
        loaded = g_loaded_slot;
        persisted = g_last_persist_ok;
        needs_load = (g_loaded_slot != slot);
    }
    if (!needs_load) {
        autosell_set_store_status(slot, loaded, persisted);
        return;
    }

    std::vector<uint8_t> payload;
    uint16_t version = 0;
    const ReadOutcome outcome = read_section(slot, &payload, &version);
    if (outcome == ReadOutcome::kUnavailable) {
        // 存储尚未初始化/异常：不覆盖运行时配置，也不标记 loaded，下帧重试。
        autosell_set_store_status(slot, loaded, persisted);
        return;
    }
    // section 未命中（含坏容器回退空）或 payload 非合法 JSON：回退默认配置
    //（空白/无 '{' 与 v>2 均由 autosell_config_from_json 判 false）。
    autosell::Config cfg;
    bool parsed = false;
    if (outcome == ReadOutcome::kOk) {
        const std::string json(payload.begin(), payload.end());
        parsed = autosell_config_from_json(json.c_str(), &cfg);
    }
    autosell_set_runtime_config(parsed ? cfg : autosell::Config{});
    {
        std::lock_guard<std::mutex> lock(g_store_mtx);
        g_loaded_slot = slot;
        loaded = slot;
        persisted = g_last_persist_ok;
    }
    autosell_set_store_status(slot, loaded, persisted);
    AUTOSELL_LOG_INFO("sidecar config load slot=%d parsed=%d", slot, parsed ? 1 : 0);
}

bool autosell_store_persist(int slot, const autosell::Config& config) {
    if (!valid_slot(slot)) return false;
    const std::string json = autosell_config_to_json(config);
    const qol::contracts::QolHostSdk* sdk = qol::features::autosell::host_sdk();
    const QolResult result =
        sdk != nullptr && sdk->store.write != nullptr
            ? sdk->store.write(slot, kSectionName, kSectionVersion,
                               reinterpret_cast<const uint8_t*>(json.data()),
                               static_cast<uint32_t>(json.size()))
            : QolResult{kQolErrNotReady, kQolErrNotReady, nullptr, 0};
    const bool ok = (result.ok == kQolOk);
    int loaded = -1;
    {
        std::lock_guard<std::mutex> lock(g_store_mtx);
        g_last_persist_ok = ok;
        loaded = g_loaded_slot;
    }
    autosell_set_store_status(slot, loaded, ok);
    AUTOSELL_LOG_INFO("sidecar config persist slot=%d ok=%d", slot, ok ? 1 : 0);
    return ok;
}
