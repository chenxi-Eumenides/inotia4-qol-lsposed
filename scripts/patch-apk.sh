#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat >&2 <<'EOF'
用法：
  scripts/patch-apk.sh [选项] <模块 APK> [新包名]

行为：
  对 apk/game-apk/ 下的每个游戏 APK 生成一个集成 APK，输出到
  output/<发布前缀>_npatched[_v<模块版本>].apk（LSPatch JAR 则为 _lspatched[_v<模块版本>]）。
  发布前缀全 ASCII：Inotia4_v<游戏版本>_monster[_<子版本>] / _original / _overhaul[_<日期>]。
  模块版本从 release 模块 APK 文件名 `..._vX.Y.Z.apk` 提取；debug 包无版本则不追加。
  - 提供「新包名」时用 NPatch --newpackage 修改输出 applicationId；不提供则保留原包名。
  - 默认覆盖已有输出（-f）；生成前先清理 output/ 下旧的 NPatch 产物（*npatch*.apk）。
  - 默认用 AOSP 公开 testkey（scripts/keys/aosp-testkey.bks）签名输出，使集成版与
    同样用该 testkey 签名的游戏改版证书身份一致；原版游戏签名身份仍会不同。

默认值：
  LSPatch JAR：tools/lspatch/npatch-v1.0.7-741-release.jar（NPatch）

选项：
  --lspatch-jar PATH  使用指定版本的 lspatch/npatch JAR
  --debuggable        将输出 APK 标记为 debuggable
  --sigbypasslv N     设置 signature bypass level（0/1/2/3）
  -h, --help          显示帮助
EOF
    exit 2
}

repo_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
lspatch_jar=${LSPATCH_JAR:-"$repo_root/tools/lspatch/npatch-v1.0.7-741-release.jar"}

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
            usage
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

if [[ ! -f "$module_apk" ]]; then
    printf '模块 APK 不存在：%s\n' "$module_apk" >&2
    exit 1
fi
module_apk=$(realpath "$module_apk")

# 模块版本：从 release 模块 APK 文件名（inotia4_qol_lsposed_release_vX.Y.Z.apk）提取，
# 追加在 _npatched 之后；debug 包文件名无版本，则不追加，输出仍为 <发布前缀>_npatched.apk。
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
if [[ -n "$newpackage" && ! "$newpackage" =~ ^[A-Za-z][A-Za-z0-9_]*(\.[A-Za-z][A-Za-z0-9_]*)+$ ]]; then
    printf '新包名不是合法 Android applicationId：%s\n' "$newpackage" >&2
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

is_npatch=false
if jar tf "$lspatch_jar" | grep -q '^top/nkbe/npatch/patch/NPatch.class$'; then
    is_npatch=true
fi
if [[ -n "$newpackage" && "$is_npatch" != true ]]; then
    printf '%s\n' '新包名仅支持 NPatch，不支持当前 LSPatch JAR。' >&2
    exit 1
fi
patch_suffix=npatched
[[ "$is_npatch" != true ]] && patch_suffix=lspatched

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

# 清理旧 NPatch 产物（一次）
shopt -s nullglob
old_npatch_apks=("$repo_root/output/"*npatch*.apk)
shopt -u nullglob
for old_apk in "${old_npatch_apks[@]}"; do
    rm -f "$old_apk"
    printf '已删除旧 NPatch 输出：%s\n' "$(basename "$old_apk")"
done

# --newpackage 后处理：把 AOSP BKS keystore 转为 apksigner 可读的 PKCS12
apksigner=
sign_keystore=
prep_dir=
if [[ -n "$newpackage" ]]; then
    apksigner=$(command -v apksigner || true)
    if [[ -z "$apksigner" && -x /opt/android-sdk/build-tools/37.0.0/apksigner ]]; then
        apksigner=/opt/android-sdk/build-tools/37.0.0/apksigner
    fi
    if [[ -z "$apksigner" ]]; then
        printf '%s\n' '未找到 apksigner，无法为 Manifest 后处理后的 NPatch APK 重签名。' >&2
        exit 1
    fi
    prep_dir=$(mktemp -d "$repo_root/.tmp/lspatch-key.XXXXXX")
    trap '[[ -n "$prep_dir" ]] && rm -rf "$prep_dir"' EXIT
    sign_keystore="$prep_dir/aosp-testkey.p12"
    keytool -importkeystore -noprompt \
        -srckeystore "$aosp_keystore" -srcstoretype BKS \
        -srcstorepass "$aosp_storepass" -srcalias "$aosp_alias" -srckeypass "$aosp_keypass" \
        -providerclass org.bouncycastle.jce.provider.BouncyCastleProvider \
        -providerpath "$lspatch_jar" \
        -destkeystore "$sign_keystore" -deststoretype PKCS12 \
        -deststorepass "$aosp_storepass" -destalias "$aosp_alias" -destkeypass "$aosp_keypass" >/dev/null
