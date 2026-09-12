#include "feature/save_backup/save_backup_bundle.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace save_backup {
namespace {

// ---- 大端读写（bundle 与 sidecar 容器均为大端布局）----

void wb16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v));
}

void wb32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v >> 24));
    out.push_back(static_cast<uint8_t>(v >> 16));
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v));
}

void wb64(std::vector<uint8_t>& out, uint64_t v) {
    for (int i = 7; i >= 0; --i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

uint16_t be16(const uint8_t* p) {
    return static_cast<uint16_t>(static_cast<uint16_t>(p[0]) << 8 | static_cast<uint16_t>(p[1]));
}

uint32_t be32(const uint8_t* p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | uint32_t(p[3]);
}

uint64_t be64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = v << 8 | p[i];
    return v;
}

// ---- SHA-256（FIPS 180-4）----

struct Sha256Ctx {
    uint32_t h[8];
    uint64_t total_len;
    uint8_t buf[64];
    size_t buf_len;
};

constexpr uint32_t kSha256K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
    0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
    0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
    0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

uint32_t rotr32(uint32_t v, int n) { return v >> n | v << (32 - n); }

void sha256_init(Sha256Ctx& c) {
    c.h[0] = 0x6a09e667u;
    c.h[1] = 0xbb67ae85u;
    c.h[2] = 0x3c6ef372u;
    c.h[3] = 0xa54ff53au;
    c.h[4] = 0x510e527fu;
    c.h[5] = 0x9b05688cu;
    c.h[6] = 0x1f83d9abu;
    c.h[7] = 0x5be0cd19u;
    c.total_len = 0;
    c.buf_len = 0;
}

void sha256_block(Sha256Ctx& c, const uint8_t* p) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = uint32_t(p[i * 4]) << 24 | uint32_t(p[i * 4 + 1]) << 16 |
               uint32_t(p[i * 4 + 2]) << 8 | uint32_t(p[i * 4 + 3]);
    }
    for (int i = 16; i < 64; ++i) {
        const uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = c.h[0], b = c.h[1], cc = c.h[2], d = c.h[3];
    uint32_t e = c.h[4], f = c.h[5], g = c.h[6], h = c.h[7];
    for (int i = 0; i < 64; ++i) {
        const uint32_t s1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        const uint32_t ch = (e & f) ^ (~e & g);
        const uint32_t t1 = h + s1 + ch + kSha256K[i] + w[i];
        const uint32_t s0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        const uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
        const uint32_t t2 = s0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = cc;
        cc = b;
        b = a;
        a = t1 + t2;
    }
    c.h[0] += a;
    c.h[1] += b;
    c.h[2] += cc;
    c.h[3] += d;
    c.h[4] += e;
    c.h[5] += f;
    c.h[6] += g;
    c.h[7] += h;
}

void sha256_update(Sha256Ctx& c, const uint8_t* data, size_t len) {
    c.total_len += len;
    while (len > 0) {
        if (c.buf_len == 0 && len >= 64) {
            sha256_block(c, data);
            data += 64;
            len -= 64;
            continue;
        }
        const size_t take = len < 64 - c.buf_len ? len : 64 - c.buf_len;
        std::memcpy(c.buf + c.buf_len, data, take);
        c.buf_len += take;
        data += take;
        len -= take;
        if (c.buf_len == 64) {
            sha256_block(c, c.buf);
            c.buf_len = 0;
        }
    }
}

void sha256_final(Sha256Ctx& c, uint8_t digest[32]) {
    const uint64_t bit_len = c.total_len * 8;
    const uint8_t pad = 0x80;
    sha256_update(c, &pad, 1);
    const uint8_t zero = 0;
    while (c.buf_len != 56) sha256_update(c, &zero, 1);
    uint8_t len_bytes[8];
    for (int i = 0; i < 8; ++i) len_bytes[i] = static_cast<uint8_t>(bit_len >> (8 * (7 - i)));
    sha256_update(c, len_bytes, 8);
    for (int i = 0; i < 8; ++i) {
        digest[i * 4] = static_cast<uint8_t>(c.h[i] >> 24);
        digest[i * 4 + 1] = static_cast<uint8_t>(c.h[i] >> 16);
        digest[i * 4 + 2] = static_cast<uint8_t>(c.h[i] >> 8);
        digest[i * 4 + 3] = static_cast<uint8_t>(c.h[i]);
    }
}

std::string bytes_to_hex(const uint8_t* data, size_t len) {
    static const char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out += kHex[data[i] >> 4];
        out += kHex[data[i] & 0x0F];
    }
    return out;
}

// ---- MD5（RFC 1321）----

