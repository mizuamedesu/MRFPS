# MRFPS 管理パネル

管理画面は `https://mrfps-admin.mizuame.app/`。Basic 認証の ID とパスワードは Worker secrets `MRFPS_ADMIN_USERNAME` / `MRFPS_ADMIN_PASSWORD` と GitHub Actions secrets に設定する。Worker、コード、コンテナに秘密鍵は置かない。

## 鍵と発行の仕組み

1. 管理画面で「鍵を作成」を押す。Ed25519 の秘密鍵は抽出できない `CryptoKey` として、このブラウザの IndexedDB に保存する。鍵作成時の一時的な PKCS#8 データは AES-256-GCM で暗号化し、非抽出鍵として再インポートする。暗号鍵はパスフレーズと PBKDF2-SHA256（310,000回）から導出する。
2. 「バックアップ」で暗号化 JSON を保存する。復元パスフレーズとバックアップを別々に安全に保管する。両方を失うと鍵を復元できない。公開鍵は画面からコピーでき、コンテナには PEM または `MRFPS_AUTH_PUBLIC_KEY_B64` を設定する。
3. 実際に動いているサーバーの起動ログから `MRFPS_AUTH_TRANSPORT_READY fingerprint=...` の 64 桁を入力して「保存」。サーバー再起動で証明書が変わった場合は、新しい指紋に更新して発行し直す。
4. 「発行開始」後、プレイヤー・観戦・管理者それぞれの 6 桁コードを表示。各コードの寿命は最大30秒で、交換は1回限り。交換されたら画面が次のコードを作る。秘密鍵がローカル限定のため、発行中はこの画面を開いたままにする。別の発行者が有効なコードを持っている場合は勝手に上書きせず、「他端末から切替」で明示的に引き継ぐ。

コード交換後の JWT は別の有効期間を持ち、現行サーバー仕様の上限である最大1時間。期限到達後は再ログインが必要。交換時は、その時点の公開鍵と署名済み JWT だけを HTTPS で送る。クライアントと専用サーバーがそれぞれ署名を検証し、サーバーがトークンの再使用を拒否する。公開鍵の初回配布は管理ドメインの TLS を信頼する。プレイヤー2人・アドミン1人の上限は専用サーバー側で扱う。

`panel-control open` の専用 Chromium はバックグラウンドでのタイマー抑制を抑える起動オプションを付けるが、OS のスリープ・ブラウザの破棄・将来のブラウザ挙動を防ぐ保証ではなく、長時間の実測は未実施。安定して継続発行したい場合は同じ暗号化バックアップを使う `admin-cli watch` を実行し、PC のスリープを避ける。一般ブラウザで画面を開く場合も、非表示時のタイマー抑制によりコード更新が途切れることがある。

## この Windows PC の鍵を GUI で開く

鍵は OneDrive 配下には置かず、Windows ユーザー専用の ACL を付けた `%LOCALAPPDATA%\MRFPS\Admin` にある。

| 内容 | 場所 |
|---|---|
| ブラウザの IndexedDB | `%LOCALAPPDATA%\MRFPS\Admin\browser-profile` |
| 暗号化バックアップ | `%LOCALAPPDATA%\MRFPS\Admin\issuer.encrypted.json` |
| 復元パスフレーズ | `%LOCALAPPDATA%\MRFPS\Admin\issuer-passphrase.txt` |
| サーバーに渡す公開鍵 | `%LOCALAPPDATA%\MRFPS\Admin\issuer-public.pem` |
| CLI 用 Basic 認証 | `%LOCALAPPDATA%\MRFPS\Admin\basic-credentials.json` |

パスフレーズを知る人と暗号化バックアップを扱える人は署名できる。上記の同じフォルダに置いたままのものは利便性のためのローカル控えであり、PC の故障や紛失に備える場合は別々の安全な場所に保管する。

リポジトリの `Deploy/CloudflareAdmin` で `npm ci` を実行し、同じ専用プロファイルを通常のブラウザ画面で開く：

