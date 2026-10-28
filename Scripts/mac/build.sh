#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
engine="${UE_ROOT:-/Users/Shared/Epic Games/UE_5.8}"
target="${1:-Editor}"
[[ "$(uname -s)" == Darwin ]] || { echo 'macOS上で実行してください。' >&2; exit 2; }
[[ -f "$engine/Engine/Build/Build.version" ]] || { echo 'UE_ROOTにUE5.8の場所を設定してください。' >&2; exit 2; }
export DOTNET_PROCESSOR_COUNT=2 DOTNET_GCHeapHardLimit=0x80000000 MSBUILDDISABLENODEREUSE=1
project="$root/MRFPS.uproject"
guard=(python3 "$root/tools/dev/mac_resource_guard.py")
"${guard[@]}" -- "$engine/Engine/Build/BatchFiles/Mac/Build.sh" MRFPSEditor Mac Development "-Project=$project" -WaitMutex -MaxParallelActions=1 -NoUBA
editor="$engine/Engine/Binaries/Mac/UnrealEditor.app/Contents/MacOS/UnrealEditor"
"${guard[@]}" -- "$editor" "$project" -run=MRFPSSetup -unattended -NullRHI -nohmd -nosound -nop4
case "$target" in
  Editor) ;;
  Client)
    "${guard[@]}" -- "$engine/Engine/Build/BatchFiles/Mac/Build.sh" MRFPS Mac Development "-Project=$project" -WaitMutex -MaxParallelActions=1 -NoUBA
    "${guard[@]}" -- "$engine/Engine/Build/BatchFiles/RunUAT.sh" BuildCookRun "-project=$project" -noP4 -utf8output -skipbuild -cook -stage -pak -package -archive "-archivedirectory=$root/Artifacts/Packages" -target=MRFPS -platform=Mac -clientconfig=Development -map=/Game/Maps/MRFPSArena -unattended ;;
  *) echo '使い方: build.sh [Editor|Client]' >&2; exit 2 ;;
esac
