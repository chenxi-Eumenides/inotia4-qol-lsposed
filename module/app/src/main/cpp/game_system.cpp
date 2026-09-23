// game_system.cpp —— 系统聚合域：帧计数 + 初始化报告 + 事件流（唯一允许 include 其他域头的聚合域）（parse 域）
// 由 game_misc.cpp 拆分生成（纯搬代码，零逻辑变更）。

#include "game_system.h"

#include "game/access/game_access.h"
#include "game/state/game_state.h"
#include "game/state/system_snapshot.h"
#include "game_world.h"
#include "api/native/game_ui.h"
#include "api/native/game_save.h"
#include "core/native/game_cache.h"
#include "game_dialog.h"
#include "game/json/game_json.h"

#include <mutex>

// 事件流基线（审计 H4 修复：diff 全程锁保护）
std::mutex g_events_mtx;
bool g_events_has_last = false;
Snapshot g_events_last;

int64_t data_frame_count() {
    if (g_base == 0) return -1;
    // 帧计数 GOT 槽（G_FRAME_COUNT_VMA）：先解引用取 u64 指针，再读计数
    uintptr_t* slot = reinterpret_cast<uintptr_t*>(g_base + G_FRAME_COUNT_VMA);
    uint64_t* cnt = reinterpret_cast<uint64_t*>(*slot);
    return cnt != nullptr ? static_cast<int64_t>(*cnt) : -1;
}

std::string data_init_report() {
    std::string s = "{";
    bool first = true;
    for (const auto& item : g_symbol_report) {
        if (!first) s += ",";
        s += "\"" + std::string(item.first) + "\":" + (item.second ? "true" : "false");
        first = false;
    }
    if (!g_dl_error.empty()) {
        s += ",\"error\":\"" + g_dl_error + "\"";
    }
    if (!g_lib_path.empty()) {
        s += ",\"path\":\"" + g_lib_path + "\"";
    }
    s += "}";
    return s;
}

namespace {

Snapshot take_snapshot() {
    Snapshot s{};
    s.money = (fn_get_money != nullptr) ? fn_get_money() : -1;
    s.x = s.y = -1;
    s.inv_count = inventory_count();
    for (int i = 0; i < 3; ++i) {
        void* ch = (fn_get_member != nullptr) ? fn_get_member(i) : nullptr;
        s.hp[i] = s.mp[i] = s.level[i] = -1;
        s.exp[i] = -1;
        if (ch != nullptr) {
            uint8_t* b = reinterpret_cast<uint8_t*>(ch);
            s.hp[i] = *reinterpret_cast<int32_t*>(b + C_HP);
            s.mp[i] = *reinterpret_cast<int32_t*>(b + C_MP);
            s.level[i] = reinterpret_cast<int8_t*>(ch)[C_LEVEL];
            s.exp[i] = (fn_get_exp != nullptr) ? fn_get_exp(ch) : -1;
        }
    }
    void* lead = (fn_get_member != nullptr) ? fn_get_member(0) : nullptr;
    if (lead != nullptr) {
        uint8_t* b = reinterpret_cast<uint8_t*>(lead);
        s.x = *reinterpret_cast<int16_t*>(b + C_POS_X);
        s.y = *reinterpret_cast<int16_t*>(b + C_POS_Y);
    }
    return s;
}

void emit(std::string& out, bool& first, const char* type, int role, int64_t a, int64_t b) {
    if (!first) out += ",";
    first = false;
    out += "{\"type\":\"" + std::string(type) + "\"";
    if (role >= 0) out += ",\"role\":" + std::to_string(role);
    out += ",\"old\":" + std::to_string(a);
    out += ",\"new\":" + std::to_string(b);
    out += "}";
}

}  // namespace