```powershell
node panel-control.mjs open --profile "$env:LOCALAPPDATA\MRFPS\Admin\browser-profile"
```

ログイン後は通常の UI を操作する。専用プロファイルを開いている間は、そのプロファイルを操作する自動コマンドを同時に実行できない。閉じるとコードは最長30秒で無効となる。

別のブラウザや Mac では管理画面を開き、鍵を新規作成する前に「復元」で暗号化 JSON を選び、別途保管したパスフレーズを入力する。同じ公開鍵が表示されることを確認する。ブラウザのサイトデータを削除すると IndexedDB の鍵は消えるが、バックアップとパスフレーズがあれば復元できる。

## コマンド操作

`panel-control.mjs init` は専用ブラウザで実画面の鍵生成・バックアップ取得までを実行する。鍵や一致するバックアップがあれば再利用し、違う鍵のバックアップを上書きしない。`fingerprint` は同画面の指紋保存、`status` は状態確認。各コマンドは繰り返し実行可能。`open` 以外は `--credentials-file` と、必要に応じ `--passphrase-file` を指定する。`--browser` で Chromium 実行ファイルを指定可能。

```powershell
$adminDir = Join-Path $env:LOCALAPPDATA 'MRFPS\Admin'
node panel-control.mjs init --profile "$adminDir\browser-profile" `
  --credentials-file "$adminDir\basic-credentials.json" --passphrase-file "$adminDir\issuer-passphrase.txt" `
  --backup-file "$adminDir\issuer.encrypted.json" --public-key-file "$adminDir\issuer-public.pem"
node panel-control.mjs fingerprint --profile "$adminDir\browser-profile" `
  --credentials-file "$adminDir\basic-credentials.json" --fingerprint '<server fingerprint>'
```

暗号化バックアップを同じ画面に復元するには、上の `init` を `restore` に替えて同じ `--profile`, `--credentials-file`, `--passphrase-file`, `--backup-file`, `--public-key-file` を指定する。同じ鍵の復元は既存の発行を停止せず終了する。別の鍵が既にある場合は上書きしない。意図して交換する場合は `--replace true` を付けると、パスフレーズと鍵を検証し、全発行を停止してから置き換える。交換後は新しい公開鍵を専用サーバーに設定し直す必要がある。GUI を開いたままでは同じプロファイルへの復元コマンドは実行できない。

GUI と同じ鍵で、ブラウザを起動せずコードを用意する場合：

```powershell
$stateDir = Join-Path $adminDir 'cli-issuer'
node admin-cli.mjs ensure --key-file "$adminDir\issuer.encrypted.json" `
  --passphrase-file "$adminDir\issuer-passphrase.txt" --credentials-file "$adminDir\basic-credentials.json" `
  --state-dir $stateDir --fingerprint '<server fingerprint>'
