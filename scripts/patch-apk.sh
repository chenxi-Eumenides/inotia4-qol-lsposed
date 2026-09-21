#!/usr/bin/env bash
set -euo pipefail

usage() {
    local exit_code=${1:-2}
    cat >&2 <<'EOF'
用法：
  scripts/patch-apk.sh [选项] <模块 APK> [新包名]

行为：
  对 apk/game-apk/ 下的每个游戏 APK 生成一个集成 APK，输出到
  output/<发布前缀>_lspatched[_v<模块版本>].apk。
  发布前缀全 ASCII：Inotia4_v<游戏版本>_monster[_<子版本>] / _original / _overhaul[_<日期>]。
  模块版本从 release 模块 APK 文件名 `..._vX.Y.Z.apk` 提取；debug 包无版本则不追加。
  - 保留「新包名」位置参数解析，但 LSPatch 不支持该参数，传入后会报错退出。
  - 默认覆盖已有输出（--force）；生成前先清理 output/ 下旧的 LSPatch 产物（*lspatch*.apk）。
  - 默认用 AOSP 公开 testkey（scripts/keys/aosp-testkey.bks）签名输出，使集成版与
    同样用该 testkey 签名的游戏改版证书身份一致；原版游戏签名身份仍会不同。

默认值：
  LSPatch JAR：tools/lspatch/lspatch-v1.2-487-release.jar

选项：
  --lspatch-jar PATH  使用指定版本的 LSPatch JAR
  --debuggable        将输出 APK 标记为 debuggable
  --sigbypasslv N     设置 signature bypass level（0/1/2/3）
  -h, --help          显示帮助
EOF
    exit "$exit_code"
}

repo_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
lspatch_jar=${LSPATCH_JAR:-"$repo_root/tools/lspatch/lspatch-v1.2-487-release.jar"}

# 默认签名证书：AOSP 公开 testkey（CN=Android, android@android.com），
# 令集成版与同样使用该 testkey 签名的游戏改版证书身份一致，可覆盖安装。
# 原版游戏由 Com2us 私钥签名，私钥不可得，输出签名身份仍会与原版不同。
aosp_keystore="$repo_root/scripts/keys/aosp-testkey.bks"
aosp_storepass=123456
aosp_alias=testkey
aosp_keypass=123456

debuggable=false
sigbypasslv=
positional=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --lspatch-jar)
            [[ $# -ge 2 ]] || usage
            lspatch_jar=$2
            shift 2
            ;;
        --debuggable)
            debuggable=true
            shift
            ;;
        --sigbypasslv)
            [[ $# -ge 2 ]] || usage
            sigbypasslv=$2
            shift 2
            ;;
        -h|--help)
            usage 0
            ;;
        --)
            shift
            positional+=("$@")
            break
            ;;
        -* )
            printf '未知选项：%s\n' "$1" >&2
            usage
            ;;
        *)
            positional+=("$1")
            shift
            ;;
    esac
done

