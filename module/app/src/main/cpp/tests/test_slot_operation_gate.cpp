#include "contracts/slot_operation.h"
#include "infra/slot_operation_gate.h"
#include "infra/store.h"

#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

namespace op = qol::infra::slot_operation;
namespace store = qol::infra::store;
using namespace qol::contracts;
static int g_pass=0, g_fail=0;
#define CHECK(c) do { if(c) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d: %s\n",__FILE__,__LINE__,#c); } } while(0)
QolSlotOperationLease lease() { QolSlotOperationLease out{}; out.size=sizeof(out); return out; }
int main() {
    char dir[]="slot_operation_test_XXXXXX";
    CHECK(::mkdtemp(dir)!=nullptr);
    store::init(dir);
    auto a=lease(), b=lease(), c=lease();
    CHECK(!succeeded(op::acquire(0,1,&a)));
    CHECK(!succeeded(op::acquire(7,0,&a)) && !succeeded(op::acquire(7,8,&a)));
    CHECK(succeeded(op::acquire(7,3,&a)));
    CHECK(op::acquire(7,1,&b).code==kQolErrBusy); // 同 owner 不隐式重入。
    CHECK(op::acquire(8,6,&b).code==kQolErrBusy);
    CHECK(succeeded(op::acquire(8,4,&b))); // 失败的 mask=6 未部分占用 slot2。
    CHECK(op::refresh(8,a.id,&c).code==kQolErrBlocked);
    CHECK(succeeded(op::refresh(7,a.id,&c)) && c.id==a.id);
    CHECK(store::ensure_slot(0)==store::Status::kBusy);
    CHECK(store::reset_slot(0)==store::Status::kBusy);
    CHECK(store::delete_slot(0)==store::Status::kBusy);
    CHECK(store::import_container(0,nullptr,0)==store::Status::kBusy);
    CHECK(store::write_section(0,"a",1,nullptr,0)==store::Status::kBusy);
    CHECK(store::remove_section(0,"a")==store::Status::kBusy);
    CHECK(store::replace_sections(0,{}, {})==store::Status::kBusy);
    std::vector<uint8_t> bytes; uint16_t version=0;
    CHECK(store::read_section(0,"a",&bytes,&version)==store::Status::kNotFound);
    CHECK(store::ensure_in_operation(7,&a,0)==store::Status::kOk);
    CHECK(store::reset_in_operation(7,&a,2)==store::Status::kBadSlot);
    QolSlotOperationApply probe{sizeof(probe),99};
    CHECK(store::import_in_operation(7,&a,2,nullptr,0,&probe)==store::Status::kBadSlot);
    CHECK(probe.apply==static_cast<uint32_t>(QolSlotOperationApplyState::kUnchanged));
    uint8_t payload=9;
    QolStoreSection section{"a",1,&payload,1};
    QolOwnerStoreCandidate candidate{sizeof(candidate),1,&section,0,nullptr,nullptr};
    QolOwnerStoreReservation reservation{}; reservation.size=sizeof(reservation);
    CHECK(store::reserve(7,0,&candidate,1,&reservation)==store::Status::kBusy);
    CHECK(store::reserve_in_operation(7,&a,0,&candidate,1,&reservation)==store::Status::kOk);
    CHECK(reservation.operation==a.id);
    CHECK(op::release(7,&a).code==kQolErrBusy);
    const auto old=reservation;
    CHECK(store::replace_reserved(7,0,&reservation,{{"a",{1,{9}}}}, {})==store::Status::kOk);
    CHECK(reservation.generation>old.generation && reservation.revision>old.revision);
    CHECK(store::release(7,old)==store::Status::kBusy);
    auto forged=reservation; forged.operation=b.id;
    CHECK(store::replace_reserved(7,0,&forged,{}, {})==store::Status::kBusy);
    const auto old_a=a;
    CHECK(succeeded(op::set_referenced(7,&a,1)));
    CHECK(op::release(7,&old_a).code==kQolErrBusy);
    CHECK(op::set_referenced(7,&c,0).code==kQolErrBusy);
    store::owner_stopped(7); // operation 引用保护关联的未置 retain 预算。
    CHECK(store::write_section(0,"a",1,nullptr,0)==store::Status::kBusy);
    store::owner_rebound(7,9);
    CHECK(op::refresh(7,a.id,&c).code==kQolErrBlocked);
    CHECK(succeeded(op::refresh(9,a.id,&c)) && c.revision>a.revision);
    QolOwnerStoreSlotState state{}; state.size=sizeof(state);
    CHECK(store::inspect_slot(0,&state)==store::Status::kOk && state.reservation.owner==9);
    CHECK(op::release(9,&c).code==kQolErrBusy);
    CHECK(store::release(9,state.reservation)==store::Status::kOk);
    CHECK(succeeded(op::release(9,&c)));
    CHECK(!succeeded(op::release(9,&c)));
    CHECK(succeeded(op::release(8,&b)));

    std::atomic<bool> start{false};
    QolSlotOperationLease concurrent[2]{lease(),lease()};
    QolResult result[2]{};
    std::thread first([&] { while(!start.load()) std::this_thread::yield(); result[0]=op::acquire(20,3,&concurrent[0]); });
    std::thread second([&] { while(!start.load()) std::this_thread::yield(); result[1]=op::acquire(21,3,&concurrent[1]); });
    start.store(true); first.join(); second.join();
    CHECK(succeeded(result[0]) != succeeded(result[1]));
    const unsigned winner=succeeded(result[0])?0:1;
    CHECK(result[1-winner].code==kQolErrBusy);
    CHECK(succeeded(op::release(20+winner,&concurrent[winner])));

    CHECK(store::reset_slot(0)==store::Status::kOk);
    CHECK(store::write_section(0,"a",1,&payload,1)==store::Status::kOk);
    std::vector<uint8_t> original;
    CHECK(store::read_container(0,&original)==store::Status::kOk);
    payload=5;
    CHECK(store::write_section(1,"a",1,&payload,1)==store::Status::kOk);
    std::vector<uint8_t> imported;
    CHECK(store::read_container(1,&imported)==store::Status::kOk);
    a=lease(); CHECK(succeeded(op::acquire(7,1,&a)) && succeeded(op::set_referenced(7,&a,1)));
    const auto blocked_tmp=std::string(dir)+"/module-saves/slot-0.module-save.last-good.tmp";
    CHECK(::mkdir(blocked_tmp.c_str(),0755)==0);
    QolSlotOperationApply apply{sizeof(apply),99};
    CHECK(store::import_in_operation(7,&a,0,imported.data(),imported.size(),&apply)==store::Status::kIoError);
    CHECK(apply.apply==static_cast<uint32_t>(QolSlotOperationApplyState::kPrimaryApplied));
    CHECK(store::read_section(0,"a",&bytes,&version)==store::Status::kOk && bytes==std::vector<uint8_t>{5});
    CHECK(store::write_section(0,"a",1,nullptr,0)==store::Status::kBusy);
    CHECK(::rmdir(blocked_tmp.c_str())==0);
    CHECK(store::import_in_operation(7,&a,0,original.data(),original.size(),&apply)==store::Status::kOk);
    CHECK(apply.apply==static_cast<uint32_t>(QolSlotOperationApplyState::kFullyApplied));
    CHECK(store::read_section(0,"a",&bytes,&version)==store::Status::kOk && bytes==std::vector<uint8_t>{9});
    CHECK(succeeded(op::release(7,&a))); // 完整回滚后才释放。
    CHECK(store::write_section(0,"a",1,&payload,1)==store::Status::kOk);
    // operation 内 delete 与槽内容无关，只要求 slot 在 lease mask 内且无关联预约。
    a=lease(); CHECK(succeeded(op::acquire(7,1,&a)));
    CHECK(store::delete_in_operation(7,&a,0)==store::Status::kOk);
    CHECK(store::write_section(0,"a",1,nullptr,0)==store::Status::kBusy);
    CHECK(store::ensure_in_operation(7,&a,0)==store::Status::kOk);
    CHECK(store::reserve_in_operation(7,&a,0,&candidate,1,&reservation)==store::Status::kOk);
    CHECK(store::delete_in_operation(7,&a,0)==store::Status::kBusy);
    CHECK(store::release(7,reservation)==store::Status::kOk);
    CHECK(succeeded(op::release(7,&a)));
    CHECK(store::write_section(0,"a",1,&payload,1)==store::Status::kOk);
    a=lease(); CHECK(succeeded(op::acquire(7,1,&a))); store::owner_stopped(7);
    CHECK(!succeeded(op::refresh(7,a.id,&c)));
    CHECK(store::delete_slot(0)==store::Status::kOk);
    CHECK(store::delete_slot(1)==store::Status::kOk);
    CHECK(::rmdir((std::string(dir)+"/module-saves").c_str())==0);
    CHECK(::rmdir(dir)==0);
    std::printf("slot_operation_gate: pass=%d fail=%d\n",g_pass,g_fail);
    return g_fail==0?0:1;
}