std::string data_events_json() {
    Snapshot cur = take_snapshot();
    std::lock_guard<std::mutex> lock(g_events_mtx);
    static Snapshot last;
    static bool has_last = false;
    std::string s = "{\"events\":[";
    bool first = true;
    if (!has_last) {
        has_last = true;
        last = cur;
        s += "]}";
        return s;
    }
    if (cur.money >= 0 && last.money >= 0 && cur.money != last.money)
        emit(s, first, "money", -1, last.money, cur.money);
    if (cur.inv_count >= 0 && last.inv_count >= 0 && cur.inv_count != last.inv_count)
        emit(s, first, "inventory", -1, last.inv_count, cur.inv_count);
    if (cur.x >= 0 && last.x >= 0 && (cur.x != last.x || cur.y != last.y))
        emit(s, first, "move", -1, 0, 0);
    for (int i = 0; i < 3; ++i) {
        if (cur.hp[i] >= 0 && last.hp[i] >= 0 && cur.hp[i] != last.hp[i])
            emit(s, first, "hp", i, last.hp[i], cur.hp[i]);
        if (cur.mp[i] >= 0 && last.mp[i] >= 0 && cur.mp[i] != last.mp[i])
            emit(s, first, "mp", i, last.mp[i], cur.mp[i]);
        if (cur.level[i] >= 0 && last.level[i] >= 0 && cur.level[i] != last.level[i])
            emit(s, first, "level_up", i, last.level[i], cur.level[i]);
        if (cur.exp[i] >= 0 && last.exp[i] >= 0 && cur.exp[i] != last.exp[i])
            emit(s, first, "exp", i, last.exp[i], cur.exp[i]);
    }
    last = cur;
    s += "]}";
    return s;
}

std::string build_gamestate_json() {
    bool story_active = data_story_active();

    // v0.5.42：screen 统一判定（data_ui_screen：主菜单细分/教学/弹窗/剧情/对话框/面板），
    // 完全基于 popup 栈顶 + 状态机，替代旧内联判定与 dialog_active 布尔（数据残留误报修复）。
    const char* screen = data_ui_screen();

    // 帧计数：FPS 系统每帧 +1（0x3075f0 u64，FPS_getTotalFrameCount 官方读取；G_FRAME_COUNT_VMA 为实测启用源）
    uint64_t frame = 0;
    if (g_base != 0) {
        uintptr_t* slot = reinterpret_cast<uintptr_t*>(g_base + G_FRAME_COUNT_VMA);
        uint64_t* cnt = reinterpret_cast<uint64_t*>(*slot);
        if (cnt != nullptr) frame = *cnt;
    }

    std::string result = "{\"screen\":\"" + std::string(screen) + "\",\"frame\":" + std::to_string(frame);
    // v0.5.42：dialog_active 移除——screen 已精确表达 UI 占据（dialog_*/panel_* 前缀）
    // dialog 字段 = data_dialog_content_json 完整输出（popup/story/npc/wipeout/npc_quest/面板态 全类型，
    // 与 /api/ui/dialog 端点一致）
    std::string dcontent = data_dialog_content_json();
    if (dcontent.size() > 1 && dcontent[0] == '{') {
        result += ",\"dialog\":" + dcontent;
    }
    if (story_active) {
        result += ",\"story\":" + data_story_json();
    }
    result += "}";
    return result;
}

