#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
engine="${UE_ROOT:-/Users/Shared/Epic Games/UE_5.8}"
target="${1:-Editor}"
[[ $# -le 1 ]] || { echo '使い方: build.sh [Editor|Client]' >&2; exit 2; }
case "$target" in Editor|Client) ;; *) echo '使い方: build.sh [Editor|Client]' >&2; exit 2;; esac
[[ "$(uname -s)" == Darwin ]] || { echo 'macOS上で実行してください。' >&2; exit 2; }
command -v python3 >/dev/null || { echo 'Python 3をインストールしてください。' >&2; exit 2; }
[[ -f "$engine/Engine/Build/Build.version" ]] || { echo 'UE_ROOTにUE5.8の場所を設定してください。' >&2; exit 2; }
python3 - "$engine/Engine/Build/Build.version" <<'PY'
import json, sys
with open(sys.argv[1], encoding="utf-8-sig") as stream:
    version = json.load(stream)
if (version.get("MajorVersion"), version.get("MinorVersion")) != (5, 8):
    raise SystemExit("UE_ROOT must select Unreal Engine 5.8.")
PY
build="$engine/Engine/Build/BatchFiles/Mac/Build.sh"
uat="$engine/Engine/Build/BatchFiles/RunUAT.sh"
[[ -x "$build" ]] || { echo 'UE_ROOTのMac Build.shを実行できません。' >&2; exit 2; }
[[ "$target" != Client || -x "$uat" ]] || { echo 'UE_ROOTのRunUAT.shを実行できません。' >&2; exit 2; }
export DOTNET_PROCESSOR_COUNT=2 DOTNET_GCHeapHardLimit=0x80000000 MSBUILDDISABLENODEREUSE=1
project="$root/MRFPS.uproject"
guard=(python3 "$root/tools/dev/mac_resource_guard.py")
"${guard[@]}" -- "$build" MRFPSEditor Mac Development "-Project=$project" -WaitMutex -MaxParallelActions=1 -NoUBA
editor="$engine/Engine/Binaries/Mac/UnrealEditor.app/Contents/MacOS/UnrealEditor"
[[ -x "$editor" ]] || { echo 'ビルド後のUnrealEditor実行ファイルがありません。' >&2; exit 2; }
"${guard[@]}" -- "$editor" "$project" -run=MRFPSSetup -unattended -NullRHI -nohmd -nosound -nop4
case "$target" in
  Editor) ;;
  Client)
    "${guard[@]}" -- "$build" MRFPS Mac Development "-Project=$project" -WaitMutex -MaxParallelActions=1 -NoUBA
    "${guard[@]}" -- "$uat" BuildCookRun "-project=$project" -noP4 -utf8output -skipbuild -cook -stage -pak -package -archive "-archivedirectory=$root/Artifacts/Packages" -target=MRFPS -platform=Mac -clientconfig=Development -map=/Game/Maps/MRFPSArena -unattended ;;
esac
