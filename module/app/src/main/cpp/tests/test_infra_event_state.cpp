#include "contracts/events.h"
#include "infra/event_state.h"

#include <cstdio>
#include <vector>

static int g_pass=0,g_fail=0;
#define CHECK(x) do { if(x) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d: %s\n",__FILE__,__LINE__,#x); } } while(0)
namespace {
using namespace qol::contracts;
bool g_valid=true,g_main=true;
QolOwnerToken g_invalid_owner=0;
QolResult verify(QolOwnerToken token){return g_valid&&token!=g_invalid_owner?ok():error(kQolErrBlocked,"x",1);}
bool main_gate(){return g_main;}
std::vector<EventStateChange> changes;
void receive(void*,const EventStateChange*c){changes.push_back(*c);}
EventStateDescriptor descriptor(uint32_t id=kEventStateSaveReady){return {sizeof(EventStateDescriptor),id,kEventStateSaveReadySchema,0};}
EventStateValue value(uint8_t avail,uint8_t ready,int32_t slot=2){return {sizeof(EventStateValue),kEventStateSaveReady,kEventStateSaveReadySchema,avail,ready,slot,0};}
void setup(){qol::infra::event_state_reset_for_test();g_valid=true;g_invalid_owner=0;g_main=true;changes.clear();qol::infra::event_state_set_owner_gate(verify);qol::infra::event_state_set_main_thread_gate(main_gate);}
void test_core(){setup();uint64_t id=0;CHECK(succeeded(qol::infra::event_state_subscribe(4,receive,nullptr,&id)));CHECK(qol::infra::event_state_dispatch_pending()==0);CHECK(succeeded(qol::infra::event_state_declare(11,descriptor())));CHECK(qol::infra::event_state_dispatch_pending()==0);
 CHECK(succeeded(qol::infra::event_state_set(11,value(1,0))));CHECK(qol::infra::event_state_dispatch_pending()==1);CHECK(changes.size()==1&&changes.back().change_kind==0&&changes.back().value==0&&changes.back().revision==1);
 CHECK(succeeded(qol::infra::event_state_set(11,value(1,0))));CHECK(qol::infra::event_state_dispatch_pending()==0);EventStateValue out{};CHECK(succeeded(qol::infra::event_state_read(4,&out))&&out.revision==1);
 CHECK(succeeded(qol::infra::event_state_set(11,value(1,1))));qol::infra::event_state_dispatch_pending();CHECK(changes.back().change_kind==1&&changes.back().revision==2);
 CHECK(succeeded(qol::infra::event_state_set(11,value(1,0))));qol::infra::event_state_dispatch_pending();CHECK(changes.back().change_kind==2&&changes.back().revision==3);
 uint64_t late=0;CHECK(succeeded(qol::infra::event_state_subscribe(4,receive,nullptr,&late)));qol::infra::event_state_dispatch_pending();CHECK(changes.back().change_kind==0&&changes.back().revision==3);
  CHECK(succeeded(qol::infra::event_state_set(11,value(1,1))));g_invalid_owner=11;CHECK(qol::infra::event_state_dispatch_pending()>=1);CHECK(changes.back().change_kind==3);CHECK(!qol::infra::event_state_has_active_declaration(4));CHECK(qol::infra::event_state_dispatch_pending()==0);
}
void test_invalid_owner_without_pending(){
 setup();uint64_t id=0;CHECK(succeeded(qol::infra::event_state_subscribe(4,receive,nullptr,&id)));
 CHECK(succeeded(qol::infra::event_state_declare(11,descriptor())));
 CHECK(succeeded(qol::infra::event_state_set(11,value(1,1))));
 CHECK(qol::infra::event_state_dispatch_pending()==1);changes.clear();
 // 停用回调里的 set_state 可能因 token 已 suspend 而失败；队列为空时仍必须补发 unavailable。
 g_invalid_owner=11;
 CHECK(qol::infra::event_state_dispatch_pending()==1);
 CHECK(!changes.empty()&&changes.back().change_kind==3&&changes.back().availability==0);
 CHECK(!qol::infra::event_state_has_active_declaration(4));
 CHECK(qol::infra::event_state_dispatch_pending()==0);
}
void test_errors_and_takeover(){setup();uint64_t x=0;CHECK(succeeded(qol::infra::event_state_subscribe(4,receive,nullptr,&x)));CHECK(qol::infra::event_state_declare(1,descriptor(99)).code==kQolErrNotFound);EventStateValue out{};CHECK(qol::infra::event_state_read(99,&out).code==kQolErrNotFound);CHECK(qol::infra::event_state_subscribe(99,receive,nullptr,&x).code==kQolErrNotFound);CHECK(qol::infra::event_state_subscribe(4,nullptr,nullptr,&x).code==kQolErrInvalidArgument);CHECK(qol::infra::event_state_read(4,nullptr).code==kQolErrInvalidArgument);
 CHECK(succeeded(qol::infra::event_state_declare(1,descriptor())));CHECK(succeeded(qol::infra::event_state_declare(1,descriptor())));CHECK(qol::infra::event_state_declare(2,descriptor()).code==kQolErrConflict);g_invalid_owner=1;CHECK(succeeded(qol::infra::event_state_declare(2,descriptor())));
 g_invalid_owner=0;CHECK(succeeded(qol::infra::event_state_set(2,value(1,1))));g_main=false;CHECK(qol::infra::event_state_set(2,value(1,0)).code==kQolErrBlocked);g_main=true;CHECK(succeeded(qol::infra::event_state_read(4,&out))&&out.value==1);
  CHECK(!qol::infra::event_state_host_publish(4,1,0,1));g_invalid_owner=2;CHECK(qol::infra::event_state_host_publish(4,1,0,1));CHECK(qol::infra::event_state_dispatch_pending()>=1);
}
}
int main(){test_core();test_invalid_owner_without_pending();test_errors_and_takeover();std::printf("infra_event_state: pass=%d fail=%d\n",g_pass,g_fail);return g_fail?1:0;}
