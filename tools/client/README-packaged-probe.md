# Windows 配布版の認証・参加確認

`packaged_probe.py` は **Development 配布版**の実ウィンドウを対象に、既存の 10 GiB / 75% メモリガードを使って段階的に検証します。起動前には空き 12.5 GiB 以上・コミット率 72% 以下の余裕も確認します。別の test directory を使い、パッケージ内の実行ファイル、SHA-256、実ウィンドウの所属プロセスと guard の親子関係を確認します。管理 Worker のコードをクライアント自身に入力し、発行された識別子と同じサーバー側の認証・参加を照合します。コード、JWT、秘密鍵、識別子はレポートに出力しません。

実行前に、同じ protocol で構築された健康な検証用コンテナの名前・現在の DTLS fingerprint と、新しい Windows package のネイティブ exe を用意してください。管理パネル用の暗号化鍵バックアップ・パスフレーズ・Basic 資格情報が `%LOCALAPPDATA%\MRFPS\Admin` に必要です。対象のコンテナは検証中に再作成しないでください。

fingerprint は直近のログ行数ではなく、そのコンテナの現在の起動時刻から 5 分以内のログで確認します。長時間動いていても、起動時ログが残っていれば照合できます。ログがローテーションなどで失われた場合は確認不能として停止します。取得前後にコンテナ ID・起動時刻・再起動回数・PID・health を確認し、途中で変更された場合も停止します。ログは逐次読み取りし、参加照合に必要な最小限の情報だけをメモリに保持します。

```powershell
$python = 'C:\UnrealEngine\UE5.8\Engine\Binaries\ThirdParty\Python3\Win64\python.exe'
$exe = '<新規パッケージ>\Windows\MRFPS\Binaries\Win64\MRFPS.exe'
$sha = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.ToLowerInvariant()
$run = 'Saved\Validation\PackageLogin-001'
$server = '<テスト用の到達先>:<port>'
$container = '<テスト用のコンテナ名>'
$fingerprint = '<そのコンテナの64桁の指紋>'

& $python tools/client/packaged_probe.py start --run-dir $run --executable $exe --sha256 $sha `
  --server $server --container $container --fingerprint $fingerprint --role inspector
# 実ウィンドウ生成を待ち、initial_window_title と window_image_and_sha_match を確認。
& $python tools/client/packaged_probe.py observe --run-dir $run

# 設定画面からログイン画面へ一度だけ移行し、settings_applied を待つ。
& $python tools/client/packaged_probe.py configure --run-dir $run
& $python tools/client/packaged_probe.py observe --run-dir $run

# 管理 Worker に新しいコードを発行させ、同じ実ウィンドウへ一度だけ渡す。
& $python tools/client/packaged_probe.py login --run-dir $run
# exchange_verified、travel_completed、server_auth_join_matches_issued_subject、
# connected_window_title がすべて true になるまで observe する。
& $python tools/client/packaged_probe.py observe --run-dir $run

# 検証用ウィンドウだけを通常終了。再実行は already_stopped。
& $python tools/client/packaged_probe.py stop --run-dir $run
& $python tools/client/packaged_probe.py stop --run-dir $run
```

プレイヤー画面の確認は、新しい run directory で `--role player` を指定します。各段階は完了後に繰り返せます。途中でコードの送信結果が不明になった場合は同じコードを再送せず、`outcome is unknown` として停止します。新しい試験には新しい run directory を使います。`stop` はその試験で発行したコードの発行者のみ停止します。画面キャプチャを行うときは、`observe` の `name` と `window_pid` が示すウィンドウだけを対象にしてください。`report.json` と guard のレポートには試験結果が残ります。

`login` は発行 CLI と共通のロックを取り、役割・指紋・有効期限・発行 ID・識別子・コードファイルを一組として検証します。クライアントにも期待するファイル記録とコードを渡して送信直前に再照合し、変更があればコードを投入せず停止します。送信後も launcher の記録が一致した場合だけ `login_submitted` として記録します。コードの値は記録やコマンド引数には出しません。
