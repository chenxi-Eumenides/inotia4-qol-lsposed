#include "plugins/core.save/save_journal.h"
#include "game/access/save_file_evidence.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <unistd.h>

namespace qol::game {
void set_save_file_evidence_read_hook_for_test(void (*hook)(const char*));
}

using namespace qol::features::core_save;
static int g_pass = 0, g_fail = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d: %s\n",__FILE__,__LINE__,#c); } } while (0)
namespace {
int g_replace_hook_calls = 0;
void replace_dat_while_reading(const char* path) {
    qol::game::set_save_file_evidence_read_hook_for_test(nullptr);
    ++g_replace_hook_calls;
    const auto replacement = std::string(path) + ".replacement";
    CHECK(std::rename(replacement.c_str(), path) == 0);
}

void test_evidence_atomic_replacement() {
    char path[] = "save_evidence_replace_XXXXXX";
    const int fd = ::mkstemp(path);
    CHECK(fd >= 0);
    if (fd < 0) return;
    ::close(fd);
    const std::string bytes = "same dat bytes";
    { std::ofstream file(path, std::ios::binary); file << bytes; }
    const auto replacement = std::string(path) + ".replacement";
    // 新旧内容及长度相同，不能仅凭 SHA/长度误收已经被替换路径的旧 inode。
    { std::ofstream file(replacement, std::ios::binary); file << bytes; }
    qol::contracts::QolSaveFileEvidence output{};
    output.file_length = 777;
    std::fill(std::begin(output.sha256), std::end(output.sha256), 0x5a);
    const auto untouched = output;
    g_replace_hook_calls = 0;
    qol::game::set_save_file_evidence_read_hook_for_test(&replace_dat_while_reading);
    CHECK(!qol::game::read_save_file_evidence(path, &output));
    qol::game::set_save_file_evidence_read_hook_for_test(nullptr);
    CHECK(g_replace_hook_calls == 1);
    CHECK(output.file_length == untouched.file_length &&
          std::memcmp(output.sha256, untouched.sha256, sizeof(output.sha256)) == 0);
    CHECK(qol::game::read_save_file_evidence(path, &output) && output.file_length == bytes.size());
    ::unlink(path);
    ::unlink(replacement.c_str());
}
uint32_t crc(const std::vector<uint8_t>& bytes) {
    uint32_t c = 0xffffffff;
    for (std::size_t i = 0; i < bytes.size() - 4; ++i) {
        c ^= bytes[i];
        for (int j=0;j<8;++j) c = (c >> 1) ^ ((c & 1) ? 0xedb88320u : 0u);
    }
    return c ^ 0xffffffff;
}
void repair_crc(std::vector<uint8_t>& bytes) {
    const auto value = crc(bytes);
    for (unsigned i=0;i<4;++i) bytes[bytes.size()-4+i] = static_cast<uint8_t>(value >> (24-i*8));
}
std::vector<uint8_t> hex(const char* text) {
    std::vector<uint8_t> out;
    for (std::size_t i=0;i<std::strlen(text);i+=2) { unsigned v=0; std::sscanf(text+i,"%2x",&v); out.push_back(static_cast<uint8_t>(v)); }
    return out;
}
JournalDecode decode(const std::vector<uint8_t>& bytes, SaveJournal* out) {
    return decode_save_journal(2, 1, bytes.data(), bytes.size(), out);
}
void test_journal() {
    SaveJournal j;
    j.tx_id=9; j.slot=1; j.session_epoch=11;
    j.entries.push_back({7,"a",1,{1,2}});
    std::vector<uint8_t> bytes;
    CHECK(encode_save_journal(j,&bytes));
    CHECK(bytes.size() == 111);
    // 固定字节布局金样；尾 CRC 另以标准向量和损坏用例检查。
    const auto header = hex("53444a32000200300000006f0000000000000000000000090000000100000001000000000000000b0000000000000028");
    CHECK(std::equal(header.begin(),header.end(),bytes.begin()));
    const auto entry = hex("00000000000000070001000100000002610102");
    CHECK(std::equal(entry.begin(),entry.end(),bytes.begin()+88));
    CHECK(bytes[107]==0xee && bytes[108]==0x83 && bytes[109]==0x22 && bytes[110]==0x69);
    SaveJournal out;
    CHECK(decode(bytes,&out) == JournalDecode::kValid);
    CHECK(out.tx_id==9 && out.entries.size()==1 && out.entries[0].payload==j.entries[0].payload);
    for (std::size_t size=0;size<bytes.size();++size)
        CHECK(decode_save_journal(2,1,bytes.data(),size,&out)==JournalDecode::kCorrupt);
    const auto rejects = [&](std::size_t pos, uint8_t v) {
        auto bad=bytes; bad[pos]=v; repair_crc(bad); CHECK(decode(bad,&out)==JournalDecode::kCorrupt);
    };
    rejects(0,0); rejects(5,3); rejects(7,47); rejects(11,112); rejects(15,2);
    rejects(23,0); rejects(27,2); rejects(28,0xff); rejects(43,1); rejects(47,39);
    rejects(48,1); rejects(56,1); rejects(96,0xff); rejects(99,0); rejects(100,0xff);
    auto bad=bytes; bad.back()^=1; CHECK(decode(bad,&out)==JournalDecode::kCorrupt);
    rejects(104,'A');
    bad=bytes; bad.push_back(0); CHECK(decode(bad,&out)==JournalDecode::kCorrupt);
    bad=bytes; bad.insert(bad.end()-4,0); bad[11]=112; repair_crc(bad); CHECK(decode(bad,&out)==JournalDecode::kCorrupt);
    CHECK(decode_save_journal(2,0,bytes.data(),bytes.size(),&out)==JournalDecode::kCorrupt);
    j.entries.push_back(j.entries[0]); CHECK(!encode_save_journal(j,&bad));
    j.entries[1].name="b"; CHECK(encode_save_journal(j,&bad)); // 同 owner 不同 section 合法。
    bad[88+19+16]='a'; repair_crc(bad); CHECK(decode(bad,&out)==JournalDecode::kCorrupt);
    j.entries.resize(1);
    for (const auto name : {"module.save.journal","module.save.other","module.save","A",""}) {
        j.entries[0].name=name; CHECK(!encode_save_journal(j,&bad));
        if (*name != '\0') {
            j.entries[0].name=std::string(std::strlen(name),'a');
            CHECK(encode_save_journal(j,&bad));
            std::memcpy(bad.data()+104,name,std::strlen(name)); repair_crc(bad);
            CHECK(decode(bad,&out)==JournalDecode::kCorrupt);
        }
    }
    j.entries[0].name="a"; j.entries[0].version=0; CHECK(!encode_save_journal(j,&bad));
    j.entries[0].version=1;
    j.evidence.file_length=1; CHECK(!encode_save_journal(j,&bad)); j.evidence={};
    j.stage=JournalStage::kOriginalConfirmed; CHECK(!encode_save_journal(j,&bad));
    j.evidence.file_length=3; j.evidence.sha256[0]=1;
    CHECK(encode_save_journal(j,&bad) && bad.size()==bytes.size());
    CHECK(decode(bad,&out)==JournalDecode::kValid);
    auto no_evidence=bad; std::fill(no_evidence.begin()+48,no_evidence.begin()+88,0); repair_crc(no_evidence);
    CHECK(decode(no_evidence,&out)==JournalDecode::kCorrupt);
    bad[43]=0; repair_crc(bad); CHECK(decode(bad,&out)==JournalDecode::kCorrupt);
    j.stage=JournalStage::kPrepared; j.evidence={};
    j.entries[0].payload.assign(kSaveJournalMaxBytes-109,7);
    CHECK(encode_save_journal(j,&bytes) && bytes.size()==kSaveJournalMaxBytes);
    CHECK(decode(bytes,&out)==JournalDecode::kValid);
    j.entries[0].payload.push_back(7); CHECK(!encode_save_journal(j,&bad));
    j.entries[0].payload.assign(kSaveJournalMaxBytes+1,7); CHECK(!encode_save_journal(j,&bad));
}
void test_legacy() {
    const std::string valid="{\"transactionId\":\"tx\",\"slot\":1,\"stage\":0,\"sections\":[{\"name\":\"a\",\"version\":1,\"payload\":\"AQI=\"}]}";
    SaveJournal untouched; untouched.tx_id=99;
    const auto check = [&](const std::string& json, JournalDecode expected) {
        const auto original=json;
        CHECK(decode_save_journal(1,1,reinterpret_cast<const uint8_t*>(json.data()),json.size(),&untouched)==expected);
        CHECK(json==original && untouched.tx_id==99);
    };
    check(valid,JournalDecode::kLegacyUnresolved);
    for (const auto json : {"{", "{}", "{\"stage\":0,\"entries\":[]}", "[]"}) check(json,JournalDecode::kCorruptLegacy);
    for (const auto key : {"slot","stage","version"}) {
        auto bad=valid; const auto start=bad.find(std::string("\"")+key+"\":")+std::strlen(key)+3;
        bad[start]=key==std::string("version")?'0':'2'; check(bad,JournalDecode::kCorruptLegacy);
    }
    auto bad=valid; bad.replace(bad.find("AQI="),4,"!!!!"); check(bad,JournalDecode::kCorruptLegacy);
    check(valid+"x",JournalDecode::kCorruptLegacy);
    check(std::string(1,static_cast<char>(0xff))+valid,JournalDecode::kCorruptLegacy);
    check(std::string(kSaveJournalMaxBytes+1,' '),JournalDecode::kCorruptLegacy);
    CHECK(decode_save_journal(2,1,reinterpret_cast<const uint8_t*>(valid.data()),valid.size(),&untouched)==JournalDecode::kCorrupt);
    CHECK(decode_save_journal(3,1,reinterpret_cast<const uint8_t*>(valid.data()),valid.size(),&untouched)==JournalDecode::kUnsupported);
}
void test_evidence_and_health() {
    char path[]="save_evidence_XXXXXX";
    const int fd=::mkstemp(path); CHECK(fd>=0); if(fd<0) return; ::close(fd);
    qol::contracts::QolSaveFileEvidence evidence{};
    CHECK(qol::game::read_save_file_evidence(path,&evidence));
    const auto empty=hex("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(evidence.file_length==0 && std::memcmp(evidence.sha256,empty.data(),32)==0);
    { std::ofstream file(path,std::ios::binary); file<<"abc"; }
    CHECK(qol::game::read_save_file_evidence(path,&evidence));
    const auto abc=hex("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(evidence.file_length==3 && std::memcmp(evidence.sha256,abc.data(),32)==0);
    { std::ofstream file(path,std::ios::binary); file<<"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"; }
    CHECK(qol::game::read_save_file_evidence(path,&evidence));
    const auto padded=hex("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    CHECK(evidence.file_length==56 && std::memcmp(evidence.sha256,padded.data(),32)==0);
    { std::ofstream file(path,std::ios::binary); file<<std::string(1000000,'a'); }
    CHECK(qol::game::read_save_file_evidence(path,&evidence));
    const auto million=hex("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    CHECK(evidence.file_length==1000000 && std::memcmp(evidence.sha256,million.data(),32)==0);
    CHECK(!qol::game::read_save_file_evidence("no_such_dat",&evidence));
    CHECK(!qol::game::read_save_file_evidence(".",&evidence));
    ::unlink(path);
    SaveJournal j; j.tx_id=1; j.slot=1; j.evidence=evidence; j.stage=JournalStage::kOriginalConfirmed;
    CHECK(may_auto_recover(j,evidence));
    auto different=evidence; ++different.file_length; CHECK(!may_auto_recover(j,different));
    j.stage=JournalStage::kPrepared; CHECK(!may_auto_recover(j,evidence)); j.stage=JournalStage::kOriginalConfirmed;
    j.entries.push_back({7,"a",1,{1}});
    SaveRecoveryState health;
    CHECK(health.before_original_failure(SaveFailure::kPreparedWrite)==LeaseAction::kRelease);
    CHECK(health.before_original_failure(SaveFailure::kCancelledBeforeOriginal)==LeaseAction::kRelease);
    CHECK(health.before_original_failure(SaveFailure::kOriginalFailed)==LeaseAction::kRelease);
    CHECK(health.original_confirmed(j)); j.entries[0].payload[0]=9;
    CHECK(health.captured().entries[0].payload[0]==1);
    CHECK(!health.permits_save() && !health.permits_transition() && !health.permits_import() && !health.permits_data_write());
    CHECK(health.before_original_failure(SaveFailure::kOriginalFailed)==LeaseAction::kHold);
    for (uint64_t frame=1;frame<=3;++frame) {
        CHECK(health.retry_frame(frame)); CHECK(!health.retry_frame(frame));
        CHECK(health.retry_result(false)==LeaseAction::kHold);
    }
    CHECK(health.health()==SlotHealth::kDegraded && health.retries()==3 && !health.retry_frame(4));
    CHECK(health.explicit_recovery_finished(false)==LeaseAction::kHold);
    CHECK(health.explicit_recovery_finished(true)==LeaseAction::kRelease && health.permits_save());
    CHECK(health.original_confirmed(j) && health.retry_frame(10));
    CHECK(health.retry_result(true)==LeaseAction::kRelease && health.permits_save());
}
}
int main() {
    test_journal(); test_legacy(); test_evidence_and_health(); test_evidence_atomic_replacement();
    std::printf("save_journal: pass=%d fail=%d\n",g_pass,g_fail);
    return g_fail==0?0:1;
}
