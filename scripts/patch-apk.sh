#!/usr/bin/env bash
set -euo pipefail

usage() {
    local exit_code=${1:-2}
    cat >&2 <<'EOF'
用法：
  scripts/patch-apk.sh [选项] <模块 APK> [新包名]

行为：
  对 apk/game-apk/ 下的每个游戏 APK 生成一个集成 APK，输出到 output/。
  命名固定：原始 艾诺迪亚4_<name>_<version>.apk
            -> 集成 Inotia4_<name>_<version>_<debug|release>_<lspatch|npatch>.apk。
  <name> 变体映射：原版->Original、大修->Overhaul、monster->Monster、C18L->C18L、C18->C18；
  <version> 从原始 APK 文件名透传（如 v507、v29、v1.3.2）；
  <debug|release> 取模块 APK 文件名中的构建通道（..._debug_... / ..._release_...）；
  <lspatch|npatch> 取所用 JAR（JAR 文件名含 npatch 即 NPatch，否则 LSPatch；当前默认 LSPatch）。
  不符合固定命名的游戏 APK 直接失败。
  - 第二位置参数（新包名）不支持，传入会明确报错。
  - 默认覆盖已有输出（--force）；生成前先清理 output/ 下旧集成产物（*lspatch*.apk、*npatch*.apk）。

默认值：
  LSPatch JAR：tools/lspatch/lspatch-v1.2-487-release.jar

选项：
  --lspatch-jar PATH  使用指定版本的 LSPatch JAR
  --name NAME         设置 LSPatch 输出应用名称；未指定时按目标 APK 变体自动命名
  --debuggable        将输出 APK 标记为 debuggable
  --sigbypasslv N     设置 signature bypass level（0/1/2/3，默认 2）
  -h, --help          显示帮助
EOF
    exit "$exit_code"
}

repo_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
lspatch_jar=${LSPATCH_JAR:-"$repo_root/tools/lspatch/lspatch-v1.2-487-release.jar"}

debuggable=false
sigbypasslv=2
name_override=
positional=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --lspatch-jar)
            [[ $# -ge 2 ]] || usage
            lspatch_jar=$2
            shift 2
            ;;
        --name)
            [[ $# -ge 2 ]] || usage
            [[ -n "$2" ]] || { printf '%s\n' '--name 不能为空。' >&2; exit 2; }
            name_override=$2
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
if [[ ${#positional[@]} -eq 2 ]]; then
    printf '%s\n' 'LSPatch 不支持通过第二位置参数修改新包名。' >&2
    exit 2
fi

if [[ ! -f "$module_apk" ]]; then
    printf '模块 APK 不存在：%s\n' "$module_apk" >&2
    exit 1
fi
module_apk=$(realpath "$module_apk")

# 构建通道：从模块 APK 文件名（..._debug_<时间戳>_<hash>.apk / ..._release_vX.Y.Z.apk）识别，
# 作为输出文件名固定的 <debug|release> 段。
module_channel=
case "$(basename "$module_apk")" in
    *_debug_*) module_channel=debug ;;
    *_release_*) module_channel=release ;;
esac
if [[ -z "$module_channel" ]]; then
    printf '无法从模块 APK 文件名识别 debug/release 通道：%s\n' "$(basename "$module_apk")" >&2
    exit 1
fi

# 集成工具标识：JAR 文件名含 npatch 即 NPatch，否则 LSPatch；作为输出文件名固定的 <lspatch|npatch> 段。
patcher=lspatch
case "$(basename "$lspatch_jar")" in
    *npatch*) patcher=npatch ;;
esac

if [[ ! -f "$lspatch_jar" ]]; then
    printf 'LSPatch JAR 不存在：%s\n' "$lspatch_jar" >&2
    printf '可用 --lspatch-jar 指定下载的版本。\n' >&2
    exit 1
fi
if ! command -v java >/dev/null 2>&1; then
    printf '未找到 java；LSPatch CLI 需要 Java。\n' >&2
    exit 1
fi
if ! command -v keytool >/dev/null 2>&1; then
    printf '%s\n' '未找到 keytool；需要它将 BKS 临时转换为 PKCS12。' >&2
    exit 1
fi
if [[ -n "$sigbypasslv" && ! "$sigbypasslv" =~ ^[0-3]$ ]]; then
    printf 'signature bypass level 必须是 0、1、2 或 3：%s\n' "$sigbypasslv" >&2
    exit 1
fi

signing_bks="$repo_root/scripts/keys/aosp-testkey.bks"
if [[ ! -f "$signing_bks" ]]; then
    printf '签名 keystore 不存在：%s\n' "$signing_bks" >&2
    exit 1
fi
if ! mkdir -p "$repo_root/.tmp"; then
    printf '无法创建临时目录：%s\n' "$repo_root/.tmp" >&2
    exit 1
fi
signing_dir=$(mktemp -d "$repo_root/.tmp/patch-apk-signing.XXXXXX")
cleanup_signing_dir() {
    rm -rf "$signing_dir"
}
trap cleanup_signing_dir EXIT
signing_p12="$signing_dir/aosp-testkey.p12"
if ! keytool -importkeystore -noprompt \
    -srckeystore "$signing_bks" -srcstoretype BKS -srcstorepass 123456 \
    -srcalias testkey -srckeypass 123456 \
    -destkeystore "$signing_p12" -deststoretype PKCS12 -deststorepass 123456 \
    -destalias testkey -destkeypass 123456 \
    -providerclass org.bouncycastle.jce.provider.BouncyCastleProvider \
    -providerpath "$lspatch_jar"; then
    printf '签名 keystore 转换失败：无法使用 LSPatch JAR 内置 BouncyCastle provider 将 BKS 转为 PKCS12。\n' >&2
    printf '请确认 JAR 包含 BouncyCastle provider，且 keystore 密码、别名和 key 密码正确。\n' >&2
    exit 1
fi
if [[ ! -s "$signing_p12" ]]; then
    printf '签名 keystore 转换失败：未生成有效 PKCS12 文件：%s\n' "$signing_p12" >&2
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

if ! mkdir -p "$repo_root/output"; then
    printf '无法创建输出目录：%s\n' "$repo_root/output" >&2
    exit 1
fi

shopt -s nullglob
old_outputs=("$repo_root/output/"*lspatch*.apk "$repo_root/output/"*npatch*.apk)
if [[ ${#old_outputs[@]} -gt 0 ]]; then
    rm -f "${old_outputs[@]}"
fi
shopt -u nullglob

# 发布文件名：原始 APK 固定命名 艾诺迪亚4_<name>_<version>.apk，集成 APK 固定命名为
#   Inotia4_<name>_<version>_<debug|release>_<lspatch|npatch>.apk（后两段由调用方拼装）。
#   <name> 按变体映射（先判 C18L 再判 C18，避免子串误匹配）：
#     艾诺迪亚4_C18L_v507.apk     -> Inotia4_C18L_v507
#     艾诺迪亚4_C18_v0.2.apk      -> Inotia4_C18_v0.2
#     艾诺迪亚4_monster_v29.apk   -> Inotia4_Monster_v29
#     艾诺迪亚4_原版_v1.3.2.apk    -> Inotia4_Original_v1.3.2
#     艾诺迪亚4_盗版大修_v0810.apk -> Inotia4_Overhaul_v0810
#   <version> 取尾部 v 开头的版本号原样透传；变体未登记或无版本号则失败。
release_stem() {
    local stem=$1 name=
    case "$stem" in
        *C18L*) name=C18L ;;
        *C18*) name=C18 ;;
        *大修*) name=Overhaul ;;
        *原版*) name=Original ;;
        *[Mm][Oo][Nn][Ss][Tt][Ee][Rr]*) name=Monster ;;
        *)
            return 1
            ;;
    esac
    if [[ ! "$stem" =~ (v[0-9][0-9A-Za-z._-]*)$ ]]; then
        printf '无法从文件名提取版本号：%s\n' "$stem" >&2
        return 1
    fi
    printf 'Inotia4_%s_%s' "$name" "${BASH_REMATCH[1]}"
}

