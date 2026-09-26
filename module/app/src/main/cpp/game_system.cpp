// game_system.cpp —— 系统聚合域：帧计数 + 初始化报告 + 事件流（唯一允许 include 其他域头的聚合域）（parse 域）
// 由 game_misc.cpp 拆分生成（纯搬代码，零逻辑变更）。

#include "game_system.h"

#include "game/access/game_access.h"
#include "game/adapter_ports/item_port.h"
#include "game/state/character_http.h"
#include "game/state/choice_http.h"
#include "game/state/game_state.h"
#include "game/state/quest_http.h"
#include "game/state/save_http.h"
#include "game/state/system_snapshot.h"
#include "game/state/ui_http.h"
#include "game/state/world_http.h"
#include "game_world.h"
#include "api/native/game_character.h"
#include "api/native/game_inventory.h"
#include "api/native/game_party.h"
#include "api/native/game_quest.h"
#include "api/native/game_shop.h"
#include "api/native/game_ui.h"
#include "api/native/game_save.h"
#include "core/native/game_cache.h"
#include "game_dialog.h"
#include "game/json/game_json.h"
#include "contracts/game_ports/save_write_port.h"
#include "contracts/game_ports/choice_write_port.h"
#include "contracts/game_ports/ui_write_port.h"
#include "contracts/game_ports/world_write_port.h"
#include "infra/store.h"

#include <mutex>
#include <utility>

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

// 退出任务的结果拼装：成功时附上 player 状态对象（复刻迁移前 Kotlin
// ActionSupport.attachPlayer(NativeBridge.nativeOpQuestQuit(id)) 的响应）；失败体原样返回。
std::string build_quest_quit_result_json(int32_t quest_id) {
    const std::string op = data_op_quest_quit(quest_id);
    if (op.find("\"ok\":true") == std::string::npos) return op;
    // 写成功路径 op_ok() 已同步力刷 player 槽，此处读 cache 即写后即时字节
    //（cache[player] 的 owner 适配器与 legacy 现构逐字节同源）。
    return "{\"ok\":true,\"state\":" + data_player_json() + "}";
}

// ---- core.character owner 响应体拼装（批 4-4）----
//
// 复刻迁移前 Kotlin NameInjectorCore 对 character 域原生 JSON 的加工：
// - party 成员：injectTypeName/injectClassName/injectEquipmentNames + restructureRole
// - 装备：injectItemName(item, true) 内的 restructureItem 字段重排
// - skills：injectSkillNames
// 直接读游戏内存按目标字段序输出，避免在 legacy 侧引入 JSON 解析器（插件不得依赖 infra）。

namespace {

const char* const kCharacterClassNames[6] = {"黑暗骑士", "忍者", "黑魔导", "祭司",
                                             "暗影猎手", "狂战士"};
const char* const kCharacterStatNames[5] = {"力量", "敏捷", "体力", "智力", "精力"};

// 启动期由 Kotlin 下发的静态文本表（与 item_facts 同源）；无记录返回 nullptr。
const char* character_static_text(contracts::QolItemStaticTextKind kind, int32_t id) {
    const contracts::QolItemPort& port = item_port();
    if (port.read.static_text == nullptr) return nullptr;
    const char* text = nullptr;
    if (!contracts::succeeded(port.read.static_text(static_cast<int32_t>(kind), id, &text))) {
        return nullptr;
    }
    return text;
}

// 迁移前 StaticData.rarityPrefix（raw_rarity → 品质前缀）。
const char* character_rarity_prefix(long long raw_rarity) {
    switch (raw_rarity) {
        case 0: return "生锈的 ";
        case 1: return "陈旧的 ";
        case 3: return "太古的 ";
        case 4: return "锐利的 ";
        case 5: return "打磨的 ";
        case 6: return "工匠的 ";
        case 7: return "钢铁 ";
        case 8: return "钛金 ";
        case 9: return "秘银 ";
        default: return "";
    }
}

// 迁移前 StaticData.rarityTierName（GetRarity 0-4 → 白/绿/蓝/黄/紫）。
const char* character_rarity_tier(long long rarity) {
    switch (rarity) {
        case 0: return "白";
        case 1: return "绿";
        case 2: return "蓝";
        case 3: return "黄";
        case 4: return "紫";
        default: return "";
    }
}

// magic_rate 显示 = 原始值/100（110 → 1.1）；org.json 对整数倍输出 "0"。
std::string character_magic_rate_text(long long value) {
    const long long integer = value / 100;
    const long long remainder = value % 100;
    if (remainder == 0) return std::to_string(integer);
    if (remainder % 10 == 0) {
        return std::to_string(integer) + "." + static_cast<char>('0' + remainder / 10);
    }
    std::string fraction = std::to_string(remainder);
    if (fraction.size() == 1) fraction = "0" + fraction;
    return std::to_string(integer) + "." + fraction;
}

// 迁移前 NameInjectorCore.buildOptionList：词缀链表按 type（0=词缀 1=宝石）过滤。
std::string character_option_list(void* item, int type) {
    std::string out = "[";
    bool first = true;
    uint8_t* node = *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(item) + I_OPTION_LIST);
    int count = 0;
    while (node != nullptr && count < 32) {
        const uint16_t raw = *reinterpret_cast<uint16_t*>(node + O_INDEX);
        const int node_type = (raw >> 13) & 0x7;
        const int id = raw & 0x7F;
        const int value = *reinterpret_cast<int16_t*>(node + O_VALUE);
        node = *reinterpret_cast<uint8_t**>(node + O_NEXT);
        ++count;
        if (node_type != type) continue;
        const char* name =
            character_static_text(contracts::QolItemStaticTextKind::kOptionName, id);
        if (!first) out += ",";
        first = false;
        out += "{\"id\":" + std::to_string(id) + ",\"name\":\"";
        out += name != nullptr ? name : "";
        out += "\",\"value\":" + std::to_string(value) + "}";
    }
    out += "]";
    return out;
}

