#pragma once

struct ModuleSaveParticipant {
    const char* name;
    bool (*prepare)(int slot, const char* transaction_id);
    bool (*commit)(int slot, const char* transaction_id);
    bool (*abort)(int slot, const char* transaction_id);
};

bool module_save_register_participant(const ModuleSaveParticipant& participant);
bool module_save_game();
