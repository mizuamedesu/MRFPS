# Windows の対象ウィンドウ撮影

このツールは、明示した MRFPS クライアントのメインウィンドウ、または同じプロセスの P1・P2・観戦ポップアウトを 1 枚撮影します。ディスプレイ全体への切り替えはありません。Windows Graphics Capture を使い、最小化・非表示の対象は拒否します。Windows 10 1903 以降、64-bit Python 3.11 が必要です。

## 準備

依存パッケージは専用 venv に分離し、3 個の Windows x64 wheel のバージョンと SHA-256 を `requirements.txt` で固定しています。setup は同じ環境に繰り返し実行できます。既存のメモリガード経由で実行してください。

```powershell
$capturePython = 'C:\UnrealEngine\UE5.8\Engine\Binaries\ThirdParty\Python3\Win64\python.exe'
& .\Scripts\Invoke-MRFPSResourceLimited.ps1 `
  -ScriptFile .\tools\window_capture\setup.ps1 `
  -ScriptArguments @('-Python', $capturePython) `
  -MinimumFreeGB 10 -MaximumCommitPercent 75
```

別の Python 3.11 x64 を使う場合は `$capturePython` をその絶対パスにします。パッケージの hash はダウンロードした wheel の一致を保証します。PyPI の当該 wheel にはビルドの provenance 証明がありません。

## 撮影

実行中の MRFPS クライアント PID を選びます。メタデータを一覧表示する `list` は画像を取得しません。

```powershell
$capturePython = 'C:\UnrealEngine\UE5.8\Engine\Binaries\ThirdParty\Python3\Win64\python.exe'
$clientProcessId = 12345   # 実行中のクライアント PID に置換
& $capturePython .\tools\window_capture\capture.py list --pid $clientProcessId
```

一覧からメインウィンドウと撮影対象の HWND を明示します。メイン画面を撮るときは両方に同じ HWND を指定します。異なる HWND を指定する場合、対象は同じ PID の `MRFPS - P1`、`MRFPS - P2`、`MRFPS - 観戦2` のいずれかです。同名のウィンドウが複数ある場合、取り違えを避けるため拒否します。

```powershell
$mainHwnd = 111111         # list のメインウィンドウ HWND に置換
$targetHwnd = 222222       # list の撮影対象 HWND に置換
$identityId = [guid]::NewGuid().ToString()
& $capturePython .\tools\window_capture\capture.py identify `
  --identity-id $identityId --pid $clientProcessId --main-hwnd $mainHwnd --target-hwnd $targetHwnd

$captureId = [guid]::NewGuid().ToString()
& $capturePython .\tools\window_capture\capture.py capture `
  --identity-id $identityId --id $captureId --timeout-seconds 15

& $capturePython .\tools\window_capture\capture.py release --identity-id $identityId
```

`identify` はウィンドウのインスタンスを短い property で識別し、同じ PID・同名ウィンドウへの HWND 再利用を検出します。終了時に `release` で property を外してください。途中でプロセスが終了した場合はウィンドウ破棄時まで property が残ることがあります。識別 ID と同じ引数の再実行は既存結果を返します。異なる対象への同じ ID の再使用は拒否されます。

撮影結果と journal は `%LOCALAPPDATA%\MRFPS\WindowCapture` にのみ保存し、ディレクトリのアクセス権は現在の Windows ユーザーに制限します。成功時の JSON の `image_path` が PNG です。書き込み完了・寸法・PNG 構造・CRC・圧縮データ・SHA-256 を確認してから公開します。撮影 ID と同じ内容の再実行は、元画像の hash も確かめて結果を再利用します。別内容で同じ ID を使うと拒否されます。

途中中断、応答不明、画像の欠落や改変は `unknown` として記録し、同じ ID では再撮影しません。新しい撮影を意図するときは新しい撮影 ID を使ってください。同時操作中はもう一方を開始せず、再試行可能なエラーを返します。短命 helper は専用 Windows job に入り、時間制限と空き RAM 10 GiB・コミット率 75% のガードを満たせないときは終了します。

再実行の判定はローカル journal が残っていることを前提とします。停電やストレージ障害などで journal 自体が失われた場合の一意性は保証できません。

WGC や GPU、対象ウィンドウの状態によっては timeout や黒い画像になります。PNG の保存成功は、Quest の最新映像が届いたことや人間の目による確認を意味しません。実画面のログインコードなども画像に含まれ得るため、ログイン画面を閉じてから対象を選んでください。

実機の GPU 映像、ウィンドウの background 描画、対象を閉じた場合の実際の後片付けは未検証です。mock テストは次のコマンドで実行できます。

```powershell
& $capturePython -m unittest discover -s tools/window_capture -p 'test_*.py' -v
```
