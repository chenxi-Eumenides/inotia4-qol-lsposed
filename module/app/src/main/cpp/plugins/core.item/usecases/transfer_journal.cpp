#include "transfer_journal.h"

#include <chrono>
#include <cstring>

#include "contracts/host_sdk.h"
#include "contracts/save_facts.h"
#include "plugins/core.item/bags/native_transport.h"
#include "plugins/core.item/bags/source_transport.h"
#include "plugins/core.item/runtime/core_item_internal.h"
#if !defined(QOL_CORE_ITEM_SOURCE_TRANSPORT_SEPARATE)
#include "plugins/core.item/bags/source_transport.cpp"
#endif

namespace qol::features::core_item::usecases::transfer_journal {
namespace {

constexpr char kSectionName[] = "item.transfer.journal";
constexpr uint16_t kSectionVersion = 1;
constexpr uint16_t kJournalMagic = 0x5149;
constexpr uint16_t kJournalFormatVersion = 1;
constexpr std::size_t kMaximumRecords = 8;

struct JournalHeader {
    uint16_t magic;
    uint16_t version;
    uint16_t count;
};

struct JournalRecord {
    uint8_t id[16];
    uint8_t state;
    uint8_t reserved[3];
    int32_t source_space;
    int32_t source_bag;
    int32_t source_slot;
    int32_t target_space;
    int32_t target_bag;
    int32_t target_slot;
    int32_t category;
    int32_t count;
    uint8_t alignment[4];
    uint64_t payload_hash;
    uint64_t time_ms;
};

static_assert(sizeof(JournalHeader) == 6, "transfer journal header layout changed");
static_assert(sizeof(JournalRecord) == 72, "transfer journal record layout changed");

JournalRecord g_records[kMaximumRecords]{};
std::size_t g_count = 0;
int32_t g_store_slot = -1;
bool g_loaded = false;
bool g_memory_only_warned = false;
bool g_store_error_warned = false;
uint64_t g_next_id = 1;

void log_message(int32_t level, const char* message) {
    if (g_sdk == nullptr || g_sdk->log.write == nullptr || message == nullptr) return;
    uint32_t length = 0;
    while (message[length] != '\0') ++length;
    (void)g_sdk->log.write(level, 1, kPluginId, message, length);
}

void warn_memory_only() {
    if (g_memory_only_warned) return;
    g_memory_only_warned = true;
    log_message(2, "[core.item] transfer journal is memory-only: save slot unavailable");
}

void log_store_error() {
    if (g_store_error_warned) return;
    g_store_error_warned = true;
    log_message(3, "[core.item] transfer journal store operation failed");
}

bool current_slot(int32_t* out_slot) {
    if (out_slot == nullptr || g_sdk == nullptr || g_sdk->capabilities.require == nullptr) {
        return false;
    }
    const void* iface = nullptr;
    const QolResult required = g_sdk->capabilities.require(
        contracts::kSaveFactsCapabilityId,
        {contracts::kSaveFactsVersionMajor, contracts::kSaveFactsVersionMinor}, &iface);
    if (!contracts::succeeded(required) || iface == nullptr) return false;
    const auto* facts = static_cast<const contracts::SaveFactsApi*>(iface);
    if (facts->current_slot == nullptr) return false;
    int32_t slot = -1;
    if (!contracts::succeeded(facts->current_slot(&slot)) || slot < 0) return false;
    *out_slot = slot;
    return true;
}

uint64_t now_ms() {
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch());
    return elapsed.count() < 0 ? 0 : static_cast<uint64_t>(elapsed.count());
}

bool write_records() {
    if (g_store_slot < 0) return true;
    if (g_sdk == nullptr) {
        log_store_error();
        return false;
    }
    if (g_count == 0 && g_sdk->store.remove != nullptr) {
        const QolResult result = g_sdk->store.remove(g_store_slot, kSectionName);
        if (contracts::succeeded(result) || result.code == kQolErrNotFound) return true;
        log_store_error();
        return false;
    }
    if (g_sdk->store.write == nullptr) {
        log_store_error();
        return false;
    }
    JournalHeader header{kJournalMagic, kJournalFormatVersion,
                         static_cast<uint16_t>(g_count)};
    uint8_t bytes[sizeof(JournalHeader) + kMaximumRecords * sizeof(JournalRecord)]{};
    std::memcpy(bytes, &header, sizeof(header));
    if (g_count != 0) {
        std::memcpy(bytes + sizeof(header), g_records, g_count * sizeof(JournalRecord));
    }
    const uint32_t size = static_cast<uint32_t>(sizeof(header) + g_count * sizeof(JournalRecord));
    const QolResult result = g_sdk->store.write(g_store_slot, kSectionName, kSectionVersion,
                                                 bytes, size);
    if (!contracts::succeeded(result)) {
        log_store_error();
        return false;
    }
    return true;
}

bool load_records(int32_t slot) {
    if (g_sdk == nullptr || g_sdk->store.read == nullptr) {
        log_store_error();
        return false;
    }
    uint8_t bytes[sizeof(JournalHeader) + kMaximumRecords * sizeof(JournalRecord)]{};
    contracts::QolStoreBuffer buffer{bytes, 0, static_cast<uint32_t>(sizeof(bytes))};
    uint16_t section_version = 0;
    const QolResult result = g_sdk->store.read(slot, kSectionName, &buffer, &section_version);
    if (!contracts::succeeded(result)) {
        if (result.code == kQolErrNotFound) {
            g_count = 0;
            g_store_slot = slot;
            g_loaded = true;
            return true;
        }
        log_store_error();
        return false;
    }
    if (section_version != kSectionVersion || buffer.size < sizeof(JournalHeader) ||
        buffer.size > sizeof(bytes)) {
        log_store_error();
        return false;
    }
    JournalHeader header{};
    std::memcpy(&header, bytes, sizeof(header));
    if (header.magic != kJournalMagic || header.version != kJournalFormatVersion ||
        header.count > kMaximumRecords ||
        buffer.size != sizeof(header) + header.count * sizeof(JournalRecord)) {
        log_store_error();
        return false;
    }
    JournalRecord loaded[kMaximumRecords]{};
    if (header.count != 0) {
        std::memcpy(loaded, bytes + sizeof(header), header.count * sizeof(JournalRecord));
    }
    for (uint16_t index = 0; index < header.count; ++index) {
        if (loaded[index].state > 1 || loaded[index].source_space < 0 ||
            loaded[index].source_bag < 0 || loaded[index].source_slot < 0 ||
            loaded[index].target_space < 0 || loaded[index].target_bag < 0 ||
            loaded[index].target_slot < 0 || loaded[index].category <= 0 ||
            loaded[index].count <= 0) {
            log_store_error();
            return false;
        }
        if (loaded[index].source_space == loaded[index].target_space &&
            loaded[index].source_bag == loaded[index].target_bag &&
            loaded[index].source_slot == loaded[index].target_slot) {
            log_store_error();
            return false;
        }
    }
    std::memset(g_records, 0, sizeof(g_records));
    if (header.count != 0) {
        std::memcpy(g_records, loaded, header.count * sizeof(JournalRecord));
    }
    g_count = header.count;
    g_store_slot = slot;
    g_loaded = true;
    return true;
}

bool ensure_loaded() {
    int32_t slot = -1;
    if (!current_slot(&slot)) {
        g_store_slot = -1;
        warn_memory_only();
        return true;
    }
    if (g_loaded && g_store_slot == slot) return true;
    return load_records(slot);
}

bool same_id(const uint8_t left[16], const JournalId& right) {
    return std::memcmp(left, right.bytes, sizeof(right.bytes)) == 0;
}

int find_id(const JournalId& id) {
    for (std::size_t index = 0; index < g_count; ++index) {
        if (same_id(g_records[index].id, id)) return static_cast<int>(index);
    }
    return -1;
}

JournalId make_id(const JournalRecord& record) {
    const uint64_t time = record.time_ms;
    const uint64_t sequence = g_next_id++;
    uint64_t first = time ^ (sequence * 0x9e3779b97f4a7c15ull);
    uint64_t second = sequence ^
                      (static_cast<uint64_t>(static_cast<uint32_t>(record.source_space)) << 32) ^
                      static_cast<uint32_t>(record.source_bag);
    second ^= static_cast<uint64_t>(static_cast<uint32_t>(record.target_bag)) << 17;
    JournalId id{};
    std::memcpy(id.bytes, &first, sizeof(first));
    std::memcpy(id.bytes + sizeof(first), &second, sizeof(second));
    return id;
}

QolResult item_at(const contracts::QolBagRef& ref, int32_t slot,
                  contracts::QolInventoryItemRef* out) {
    if (out == nullptr || ref.space_id < 0 || ref.bag < 0 || slot < 0) {
        constexpr char kMessage[] = "invalid transfer journal item location";
        return contracts::error(kQolErrInvalidArgument, kMessage,
                                static_cast<uint32_t>(sizeof(kMessage) - 1));
    }
    if (const contracts::QolBagSource* source = find_bag_source(ref.space_id)) {
        if (source->item_at == nullptr) {
            constexpr char kMessage[] = "bag source item_at unavailable";
            return contracts::error(kQolErrUnsupported, kMessage,
                                    static_cast<uint32_t>(sizeof(kMessage) - 1));
        }
        return source->item_at(ref.bag, slot, out);
    }
    if ((ref.space_id != contracts::kQolBagSpaceOriginal &&
         ref.space_id != contracts::kQolBagSpaceVariant) ||
        g_port == nullptr || !contracts::qol_item_port_bag_space_size_valid(g_port) ||
        g_port->read.item_at == nullptr) {
        constexpr char kMessage[] = "bag item_at unavailable";
        return contracts::error(kQolErrUnsupported, kMessage,
                                static_cast<uint32_t>(sizeof(kMessage) - 1));
    }
    return g_port->read.item_at(&ref, slot, out);
}

bool item_exists(const contracts::QolBagRef& ref, int32_t slot,
                 bool* out_exists, contracts::QolItemHandle* out_item) {
    if (out_exists == nullptr || out_item == nullptr) return false;
    contracts::QolInventoryItemRef item{};
    const QolResult result = item_at(ref, slot, &item);
    if (!contracts::succeeded(result)) {
        if (result.code == kQolErrNotFound) {
            *out_exists = false;
            *out_item = contracts::kInvalidItemHandle;
            return true;
        }
        return false;
    }
    if (item.space_id != ref.space_id || item.bag != ref.bag || item.slot != slot ||
        item.item.slot == 0 || item.item.generation == 0) {
        return false;
    }
    *out_exists = true;
    *out_item = item.item;
    return true;
}

void remove_record(std::size_t index) {
    if (index >= g_count) return;
    for (std::size_t move = index + 1; move < g_count; ++move) {
        g_records[move - 1] = g_records[move];
    }
    --g_count;
    std::memset(&g_records[g_count], 0, sizeof(g_records[g_count]));
    (void)write_records();
}

}  // namespace

