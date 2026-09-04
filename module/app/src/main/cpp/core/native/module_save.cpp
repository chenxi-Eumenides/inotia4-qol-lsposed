#include "core/native/module_save_port.h"

#include <android/log.h>

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "game_access.h"
#include "game_state.h"

namespace {

constexpr char kLogTag[] = "Inotia4ModuleSave";
std::mutex g_participant_mtx;
std::vector<ModuleSaveParticipant> g_participants;
std::atomic<uint64_t> g_transaction_counter{0};
thread_local bool g_module_save_active = false;

struct SaveActivityGuard {
    bool& active;
    ~SaveActivityGuard() { active = false; }
};

std::vector<ModuleSaveParticipant> participants_snapshot() {
    std::lock_guard<std::mutex> lock(g_participant_mtx);
    return g_participants;
}

void abort_participants(const std::vector<ModuleSaveParticipant>& participants, int slot,
                        const std::string& transaction_id) {
    for (auto it = participants.rbegin(); it != participants.rend(); ++it) {
        if (it->abort != nullptr && !it->abort(slot, transaction_id.c_str())) {
            __android_log_print(ANDROID_LOG_WARN, kLogTag,
                                "participant abort failed name=%s slot=%d tx=%s",
                                it->name != nullptr ? it->name : "unknown", slot,
                                transaction_id.c_str());
        }
    }
}

}

bool module_save_register_participant(const ModuleSaveParticipant& participant) {
    if (participant.name == nullptr || participant.name[0] == '\0' ||
        participant.prepare == nullptr || participant.commit == nullptr ||
        participant.abort == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> lock(g_participant_mtx);
    for (ModuleSaveParticipant& registered : g_participants) {
        if (std::string(registered.name) == participant.name) {
            registered = participant;
            return true;
        }
    }
    g_participants.push_back(participant);
    return true;
}

bool module_save_game() {
    if (!game_in_world()) return false;
    if (fn_save == nullptr) return false;
    if (g_module_save_active) return true;
    g_module_save_active = true;
    const SaveActivityGuard activity_guard{g_module_save_active};
    const int slot = current_save_slot();
    if (slot < 0 || slot > 2) return false;

    const std::string transaction_id =
        "save-" + std::to_string(g_transaction_counter.fetch_add(1) + 1);
    const std::vector<ModuleSaveParticipant> participants = participants_snapshot();
    size_t prepared_count = 0;
    for (const ModuleSaveParticipant& participant : participants) {
        if (!participant.prepare(slot, transaction_id.c_str())) {
            __android_log_print(ANDROID_LOG_WARN, kLogTag,
                                "prepare failed name=%s slot=%d tx=%s",
                                participant.name, slot, transaction_id.c_str());
            abort_participants(
                std::vector<ModuleSaveParticipant>(participants.begin(),
                                                   participants.begin() + prepared_count),
                slot, transaction_id);
            return false;
        }
        ++prepared_count;
    }

    const int result = fn_save();
    if (result == 0) {
        abort_participants(
            std::vector<ModuleSaveParticipant>(participants.begin(),
                                               participants.begin() + prepared_count),
            slot, transaction_id);
        __android_log_print(ANDROID_LOG_WARN, kLogTag,
                            "original save failed slot=%d tx=%s", slot, transaction_id.c_str());
        return false;
    }

    for (const ModuleSaveParticipant& participant : participants) {
        if (!participant.commit(slot, transaction_id.c_str())) {
            __android_log_print(ANDROID_LOG_ERROR, kLogTag,
                                "commit failed name=%s slot=%d tx=%s",
                                participant.name, slot, transaction_id.c_str());
            return false;
        }
    }
    __android_log_print(ANDROID_LOG_INFO, kLogTag,
                        "save complete slot=%d tx=%s participants=%zu", slot,
                        transaction_id.c_str(), participants.size());
    return true;
}
