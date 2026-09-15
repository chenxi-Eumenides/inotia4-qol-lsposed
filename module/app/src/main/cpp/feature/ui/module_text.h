#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

// 模块自定义文本层（module-text）：让模块对**任意游戏 text id** 在**指定窗口**内提供自有文本。
//
// 为什么需要统一设施：
//  · `MEMORYTEXT_GetText` 是全游戏唯一的文本读取热点（232 个调用点），所有走文本表的 UI 文案
//    （按钮 / 标签 / 描述）都经它 —— 一处替换即可覆盖，且**不写任何游戏文本数据**
//    （MEMORYTEXT / *BASE 表全不动）。
//  · LSPosed NativeHook 对**同一地址二次挂载会失败**（rc=-1，真机实证：早期把 `UIDesc_MakeItem`
//    串进主链导致 5 个核心 hook 全未安装），因此该热点只能有**一个 owner** —— 本设施即是它。
//    其它功能不再各自挂 hook，而是把「文案 + 生效窗口」登记进下表。
//  · 游戏文本表里没有模块想要的串（如「宝石升阶」），且某些 id（如 35291）**同时被页签名与
//    按钮名复用**，所以替换必须带**窗口作用域**，否则会误改其它界面的同名文本。
//
// 能力边界：**只做读取侧替换**。不改控件文本缓冲、不写文本表、不新增端点。
//
// 本文件的表与查表逻辑是**纯逻辑**（不触任何游戏内存），编入 host 单测；
// hook 安装与作用域状态在 module_text.cpp。

