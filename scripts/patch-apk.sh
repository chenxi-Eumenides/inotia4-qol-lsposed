#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat >&2 <<'EOF'
用法：
  scripts/patch-apk.sh [选项] <原始 APK> <LSPosed 模块 APK> [输出 APK]

默认值：
  LSPatch JAR：tools/lspatch/npatch-v1.0.7-741-release.jar（NPatch）
  输出 APK：output/<原始文件名>-npatched.apk（NPatch）/ -lspatched.apk（LSPatch）
  NPatch 生成前会删除 output/ 下旧的 NPatch 产物（*npatch*.apk），保持输出目录干净

选项：
  --lspatch-jar PATH  使用指定版本的 lspatch/npatch JAR
  --debuggable        将输出 APK 标记为 debuggable
  --sigbypasslv N     设置 LSPatch signature bypass level（0/1/2/3）
  --newpackage PKG    NPatch 专用：将输出 APK 改为新的 applicationId
  -f, --force         覆盖已有输出 APK
  -h, --help          显示帮助
EOF
    exit 2
}

repo_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
lspatch_jar=${LSPATCH_JAR:-"$repo_root/tools/lspatch/npatch-v1.0.7-741-release.jar"}
debuggable=false
sigbypasslv=
newpackage=
force=false
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
        --newpackage)
            [[ $# -ge 2 ]] || usage
            newpackage=$2
            shift 2
            ;;
        -f|--force)
            force=true
            shift
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

if [[ ${#positional[@]} -lt 2 || ${#positional[@]} -gt 3 ]]; then
    usage
fi

if [[ ! -f "${positional[0]}" ]]; then
    printf '原始 APK 不存在：%s\n' "${positional[0]}" >&2
    exit 1
fi
if [[ ! -f "${positional[1]}" ]]; then
    printf '模块 APK 不存在：%s\n' "${positional[1]}" >&2
    exit 1
fi
target_apk=$(realpath "${positional[0]}")
module_apk=$(realpath "${positional[1]}")
if [[ ${#positional[@]} -eq 3 ]]; then
    output_apk=${positional[2]}
    if [[ "$output_apk" != /* ]]; then
        output_apk="$repo_root/$output_apk"
    fi
else
    target_name=$(basename "$target_apk")
    target_stem=${target_name%.apk}
    case "$lspatch_jar" in
        *npatch*) patch_suffix=npatched ;;
        *) patch_suffix=lspatched ;;
    esac
    output_apk="$repo_root/output/${target_stem}-${patch_suffix}.apk"
fi

if [[ ! -f "$lspatch_jar" ]]; then
    printf 'LSPatch JAR 不存在：%s\n' "$lspatch_jar" >&2
    printf '可用 --lspatch-jar 指定下载的版本。\n' >&2
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

case "$output_apk" in
    "$repo_root/output/"*.apk) ;;
    *)
        printf '输出 APK 必须位于项目 output/ 目录：%s\n' "$output_apk" >&2
        exit 1
        ;;
esac

if [[ -e "$output_apk" && "$force" != true ]]; then
    printf '输出文件已存在（使用 --force 覆盖）：%s\n' "$output_apk" >&2
    exit 1
fi

sigbypass_args=()
if [[ -n "$sigbypasslv" ]]; then
    if [[ ! "$sigbypasslv" =~ ^[0-3]$ ]]; then
        printf 'signature bypass level 必须是 0、1、2 或 3：%s\n' "$sigbypasslv" >&2
        exit 1
    fi
    sigbypass_args+=(--sigbypasslv "$sigbypasslv")
fi

work_dir=$(mktemp -d "$repo_root/.tmp/lspatch-apk.XXXXXX")
trap 'rm -rf "$work_dir"' EXIT
mkdir -p "$(dirname "$output_apk")"
patch_target_apk="$target_apk"

if jar tf "$lspatch_jar" | grep -q '^top/nkbe/npatch/patch/NPatch.class$'; then
    if [[ -n "$sigbypasslv" ]]; then
        sigbypass_args=(-l "$sigbypasslv")
    fi
    shopt -s nullglob
    old_npatch_apks=("$repo_root/output/"*npatch*.apk)
    shopt -u nullglob
    for old_apk in "${old_npatch_apks[@]}"; do
        if [[ "$old_apk" == "$output_apk" ]]; then
            continue
        fi
        rm -f "$old_apk"
        printf '已删除旧 NPatch 输出：%s\n' "$(basename "$old_apk")"
    done
    printf '%s\n' 'security.provider.13=org.bouncycastle.jce.provider.BouncyCastleProvider' > "$work_dir/npatch-java-security.properties"
    lspatch_args=(java "-Djava.security.properties=$work_dir/npatch-java-security.properties" -cp "$lspatch_jar" top.nkbe.npatch.patch.NPatch -m "$module_apk" -o "$work_dir" "${sigbypass_args[@]}")
    [[ "$debuggable" == true ]] && lspatch_args+=(-d)
    [[ "$force" == true ]] && lspatch_args+=(-f)
    if [[ -n "$newpackage" ]]; then
        lspatch_args+=(--newpackage "$newpackage")
    fi
else
    if [[ -n "$newpackage" ]]; then
        printf '%s\n' '--newpackage 仅支持 NPatch，不支持当前 LSPatch JAR。' >&2
        exit 1
    fi
    lspatch_args=(java -jar "$lspatch_jar" -m "$module_apk" -o "$work_dir" "${sigbypass_args[@]}")
    [[ "$debuggable" == true ]] && lspatch_args+=(--debuggable)
    [[ "$force" == true ]] && lspatch_args+=(--force)
fi
lspatch_args+=("$patch_target_apk")

printf '原始 APK：%s\n' "$target_apk"
printf '模块 APK：%s\n' "$module_apk"
printf 'LSPatch：%s\n' "$lspatch_jar"
"${lspatch_args[@]}"

shopt -s nullglob
patched_apks=("$work_dir"/*-lspatched.apk "$work_dir"/*-npatched.apk)
shopt -u nullglob
if [[ ${#patched_apks[@]} -ne 1 ]]; then
    printf 'LSPatch 输出 APK 数量异常：%d（期望 1）\n' "${#patched_apks[@]}" >&2
    exit 1
fi
if [[ ! -s "${patched_apks[0]}" ]]; then
    printf '%s\n' 'LSPatch 输出 APK 无效，已停止，不覆盖交付文件。' >&2
    exit 1
fi

if [[ -n "$newpackage" ]]; then
    # NPatch 输出包含与原包冲突的 C2D_MESSAGE 权限声明；无法在输入端预处理（-l 1 需读取
    # 未修改原包的原始签名），只能在输出端删除权限并用 NPatch 内置证书重签名。
    # 已验证不可行的瘦身路径：-l 0 预处理虽能保住 NPatch 重叠条目（52MB），但 NPatch 重写
    # zip 时把 STORED 资源重压缩为 DEFLATED，游戏引擎 mmap 直读即崩（SGL_Texture::FromResource）。
    permission_fixed_apk="$work_dir/permission-fixed.apk"
    uv run python "$repo_root/scripts/maintenance/strip-conflicting-permission.py" \
        "${patched_apks[0]}" "$permission_fixed_apk" \
        "com.com2us.inotia4.normal.freefull.google.global.android.common.permission.C2D_MESSAGE"

    npatch_key="$work_dir/npatch.key"
    npatch_keystore="$work_dir/npatch.p12"
    unzip -p "$lspatch_jar" assets/npatch.key > "$npatch_key"
    keytool -importkeystore -noprompt \
        -srckeystore "$npatch_key" -srcstoretype BKS -srcstorepass 123456 \
        -srcalias key0 -providerclass org.bouncycastle.jce.provider.BouncyCastleProvider \
        -providerpath "$lspatch_jar" \
        -destkeystore "$npatch_keystore" -deststoretype PKCS12 -deststorepass 123456 \
        -destalias key0 -destkeypass 123456 >/dev/null

    apksigner=$(command -v apksigner || true)
    if [[ -z "$apksigner" && -x /opt/android-sdk/build-tools/37.0.0/apksigner ]]; then
        apksigner=/opt/android-sdk/build-tools/37.0.0/apksigner
    fi
    if [[ -z "$apksigner" ]]; then
        printf '%s\n' '未找到 apksigner，无法为 Manifest 后处理后的 NPatch APK 重签名。' >&2
        exit 1
    fi
    resigned_apk="$work_dir/resigned.apk"
    "$apksigner" sign --ks "$npatch_keystore" --ks-type PKCS12 \
        --ks-pass pass:123456 --key-pass pass:123456 --ks-key-alias key0 \
        --out "$resigned_apk" "$permission_fixed_apk" >/dev/null
    patched_apks[0]="$resigned_apk"
fi

if [[ "$force" == true ]]; then
    mv -f "${patched_apks[0]}" "$output_apk"
else
    mv "${patched_apks[0]}" "$output_apk"
fi

printf '生成集成 APK：%s\n' "$output_apk"
sha256sum "$output_apk"
