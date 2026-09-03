#include "game_tiles.h"

#include <android/log.h>

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>

#define TILES_TAG "Inotia4Tiles"
#define TILES_LOG(...) __android_log_print(ANDROID_LOG_INFO, TILES_TAG, __VA_ARGS__)

namespace {

struct StaticTileEntry {
    int width = 0;
    int height = 0;
    uint8_t tiles[STATIC_TILE_BYTES];
};

std::mutex g_tiles_mtx;
std::unordered_map<int, StaticTileEntry> g_tiles;
bool g_tiles_ready = false;

int b64_val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

bool base64_decode(const std::string& in, uint8_t* out, size_t out_cap) {
    size_t oi = 0;
    int buf = 0, bits = 0;
    for (char c : in) {
        if (c == '=' || c == '\n' || c == '\r') continue;
        int v = b64_val(c);
        if (v < 0) return false;
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (oi >= out_cap) return false;
            out[oi++] = static_cast<uint8_t>((buf >> bits) & 0xFF);
        }
    }
    return oi == out_cap;
}

int parse_int_field(const std::string& json, size_t pos, const char* key) {
    std::string pat = std::string("\"") + key + "\":";
    size_t k = json.find(pat, pos);
    if (k == std::string::npos) return 0;
    k += pat.size();
    while (k < json.size() && (json[k] == ' ' || json[k] == '\t')) ++k;
    return atoi(json.c_str() + k);
}

}

void set_static_tiles(const std::string& json) {
    std::lock_guard<std::mutex> lock(g_tiles_mtx);
    g_tiles.clear();
    g_tiles_ready = false;

    size_t pos = 0;
    int parsed = 0;
    while ((pos = json.find("\"m", pos)) != std::string::npos) {
        size_t colon = json.find("\":", pos + 2);
        if (colon == std::string::npos) break;
        std::string key = json.substr(pos + 2, colon - pos - 2);
        if (key.empty() || !(key[0] >= '0' && key[0] <= '9')) {
            pos = colon + 2;
            continue;
        }
        int map_id = atoi(key.c_str());

        size_t tpos = json.find("\"tiles\":\"", colon);
        if (tpos == std::string::npos) break;
        size_t tstart = tpos + 9;
        size_t tend = json.find('"', tstart);
        if (tend == std::string::npos) break;

        StaticTileEntry e;
        e.width = parse_int_field(json, colon, "width");
        e.height = parse_int_field(json, colon, "height");
        if (!base64_decode(json.substr(tstart, tend - tstart), e.tiles, STATIC_TILE_BYTES)) {
            TILES_LOG("m%d base64 decode failed (len=%zu)", map_id, tend - tstart);
            pos = tend + 1;
            continue;
        }
        g_tiles[map_id] = e;
        ++parsed;
        pos = tend + 1;
    }
    g_tiles_ready = parsed > 0;
    TILES_LOG("static tiles loaded: %d maps, ready=%d", parsed, g_tiles_ready ? 1 : 0);
}

const uint8_t* static_tiles_for(int map_id) {
    std::lock_guard<std::mutex> lock(g_tiles_mtx);
    auto it = g_tiles.find(map_id);
    if (it == g_tiles.end()) return nullptr;
    return it->second.tiles;
}

bool static_tiles_ready() {
    std::lock_guard<std::mutex> lock(g_tiles_mtx);
    return g_tiles_ready;
}

int static_tiles_width(int map_id) {
    std::lock_guard<std::mutex> lock(g_tiles_mtx);
    auto it = g_tiles.find(map_id);
    return it == g_tiles.end() ? 0 : it->second.width;
}

int static_tiles_height(int map_id) {
    std::lock_guard<std::mutex> lock(g_tiles_mtx);
    auto it = g_tiles.find(map_id);
    return it == g_tiles.end() ? 0 : it->second.height;
}
