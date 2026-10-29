#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
mode=token
if [[ "${1:-}" == --device-login ]]; then
  mode=device-login
  [[ $# == 2 && "$2" =~ ^[A-Za-z0-9_-]{1,64}$ ]] || { echo '使い方: run-client.sh --device-login クライアント名' >&2; exit 2; }
  name="$2"
else
  [[ $# -ge 3 && $# -le 4 ]] || { echo '使い方: run-client.sh 接続先:ポート トークンファイル 公開鍵ファイル [inspector|admin]' >&2; exit 2; }
fi
[[ "$(uname -s)" == Darwin ]] || { echo 'macOS上で実行してください。' >&2; exit 2; }
command -v python3 >/dev/null || { echo 'Python 3をインストールしてください。' >&2; exit 2; }
if [[ "$mode" == token ]]; then
  server="$1"; token="$2"; public="$3"; role="${4:-inspector}"
  [[ "$server" =~ ^[][A-Za-z0-9.:-]+$ && -f "$token" && -f "$public" ]] || { echo '接続先または認証ファイルを確認してください。' >&2; exit 2; }
  case "$role" in inspector) role_flag=-MRFPSInspector;; admin) role_flag=-MRFPSAdmin;; *) echo 'roleはinspectorまたはadminです。' >&2; exit 2;; esac
  token="$(python3 -c 'import pathlib,sys;print(pathlib.Path(sys.argv[1]).resolve())' "$token")"
  public="$(python3 -c 'import pathlib,sys;print(pathlib.Path(sys.argv[1]).resolve())' "$public")"
fi
editor=false
if [[ -n "${MRFPS_APP:-}" ]]; then
  command=("$MRFPS_APP")
elif [[ -x "$root/Artifacts/Packages/Mac/MRFPS.app/Contents/MacOS/MRFPS" ]]; then
  command=("$root/Artifacts/Packages/Mac/MRFPS.app/Contents/MacOS/MRFPS")
else
  engine="${UE_ROOT:-/Users/Shared/Epic Games/UE_5.8}"
  command=("$engine/Engine/Binaries/Mac/UnrealEditor.app/Contents/MacOS/UnrealEditor" "$root/MRFPS.uproject" -game)
  editor=true
fi
[[ -f "${command[0]}" && -x "${command[0]}" ]] || { echo 'Macクライアント実行ファイルがありません。先にビルドするかMRFPS_APPを指定してください。' >&2; exit 2; }
if [[ "$mode" == device-login ]]; then
  arguments=(start --name "$name" --executable "${command[0]}")
  if [[ "$editor" == true ]]; then arguments+=(--editor); fi
  exec python3 "$root/tools/client/mac_login.py" "${arguments[@]}"
fi
exec python3 "$root/tools/dev/mac_resource_guard.py" -- "${command[@]}" /Game/Maps/MRFPSArena "-MRFPSServer=$server" "$role_flag" -MRFPSDesktop -nohmd -nosound -NoLiveCoding "-MRFPSAuthTokenFile=$token" "-MRFPSAuthPublicKeyFile=$public" '-ExecCmds=t.MaxFPS 60,t.IdleWhenNotForeground 0' -windowed -ResX=1280 -ResY=720