// 迁移前 NameInjectorCore.restructureItem 的字段顺序与取值（入参为原生装备物品指针）。
void append_character_equipment_item(std::string& out, void* item, int slot) {
    const uint8_t* it = reinterpret_cast<const uint8_t*>(item);
    const uint16_t flags = *reinterpret_cast<const uint16_t*>(it + I_TYPE);
    const long long raw_rarity = (flags >> 2) & 0x0F;
    const bool has_category = fn_get_bit != nullptr;
    const long long category = has_category ? fn_get_bit(flags, 15, 6) : -1;
    const bool has_count = fn_get_cumulate_count != nullptr;
    const long long count = has_count ? fn_get_cumulate_count(item) : 0;
    const bool has_rarity = fn_get_rarity != nullptr;
    const long long rarity = has_rarity ? fn_get_rarity(item) : 0;
    const bool has_damage = fn_get_damage != nullptr;
    const long long damage = has_damage ? fn_get_damage(item) : 0;
    const bool has_defense = fn_get_defense != nullptr;
    const long long defense = has_defense ? fn_get_defense(item) : 0;
    const long long magic_rate = it[I_MAGIC_RATE];
    const long long need_level = fn_item_get_ability_level != nullptr
                                     ? fn_item_get_ability_level(item)
                                     : 0;
    const long long socket_total = (it[I_SOCKET] >> 4) & 0x0F;
    const uint16_t enchant = *reinterpret_cast<const uint16_t*>(it + I_ENCHANT);
    const bool chaos = (enchant & 1) != 0;
    const long long enchant_id = (enchant >> 11) & 0x1F;
    const long long enchant_level = (enchant >> 6) & 0x1F;
    const uint32_t count_bits = *reinterpret_cast<const uint32_t*>(it + I_COUNT);
    const long long chaos_level = (count_bits >> 0) & 0xFF;
    const long long chaos_rate = (count_bits >> 8) & 0xFF;

    const char* base_name =
        category >= 0 ? character_static_text(contracts::QolItemStaticTextKind::kItemName,
                                              static_cast<int32_t>(category))
                      : nullptr;

    out += "{";
    bool first = true;
    auto field = [&out, &first](const char* name) {
        if (!first) out += ",";
        first = false;
        out += "\"";
        out += name;
        out += "\":";
    };
    field("slot");
    out += std::to_string(slot);
    field("name");
    if (base_name != nullptr) {
        out += "\"" + std::string(character_rarity_prefix(raw_rarity)) + base_name + "\"";
    } else {
        out += "null";
    }
    field("category");
    if (has_category) out += std::to_string(category); else out += "null";
    field("count");
    if (has_count) out += std::to_string(count); else out += "null";
    field("item_type");
    {
        const char* type_text =
            category >= 0 ? character_static_text(contracts::QolItemStaticTextKind::kItemType,
                                                  static_cast<int32_t>(category))
                          : nullptr;
        out += "\"";
        out += type_text != nullptr ? type_text : "consumable";
        out += "\"";
    }
    field("need_level");
    out += std::to_string(need_level < 0 ? 0 : need_level);
    field("rarity");
    if (has_rarity) out += std::to_string(rarity); else out += "null";
    field("rarity_tier");
    if (has_rarity) {
        const char* tier = character_rarity_tier(rarity);
        if (tier[0] != '\0') {
            out += "\"";
            out += tier;
            out += "\"";
        } else {
            out += "null";
        }
    } else {
        out += "null";
    }
    field("base");
    out += "{\"damage\":";
    if (has_damage) out += std::to_string(damage); else out += "null";
    out += ",\"defense\":";
    if (has_defense) out += std::to_string(defense); else out += "null";
    out += ",\"magic_rate\":" + character_magic_rate_text(magic_rate) + "}";
    field("bonus");
    out += character_option_list(item, 0);
    field("gem");
    out += "{\"total_slots\":" + std::to_string(socket_total) +
           ",\"slots\":" + character_option_list(item, 1) + "}";
    field("chaos");
    out += "{\"is_chaos\":" + std::string(chaos ? "true" : "false");
    out += ",\"level\":" + std::to_string(chaos_level);
    out += ",\"rate\":" + std::to_string(chaos_rate) + "}";
    field("enchant");
    out += "{\"id\":" + std::to_string(enchant_id);
    out += ",\"level\":" + std::to_string(enchant_level);
    out += ",\"effect\":";
    if (enchant_id > 0) {
        const char* effect = character_static_text(contracts::QolItemStaticTextKind::kEnchantEffect,
                                                   static_cast<int32_t>(enchant_id));
        if (effect != nullptr) {
            out += "\"";
            out += effect;
            out += "\"";
        } else {
            out += "null";
        }
    } else {
        out += "null";
    }
    out += "}";
    out += "}";
}