std::string build_snapshot_json() {
    std::string s = "{";

    // 帧计数：FPS 系统每帧 +1（0x3075f0 u64，FPS_getTotalFrameCount 官方读取；G_FRAME_COUNT_VMA 为实测启用源）
    uint64_t frame = 0;
    if (g_base != 0) {
        uintptr_t* slot = reinterpret_cast<uintptr_t*>(g_base + G_FRAME_COUNT_VMA);
        uint64_t* cnt = reinterpret_cast<uint64_t*>(*slot);
        if (cnt != nullptr) frame = *cnt;
    }
    s += "\"frame\":" + std::to_string(frame);

    // v0.5.42：snapshot 的 screen 与 /api/ui 统一（data_ui_screen 完整枚举），
    // 替代旧简化三态（main_menu/world/loading）——AI 可精确判断当前界面
    s += ",\"screen\":";
    s += "\"" + std::string(data_ui_screen()) + "\"";

    s += ",\"money\":" + std::to_string(fn_get_money != nullptr ? fn_get_money() : -1);
    s += ",\"map_id\":" + std::to_string(current_map_id());
    void* hero = lead_member();
    if (hero != nullptr) {
        // API 坐标统一瓦片（B 方案）：像素 >> 4；与 units/interactives/exits 同单位
        s += ",\"x\":" + std::to_string(*reinterpret_cast<int16_t*>(reinterpret_cast<uint8_t*>(hero) + C_POS_X) >> 4);
        s += ",\"y\":" + std::to_string(*reinterpret_cast<int16_t*>(reinterpret_cast<uint8_t*>(hero) + C_POS_Y) >> 4);
    } else {
        s += ",\"x\":-1,\"y\":-1";
    }
    s += ",\"leader_slot\":" + std::to_string(g_main_merc_slot != nullptr ? *reinterpret_cast<uint8_t*>(g_main_merc_slot) : -1);

    s += ",\"party\":[";
    for (int i = 0; i < 3; ++i) {
        void* ch = (fn_get_member != nullptr) ? fn_get_member(i) : nullptr;
        if (i > 0) s += ",";
        if (ch == nullptr) {
            s += "null";
            continue;
        }
        uint8_t* b = reinterpret_cast<uint8_t*>(ch);
        int ch_type = static_cast<int>(reinterpret_cast<int8_t*>(ch)[C_TYPE]);
        int level = static_cast<int>(b[C_LEVEL]);
        s += "{\"type\":" + std::to_string(ch_type);
        s += ",\"class_idx\":" + std::to_string(static_cast<int>(reinterpret_cast<int8_t*>(ch)[C_CLASS]));
        s += ",\"level\":" + std::to_string(level);
        s += ",\"hp\":" + std::to_string(*reinterpret_cast<int32_t*>(b + C_HP));
        s += ",\"mp\":" + std::to_string(*reinterpret_cast<int32_t*>(b + C_MP));
        // 直接读属性缓存字段（attr 0x1e/0x1f），不调用 CHAR_GetAttr：该函数 attr=0x1e 分支
        // 在 HP>maxHP 时会写回 HP，预取线程每帧调用会与主线程属性重算竞争钳低 HP。
        s += ",\"max_hp\":" + std::to_string(char_max_hp(ch));
        s += ",\"max_mp\":" + std::to_string(char_max_mp(ch));
        // main_stats 直读 Base+Main+Bonus+Sub（见 char_stat_total），避免 CHAR_GetStat 经
        // CHAR_GetStatSub 触发动态派生重算写回角色对象。
        s += ",\"main_stats\":[";
        for (int a = 0; a < 5; ++a) {
            if (a > 0) s += ",";
            s += std::to_string(char_stat_total(ch, a));
        }
        s += "]";
        // base_stats：CHAR_GetStatBase(0xdb9e4) 为纯读（add + ldrsb），保留函数调用。
        if (fn_get_stat_base != nullptr) {
            s += ",\"base_stats\":[";
            for (int a = 0; a < 5; ++a) {
                if (a > 0) s += ",";
                s += std::to_string(fn_get_stat_base(ch, a));
            }
            s += "]";
        }
        if (fn_get_name != nullptr) {
            char* nm = fn_get_name(ch);
            s += ",\"name\":\"" + json_escape(nm) + "\"";
        }
        s += "}";
    }
    s += "]";

    s += "}";
    return s;
}

namespace {

// 与 StaticData.className（CHARCLASSBASE 记录 0-5）同源；save_backup 的
// save_backup_class_name 另有同表副本（core 不得依赖插件）。仅用于 /api/system/snapshot
// 的 class_name，迁移前由 Kotlin NameInjector.injectClassName 注入。
const char* const kSnapshotClassNames[6] = {"黑暗骑士", "忍者", "黑魔导", "祭司",
                                            "暗影猎手", "狂战士"};
const char* const kSnapshotStatNames[5] = {"力量", "敏捷", "体力", "智力", "精力"};

void append_main_stats(std::string& out, void* ch) {
    out += "[";
    for (int i = 0; i < 5; ++i) {
        if (i > 0) out += ",";
        const int total = char_stat_total(ch, i);
        const int base = fn_get_stat_base != nullptr ? fn_get_stat_base(ch, i) : 0;
        out += "{\"stat_name\":\"";
        out += kSnapshotStatNames[i];
        out += "\",\"base_stat\":";
        out += std::to_string(base);
        out += ",\"additional_stat\":";
        out += std::to_string(total - base);
        out += "}";
    }
    out += "]";
}

// restructureRole 后的成员字段顺序：name/type_name/class_name/level/hp/max_hp/mp/max_mp/
// type/class_idx/main_stats（快照无 exp_next，main_stats 追加到末尾）。
void append_party_member_enriched(std::string& out, void* ch) {
    const uint8_t* b = reinterpret_cast<const uint8_t*>(ch);    const int type = static_cast<int>(reinterpret_cast<const int8_t*>(ch)[C_TYPE]);
    const int class_idx = static_cast<int>(reinterpret_cast<const int8_t*>(ch)[C_CLASS]);
    const int level = static_cast<int>(b[C_LEVEL]);
    out += "{";
    bool first = true;
    auto append = [&out, &first](const std::string& field) {
        if (!first) out += ",";
        out += field;
        first = false;
    };
    if (fn_get_name != nullptr) {
        char* nm = fn_get_name(ch);
        append("\"name\":\"" + json_escape(nm) + "\"");
    }
    if (type == 0) append("\"type_name\":\"主角\"");
    else if (type == 1) append("\"type_name\":\"佣兵\"");
    if (class_idx >= 0 && class_idx <= 5) {
        append(std::string("\"class_name\":\"") + kSnapshotClassNames[class_idx] + "\"");
    }
    append("\"level\":" + std::to_string(level));
    append("\"hp\":" + std::to_string(*reinterpret_cast<const int32_t*>(b + C_HP)));
    append("\"max_hp\":" + std::to_string(char_max_hp(ch)));
    append("\"mp\":" + std::to_string(*reinterpret_cast<const int32_t*>(b + C_MP)));
    append("\"max_mp\":" + std::to_string(char_max_mp(ch)));
    append("\"type\":" + std::to_string(type));
    append("\"class_idx\":" + std::to_string(class_idx));
    std::string stats;
    append_main_stats(stats, ch);
    append("\"main_stats\":" + stats);
    out += "}";
}

}  // namespace