namespace module_text {

// 文本作用域：替换只在「当前线程正处于该窗口」时生效。
// 新增作用域 = 在此追加 + 在 kEntries 中引用 + 由对应功能在自己的入口构造 TextScopeGuard。
enum class Scope : uint8_t {
    kNone = 0,
    kCharacterPanel,     // 角色属性面板绘制（Scene_Draw_POPUP_SC_CHARACTER_INFO）
    kUimixRecipeButton,  // UIMix 配方按钮绘制（UIMix_ButtonRecipeDraw）
    kUimixPanelTitle,    // UIMix 面板标题绘制（UIMix_Draw 内的「当前配方名」，读配方记录 b0-1）
    kUimixPageTab,       // UIMix 页签按钮绘制（UIMix_ButtonMenuListDraw）：**显式不替换** ——
                         // 页签名与配方名同 id（35291），必须保持游戏原文
};

constexpr const char* scope_token(Scope scope) {
    switch (scope) {
        case Scope::kNone:
            return "none";
        case Scope::kCharacterPanel:
            return "character_panel";
        case Scope::kUimixRecipeButton:
            return "uimix_recipe_button";
        case Scope::kUimixPanelTitle:
            return "uimix_panel_title";
        case Scope::kUimixPageTab:
            return "uimix_page_tab";
    }
    return "unknown";
}

struct Entry {
    const char* key;   // 语义键（唯一；下方 host test 断言唯一性）
    uint16_t text_id;  // 被替换的游戏 text id
    Scope scope;       // 生效窗口
    // nullptr = 明确保持游戏原文（占位用，维持连续 id 区间不被打破），不做替换。
    const char* text;
};

// ---- 内置文本表（唯一真源）----
//
// ① 角色面板战斗属性标签（kCharacterPanel）：游戏文本表漏翻了 11 个属性缩写。
//    35181 LV / 35182 EXP / 35183 HP / 35184 MP 按用户裁决**保持原文**；
//    35188 M. DEF 面板未显示，显式登记 nullptr 占位以维持 35185..35196 连续区间。
//    文案不超过四个汉字（用户裁决）：物攻 / 法攻 / 防御力 / 暴击率 / 命中率 / 爆伤 /
//    物抗 / 法抗 / 闪避率 / 武器格挡 / 盾牌格挡。35192 的含义按用户裁决为 P.RES = 物理抗性。
// ② UIMix 配方按钮（kUimixRecipeButton）：模块「宝石强化」条目改为显示「宝石升阶」。
//    该 id（35291）同时是宝石强化页的**页签名**，而页签由 UIMix_ButtonMenuListDraw 绘制、
//    不在本作用域内，故只有配方按钮改名（见 lookup 矩阵用例）。文案表里不存在这一串，
//    因此只能由模块提供。
inline constexpr Entry kEntries[] = {
    {"charinfo.dmg", 35185, Scope::kCharacterPanel, "物攻"},        // DMG   物理攻击力
    {"charinfo.magic_dmg", 35186, Scope::kCharacterPanel, "法攻"},  // M. DMG 魔法攻击力
    {"charinfo.def", 35187, Scope::kCharacterPanel, "防御力"},      // DEF   防御力
    {"charinfo.magic_def", 35188, Scope::kCharacterPanel, nullptr},  // M. DEF 面板未显示
    {"charinfo.crit_rate", 35189, Scope::kCharacterPanel, "暴击率"},  // CRT   暴击率
    {"charinfo.hit_rate", 35190, Scope::kCharacterPanel, "命中率"},   // H.RATE 命中率
    {"charinfo.crit_dmg", 35191, Scope::kCharacterPanel, "爆伤"},     // C.DMG 暴击伤害
    {"charinfo.phys_res", 35192, Scope::kCharacterPanel, "物抗"},     // P.RES 物理抗性
    {"charinfo.magic_res", 35193, Scope::kCharacterPanel, "法抗"},    // M. RES 魔法抗性
    {"charinfo.evade", 35194, Scope::kCharacterPanel, "闪避率"},      // EVD   闪避率
    {"charinfo.weapon_block", 35195, Scope::kCharacterPanel, "武器格挡"},  // W.D.R 武器格挡率
    {"charinfo.shield_block", 35196, Scope::kCharacterPanel, "盾牌格挡"},  // S.D.R 盾牌格挡率
    {"recipe.jewel_tier_up", 35291, Scope::kUimixRecipeButton, "宝石升阶"},
    // 面板标题（选中配方后标题框里的「当前配方名」）：与配方按钮同 id，但由 UIMix_Draw 绘制。
    {"recipe.jewel_tier_up.title", 35291, Scope::kUimixPanelTitle, "宝石升阶"},
};

inline constexpr size_t kEntryCount = sizeof(kEntries) / sizeof(kEntries[0]);

// 编译期保证角色面板段位于表首、连续升序且恰好覆盖 35185..35196（无空洞）：
// 该段的 id 与文案一一对应，是最容易改错的地方（历史上 35192 曾被误标为「毒抗」）。
inline constexpr uint16_t kCharinfoIdMin = 35185;
inline constexpr uint16_t kCharinfoIdMax = 35196;
inline constexpr size_t kCharinfoIdCount = static_cast<size_t>(kCharinfoIdMax - kCharinfoIdMin) + 1;
constexpr bool charinfo_block_ok() {
    for (size_t i = 0; i < kCharinfoIdCount; ++i) {
        if (kEntries[i].scope != Scope::kCharacterPanel) return false;
        if (kEntries[i].text_id != static_cast<uint16_t>(kCharinfoIdMin + i)) return false;
    }
    return kEntryCount > kCharinfoIdCount &&
           kEntries[kCharinfoIdCount].scope != Scope::kCharacterPanel;
}
static_assert(charinfo_block_ok(),
              "角色面板文本段必须位于表首并覆盖 35185..35196 连续升序、无空洞");

// 内置表访问；out_count 回传条目数。
inline const Entry* entries(size_t* out_count) {
    if (out_count != nullptr) *out_count = kEntryCount;
    return kEntries;
}

// 查表：text_id 命中、scope == active、且 text != nullptr 时返回字面量；否则 nullptr。
// 纯函数（active 由调用方从当前作用域栈传入）；零分配、无字符串比较、无字符串构造，
// 可安全用于每帧热点（未命中时只做一次 id 比较即返回）。
inline const char* lookup(uint16_t text_id, Scope active) {
    if (active == Scope::kNone) return nullptr;
    for (size_t i = 0; i < kEntryCount; ++i) {
        if (kEntries[i].text_id != text_id) continue;
        if (kEntries[i].scope != active) continue;
        return kEntries[i].text;  // 可能是 nullptr（显式保持原文）
    }
    return nullptr;
}

// 按键取条目（诊断 / host 断言）；未命中返回 nullptr。
inline const Entry* entry_by_key(const char* key) {
    if (key == nullptr) return nullptr;
    for (size_t i = 0; i < kEntryCount; ++i) {
        if (kEntries[i].key != nullptr && std::strcmp(kEntries[i].key, key) == 0) {
            return &kEntries[i];
        }
    }
    return nullptr;
}

// 作用域守卫：在窗口入口构造、出口析构，自动恢复**上一层**作用域（保存/恢复而非置空，
// 因此对「原函数内部再次进入同一 wrapper」天然可重入）。
// 用法：
//   void draw_wrapper(void* ctrl) {
//       module_text::TextScopeGuard scope(module_text::Scope::kUimixRecipeButton);
//       if (backup != nullptr) backup(ctrl);
//   }
class TextScopeGuard {
public:
    explicit TextScopeGuard(Scope scope);
    ~TextScopeGuard();
    TextScopeGuard(const TextScopeGuard&) = delete;
    TextScopeGuard& operator=(const TextScopeGuard&) = delete;

private:
    Scope previous_;
};

// 安装 `MEMORYTEXT_GetText` 单一 hook（幂等；bridge 未就绪时静默返回，可后续重试）。
// 调用时机：nativeInit 在 bridge_init 成功后，且早于任何会绘制文案的功能安装。
void module_text_install_if_ready();

// 是否安装成功（诊断用）；未安装时 lookup 永不生效，全部退化为游戏原文。
bool module_text_installed();

}  // namespace module_text