# 位置参数：<模块 APK> [新包名]
if [[ ${#positional[@]} -lt 1 || ${#positional[@]} -gt 2 ]]; then
    usage
fi
module_apk=${positional[0]}
newpackage=
if [[ ${#positional[@]} -eq 2 ]]; then
    newpackage=${positional[1]}
fi

if [[ -n "$newpackage" ]]; then
    printf 'LSPatch 不支持新包名参数：%s\n' "$newpackage" >&2
    exit 2
fi

if [[ ! -f "$module_apk" ]]; then
    printf '模块 APK 不存在：%s\n' "$module_apk" >&2
    exit 1
fi
module_apk=$(realpath "$module_apk")

# 模块版本：从 release 模块 APK 文件名（inotia4_qol_lsposed_release_vX.Y.Z.apk）提取，
# 追加在 _lspatched 之后；debug 包文件名无版本，则不追加，输出仍为 <发布前缀>_lspatched.apk。
module_version=
if [[ "$(basename "$module_apk")" =~ _v([0-9]+\.[0-9]+\.[0-9]+)\.apk$ ]]; then
    module_version="v${BASH_REMATCH[1]}"
fi

if [[ ! -f "$lspatch_jar" ]]; then
    printf 'LSPatch JAR 不存在：%s\n' "$lspatch_jar" >&2
    printf '可用 --lspatch-jar 指定下载的版本。\n' >&2
    exit 1
fi
if [[ ! -f "$aosp_keystore" ]]; then
    printf 'AOSP testkey keystore 不存在：%s\n' "$aosp_keystore" >&2
    printf '生成步骤见 docs/guides/build-and-deploy.md §1。\n' >&2
    exit 1
fi
if ! command -v java >/dev/null 2>&1; then
    printf '未找到 java；LSPatch CLI 需要 Java。\n' >&2
    exit 1
fi
if [[ -n "$sigbypasslv" && ! "$sigbypasslv" =~ ^[0-3]$ ]]; then
    printf 'signature bypass level 必须是 0、1、2 或 3：%s\n' "$sigbypasslv" >&2
    exit 1
fi

game_dir="$repo_root/apk/game-apk"
shopt -s nullglob
game_apks=("$game_dir"/*.apk)
shopt -u nullglob
if [[ ${#game_apks[@]} -eq 0 ]]; then
    printf 'apk/game-apk/ 下未找到任何 .apk\n' >&2
    exit 1
fi

# 发布前缀：游戏 APK 文件名 -> 全 ASCII 前缀（发布附件名不得含中文）。
#   艾诺迪亚4_v1.3.2_原版.apk              -> Inotia4_v1.3.2_original
#   艾诺迪亚4_v1.3.2_盗版大修_20260810.apk -> Inotia4_v1.3.2_overhaul_20260810
#   Inotia4_v1.3.2_monster_v25.apk        -> Inotia4_v1.3.2_monster_v25
release_stem() {
    local stem=$1 game_version= date=
    if [[ "$stem" =~ (v[0-9]+(\.[0-9]+)+) ]]; then
        game_version=${BASH_REMATCH[1]}
    fi
    case "$stem" in
        *monster*) printf '%s' "$stem" ;;
        *原版*) printf 'Inotia4_%s_original' "${game_version:-v1.3.2}" ;;
        *盗版大修*)
            if [[ "$stem" =~ (20[0-9]{6}) ]]; then
                date=${BASH_REMATCH[1]}
            fi
            printf 'Inotia4_%s_overhaul%s' "${game_version:-v1.3.2}" "${date:+_$date}" ;;
        *) printf '%s' "$stem" ;;
    esac
}

# 清理旧 LSPatch 产物（一次）
shopt -s nullglob
old_lspatch_apks=("$repo_root/output/"*lspatch*.apk)
shopt -u nullglob
for old_apk in "${old_lspatch_apks[@]}"; do
    rm -f "$old_apk"
    printf '已删除旧 LSPatch 输出：%s\n' "$(basename "$old_apk")"
done

printf '模块 APK：%s\n' "$module_apk"
printf 'LSPatch：%s\n' "$lspatch_jar"
printf '游戏 APK 数量：%d\n' "${#game_apks[@]}"
[[ -n "$module_version" ]] && printf '模块版本：%s\n' "$module_version"

failed=0
generated=()
for target_apk in "${game_apks[@]}"; do
    target_name=$(basename "$target_apk")
    target_stem=${target_name%.apk}
    output_apk="$repo_root/output/$(release_stem "$target_stem")_lspatched${module_version:+_$module_version}.apk"
    work_dir=$(mktemp -d "$repo_root/.tmp/lspatch-apk.XXXXXX")

    printf '\n===== 处理：%s\n' "$target_name"

    args=(java -jar "$lspatch_jar" -m "$module_apk" -o "$work_dir"
          -k "$aosp_keystore" "$aosp_storepass" "$aosp_alias" "$aosp_keypass")
    [[ -n "$sigbypasslv" ]] && args+=(--sigbypasslv "$sigbypasslv")
    [[ "$debuggable" == true ]] && args+=(--debuggable)
    args+=(--force "$target_apk")

    if ! "${args[@]}"; then
        printf '集成失败：%s\n' "$target_name" >&2
        failed=1
        rm -rf "$work_dir"
        continue
    fi

    shopt -s nullglob
    patched_apks=("$work_dir"/*-lspatched.apk)
    shopt -u nullglob
    if [[ ${#patched_apks[@]} -ne 1 || ! -s "${patched_apks[0]}" ]]; then
        printf '输出 APK 数量异常或无效：%d（%s）\n' "${#patched_apks[@]}" "$target_name" >&2
        failed=1
        rm -rf "$work_dir"
        continue
    fi

    final_apk=${patched_apks[0]}
    mv -f "$final_apk" "$output_apk"
    printf '生成集成 APK：%s\n' "$output_apk"
    sha256sum "$output_apk"
    generated+=("$output_apk")
    rm -rf "$work_dir"
done

printf '\n===== 汇总：成功 %d / 失败 %d\n' "${#generated[@]}" "$failed"
for apk in "${generated[@]}"; do
    printf '  %s\n' "$apk"
done
exit "$failed"
