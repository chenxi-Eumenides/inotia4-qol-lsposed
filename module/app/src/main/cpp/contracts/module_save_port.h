#pragma once

// 存档事务参与者的稳定端口；实现保留在存档域，消费者只依赖此声明。
struct ModuleSaveParticipant {
    const char* name;
    bool (*prepare)(int slot, const char* transaction_id);
    bool (*commit)(int slot, const char* transaction_id);
    bool (*abort)(int slot, const char* transaction_id);
};

bool module_save_register_participant(const ModuleSaveParticipant& participant);
bool module_save_game();
