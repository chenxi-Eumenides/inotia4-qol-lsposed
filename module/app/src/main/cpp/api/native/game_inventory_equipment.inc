std::string data_op_jewel(int role, int bag, int slot, int equip_slot) {
    qol::contracts::QolItemInventoryStateWriteResult operation_data{};
    const QolResult result = qol::game::ops::put_jewel(
        role, bag, slot, equip_slot, extension_bag_capability(), &operation_data);
    return qol::contracts::succeeded(result) ? "{\"ok\":true}" : op_err(result.message);
}
std::string data_op_enchant(int role, int bag, int slot, int equip_slot) {
    qol::contracts::QolItemInventoryStateWriteResult operation_data{};
    const QolResult result = qol::game::ops::enchant(
        role, bag, slot, equip_slot, extension_bag_capability(), &operation_data);
    return qol::contracts::succeeded(result) ? "{\"ok\":true}" : op_err(result.message);
}
std::string data_op_equip(int role, int bag, int slot) {
    qol::contracts::QolItemInventoryStateWriteResult operation_data{};
    const QolResult result = qol::game::ops::equip_item(
        role, bag, slot, extension_bag_capability(), &operation_data);
    return qol::contracts::succeeded(result) ? "{\"ok\":true}" : op_err(result.message);
}
std::string data_op_unequip(int role, int32_t equip_slot) {
    qol::contracts::QolItemInventoryStateWriteResult operation_data{};
    const QolResult result = qol::game::ops::unequip_item(
        role, equip_slot, extension_bag_capability(), &operation_data);
    return qol::contracts::succeeded(result) ? "{\"ok\":true}" : op_err(result.message);
}