fi

printf '模块 APK：%s\n' "$module_apk"
printf 'LSPatch：%s\n' "$lspatch_jar"
printf '游戏 APK 数量：%d\n' "${#game_apks[@]}"
[[ -n "$module_version" ]] && printf '模块版本：%s\n' "$module_version"
[[ -n "$newpackage" ]] && printf '新包名：%s\n' "$newpackage"

failed=0
generated=()
for target_apk in "${game_apks[@]}"; do
    target_name=$(basename "$target_apk")
    target_stem=${target_name%.apk}
    output_apk="$repo_root/output/$(release_stem "$target_stem")_${patch_suffix}${module_version:+_$module_version}.apk"
    work_dir=$(mktemp -d "$repo_root/.tmp/lspatch-apk.XXXXXX")

    printf '\n===== 处理：%s\n' "$target_name"

    if [[ "$is_npatch" == true ]]; then
        props="$work_dir/npatch-java-security.properties"
        printf '%s\n' 'security.provider.13=org.bouncycastle.jce.provider.BouncyCastleProvider' > "$props"
        args=(java "-Djava.security.properties=$props" -cp "$lspatch_jar"
              top.nkbe.npatch.patch.NPatch -m "$module_apk" -o "$work_dir"
              -k "$aosp_keystore" "$aosp_storepass" "$aosp_alias" "$aosp_keypass")
        [[ -n "$sigbypasslv" ]] && args+=(-l "$sigbypasslv")
        [[ "$debuggable" == true ]] && args+=(-d)
        args+=(-f)
        [[ -n "$newpackage" ]] && args+=(--newpackage "$newpackage")
        args+=("$target_apk")
    else
        args=(java -jar "$lspatch_jar" -m "$module_apk" -o "$work_dir"
              -k "$aosp_keystore" "$aosp_storepass" "$aosp_alias" "$aosp_keypass")
        [[ -n "$sigbypasslv" ]] && args+=(--sigbypasslv "$sigbypasslv")
        [[ "$debuggable" == true ]] && args+=(--debuggable)
        args+=(--force "$target_apk")
    fi

    if ! "${args[@]}"; then
        printf '集成失败：%s\n' "$target_name" >&2
        failed=1
        rm -rf "$work_dir"
        continue
    fi

    shopt -s nullglob
    patched_apks=("$work_dir"/*-lspatched.apk "$work_dir"/*-npatched.apk)
    shopt -u nullglob
    if [[ ${#patched_apks[@]} -ne 1 || ! -s "${patched_apks[0]}" ]]; then
        printf '输出 APK 数量异常或无效：%d（%s）\n' "${#patched_apks[@]}" "$target_name" >&2
        failed=1
        rm -rf "$work_dir"
        continue
    fi

    final_apk=${patched_apks[0]}
    if [[ -n "$newpackage" ]]; then
        # NPatch 输出保留与原包冲突的 C2D_MESSAGE 权限声明；输出端删除该权限并用
        # AOSP testkey 重签名（输入端 -l 1 需读未修改原包签名，无法预处理）。
        permission_fixed_apk="$work_dir/permission-fixed.apk"
        uv run python "$repo_root/scripts/maintenance/strip-conflicting-permission.py" \
            "${patched_apks[0]}" "$permission_fixed_apk" \
            "com.com2us.inotia4.normal.freefull.google.global.android.common.permission.C2D_MESSAGE"
        resigned_apk="$work_dir/resigned.apk"
        "$apksigner" sign --ks "$sign_keystore" --ks-type PKCS12 \
            --ks-pass "pass:$aosp_storepass" --key-pass "pass:$aosp_keypass" --ks-key-alias "$aosp_alias" \
            --out "$resigned_apk" "$permission_fixed_apk" >/dev/null
        final_apk="$resigned_apk"
    fi

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