// 迁移前 NameInjectorCore.restructureRole 的字段顺序（含类型/职业名与 main_stats 拆分）。
void append_character_role_enriched(std::string& out, void* ch) {
    const uint8_t* b = reinterpret_cast<const uint8_t*>(ch);
    const int type = static_cast<int>(reinterpret_cast<const int8_t*>(ch)[C_TYPE]);
    const int class_idx = static_cast<int>(reinterpret_cast<const int8_t*>(ch)[C_CLASS]);
    out += "{";
    bool first = true;
    auto field = [&out, &first](const char* name) {
        if (!first) out += ",";
        first = false;
        out += "\"";
        out += name;
        out += "\":";
    };
    if (fn_get_name != nullptr) {
        field("name");
        out += "\"" + json_escape(fn_get_name(ch)) + "\"";
    }
    if (type == 0) {
        field("type_name");
        out += "\"主角\"";
    } else if (type == 1) {
        field("type_name");
        out += "\"佣兵\"";
    }
    if (class_idx >= 0 && class_idx <= 5) {
        field("class_name");
        out += "\"" + std::string(kCharacterClassNames[class_idx]) + "\"";
    }
    field("level");
    out += std::to_string(static_cast<int>(b[C_LEVEL]));
    field("hp");
    out += std::to_string(*reinterpret_cast<const int32_t*>(b + C_HP));
    field("max_hp");
    out += std::to_string(char_max_hp(ch));
    field("mp");
    out += std::to_string(*reinterpret_cast<const int32_t*>(b + C_MP));
    field("max_mp");
    out += std::to_string(char_max_mp(ch));
    field("exp");
    out += std::to_string(*reinterpret_cast<const int32_t*>(b + C_EXP));
    field("exp_next");
    out += std::to_string(char_next_exp_cached(ch));
    // main_stats 紧跟 exp_next（restructureRole 语义），total=char_stat_total、base=fn_get_stat_base。
    field("main_stats");
    out += "[";
    for (int i = 0; i < 5; ++i) {
        if (i > 0) out += ",";
        const int total = char_stat_total(ch, i);
        const int base = fn_get_stat_base != nullptr ? fn_get_stat_base(ch, i) : 0;
        out += "{\"stat_name\":\"";
        out += kCharacterStatNames[i];
        out += "\",\"base_stat\":" + std::to_string(base);
        out += ",\"additional_stat\":" + std::to_string(total - base) + "}";
    }
    out += "]";
    if (fn_get_status_point != nullptr) {
        field("status_point");
        out += std::to_string(fn_get_status_point(ch));
    }
    field("equipment");
    out += "[";
    for (int slot = 0; slot < static_cast<int>(C_EQUIP_SLOTS); ++slot) {
        if (slot > 0) out += ",";
        void* item = fn_get_equip != nullptr ? fn_get_equip(ch, slot) : nullptr;
        if (item == nullptr) {
            out += "null";
        } else {
            append_character_equipment_item(out, item, slot);
        }
    }
    out += "]";
    field("type");
    out += std::to_string(type);
    field("class_idx");
    out += std::to_string(class_idx);
    field("name_id");
    out += std::to_string(*reinterpret_cast<const uint16_t*>(b + C_NAME_ID));
    field("stats");
    out += "{";
    for (int a = 0; a < 32; ++a) {
        if (a > 0) out += ",";
        out += "\"" + std::to_string(a) + "\":";
        out += std::to_string(*reinterpret_cast<const int32_t*>(b + C_ATTR + a * 4));
    }
    out += "}";
    out += "}";
}

}  // namespace