std::string build_snapshot_enriched_json() {
    // 帧计数口径与 build_snapshot_json 一致（g_base 未就绪时为 0，不用 data_frame_count 的 -1）。
    uint64_t frame = 0;
    if (g_base != 0) {
        uintptr_t* slot = reinterpret_cast<uintptr_t*>(g_base + G_FRAME_COUNT_VMA);
        uint64_t* cnt = reinterpret_cast<uint64_t*>(*slot);
        if (cnt != nullptr) frame = *cnt;
    }
    std::string s = "{\"frame\":" + std::to_string(frame);
    s += ",\"screen\":";
    s += "\"" + std::string(data_ui_screen()) + "\"";
    s += ",\"money\":" + std::to_string(fn_get_money != nullptr ? fn_get_money() : -1);
    s += ",\"map_id\":" + std::to_string(current_map_id());
    void* hero = lead_member();
    if (hero != nullptr) {
        s += ",\"x\":" + std::to_string(*reinterpret_cast<int16_t*>(reinterpret_cast<uint8_t*>(hero) + C_POS_X) >> 4);
        s += ",\"y\":" + std::to_string(*reinterpret_cast<int16_t*>(reinterpret_cast<uint8_t*>(hero) + C_POS_Y) >> 4);
    } else {
        s += ",\"x\":-1,\"y\":-1";
    }
    s += ",\"leader_slot\":" + std::to_string(g_main_merc_slot != nullptr ? *reinterpret_cast<uint8_t*>(g_main_merc_slot) : -1);

    s += ",\"party\":[";
    for (int i = 0; i < 3; ++i) {
        void* ch = (fn_get_member != nullptr) ? fn_get_member(i) : nullptr;
        if (i > 0) s += ",";
        if (ch == nullptr) {
            s += "null";
            continue;
        }
        append_party_member_enriched(s, ch);
    }
    s += "]";

    s += "}";
    return s;
}

namespace {

// data_save_slots_json() 固定输出 {"slots":[...]}；info 只嵌入 slots 数组，与迁移前
// Kotlin JsonUtil.parseObj(...).opt("slots") 同值。
std::string system_save_slots_array() {
    const std::string json = data_save_slots_json();
    const size_t begin = json.find('[');
    const size_t end = json.rfind(']');
    if (begin == std::string::npos || end == std::string::npos || end < begin) return "[]";
    return json.substr(begin, end - begin + 1);
}

}  // namespace

namespace qol::game {

std::string build_system_snapshot_json() { return build_snapshot_enriched_json(); }

std::string build_system_game_json(const std::string& info_json) {
    return "{\"snapshot\":" + data_snapshot_json() + ",\"info\":" + info_json + "}";
}

std::string build_system_info_json(const std::string& version, const std::string& package) {
    std::string s = "{\"version\":\"" + json_escape(version.c_str()) + "\"";
    s += ",\"save_slots\":" + system_save_slots_array();
    s += ",\"current_save_slot\":" + std::to_string(current_save_slot());
    s += ",\"package_name\":\"" + json_escape(package.c_str()) + "\"";
    s += ",\"base\":" + std::to_string(g_base);
    s += "}";
    return s;
}

std::string build_system_frame_json() {
    return "{\"frame\":" + std::to_string(data_frame_count()) + "}";
}

}  // namespace qol::game
