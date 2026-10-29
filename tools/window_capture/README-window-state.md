# Windows の MRFPS ウィンドウ状態

選択した MRFPS ウィンドウの位置・サイズ、最小化・復元、OS の前面化を扱います。画面や画像は取得しません。追加パッケージは不要です。撮影 CLI と同じウィンドウ識別、共有ロック、UUID の履歴、ユーザー専用ディレクトリを使います。

対象は MRFPS 製品版バイナリのメインウィンドウ、または同じプロセスの P1・P2・観戦2 ポップアウトです。メインタイトルは `MRFPS` で始まり、両ウィンドウは `UnrealWindow` クラスである必要があります。異なるタイトルの `-MRFPSWindowName` や汎用 Unreal Editor のウィンドウは対象外です。同名ウィンドウが複数あると識別を拒否します。

## 識別

対象プロセスの PID とメイン・対象 HWND を、撮影 CLI の [README.md](README.md) に従い明示します。最小化されたウィンドウを初めから選ぶときだけ `--allow-minimized` を付けます。このオプションは最小化中の撮影を許可しません。通常状態で識別した後に最小化した場合も、同じ識別を使って復元できます。

```powershell
$python = 'C:\UnrealEngine\UE5.8\Engine\Binaries\ThirdParty\Python3\Win64\python.exe'
$clientProcessId = 12345  # 対象 MRFPS の PID に置換
$mainHwnd = 111111        # 対象プロセスの main HWND に置換
$targetHwnd = 222222      # main または対象ポップアウトの HWND に置換
$identityId = [guid]::NewGuid().ToString()
& $python .\tools\window_capture\capture.py identify --identity-id $identityId --pid $clientProcessId --main-hwnd $mainHwnd --target-hwnd $targetHwnd --allow-minimized
```

## 状態を指定する

毎回新しい `snapshot-id` で対象の現在状態を取得し、その結果に基づいて 10 秒以内に新しい `operation-id` を渡します。各操作は 1 状態のみ指定できます。位置とサイズを指定する例です。座標はデスクトップ物理ピクセル、矩形は外枠です。

```powershell
$snapshotId = [guid]::NewGuid().ToString()
& $python .\tools\window_capture\window_state.py inspect --identity-id $identityId --snapshot-id $snapshotId
$operationId = [guid]::NewGuid().ToString()
& $python .\tools\window_capture\window_state.py ensure --identity-id $identityId --snapshot-id $snapshotId --operation-id $operationId --rect 50 50 960 540
```

`--rect X Y WIDTH HEIGHT` は X・Y がそれぞれ -16384〜16384、幅 320〜4096、高さ 240〜2160、最大 4096×2160 ピクセルです。最小化または最大化中の矩形変更は拒否するので、先に `--normal` を指定してください。次の操作例も、それぞれ実行直前に新しい `snapshot-id` と `operation-id` を作ります。

```powershell
# 最小化。前面のウィンドウは対象にできません。
& $python .\tools\window_capture\window_state.py ensure --identity-id $identityId --snapshot-id $snapshotId --operation-id $operationId --minimized true

# 最小化や最大化を解除。ウィンドウを前面化しません。
& $python .\tools\window_capture\window_state.py ensure --identity-id $identityId --snapshot-id $snapshotId --operation-id $operationId --normal

# 最小化だけを解除。既に最小化されていなければ何もしません。
& $python .\tools\window_capture\window_state.py ensure --identity-id $identityId --snapshot-id $snapshotId --operation-id $operationId --minimized false

# 対象を OS の前面にするよう明示的に要求。Windows が拒否することがあります。
& $python .\tools\window_capture\window_state.py ensure --identity-id $identityId --snapshot-id $snapshotId --operation-id $operationId --foreground true
```

`--foreground false` は対象が既に前面でなければ成功します。前面にある対象を他アプリへ切り替えずに確実に前面解除する方法はないため、`foreground_release_unsupported` を返して操作しません。これは観戦画面内の Esc や既存 `focus.set` のマウスカーソル解放とは別の状態です。対象が owner となるウィンドウがある場合は、OS による他ウィンドウの連動を避けるため状態変更を拒否します。ただし位置・サイズ変更だけは、対象の ImmGetDefaultIMEWnd で確認した不可視の既定 IME と、その同じプロセス・同じスレッドの不可視 MSCTFIME UI 所有窓だけがある場合に許可します。表示中の IME 候補や他の所有窓がある場合は拒否し、操作中に所有関係や表示状態が変われば unknown とします。最小化・復元・前面化には例外を適用しません。

`ok` は要求した状態を観測した結果、`already_applied` は変更不要を意味します。OS の非同期処理は受付だけでは完了としません。対象が変わった場合、タイムアウト、応答喪失、途中中断や競合時は `unknown` です。同じ操作 ID と同じ内容の再実行は元の結果を返し、操作を再送しません。別内容に同じ ID を使うと拒否します。`unknown` の後は保留中の Windows メッセージが後から適用され得るため、自動で新 ID を作って再試行しないでください。対象と OS 状態を確認してから、必要な次の操作を判断してください。共有ロックが取得できない場合は呼び出し自体を開始せずエラーを返します。

Windows は別プロセスによる状態確認とウィンドウ操作を原子的にまとめる API を提供しません。対象を前後に再検証しますが、他の操作が同時に入る極めて短い競合を完全には排除できません。作業中は同じ対象の手動操作や別の制御を避けてください。journal が削除・破損した場合、再実行の一意性は保証できません。

終了時は `capture.py release --identity-id $identityId` を実行します。実ウィンドウ・異なる DPI・前面化制限・所有ウィンドウとの相互作用はまだ実測していません。次の mock テストは実ウィンドウを操作しません。

```powershell
& $python -m unittest discover -s tools/window_capture -p 'test_*.py' -v
```