std::string build_character_party_json() {
    if (!game_in_world()) return "{\"error\":\"not in game\"}";
    std::string s = "[";
    for (int i = 0; i < 3; ++i) {
        if (i > 0) s += ",";
        void* ch = fn_get_member != nullptr ? fn_get_member(i) : nullptr;
        if (ch == nullptr) {
            s += "null";
            continue;
        }
        append_character_role_enriched(s, ch);
    }
    s += "]";
    return s;
}

std::string build_character_skills_json() {
    if (!game_in_world()) return "{\"error\":\"not in game\"}";
    std::string s = "[";
    for (int i = 0; i < 3; ++i) {
        if (i > 0) s += ",";
        void* ch = fn_get_member != nullptr ? fn_get_member(i) : nullptr;
        if (ch == nullptr) {
            s += "null";
            continue;
        }
        uint8_t* base_ch = reinterpret_cast<uint8_t*>(ch);
        s += "{\"role\":" + std::to_string(i);
        s += ",\"skills\":[";
        uint8_t* node = *reinterpret_cast<uint8_t**>(base_ch + C_SKILL_LIST);
        bool first = true;
        int count = 0;
        while (node != nullptr && count < 64) {
            const uint16_t action_id = *reinterpret_cast<uint16_t*>(node + S_ACTION_ID);
            if (!first) s += ",";
            first = false;
            s += "{\"action_id\":" + std::to_string(action_id);
            s += ",\"level\":" + std::to_string(node[S_LEVEL]);
            if (fn_get_act_max_level != nullptr) {
                s += ",\"max_level\":" + std::to_string(fn_get_act_max_level(ch, action_id));
            }
            const char* skill_name =
                character_static_text(contracts::QolItemStaticTextKind::kSkillName, action_id);
            if (skill_name != nullptr) {
                s += ",\"skill_name\":\"" + std::string(skill_name) + "\"";
            }
            s += "}";
            node = *reinterpret_cast<uint8_t**>(node + S_NEXT);
            ++count;
        }
        s += "]";
        s += ",\"unlock_bitmap\":" +
             std::to_string(*reinterpret_cast<uint16_t*>(base_ch + C_SKILL_BMP));
        uint8_t* active = *reinterpret_cast<uint8_t**>(base_ch + C_ACTIVE_SKILL);
        s += ",\"active_skill_id\":" +
             std::to_string(active != nullptr ? *reinterpret_cast<uint16_t*>(active + S_ACTION_ID)
                                              : -1);
        s += ",\"skill_points\":" +
             std::to_string(static_cast<int>(reinterpret_cast<int8_t*>(ch)[C_SKILL_POINTS]));
        s += "}";
    }
    s += "]";
    return s;
}

std::string build_character_mercenaries_json() { return data_mercenaries_json(); }

