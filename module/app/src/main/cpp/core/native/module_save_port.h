#pragma once

// 存档事务参与者的既有接口（participant prepare/commit/abort + module_save_game 编排）。
// B4-1 冻结的 `infra/store` 是 sidecar 容器层（MSAV 字节格式/原子写/last-good）；
// 本模块仍是当前的事务编排者，未接入 `infra/store`；B4-3/B4-4 收编事务与 journal 后，
// 参与者持久化改经 infra/store，本头随之收敛/删除。

struct ModuleSaveParticipant {
    const char* name;
    bool (*prepare)(int slot, const char* transaction_id);
    bool (*commit)(int slot, const char* transaction_id);
    bool (*abort)(int slot, const char* transaction_id);
};

bool module_save_register_participant(const ModuleSaveParticipant& participant);
bool module_save_game();