struct Md5Ctx {
    uint32_t a, b, cc, d;
    uint64_t total_len;
    uint8_t buf[64];
    size_t buf_len;
};

uint32_t rotl32(uint32_t v, int n) { return v << n | v >> (32 - n); }

void md5_init(Md5Ctx& c) {
    c.a = 0x67452301u;
    c.b = 0xefcdab89u;
    c.cc = 0x98badcfeu;
    c.d = 0x10325476u;
    c.total_len = 0;
    c.buf_len = 0;
}

void md5_block(Md5Ctx& c, const uint8_t* p) {
    static const uint32_t k[64] = {
        0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu, 0xf57c0fafu, 0x4787c62au,
        0xa8304613u, 0xfd469501u, 0x698098d8u, 0x8b44f7afu, 0xffff5bb1u, 0x895cd7beu,
        0x6b901122u, 0xfd987193u, 0xa679438eu, 0x49b40821u, 0xf61e2562u, 0xc040b340u,
        0x265e5a51u, 0xe9b6c7aau, 0xd62f105du, 0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u,
        0x21e1cde6u, 0xc33707d6u, 0xf4d50d87u, 0x455a14edu, 0xa9e3e905u, 0xfcefa3f8u,
        0x676f02d9u, 0x8d2a4c8au, 0xfffa3942u, 0x8771f681u, 0x6d9d6122u, 0xfde5380cu,
        0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u, 0x289b7ec6u, 0xeaa127fau,
        0xd4ef3085u, 0x04881d05u, 0xd9d4d039u, 0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u,
        0xf4292244u, 0x432aff97u, 0xab9423a7u, 0xfc93a039u, 0x655b59c3u, 0x8f0ccc92u,
        0xffeff47du, 0x85845dd1u, 0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u,
        0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu, 0xeb86d391u,
    };
    static const int r[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                              5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                              4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                              6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
    uint32_t m[16];
    for (int i = 0; i < 16; ++i) {
        m[i] = uint32_t(p[i * 4]) | uint32_t(p[i * 4 + 1]) << 8 | uint32_t(p[i * 4 + 2]) << 16 |
               uint32_t(p[i * 4 + 3]) << 24;
    }
    uint32_t a = c.a, b = c.b, cc = c.cc, d = c.d;
    for (int i = 0; i < 64; ++i) {
        uint32_t f;
        int g;
        if (i < 16) {
            f = (b & cc) | (~b & d);
            g = i;
        } else if (i < 32) {
            f = (d & b) | (~d & cc);
            g = (5 * i + 1) % 16;
        } else if (i < 48) {
            f = b ^ cc ^ d;
            g = (3 * i + 5) % 16;
        } else {
            f = cc ^ (b | ~d);
            g = (7 * i) % 16;
        }
        const uint32_t tmp = d;
        d = cc;
        cc = b;
        b = b + rotl32(a + f + k[i] + m[g], r[i]);
        a = tmp;
    }
    c.a += a;
    c.b += b;
    c.cc += cc;
    c.d += d;
}

void md5_update(Md5Ctx& c, const uint8_t* data, size_t len) {
    c.total_len += len;
    while (len > 0) {
        const size_t take = len < 64 - c.buf_len ? len : 64 - c.buf_len;
        std::memcpy(c.buf + c.buf_len, data, take);
        c.buf_len += take;
        data += take;
        len -= take;
        if (c.buf_len == 64) {
            md5_block(c, c.buf);
            c.buf_len = 0;
        }
    }
}

void md5_final(Md5Ctx& c, uint8_t digest[16]) {
    const uint64_t bit_len = c.total_len * 8;
    const uint8_t pad = 0x80;
    md5_update(c, &pad, 1);
    const uint8_t zero = 0;
    while (c.buf_len != 56) md5_update(c, &zero, 1);
    uint8_t len_bytes[8];
    for (int i = 0; i < 8; ++i) len_bytes[i] = static_cast<uint8_t>(bit_len >> (8 * i));
    md5_update(c, len_bytes, 8);
    for (int i = 0; i < 4; ++i) {
        digest[i] = static_cast<uint8_t>(c.a >> (8 * i));
        digest[4 + i] = static_cast<uint8_t>(c.b >> (8 * i));
        digest[8 + i] = static_cast<uint8_t>(c.cc >> (8 * i));
        digest[12 + i] = static_cast<uint8_t>(c.d >> (8 * i));
    }
}

// ---- 扁平 JSON 字段定位 ----

// 返回 key 对应 value 的起始指针（跳过 ':' 与空白）；未命中返回 nullptr。
const char* find_key_value(const std::string& json, const char* key) {
    const std::string pat = std::string("\"") + key + "\"";
    size_t pos = 0;
    while ((pos = json.find(pat, pos)) != std::string::npos) {
        size_t p = pos + pat.size();
        while (p < json.size() && (json[p] == ' ' || json[p] == '\t' || json[p] == '\n' ||
                                   json[p] == '\r')) {
            ++p;
        }
        if (p < json.size() && json[p] == ':') {
            ++p;
            while (p < json.size() && (json[p] == ' ' || json[p] == '\t' || json[p] == '\n' ||
                                       json[p] == '\r')) {
                ++p;
            }
            return json.c_str() + p;
        }
        pos += 1;
    }
    return nullptr;
}

bool ends_with(const std::string& s, const char* suffix, size_t suffix_len) {
    return s.size() >= suffix_len && s.compare(s.size() - suffix_len, suffix_len, suffix) == 0;
}

}  // namespace

