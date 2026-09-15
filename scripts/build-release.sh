#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
gradle_user_home=${GRADLE_USER_HOME:-"$repo_root/.gradle"}
gradle_bin=

for candidate in "$gradle_user_home"/wrapper/dists/gradle-8.11.1-bin/*/gradle-8.11.1/bin/gradle; do
    if [[ -x "$candidate" ]]; then
        gradle_bin=$candidate
        break
    fi
done

if [[ -z "$gradle_bin" ]]; then
    printf 'Gradle 8.11.1 cache not found under %s\n' "$gradle_user_home" >&2
    printf 'Expected: %s/wrapper/dists/gradle-8.11.1-bin/*/gradle-8.11.1/bin/gradle\n' "$gradle_user_home" >&2
    exit 1
fi

cd "$repo_root"
version_name=$(python3 -c 'import re, pathlib
text = pathlib.Path("module/app/build.gradle.kts").read_text()
match = re.search(r"^\s*versionName\s*=\s*\"([^\"]+)\"\s*$", text, re.MULTILINE)
if match is None:
    raise SystemExit("versionName not found in module/app/build.gradle.kts")
print(match.group(1))')

export GRADLE_USER_HOME="$gradle_user_home"
cd "$repo_root/module"
"$gradle_bin" :app:assembleRelease --no-daemon "$@"

source_apk="$repo_root/module/app/build/outputs/apk/release/app-release-unsigned.apk"
output_dir="$repo_root/output"
output_apk="$output_dir/inotia4_qol_lsposed_release_v${version_name}.apk"

# Release 与 debug 共用同一把签名（AGP 默认 debug keystore，CN=Android Debug）：
# 先 zipalign 再 apksigner；apksigner 可直接读 JKS，无需转换。
# 集成包（scripts/patch-apk.sh）另用 AOSP testkey，与模块签名互不影响。
debug_keystore=${DEBUG_KEYSTORE:-"$HOME/.android/debug.keystore"}
debug_storepass=android
debug_keypass=android
debug_keyalias=androiddebugkey

apksigner=$(command -v apksigner || true)
if [[ -z "$apksigner" && -x /opt/android-sdk/build-tools/37.0.0/apksigner ]]; then
    apksigner=/opt/android-sdk/build-tools/37.0.0/apksigner
fi
zipalign=$(command -v zipalign || true)
if [[ -z "$zipalign" && -x /opt/android-sdk/build-tools/37.0.0/zipalign ]]; then
    zipalign=/opt/android-sdk/build-tools/37.0.0/zipalign
fi
if [[ -z "$apksigner" || -z "$zipalign" ]]; then
    printf '未找到 apksigner 或 zipalign，无法签名 Release APK。\n' >&2
    exit 1
fi
if [[ ! -f "$debug_keystore" ]]; then
    printf '默认 debug keystore 不存在：%s\n' "$debug_keystore" >&2
    printf '先执行一次 scripts/build-debug.sh 让 AGP 生成，或用 DEBUG_KEYSTORE 指定路径。\n' >&2
    exit 1
fi

mkdir -p "$output_dir" "$repo_root/.tmp"
prep_dir=$(mktemp -d "$repo_root/.tmp/release-sign.XXXXXX")
trap 'rm -rf "$prep_dir"' EXIT

aligned_apk="$prep_dir/aligned.apk"
"$zipalign" -f -p 4 "$source_apk" "$aligned_apk"

"$apksigner" sign --ks "$debug_keystore" \
    --ks-pass "pass:$debug_storepass" --key-pass "pass:$debug_keypass" --ks-key-alias "$debug_keyalias" \
    --out "$output_apk" "$aligned_apk"

"$apksigner" verify --print-certs "$output_apk" >/dev/null

python3 - "$output_dir" <<'PY'
import pathlib
import sys

output_dir = pathlib.Path(sys.argv[1])
release_apks = sorted(
    (path for path in output_dir.glob("*.apk") if "release" in path.name.lower()),
    key=lambda path: path.stat().st_mtime_ns,
    reverse=True,
)
for path in release_apks[2:]:
    path.unlink()
PY

printf 'Generated Release APK filename: %s\n' "$(basename "$output_apk")"
printf 'Generated Release APK path: %s\n' "$output_apk"
sha256sum "$output_apk"