```

`ensure` は同じ発行者・鍵・指紋の有効な未使用コードがあれば再利用し、期限切れまたは交換後だけ更新する。コードは専用ディレクトリ内の `player.code`、`inspector.code`、`admin.code` に保存し、標準出力にはコードや JWT を出さない。Windows は現在のユーザー専用 ACL、POSIX は `0700` ディレクトリを設定する。指定ディレクトリはこの CLI 専用にする。状態が異なる別の発行者を勝手に奪わず、切替には `--takeover true` を明示する。`watch` は同じ引数で自動更新を継続し、`stop --credentials-file ... --state-dir ...` は自分の発行コードだけを無効化する。どちらも同じ設定で再実行できる。コードファイルは資格情報として扱い、通常ログ・Git・共有フォルダに置かない。

`stop-all --credentials-file ... --state-dir ...` は GUI、別端末、CLI を含む現在の全発行者を停止する。各発行者は停止を検知するとコードを消し、同じ発行者 ID での再発行は Worker が拒否する。再開には画面の「発行開始」か CLI の `ensure` / 新しい `watch` を明示的に実行する。別端末から停止された CLI のローカル状態がまだ更新されていない場合、最初の `ensure` は停止を検知して安全に終了し、次の `ensure` で新しい発行者を作る。

`watch` は通信タイムアウトや一時的なサーバー障害から自動再試行し、停止指示を確認してから発行を続ける。`local-status --state-dir ...` で連続エラー数と再試行状態を見られる。障害中はコードが期限切れになり得る。通信できない状態で `stop` や `stop-all` を実行した場合はローカル発行を停止するが、遠隔停止が確認できるまで保留状態となる。同じ停止コマンドを通信回復後に再実行して確認するまで、新規発行は受け付けない。

CLI のプロセスがロック回収中に強制終了すると `lock.recovery` が残り、その後のロック回収は安全のため停止することがある。対象の状態ディレクトリを使う CLI が実行中でないことを確認してから、そのファイルだけを手動で削除する。

新 Worker への更新中に旧画面や旧 CLI が開いていた場合、その発行者には新規コード発行を許可しない。既に発行済みのコードは最長30秒で失効する。画面を再読み込みし、発行開始を押すか、新 CLI で再開する。異なる発行者の有効コードが残っている間は、その期限を待つか意図的に「他端末から切替」または `--takeover true` を使う。

`exchange-test --role player --code-file <player.code>` は交換・署名検証を行うが、そのコードを消費するため実クライアントに入力する前には実行しない。`exchange-export` は `--operation-id <UUIDv4> --code-file ... --role ... --public-key-file ... --fingerprint ... --state-dir ...` を受け、署名・公開鍵・役割・指紋を検証して同ディレクトリにJWTを保護保存する。出力にはパスだけを出す。同じ操作IDの再実行は保存済みトークンを再利用し、途中で結果が不明になれば安全に停止する。使用後は `exchange-clean --operation-id <UUIDv4> --state-dir ...` で削除する。`status --credentials-file ...` は Worker の発行状態を返す。Mac では状態を `~/Library/Application Support/MRFPS/admin-cli` など端末内の専用ディレクトリに置き、バックアップとパスフレーズは安全な方法でローカルへ移す。

## API とデプロイ

`POST /api/device/exchange` は `{"role":"player|inspector|admin","code":"000000"}` を受け付け、成功時に `token`、`public_key`、`expires_at`（JWT の UTC 秒）を返す。コードは文字列。期限切れ・使用済みは 401、失敗回数の制限は 429。管理 API は Basic 認証が必要で、変更リクエストは JSON と同一 Origin に制限する。SQLite Durable Object で交換と無効化を直列化し、IP 単位と全体の失敗回数を制限する。短命 JWT は交換まで一時的に Durable Object に保存し、有効期限後に削除する。秘密鍵と復元パスフレーズは送らない。

発行 ID ごとの内容と使用状態はコード期限まで保持する。同じ発行を再送しても、交換済みコードの JWT は復元されない。異なる内容で同じ ID を使うと拒否し、別のコードへの置換や発行停止後に古い発行を再送しても元に戻さない。更新前に発行済みだったコードは期限まで交換できるが、内容確認に必要な記録がないため同じ ID での再発行は拒否する。応答が失われた発行がすでに交換済みなら、新しい ID とコードを作成する。状態が別の発行へ変化した場合は上書きせず停止する。

発行停止は未交換のコードを無効にする操作であり、すでに交換された JWT や接続済みセッションを個別に失効させる機能ではない。JWT は最大1時間有効で、サーバーは期限時に接続を終了する。初回のクライアントは HTTPS 認証先から返された公開鍵を保存し、以後の鍵変更を拒否するため、パネルで表示した公開鍵とコンテナに設定した公開鍵を一致させる必要がある。IP ごと・全体の失敗制限に達すると正しいコードや Basic 認証も一時的に拒否される。

ローカル試験は Node.js 24 で `npm test` を実行する。SQLite の in-memory fixture は外部サービスに接続せず、実際の Durable Object メソッドと CLI 復旧処理を検査する。HTTP 統合試験は `.dev.vars` にテスト専用 Basic 認証値を設定してから `npm run dev`、別のターミナルで `npm run test:integration`、`npm run test:watch`、`npm run test:panel` を順番に実行する。統合試験は発行と停止を行うため、本番や他の試験と同時に動かさない。開発用設定は本番カスタムドメインを持たず、loopback のみ HTTP を許す。本番は `npx wrangler deploy --dry-run`、`npx wrangler deploy`。`mrfps-admin.mizuame.app` はカスタムドメインとして `wrangler.jsonc` に指定。

旧 Worker からの移行試験 `npm run test:migration` は旧リビジョンの Git 履歴を必要とし、別の一時 Wrangler とデータストアを使う。`npm run test:locking` は独立した一時ディレクトリと HTTP fixture を使い、クラッシュ後の並列ロック回収を確認する。

今回の発行 receipt を追加する直前の Worker からは、Windows 上で `npm run test:receipt-migration` を実行し、未使用・使用済みコードを含む一時ローカルストアの再起動と移行を確認できる。Git の旧リビジョンを読み、2 回のローカル Wrangler を順番に専用メモリガード下で実行する。本番環境には接続しない。

GitHub Actions は secrets `MRFPS_ADMIN_USERNAME`、`MRFPS_ADMIN_PASSWORD`、`CLOUDFLARE_ACCOUNT_ID`、`CLOUDFLARE_API_TOKEN` と repository variable `MRFPS_ADMIN_CI_DEPLOY=true` が必要。現在は前者3つのみ設定し、API token 未設定のまま有効にしない。Cloudflare の [GitHub Actions の手順](https://developers.cloudflare.com/workers/ci-cd/external-cicd/github-actions/) に従って対象アカウントとゾーンに限定した `Edit Cloudflare Workers` トークンを作成し、`gh secret set CLOUDFLARE_API_TOKEN` に対話入力後、`gh variable set MRFPS_ADMIN_CI_DEPLOY --body true` で有効化する。現在の Wrangler OAuth はトークン管理 API で 403 のため、自動作成できない。Worker secrets はワークフローが安全な一時ファイルから設定する。

発行端末と Worker の時計に最大5秒の進み差がある場合でも発行を受け付け、保存されるコードの期限はサーバー側で最大30秒に制限する。応答には実際の期限とサーバー時刻が含まれ、パネルは経過時間から表示を消す。応答喪失後の再試行では元のリクエストの receipt が一致することを確認してから期限を回復する。実際の公開 Worker への反映と実機接続は別途検証する。

## 接続中の参加者

専用サーバーは認証済みの player / inspector / admin について、名前、接続 ID と UE ネットワークの ACK に基づく Ping を管理パネルに送信する。サーバー内の対戦 bot も一覧に表示するが、ネットワーク接続がないため Ping は「不明」。Ping がまだ実測されていない場合、または最後の計測から 5 秒を過ぎた場合も「不明」。HTTP の応答時間や推測値で代用しない。Worker がサーバーの最新状態を 10 秒受け取れなければ「サーバー未接続」と表示する。時刻が未来の値も「不明」として扱う。ブラウザーでの表示も期限を過ぎれば自動的に消える。

送信先は `https://mrfps-admin.mizuame.app/api/server/roster` に固定。専用サーバーの `MRFPS_ROSTER_HMAC_KEY` と、Worker secret の同名値に、同じランダム 32 バイトの小文字 hex 64 桁を設定する。UE は生 JSON に HMAC-SHA256 を付け、Worker は署名・送信時刻・起動ごとの識別子・連番を検証する。前の起動からの遅延リクエストと、同一連番の重複は受け付けない。新しい起動は旧サーバーの最終受信から 10 秒後に引き継ぐ。旧起動の拒否記録は有効な鍵の間は削除せず、記録が 4096 件に達した場合は新しい起動を受け付けない。安全に復旧するには鍵を両側でローテーションしてから拒否記録を管理者が消去する必要がある。送信元の時刻は Worker より最大 2 秒進み、10 秒遅れまで受け付ける。TLS 検証は必須。管理者 Basic 認証を通った `GET /api/admin/participants` のみ一覧を返す。

