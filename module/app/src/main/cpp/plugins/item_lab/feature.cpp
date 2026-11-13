#include "plugin.h"
#include "spec.h"
#include "log.h"
#include <cstring>

namespace qol::features::item_lab {
namespace {
bool update_payload(contracts::QolItemHandle source,
                    const contracts::QolItemPayloadMutation* mutations, uint32_t n,
                    contracts::QolItemPayload* out) {
    const auto* api = item_payload();
    if (!api || !api->update || !api->validate) return false;
    contracts::QolItemPayloadUpdateSpec spec{};
    spec.struct_size = sizeof(spec); spec.source = source; spec.mutations = mutations;
    spec.mutation_count = n; spec.flags = contracts::kItemPayloadUpdateValidateStructure;
    if (!contracts::succeeded(api->update(&spec, out))) return false;
    contracts::QolItemPayloadValidation validation{};
    validation.struct_size = sizeof(validation);
    return contracts::succeeded(api->validate(out->payload, out->payload_size, &validation));
}
bool place_payload(const contracts::QolItemPayload& payload, contracts::QolBagRef bag,
                   const char* expected, int32_t category, int32_t count) {
    const auto* action = inventory_action();
    if (!action || !action->create || !action->place) return false;
    contracts::QolItemCreateSpec create{};
    create.struct_size=sizeof(create); create.category=category; create.count=count;
    create.flags=contracts::kQolItemCreateFlagAllocateUid;
    create.payload=payload.payload; create.payload_size=payload.payload_size;
    const auto made=action->create(&create);
    if (made.status != contracts::InventoryActionStatus::kOk) {
        ITEM_LAB_WARN("create failed expected=%s category=%d count=%d status=%d",expected,category,count,static_cast<int>(made.status));
        return false;
    }
    contracts::QolItemPlaceTarget target{};
    target.struct_size=sizeof(target); target.ref=bag; target.slot=-1; target.flags=0;
    const auto placed=action->place(made.item,&target);
    if (placed.status != contracts::InventoryActionStatus::kOk) {
        // 原生空间 place 失败时 Host 已释放 detached 对象，不能再 discard。
        ITEM_LAB_WARN("place failed expected=%s category=%d count=%d space=%d bag=%d status=%d",expected,category,count,bag.space_id,bag.bag,static_cast<int>(placed.status));
        return false;
    }
    ITEM_LAB_INFO("created expected=%s category=%d count=%d space=%d bag=%d slot=auto",expected,category,count,placed.ref.space_id,placed.ref.bag);
    return true;
}
void create_equipment(const EquipmentSpec& spec, contracts::QolBagRef bag,
                      const char* expected) {
    contracts::QolItemPayloadBlob blob{};
    if (!build_equipment_payload(spec, &blob)) {
        ITEM_LAB_WARN("equipment payload build failed expected=%s", expected);
        return;
    }
    contracts::QolItemPayload payload{};
    payload.struct_size = sizeof(payload);
    payload.category = spec.category;
    payload.count = 1;
    payload.payload_size = blob.payload_size;
    std::memcpy(payload.payload, blob.payload, blob.payload_size);
    (void)place_payload(payload, bag, expected, spec.category, 1);
}

}

void item_lab_construct_all() {
    const auto* extras=inventory_extras(); const auto* access=inventory_access();
    if(!extras||!extras->read.current_bag||!access||!access->native_item_at||!access->native_bag_info){ITEM_LAB_WARN("construct rejected reason=inventory_api_unavailable");return;}
    int32_t raw=-1;
    if(!contracts::succeeded(extras->read.current_bag(&raw))){ITEM_LAB_WARN("current bag read failed fallback=0/0");raw=5;}
    contracts::QolBagRef bag{0,0};
    if(raw>=0&&raw<=4)bag={0,raw};else if(raw>=6&&raw<=10)bag={1,raw-6};else ITEM_LAB_WARN("current bag invalid raw=%d fallback=space0 bag0",raw);
    contracts::QolInventoryItemRef slot0{};
    const auto slot_result=access->native_item_at(&bag,0,&slot0);
    const bool has_slot0=contracts::succeeded(slot_result);
    if(!has_slot0 && slot_result.code!=kQolErrNotFound){ITEM_LAB_WARN("slot0 read failed space=%d bag=%d code=%d",bag.space_id,bag.bag,slot_result.code);return;}
    contracts::QolBagInfo info{};
    if(!contracts::succeeded(access->native_bag_info(&bag,&info))){ITEM_LAB_WARN("bag info failed space=%d bag=%d",bag.space_id,bag.bag);return;}
    // 固定构造 5 件非法装备。副本只在额外有 1 个空位时创建；
    // 空位不足 6 时优先保证 5 件构造物品，避免整批跳过。
    constexpr int kConstructedCount=5;
    const int free_slots=info.capacity-info.occupied_slots;
    if(free_slots<kConstructedCount){ITEM_LAB_WARN("construct skipped reason=bag_full space=%d bag=%d capacity=%d occupied=%d needed=%d",bag.space_id,bag.bag,info.capacity,info.occupied_slots,kConstructedCount);return;}
    const bool copy_slot0=has_slot0&&free_slots>=kConstructedCount+1;
    if(has_slot0&&!copy_slot0){ITEM_LAB_WARN("copy skipped reason=no_slot_for_copy space=%d bag=%d free=%d",bag.space_id,bag.bag,free_slots);}
    if(copy_slot0){
        contracts::QolItemPayload src{};
        if(!update_payload(slot0.item,nullptr,0,&src)){
            ITEM_LAB_WARN("slot0 payload copy failed space=%d bag=%d",bag.space_id,bag.bag);
        }else{
            // update 已带结构校验；create 的 allocate_uid 标志会在加载前覆盖 UID。
            (void)place_payload(src,bag,"slot0-copy uid=allocated",slot0.category,slot0.count);
        }
    }
    // 1) 白装 4 词条 + 最大孔/强化字段：rarity=0、raw=0、socket=15/0、
    //    enchant_id=1、level=31、remaining=15。
    create_equipment({462, 0, -1, 15, 0, 4, 0, 0, 31, 15, 1}, bag,
                     "illegal white4 category=462 rarity=0 raw=0 native=4 socket=15/0 enchant=id1 level31 remaining15");
    // 2) 孔 1/镶 4：filled > total，宝石节点数=4。
    create_equipment({462, 0, -1, 1, 4, 0, 4, 0, 0, 0, 0}, bag,
                     "illegal socket-overflow category=462 rarity=0 raw=0 native=0 socket=1/4 gem=4");
    // 3) 孔 2/镶 2 + 4 个附魔；其中 id29(百分比) 写整数 5，id0(整数) 写 500。
    create_equipment({462, 2, -1, 2, 2, 2, 2, 4, 0, 0, 0}, bag,
                     "illegal four-enchant category=462 rarity=2 raw=7 native=2 socket=2/2 gem=2 enchant=4 percent29=5 integer0=500");
    // 4) 白装请求 rarity=0，但前缀 raw=9（秘银）；游戏按 raw 映射，实际 rarity 可能不是 0。
    create_equipment({462, 0, 9, 0, 0, 0, 0, 0, 0, 0, 0}, bag,
                     "illegal white-mithril category=462 rarity=0 raw=9 prefix=mithril native=0");
    // 5) 紫装请求 rarity=4，但前缀 raw=0（生锈的）；实际 rarity 可能不是 4。
    create_equipment({462, 4, 0, 0, 0, 4, 0, 0, 0, 0, 0}, bag,
                     "illegal purple-rusty category=462 rarity=4 raw=0 prefix=rusty native=4");
}
}