// 存档写（save/enter_slot/create_slot）：执行游戏写；成功时同步完成模块 sidecar 槽位生命周期
// 操作（原 Kotlin ModuleSaveStore.ensureSlot/resetSlot 随之在主线程下沉，保留"成功后"语义），
// 并附玩家状态构成迁移前响应（{"ok":true,"state":<player>}）。失败时输出原 op 错误体。
int32_t build_save_write_result_json(int32_t kind, int32_t slot, int32_t class_idx, uint32_t attempt,
                                     char* out, uint32_t capacity, uint32_t* out_size) {
    if (out == nullptr || out_size == nullptr) return kSaveWriteFailed;
    *out_size = 0;
    // 结果写出：未就绪不写内容（由调用方请求延迟重试）。
    const auto finish = [&](const std::string& body, int32_t outcome) {
        if (body.size() > capacity) return kSaveWriteFailed;
        std::memcpy(out, body.data(), body.size());
        *out_size = static_cast<uint32_t>(body.size());
        return outcome;
    };
    // sidecar（模块容器）：kUnavailable = 模块容器未启用（非游戏操作失败，按不适用处理）；
    // 其余非 kOk 视为失败并写入结果（迁移前被忽略的返回值现已核对）。
    const auto sidecar_failed = [](infra::store::Status s) {
        return s != infra::store::Status::kOk && s != infra::store::Status::kUnavailable;
    };
    const auto sidecar_error = [](infra::store::Status s) {
        return std::string("{\"ok\":false,\"error\":\"sidecar: ") + infra::store::status_name(s) +
               "\"}";
    };
    // 首次执行：发起游戏侧操作（enter/create 为转场内联执行，与同步入口共用单飞门）。
    if (attempt == 0) {
        std::string op_body;
        switch (kind) {
            case contracts::kQolSaveWriteSave:
                op_body = data_op_save();
                break;
            case contracts::kQolSaveWriteEnterSlot:
                op_body = data_op_enter_slot_direct(slot);
                break;
            case contracts::kQolSaveWriteCreateSlot:
                op_body = data_op_create_slot_direct(slot, class_idx);
                break;
            default:
                return kSaveWriteFailed;
        }
        if (op_body.find("\"ok\":false") != std::string::npos) {
            return finish(op_body, kSaveWriteFailed);
        }
    }
    // 转场后就绪等待：世界态提交完成前保持 pending，不提前报告 done（迁移前该等待由阻塞式
    // sidecar 文件 I/O 偶然提供，此处改为确定性条件）。
    if (kind != contracts::kQolSaveWriteSave && !game_in_world()) {
        return kSaveWriteNotReady;
    }
    // sidecar 槽位生命周期与游戏操作在同一步结算。
    if (kind == contracts::kQolSaveWriteSave) {
        const int current = current_save_slot();
        if (current >= 0 && current <= 2) {
            const infra::store::Status s = infra::store::ensure_slot(current);
            if (sidecar_failed(s)) return finish(sidecar_error(s), kSaveWriteFailed);
        }
    } else if (kind == contracts::kQolSaveWriteEnterSlot) {
        const infra::store::Status s = infra::store::ensure_slot(slot);
        if (sidecar_failed(s)) return finish(sidecar_error(s), kSaveWriteFailed);
    } else {
        const infra::store::Status s = infra::store::reset_slot(slot);
        if (sidecar_failed(s)) return finish(sidecar_error(s), kSaveWriteFailed);
    }
    // 与迁移前 ActionSupport.attachPlayer 同构：仅成功时附 state（键序 ok→state）。
    // 首个 attempt 的写路径 op_ok() 已同步力刷 player 槽；重试 attempt 仅等世界就绪，
    // 期间预取按帧维持 cache 新鲜，故读 cache 仍是写后即时字节。
    return finish("{\"ok\":true,\"state\":" + data_player_json() + "}", kSaveWriteOk);
}

