# 観戦クライアントの映像受信診断

`measure_inspector_fps.py` は、すでに起動して認証されている Windows の **inspector ロール**から `status` を繰り返し取得し、指定プレイヤーの映像をクライアントが組み立てた枚数の変化を計測します。admin ロールは現在対象外です。認証、サーバー接続、画面共有、表示切り替え、ウィンドウ操作は行いません。状態取得のため、通常の automation と同じユーザー限定ディレクトリに操作ごとの journal が作成されます。

## 準備と実行

- UE クライアントには `video_measurement_identity` を含むバージョンが必要です。古いバイナリでは `unsupported_status` になります。
- 対象の Quest は、利用者が OS の共有許可を与えて映像を送信中にします。診断 CLI 自体は許可画面を操作しません。
- Windows クライアントで、計測するプレイヤーの視点をあらかじめ購読しておきます。診断中に視点やウィンドウ構成を変更すると計測は無効になります。
- サーバー検証は現時点では、この PC の **WSL Ubuntu 内 Docker** にある healthy コンテナに限ります。コンテナ名と、認証設定に用いた公開の DTLS 証明書 SHA-256 fingerprint が必要です。起動時の fingerprint ログと、最近 5000 行内の inspector の認証・参加ログを参照できない場合は失敗します。ログインコード、JWT、秘密鍵を引数に渡さないでください。
- `--pid` はゲーム本体の PID です。起動ガードの PowerShell の PID ではありません。対象のメイン HWND は次の一覧から確認します。この一覧は画像を取得しません。

```powershell
$clientProcessId = 12345       # 実際の MRFPS クライアント PID
$python = 'C:\UnrealEngine\UE5.8\Engine\Binaries\ThirdParty\Python3\Win64\python.exe'
& $python .\tools\window_capture\capture.py list --pid $clientProcessId
```

次の値を実際の環境に置き換えてください。`--name` は対象の `-MRFPSWindowName` と同じ文字列、`--main-hwnd` は上記一覧のメインウィンドウ、`--fingerprint` は公開 fingerprint です。`--slot 0` が P1、`--slot 1` が P2 で、両方指定できます。

```powershell
$windowName = 'MRFPS-Example'
$mainHwnd = 123456
$containerName = 'mrfps-server'
$publicFingerprint = '<64 hexadecimal characters>'
& $python .\tools\video\measure_inspector_fps.py `
  --name $windowName --pid $clientProcessId --main-hwnd $mainHwnd `
  --container $containerName --fingerprint $publicFingerprint `
  --slot 0 --slot 1 --duration-seconds 15 --interval-seconds 1
```

同じ条件で変更前後に実行し、出力の時間範囲、各 slot の `stream_id`、枚数差、`assembled_fps_estimate`、`assembled_fps_bounds`、テクスチャ鮮度サンプル数を比較してください。各実行は独立しており、異なるストリーム世代をまたいだ通算の比較はできません。実画像の確認には、別途 [対象ウィンドウだけを撮影する既存 CLI](../window_capture/README.md) を使用し、結果を目視してください。

## 結果の読み方と限界

- `assembled_fps_estimate` はクライアントが JPEG フレームを受信・組み立てた回数を経過時間で割った値です。UE のゲーム tick、ディスプレイ／GPU の描画 FPS、CPU 使用率、Quest の実際の映像内容は測っていません。テクスチャが最新であるサンプル数は別項目であり、表示 FPS や画像内容の証明ではありません。
- UE の status は要求と応答の間のどこかで採取されます。単調時計によるその時間幅から FPS の上下限を出し、中心時刻を用いて推定値を出します。`status=measured` は、指定した **全 slot** で組み立て枚数が増加し、チェックを通過したことだけを意味します。増えない slot があれば `no_progress` で終了コード 3、stream がない／接続や世代が変わる／カウンターが減る・厳密に表せない等の場合は `invalid` で終了コード 2 です。
- 同一のプロセス PID・生成時刻・メイン HWND、status の connection UUID、選択視点、stream ID を各サンプルで確認します。同一プロセス内に複数 GameInstance があると同じ control directory を共有します。異なる connection UUID の応答が混入した区間は拒否しますが、結果が示すのは応答した単一 GameInstance であり、プロセス内の全 GameInstance の性能ではありません。サンプル間に変化して元に戻るケースも検出できません。
- コンテナ ID・起動時刻・再起動回数は前後で一致させます。クライアントの `server_certificate_sha256` はその接続に設定した **expected pin** であり、独立して観測した相手証明書ではありません。暗号化された有効な観戦セッションの状態、期待 pin とコンテナ起動ログの pin 一致、サーバーログに同一 subject の inspector 認証・参加が存在することを併せて検査します。認証・参加の証拠は集計のみで、選んだ PID と結びつく証拠ではありません。クライアントとコンテナが同じ鍵を使っているだけでは、その接続が当該コンテナに到達していることも厳密には証明できません。ログの subject や生ログは診断結果に出しません。
- リモート HeteroCloud サーバーの実行個体を識別する手段は、この CLI にありません。対象がローカル Docker であることを確認してから使用してください。