// ---- 摘要与编码原语 ----

std::string sha256_hex(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    Sha256Ctx c;
    sha256_init(c);
    sha256_update(c, a.data(), a.size());
    sha256_update(c, b.data(), b.size());
    uint8_t digest[32];
    sha256_final(c, digest);
    return bytes_to_hex(digest, 32);
}

std::string md5_hex(const uint8_t* data, size_t len) {
    Md5Ctx c;
    md5_init(c);
    md5_update(c, data, len);
    uint8_t digest[16];
    md5_final(c, digest);
    return bytes_to_hex(digest, 16);
}

uint32_t crc32_ieee(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int k = 0; k < 8; ++k) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

std::string base64_encode(const uint8_t* data, size_t size) {
    static const char kTable[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((size + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 3 <= size; i += 3) {
        const uint32_t v = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8) | data[i + 2];
        out += kTable[(v >> 18) & 0x3F];
        out += kTable[(v >> 12) & 0x3F];
        out += kTable[(v >> 6) & 0x3F];
        out += kTable[v & 0x3F];
    }
    if (i + 1 == size) {
        const uint32_t v = uint32_t(data[i]) << 16;
        out += kTable[(v >> 18) & 0x3F];
        out += kTable[(v >> 12) & 0x3F];
        out += "==";
    } else if (i + 2 == size) {
        const uint32_t v = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8);
        out += kTable[(v >> 18) & 0x3F];
        out += kTable[(v >> 12) & 0x3F];
        out += kTable[(v >> 6) & 0x3F];
        out += '=';
    }
    return out;
}

bool base64_decode(const std::string& in, std::vector<uint8_t>& out) {
    out.clear();
    if (in.size() % 4 != 0) return false;
    auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    out.reserve(in.size() / 4 * 3);
    for (size_t i = 0; i < in.size(); i += 4) {
        int v[4] = {0, 0, 0, 0};
        for (int k = 0; k < 4; ++k) {
            if (in[i + k] == '=') {
                v[k] = -2;
            } else {
                v[k] = value(in[i + k]);
                if (v[k] < 0) {
                    out.clear();
                    return false;
                }
            }
        }
        if (v[0] < 0 || v[1] < 0) {
            out.clear();
            return false;
        }
        const uint32_t a = uint32_t(v[0]) << 18 | uint32_t(v[1]) << 12;
        if (v[2] == -2) {
            if (v[3] != -2) {
                out.clear();
                return false;
            }
            out.push_back(static_cast<uint8_t>(a >> 16));
            continue;
        }
        if (v[3] == -2) {
            const uint32_t b = a | uint32_t(v[2]) << 6;
            out.push_back(static_cast<uint8_t>(b >> 16));
            out.push_back(static_cast<uint8_t>((b >> 8) & 0xFF));
            continue;
        }
        const uint32_t b = a | uint32_t(v[2]) << 6 | uint32_t(v[3]);
        out.push_back(static_cast<uint8_t>(b >> 16));
        out.push_back(static_cast<uint8_t>((b >> 8) & 0xFF));
        out.push_back(static_cast<uint8_t>(b & 0xFF));
    }
    return true;
}

// ---- bundle 组装与解析 ----

