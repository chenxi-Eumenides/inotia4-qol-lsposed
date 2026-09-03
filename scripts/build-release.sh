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
output_apk="$output_dir/inotia4-qol-lsposed-v${version_name}-release-unsigned.apk"
mkdir -p "$output_dir"
cp "$source_apk" "$output_apk"

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
