#include <cassert>
#include "../plugins/item_lab/spec.h"
int main() {
    qol::contracts::QolItemPayloadBlob blob{};
    assert(qol::features::item_lab::build_equipment_payload(&blob));
    qol::contracts::QolItemPayloadHeader header{};
    qol::contracts::QolItemPayloadOption options[3]{};
    uint32_t count=0;
    assert(qol::contracts::item_payload_decode(blob.payload,blob.payload_size,&header,options,3,&count));
    assert(((header.type_flags >> 6) & 0x3ffu)==462);
    assert(((header.type_flags >> 2) & 0xfu)==4);
    assert(header.socket==0x30 && header.uid==0 && count==3);
    assert(options[0].code==0 && options[0].value==15);
    assert(options[1].code==1 && options[1].value==12);
    assert(options[2].code==2 && options[2].value==10);
    assert(qol::contracts::item_payload_structurally_valid(blob.payload,blob.payload_size));
}