std::string build_meta_json(int source_slot, long long export_time_ms, int map_id, int hero_level,
                            int hero_index, int save_version, long long save_time,
                            const std::string& original_sha256, const std::string& module_sha256,
                            const std::string& checksum, int class_idx,
                            const std::string& class_name) {
    std::string s = "{\"source_slot\":";
    s += std::to_string(source_slot);
    s += ",\"export_time\":" + std::to_string(export_time_ms);
    s += ",\"map_id\":" + std::to_string(map_id);
    s += ",\"hero_level\":" + std::to_string(hero_level);
    s += ",\"hero_index\":" + std::to_string(hero_index);
    s += ",\"save_version\":" + std::to_string(save_version);
    s += ",\"save_time\":" + std::to_string(save_time);
    s += ",\"original_sha256\":\"" + original_sha256 + "\"";
    s += ",\"module_sha256\":\"" + module_sha256 + "\"";
    s += ",\"checksum\":\"" + checksum + "\"";
    // 职业（v0.7.x 追加）：class_idx 数字 + class_name 中文名（name 为纯中文/ASCII，无转义）。
    s += ",\"class_idx\":" + std::to_string(class_idx);
    s += ",\"class_name\":\"" + class_name + "\"";
    s += "}";
    return s;
}

void build_bundle(int source_slot, long long export_time_ms, const std::vector<uint8_t>& plain,
                  const std::vector<uint8_t>& module, const std::string& meta_json,
                  std::vector<uint8_t>& out) {
    out.clear();
    wb32(out, kBundleMagic);
    wb16(out, kBundleVersion);
    out.push_back(static_cast<uint8_t>(source_slot));
    out.push_back(0);
    out.push_back(0);
    out.push_back(0);
    wb64(out, static_cast<uint64_t>(export_time_ms));
    wb32(out, static_cast<uint32_t>(plain.size()));
    out.insert(out.end(), plain.begin(), plain.end());
    wb32(out, static_cast<uint32_t>(module.size()));
    out.insert(out.end(), module.begin(), module.end());
    wb16(out, static_cast<uint16_t>(meta_json.size()));
    out.insert(out.end(), meta_json.begin(), meta_json.end());
    wb32(out, crc32_ieee(out.data(), out.size()));
}

bool parse_bundle(const uint8_t* bytes, size_t size, ParsedBundle& out) {
    if (bytes == nullptr || size < kMinBundleBytes) return false;
    const size_t body_size = size - 4;
    if (crc32_ieee(bytes, body_size) != be32(bytes + body_size)) return false;
    if (be32(bytes) != kBundleMagic) return false;
    if (be16(bytes + 4) != kBundleVersion) return false;
    const int source_slot = bytes[6];
    if (source_slot > 2) return false;
    const long long export_time_ms = static_cast<long long>(be64(bytes + 10));
    size_t off = 18;
    const uint32_t orig_len = be32(bytes + off);
    off += 4;
    if (orig_len > body_size || off + orig_len > body_size) return false;
    const uint8_t* orig_plain = bytes + off;
    off += orig_len;
    const uint32_t module_len = be32(bytes + off);
    off += 4;
    if (module_len > body_size || off + module_len > body_size) return false;
    const uint8_t* module = bytes + off;
    off += module_len;
    if (off + 2 > body_size) return false;
    const uint16_t meta_len = be16(bytes + off);
    off += 2;
    if (off + meta_len != body_size) return false;

    out.source_slot = source_slot;
    out.export_time_ms = export_time_ms;
    out.orig_plain.assign(orig_plain, orig_plain + orig_len);
    out.module.assign(module, module + module_len);
    out.meta_json.assign(reinterpret_cast<const char*>(bytes + off), meta_len);
    out.size_bytes = static_cast<long long>(size);
    return true;
}

std::string entry_json(const Entry& e) {
    std::string s = "{\"file_name\":\"" + e.file_name + "\"";
    s += ",\"size_bytes\":" + std::to_string(e.size_bytes);
    s += ",\"source_slot\":" + std::to_string(e.source_slot);
    s += ",\"export_time\":" + std::to_string(e.export_time_ms);
    s += ",\"map_id\":" + std::to_string(e.map_id);
    // map_name 缺省空串：启动前/未命中时让上层走"未知地图"显示而非丢弃字段。
    s += ",\"map_name\":\"" + e.map_name + "\"";
    s += ",\"hero_level\":" + std::to_string(e.hero_level);
    s += ",\"hero_index\":" + std::to_string(e.hero_index);
    s += ",\"class_idx\":" + std::to_string(e.class_idx);
    s += ",\"class_name\":\"" + e.class_name + "\"";
    s += ",\"save_version\":" + std::to_string(e.save_version);
    s += ",\"save_time\":" + std::to_string(e.save_time);
    s += ",\"original_sha256\":\"" + e.original_sha256 + "\"";
    s += ",\"module_sha256\":\"" + e.module_sha256 + "\"";
    s += ",\"checksum\":\"" + e.checksum + "\"";
    s += "}";
    return s;
}