bool begin(const contracts::QolBagRef& source, int32_t source_slot,
           const contracts::QolBagRef& target, int32_t target_slot, int32_t category,
           int32_t item_count, JournalId* out_id) {
    if (out_id == nullptr || source.space_id < 0 || source.bag < 0 || source_slot < 0 ||
        target.space_id < 0 || target.bag < 0 || target_slot < 0 || category <= 0 ||
        item_count <= 0 ||
        (source.space_id == target.space_id && source.bag == target.bag &&
         source_slot == target_slot) ||
        !ensure_loaded() || g_count >= kMaximumRecords) {
        return false;
    }
    JournalRecord record{};
    record.source_space = source.space_id;
    record.source_bag = source.bag;
    record.source_slot = source_slot;
    record.target_space = target.space_id;
    record.target_bag = target.bag;
    record.target_slot = target_slot;
    record.category = category;
    record.count = item_count;
    record.time_ms = now_ms();
    const JournalId id = make_id(record);
    std::memcpy(record.id, id.bytes, sizeof(record.id));
    g_records[g_count++] = record;
    if (!write_records()) {
        --g_count;
        std::memset(&g_records[g_count], 0, sizeof(g_records[g_count]));
        return false;
    }
    *out_id = id;
    return true;
}

void mark_target_written(const JournalId& id) {
    const int index = find_id(id);
    if (index < 0) return;
    g_records[index].state = 1;
    (void)write_records();
}

