// 日志域的唯一来源：枚举顺序与 token 都由本文件展开。
//
// 新增域只在这里加一行，并在 qol_log_domain_dirs.inc 加目录映射（若该域有插件目录）。
// 枚举顺序是对外契约的一部分：只追加、不插入、不重排。

QOL_LOG_DOMAIN(kPlatform, "platform")
QOL_LOG_DOMAIN(kCore, "core")
QOL_LOG_DOMAIN(kHttp, "http")
QOL_LOG_DOMAIN(kApi, "api")
QOL_LOG_DOMAIN(kOp, "op")
QOL_LOG_DOMAIN(kInventory, "inventory")
QOL_LOG_DOMAIN(kExtensionBag, "extension_bag")
QOL_LOG_DOMAIN(kSave, "save")
QOL_LOG_DOMAIN(kSaveBackup, "save_backup")
QOL_LOG_DOMAIN(kAutosell, "autosell")
QOL_LOG_DOMAIN(kCraft, "craft")
QOL_LOG_DOMAIN(kAttrRange, "attr_range")
QOL_LOG_DOMAIN(kUi, "ui")
QOL_LOG_DOMAIN(kConfig, "config")
QOL_LOG_DOMAIN(kCatalog, "catalog")
QOL_LOG_DOMAIN(kCustomRecipe, "custom_recipe")
QOL_LOG_DOMAIN(kWorldTuning, "world_tuning")
QOL_LOG_DOMAIN(kSpecialEquip, "special_equip")