void entry_from_bundle(const std::string& file_name, const ParsedBundle& p, Entry& out) {
    out = Entry{};
    out.file_name = file_name;
    out.size_bytes = p.size_bytes;
    out.source_slot = p.source_slot;
    out.export_time_ms = p.export_time_ms;
    long long v = 0;
    std::string s;
    if (json_find_int(p.meta_json, "map_id", v)) out.map_id = static_cast<int>(v);
    if (json_find_int(p.meta_json, "hero_level", v)) out.hero_level = static_cast<int>(v);
    if (json_find_int(p.meta_json, "hero_index", v)) out.hero_index = static_cast<int>(v);
    // 职业：旧备份无 class_idx/class_name → 保持默认 -1/空，UI 退化显示。
    if (json_find_int(p.meta_json, "class_idx", v)) out.class_idx = static_cast<int>(v);
    if (json_find_string(p.meta_json, "class_name", s)) out.class_name = s;
    if (json_find_int(p.meta_json, "save_version", v)) out.save_version = static_cast<int>(v);
    if (json_find_int(p.meta_json, "save_time", v)) out.save_time = v;
    if (json_find_string(p.meta_json, "original_sha256", s)) out.original_sha256 = s;
    if (json_find_string(p.meta_json, "module_sha256", s)) out.module_sha256 = s;
    // checksum 以 metaJson 为权威；旧备份缺失时从文件名兜底（与 Kotlin 一致）。
    if (!json_find_string(p.meta_json, "checksum", s) || s.empty()) {
        s = extract_checksum_from_name(file_name, s) ? s : std::string();
    }
    out.checksum = s;
}

// ---- 极简扁平 JSON 字段提取 ----

bool json_find_int(const std::string& json, const char* key, long long& out) {
    const char* p = find_key_value(json, key);
    if (p == nullptr) return false;
    char* end = nullptr;
    const long long v = std::strtoll(p, &end, 10);
    if (end == p) return false;
    out = v;
    return true;
}

bool json_find_string(const std::string& json, const char* key, std::string& out) {
    const char* p = find_key_value(json, key);
    if (p == nullptr || *p != '"') return false;
    ++p;
    const char* end = std::strchr(p, '"');
    if (end == nullptr) return false;
    out.assign(p, static_cast<size_t>(end - p));
    return true;
}

bool valid_checksum(const std::string& value) {
    if (value.size() != 12) return false;
    for (char c : value) {
        const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!ok) return false;
    }
    return true;
}

bool extract_checksum_from_name(const std::string& file_name, std::string& out) {
    // `_([0-9a-f]{12}).qol_save$`
    constexpr size_t kPat = 12 + 10;  // '_' + 12 hex + ".qol_save"
    if (file_name.size() < kPat) return false;
    if (!ends_with(file_name, ".qol_save", 9)) return false;
    const size_t start = file_name.size() - 9 - 12;
    if (file_name[start - 1] != '_') return false;
    for (size_t i = 0; i < 12; ++i) {
        const char c = file_name[start + i];
        const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!ok) return false;
    }
    out = file_name.substr(start, 12);
    return true;
}

// ---- 模块 sidecar 容器（MSAV）最小操作 ----

namespace {
// 与 ModuleSaveStore 一致的最小容器大小：magic(4)+ver(2)+slot(1)+generation(8)+count(2)+crc(4)。
constexpr size_t kMinContainerBytes = 21;
constexpr uint32_t kContainerMagic = 0x4D534156u;  // "MSAV"
constexpr size_t kMaxContainerBytes = 4 * 1024 * 1024;
constexpr size_t kContainerSlotOffset = 6;
}  // namespace

bool module_container_valid(const std::vector<uint8_t>& bytes) {
    if (bytes.size() < kMinContainerBytes || bytes.size() > kMaxContainerBytes) return false;
    if (be32(bytes.data()) != kContainerMagic) return false;
    const size_t body = bytes.size() - 4;
    return crc32_ieee(bytes.data(), body) == be32(bytes.data() + body);
}

bool module_container_reslot(std::vector<uint8_t>& bytes, int slot) {
    if (slot < 0 || slot > 255) return false;
    if (!module_container_valid(bytes)) return false;
    bytes[kContainerSlotOffset] = static_cast<uint8_t>(slot);
    const size_t body = bytes.size() - 4;
    const uint32_t crc = crc32_ieee(bytes.data(), body);
    bytes[body] = static_cast<uint8_t>(crc >> 24);
    bytes[body + 1] = static_cast<uint8_t>(crc >> 16);
    bytes[body + 2] = static_cast<uint8_t>(crc >> 8);
    bytes[body + 3] = static_cast<uint8_t>(crc);
    return true;
}

}  // namespace save_backup