// world_teleport owner 的世界写结果拼装：move_to/walk_dir/interact_with 迁移前经
// ActionSupport.attachPlayer 追加 state（仅成功时）；stop_move 与 teleport 输出原始 op JSON。
int32_t build_world_write_result_json(int32_t kind, int32_t a, int32_t b, int32_t c, int32_t d,
                                      char* out, uint32_t capacity, uint32_t* out_size) {
    std::string body;
    bool attach_state = false;
    switch (kind) {
        case contracts::kQolWorldWriteMoveTo:
            body = data_op_move(a, b);
            attach_state = true;
            break;
        case contracts::kQolWorldWriteWalkDir:
            body = data_op_walk(a);
            attach_state = true;
            break;
        case contracts::kQolWorldWriteStopMove:
            body = data_op_walk_stop();
            break;
        case contracts::kQolWorldWriteInteract:
            body = data_op_interact();
            attach_state = true;
            break;
        case contracts::kQolWorldWriteTeleport:
            body = data_op_teleport_direct(a, b, c, d != 0);
            break;
        default:
            return kWorldWriteFailed;
    }
    const bool ok = body.find("\"ok\":false") == std::string::npos;
    if (attach_state && ok && !body.empty() && body.back() == '}') {
        body.pop_back();
        body += ",\"state\":";
        // 写路径 op_ok() 已同步力刷 player 槽，读 cache 即写后即时字节。
        body += data_player_json();
        body += "}";
    }
    if (out_size != nullptr) *out_size = 0;
    if (out == nullptr || out_size == nullptr || body.size() > capacity) return kWorldWriteFailed;
    std::memcpy(out, body.data(), body.size());
    *out_size = static_cast<uint32_t>(body.size());
    return ok ? kWorldWriteOk : kWorldWriteFailed;
}

// core.ui_state owner 的对话写结果拼装：dialog/select 与 start_interact 迁移前均原样透传 op JSON
// （Kotlin 平台门禁分支留在传输层，不下沉）。
int32_t build_choice_write_result_json(int32_t kind, const char* action, int32_t index, char* out,
                                       uint32_t capacity, uint32_t* out_size) {
    std::string body;
    switch (kind) {
        case contracts::kQolChoiceWriteDialogSelect:
            body = data_op_dialog_select(action != nullptr ? std::string(action) : std::string(),
                                         index);
            break;
        case contracts::kQolChoiceWriteNpcInteract:
            body = data_op_npc_interact();
            break;
        default:
            return kChoiceWriteFailed;
    }
    const bool ok = body.find("\"ok\":false") == std::string::npos;
    if (out_size != nullptr) *out_size = 0;
    if (out == nullptr || out_size == nullptr || body.size() > capacity) return kChoiceWriteFailed;
    std::memcpy(out, body.data(), body.size());
    *out_size = static_cast<uint32_t>(body.size());
    return ok ? kChoiceWriteOk : kChoiceWriteFailed;
}

// core.ui owner 的面板写结果拼装：与迁移前 Kotlin ActionSupport.attachUi 同构——op 成功时把
// op JSON 末尾 "}" 替换为 ,"state":<gamestate JSON>}；失败原样透传错误体。
// gamestate 的平台 activity 覆盖仍留在 Kotlin 传输层（JVM-only），面板写要求已进 world，
// 该覆盖在此不生效。
int32_t build_ui_write_result_json(int32_t kind, const char* panel, char* out, uint32_t capacity,
                                  uint32_t* out_size) {
    if (out_size != nullptr) *out_size = 0;
    if (out == nullptr || out_size == nullptr) return kUiWriteFailed;
    std::string op;
    bool state_is_player = false;
    switch (kind) {
        case contracts::kQolUiWritePanelOpen:
            op = data_op_panel_open(panel != nullptr ? std::string(panel) : std::string());
            break;
        case contracts::kQolUiWritePanelClose:
            op = data_op_panel_close();
            break;
        case contracts::kQolUiWriteMainMenu:
            // owner 命令路径：同守卫与清理，状态机切换走逻辑帧内联（不跨帧同步等待）。
            op = data_op_main_menu_direct();
            state_is_player = true;
            break;
        default:
            return kUiWriteFailed;
    }
    const bool ok = op.find("\"ok\":false") == std::string::npos;
    std::string body = op;
    if (ok && !op.empty() && op.back() == '}') {
        // 写路径 op_ok() 已同步力刷 player 槽，读 cache 即写后即时字节。
        body = op.substr(0, op.size() - 1) + ",\"state\":" +
               (state_is_player ? data_player_json() : data_gamestate_json()) + "}";
    }
    if (body.size() > capacity) return kUiWriteFailed;
    std::memcpy(out, body.data(), body.size());
    *out_size = static_cast<uint32_t>(body.size());
    return ok ? kUiWriteOk : kUiWriteFailed;
}

}  // namespace qol::game