参加者のスナップショットは最大 4096 人／UTF-8 512 KiB。超過時は不完全な一覧を送らず、サーバーログに `MRFPS_ROSTER_SNAPSHOT_LIMIT` を記録する。ゲームは継続し、管理パネルの一覧は期限後に未接続になる。この表示上限はゲームの観戦接続数を制限するものではない。

鍵は Git やコマンド引数に置かず、以下の冪等コマンドでアクセス制限付きのローカルディレクトリに一度だけ生成する。出力はファイルパスのみで、鍵の値を表示しない。既存ファイルが不正な場合は上書きしない。

```powershell
node Deploy/CloudflareAdmin/admin-cli.mjs roster-key-ensure --state-dir "$env:LOCALAPPDATA\MRFPS\Admin\flash-roster"
node Deploy/CloudflareAdmin/admin-cli.mjs participants --credentials-file "$env:LOCALAPPDATA\MRFPS\Admin\basic-credentials.json"
```

署名鍵バックアップと同じパスフレーズを使い、Downloads に roster 鍵の暗号化バックアップを作成できる。既存バックアップが同じ鍵なら再利用し、別の鍵なら上書きしない。平文の鍵は Downloads に保存されない。

```powershell
node Deploy/CloudflareAdmin/admin-cli.mjs roster-backup --state-dir "$env:LOCALAPPDATA\MRFPS\Admin\flash-roster" --passphrase-file "$env:LOCALAPPDATA\MRFPS\Admin\issuer-passphrase.txt" --backup-file "$env:USERPROFILE\Downloads\MRFPS-roster-key-backup.json"
node Deploy/CloudflareAdmin/admin-cli.mjs roster-restore --state-dir "$env:LOCALAPPDATA\MRFPS\Admin\flash-roster" --passphrase-file "$env:LOCALAPPDATA\MRFPS\Admin\issuer-passphrase.txt" --backup-file "$env:USERPROFILE\Downloads\MRFPS-roster-key-backup.json"
```