printf '模块 APK：%s\n' "$module_apk"
printf '模块通道：%s\n' "$module_channel"
printf '集成工具：%s\n' "$patcher"
printf 'LSPatch JAR：%s\n' "$lspatch_jar"
printf '游戏 APK 数量：%d\n' "${#game_apks[@]}"

failed=0
generated=()
for target_apk in "${game_apks[@]}"; do
    target_name=$(basename "$target_apk")
    target_stem=${target_name%.apk}
    if ! output_stem=$(release_stem "$target_stem"); then
        printf 'APK 命名不符合固定格式 艾诺迪亚4_<name>_<version>.apk 或变体未登记：%s\n' "$target_name" >&2
        failed=1
        continue
    fi
    output_apk="$repo_root/output/${output_stem}_${module_channel}_${patcher}.apk"
    work_dir=$(mktemp -d "$repo_root/.tmp/lspatch-apk.XXXXXX")

    if [[ -n "$name_override" ]]; then
        patch_name=$name_override
    else
        case "$target_name" in
            *[Mm][Oo][Nn][Ss][Tt][Ee][Rr]*) patch_name='艾4QoL(monster)' ;;
            *原版*) patch_name='艾4QoL(original)' ;;
            *盗版大修*) patch_name='艾4QoL(overhaul)' ;;
            *) patch_name='艾4QoL' ;;
        esac
    fi

    printf '\n===== 处理：%s\n' "$target_name"
    printf '应用名称：%s\n' "$patch_name"

    args=(java -jar "$lspatch_jar" -m "$module_apk" -o "$work_dir")
    args+=(--name "$patch_name")
    args+=(-k "$signing_p12" 123456 testkey 123456)
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
    patched_apks=("$work_dir"/*-"$patcher"ed.apk)
    shopt -u nullglob
    if [[ ${#patched_apks[@]} -ne 1 || ! -s "${patched_apks[0]}" ]]; then
        printf '输出 APK 数量异常或无效：%d（%s）\n' "${#patched_apks[@]}" "$target_name" >&2
        failed=1
        rm -rf "$work_dir"
        continue
    fi

    mv -f "${patched_apks[0]}" "$output_apk"
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
