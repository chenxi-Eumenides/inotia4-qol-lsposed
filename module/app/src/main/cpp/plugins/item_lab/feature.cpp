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
bool create_by_category(int32_t category, int32_t count, contracts::QolBagRef bag,
                        const char* expected) {
    const auto* action=inventory_action();
    if (!action || !action->create || !action->place) return false;
    contracts::QolItemCreateSpec create{};
    create.struct_size=sizeof(create); create.category=category; create.count=count;
    const auto made=action->create(&create);
    if(made.status!=contracts::InventoryActionStatus::kOk){ITEM_LAB_WARN("create failed expected=%s category=%d count=%d status=%d",expected,category,count,static_cast<int>(made.status));return false;}
    contracts::QolItemPlaceTarget target{}; target.struct_size=sizeof(target);target.ref=bag;target.slot=-1;
    const auto placed=action->place(made.item,&target);
    if(placed.status!=contracts::InventoryActionStatus::kOk){ITEM_LAB_WARN("place failed expected=%s category=%d count=%d space=%d bag=%d status=%d",expected,category,count,bag.space_id,bag.bag,static_cast<int>(placed.status));return false;}
    ITEM_LAB_INFO("created expected=%s category=%d count=%d space=%d bag=%d slot=auto",expected,category,count,placed.ref.space_id,placed.ref.bag);
    return true;
}
void create_jewel(contracts::QolBagRef bag) {
    const auto* action=inventory_action();
    if(!action||!action->create)return;
    contracts::QolItemCreateSpec spec{};spec.struct_size=sizeof(spec);spec.category=30;spec.count=1;
    spec.flags=contracts::kQolItemCreateFlagAllocateUid;
    const auto source=action->create(&spec);
    if(source.status!=contracts::InventoryActionStatus::kOk){ITEM_LAB_WARN("create failed expected=jewel category=30 count=1 status=%d",static_cast<int>(source.status));return;}
    const contracts::QolItemPayloadMutation mutation{static_cast<int32_t>(contracts::QolItemPayloadMutationKind::kJewelWord),0,60};
    contracts::QolItemPayload payload{};
    if(!update_payload(source.item,&mutation,1,&payload)){
        if(action->discard_handle)(void)action->discard_handle(source.item,1);
        ITEM_LAB_WARN("jewel payload update failed expected=category30 value=60");return;
    }
    if(action->discard_handle)(void)action->discard_handle(source.item,1);
    (void)place_payload(payload,bag,"jewel category=30 count=1 strength=60",30,1);
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
    const int needed=has_slot0?4:3;
    if(info.capacity-info.occupied_slots<needed){ITEM_LAB_WARN("construct skipped reason=bag_full space=%d bag=%d capacity=%d occupied=%d needed=%d",bag.space_id,bag.bag,info.capacity,info.occupied_slots,needed);return;}
    if(has_slot0){
        contracts::QolItemPayload src{};
        if(!update_payload(slot0.item,nullptr,0,&src)){
            ITEM_LAB_WARN("slot0 payload copy failed space=%d bag=%d",bag.space_id,bag.bag);
        }else{
            // update 已带结构校验；create 的 allocate_uid 标志会在加载前覆盖 UID。
            (void)place_payload(src,bag,"slot0-copy uid=allocated",slot0.category,slot0.count);
        }
    }
    create_by_category(5,7,bag,"potion-small category=5 count=7");
    create_jewel(bag);
    contracts::QolItemPayloadBlob equip{};
    if(build_equipment_payload(&equip)){
        contracts::QolItemPayload payload{};payload.struct_size=sizeof(payload);payload.category=462;payload.count=1;payload.payload_size=equip.payload_size;std::memcpy(payload.payload,equip.payload,equip.payload_size);
        place_payload(payload,bag,"equipment category=462 count=1 options=0:15,1:12,2:10 socket=0x30 enchant=0",462,1);
    }else ITEM_LAB_WARN("equipment payload build failed expected=category462 options=0:15,1:12,2:10");
}
}
