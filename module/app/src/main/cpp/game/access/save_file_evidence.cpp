#include "save_file_evidence.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <sys/stat.h>

namespace qol::game {
namespace {
#if defined(QOL_SAVE_EVIDENCE_TEST_SEAMS)
void (*g_read_hook_for_test)(const char*) = nullptr;
#endif
constexpr uint32_t kRound[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
uint32_t rotate(uint32_t v, unsigned n) { return (v >> n) | (v << (32 - n)); }
void block(const uint8_t* bytes, std::array<uint32_t, 8>& state) {
    uint32_t w[64]{};
    for (unsigned i = 0; i < 16; ++i)
        for (unsigned j = 0; j < 4; ++j) w[i] = (w[i] << 8) | bytes[i * 4 + j];
    for (unsigned i = 16; i < 64; ++i) {
        const auto a = w[i - 15], b = w[i - 2];
        w[i] = w[i - 16] + (rotate(a,7) ^ rotate(a,18) ^ (a >> 3)) + w[i - 7] +
               (rotate(b,17) ^ rotate(b,19) ^ (b >> 10));
    }
    auto a=state[0], b=state[1], c=state[2], d=state[3], e=state[4], f=state[5], g=state[6], h=state[7];
    for (unsigned i = 0; i < 64; ++i) {
        const auto t1 = h + (rotate(e,6) ^ rotate(e,11) ^ rotate(e,25)) + ((e & f) ^ (~e & g)) + kRound[i] + w[i];
        const auto t2 = (rotate(a,2) ^ rotate(a,13) ^ rotate(a,22)) + ((a & b) ^ (a & c) ^ (b & c));
        h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    state[0]+=a; state[1]+=b; state[2]+=c; state[3]+=d;
    state[4]+=e; state[5]+=f; state[6]+=g; state[7]+=h;
}
}
bool read_save_file_evidence(const char* path, contracts::QolSaveFileEvidence* out) {
    if (path == nullptr || *path == '\0' || out == nullptr) return false;
    FILE* file = std::fopen(path, "rb");
    if (file == nullptr) return false;
    struct stat before{};
    bool valid = ::fstat(::fileno(file), &before) == 0 && S_ISREG(before.st_mode) && before.st_size >= 0;
    std::array<uint32_t,8> state{0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                                0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    uint8_t buffer[64]{};
    uint64_t length = 0;
    std::size_t n = 0;
    while (valid) {
        n = std::fread(buffer, 1, sizeof(buffer), file);
        if (n > std::numeric_limits<uint64_t>::max() / 8 - length) { valid = false; break; }
        length += n;
#if defined(QOL_SAVE_EVIDENCE_TEST_SEAMS)
        if (n != 0 && g_read_hook_for_test != nullptr) g_read_hook_for_test(path);
#endif
        if (n != sizeof(buffer)) break;
        block(buffer, state);
    }
    struct stat after{};
    struct stat path_after{};
    valid = valid && std::ferror(file) == 0 && ::fstat(::fileno(file), &after) == 0 &&
            ::stat(path, &path_after) == 0 &&
            before.st_dev == after.st_dev && before.st_ino == after.st_ino &&
            path_after.st_dev == after.st_dev && path_after.st_ino == after.st_ino &&
            before.st_size == after.st_size && static_cast<uint64_t>(after.st_size) == length &&
            before.st_mtim.tv_sec == after.st_mtim.tv_sec && before.st_mtim.tv_nsec == after.st_mtim.tv_nsec &&
            before.st_ctim.tv_sec == after.st_ctim.tv_sec && before.st_ctim.tv_nsec == after.st_ctim.tv_nsec;
    if (std::fclose(file) != 0) valid = false;
    if (!valid) return false;
    // 时间与 inode 仅检测读取中的变化/路径替换，身份始终是原字节 SHA-256。
    // 此检查不覆盖 evidence 到正式提交的窗口，调用方仍须持有槽准入。
    buffer[n++] = 0x80;
    if (n > 56) { std::memset(buffer + n, 0, 64 - n); block(buffer, state); n = 0; }
    std::memset(buffer + n, 0, 56 - n);
    const auto bits = length * 8;
    for (unsigned i = 0; i < 8; ++i) buffer[56 + i] = static_cast<uint8_t>(bits >> (56 - i * 8));
    block(buffer, state);
    contracts::QolSaveFileEvidence result{};
    result.file_length = length;
    for (unsigned i = 0; i < 8; ++i)
        for (unsigned j = 0; j < 4; ++j) result.sha256[i * 4 + j] = static_cast<uint8_t>(state[i] >> (24 - j * 8));
    *out = result;
    return true;
}
#if defined(QOL_SAVE_EVIDENCE_TEST_SEAMS)
void set_save_file_evidence_read_hook_for_test(void (*hook)(const char*)) {
    g_read_hook_for_test = hook;
}
#endif
}  // namespace qol::game