バックアップは `PBKDF2-SHA256` 310000 回、ランダム salt と AES-256-GCM で保護する。復元時に既存の鍵が異なる場合は上書きせず失敗する。パスフレーズは別途安全に保管する。

Worker secret はこのファイルから標準入力で `wrangler secret put MRFPS_ROSTER_HMAC_KEY` に渡す。Flash 側は既存 direct service と UDP port を維持し、同じ鍵を環境変数に渡す。HTTP 送信のため Cloudflare 宛ての egress が必要。Worker と Linux 専用サーバーの両方を更新し、実環境で確認するまでは本番動作を保証しない。鍵を変更する場合は両側を同じ値に更新する。

本番の確認では同じ service state と image digest を使う。`flash-runtime` は所有する service・稼働中 Pod・PID・process start・健康状態・DTLS 指紋を二重に確認し、二回の確認で現在の PID に紐付く送信成功 instance が一致した場合だけ `roster_instance` を返す。一致しない場合や送信成功前は null であり、完了とはみなさない。`participants` の `instance` がその値と一致し、各対象の `ping_ms` が null でないことを別途確認する。これは参加者側の実接続と Ping の確認であり、Flash の外向け UDP 到達性をそれだけで保証しない。

```powershell
node Deploy/CloudflareAdmin/flash-runtime.mjs --state Saved/Deployments/Flash/service.json --expected-image ghcr.io/<owner>/<server>@sha256:<digest>
node Deploy/CloudflareAdmin/admin-cli.mjs participants --credentials-file "$env:LOCALAPPDATA\MRFPS\Admin\basic-credentials.json"
```
