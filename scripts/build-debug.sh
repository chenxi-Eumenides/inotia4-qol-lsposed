#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
gradle_user_home=${GRADLE_USER_HOME:-"$repo_root/.gradle"}
gradle_bin=
all_output=false
gradle_passthrough=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --)
            gradle_passthrough+=("$@")
            break
            ;;
        --all)
            all_output=true
            shift
            ;;
        *)
            gradle_passthrough+=("$1")
            shift
            ;;
    esac
done

for candidate in "$gradle_user_home"/wrapper/dists/gradle-8.11.1-bin/*/gradle-8.11.1/bin/gradle; do
    if [[ -x "$candidate" ]]; then
        gradle_bin=$candidate
        break
    fi
done

if [[ -z "$gradle_bin" ]]; then
    printf 'Gradle 8.11.1 cache not found under %s\n' "$gradle_user_home" >&2
    printf 'Expected: %s/wrapper/dists/gradle-8.11.1-bin/*/gradle-8.11.1/bin/gradle\n' "$gradle_user_home" >&2
    printf 'Populate the project cache or provide GRADLE_USER_HOME explicitly.\n' >&2
    exit 1
fi

export GRADLE_USER_HOME="$gradle_user_home"
cd "$repo_root/module"

gradle_args=(":app:assembleDebug" "--no-daemon")
if [[ ${#gradle_passthrough[@]} -eq 0 ]]; then
    gradle_args+=("--offline")
else
    gradle_args+=("${gradle_passthrough[@]}")
fi
[[ "$all_output" == true ]] || gradle_args+=("--quiet")

"$gradle_bin" "${gradle_args[@]}"

source_apk="$repo_root/module/app/build/outputs/apk/debug/app-debug.apk"
output_dir="$repo_root/output"
checksum=$(sha256sum "$source_apk" | cut -d ' ' -f 1)
build_timestamp=$(date +%y%m%d%H%M)
output_apk="$output_dir/inotia4_qol_lsposed_debug_${build_timestamp}_${checksum:0:12}.apk"
mkdir -p "$output_dir"
cp "$source_apk" "$output_apk"

python3 - "$output_dir" <<'PY'
import pathlib
import sys

# 只清理本脚本自己的输出（inotia4_qol_lsposed_debug_<时间戳>_<sha前12>.apk）。
# 不能用 "debug" 子串匹配：集成包名同样含 _debug_（Inotia4_<变体>_<版本>_debug_<patcher>.apk），
# 匹配到会按 3 份上限把 patch-apk.sh 生成的集成交付物一并删掉。
output_dir = pathlib.Path(sys.argv[1])
debug_apks = sorted(
    output_dir.glob("inotia4_qol_lsposed_debug_*.apk"),
    key=lambda path: path.stat().st_mtime_ns,
    reverse=True,
)
for path in debug_apks[3:]:
    path.unlink()
PY

printf 'Generated Debug APK filename: %s\n' "$(basename "$output_apk")"
printf 'Generated Debug APK path: %s\n' "$output_apk"
printf 'SHA-256: %s\n' "$checksum"