void mark_target_written(const JournalId& id, uint64_t hash) {
    const int index = find_id(id);
    if (index < 0) return;
    g_records[index].state = 1;
    g_records[index].payload_hash = hash;
    (void)write_records();
}

void clear(const JournalId& id) {
    const int index = find_id(id);
    if (index >= 0) remove_record(static_cast<std::size_t>(index));
}

std::size_t count() { return g_count; }

bool reconcile() {
    if (!ensure_loaded()) return false;
    std::size_t index = 0;
    while (index < g_count) {
        const JournalRecord record = g_records[index];
        const contracts::QolBagRef source{record.source_space, record.source_bag};
        const contracts::QolBagRef target{record.target_space, record.target_bag};
        bool source_exists = false;
        bool target_exists = false;
        contracts::QolItemHandle source_item{};
        contracts::QolItemHandle target_item{};
        if (!item_exists(source, record.source_slot, &source_exists, &source_item) ||
            !item_exists(target, record.target_slot, &target_exists, &target_item)) {
            return false;
        }
        if (source_exists && target_exists) {
            contracts::QolBagMutationResult removed{};
            const auto remove_target =
                target.space_id == contracts::kQolBagSpaceOriginal ||
                        target.space_id == contracts::kQolBagSpaceVariant
                    ? &bags::native_transport::remove_item
                    : &bags::source_transport::remove_item;
            const QolResult result =
                remove_target(target.space_id, target_item, record.count, &removed);
            if (!contracts::succeeded(result)) {
                log_message(3, "[core.item] transfer journal target rollback failed");
                return false;
            }
            remove_record(index);
            continue;
        }
        if (!source_exists && !target_exists) {
            log_message(2, "[core.item] transfer journal item missing from both locations");
        }
        remove_record(index);
    }
    return true;
}

}  // namespace qol::features::core_item::usecases::transfer_journal
