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

GUI と同じ鍵で、ブラウザを起動せずコードを用意する場合：

```powershell
$stateDir = Join-Path $adminDir 'cli-issuer'
node admin-cli.mjs ensure --key-file "$adminDir\issuer.encrypted.json" `
  --passphrase-file "$adminDir\issuer-passphrase.txt" --credentials-file "$adminDir\basic-credentials.json" `
  --state-dir $stateDir --fingerprint '<server fingerprint>'
```

`ensure` は同じ発行者・鍵・指紋の有効な未使用コードがあれば再利用し、期限切れまたは交換後だけ更新する。コードは専用ディレクトリ内の `player.code`、`inspector.code`、`admin.code` に保存し、標準出力にはコードや JWT を出さない。Windows は現在のユーザー専用 ACL、POSIX は `0700` ディレクトリを設定する。指定ディレクトリはこの CLI 専用にする。状態が異なる別の発行者を勝手に奪わず、切替には `--takeover true` を明示する。`watch` は同じ引数で自動更新を継続し、`stop --credentials-file ... --state-dir ...` は自分の発行コードだけを無効化する。どちらも同じ設定で再実行できる。コードファイルは資格情報として扱い、通常ログ・Git・共有フォルダに置かない。

`exchange-test --role player --code-file <player.code>` は交換・署名検証を行うが、そのコードを消費するため実クライアントに入力する前には実行しない。`exchange-export` は `--operation-id <UUIDv4> --code-file ... --role ... --public-key-file ... --fingerprint ... --state-dir ...` を受け、署名・公開鍵・役割・指紋を検証して同ディレクトリにJWTを保護保存する。出力にはパスだけを出す。同じ操作IDの再実行は保存済みトークンを再利用し、途中で結果が不明になれば安全に停止する。使用後は `exchange-clean --operation-id <UUIDv4> --state-dir ...` で削除する。`status --credentials-file ...` は Worker の発行状態を返す。Mac では状態を `~/Library/Application Support/MRFPS/admin-cli` など端末内の専用ディレクトリに置き、バックアップとパスフレーズは安全な方法でローカルへ移す。

## API とデプロイ

`POST /api/device/exchange` は `{"role":"player|inspector|admin","code":"000000"}` を受け付け、成功時に `token`、`public_key`、`expires_at`（JWT の UTC 秒）を返す。コードは文字列。期限切れ・使用済みは 401、失敗回数の制限は 429。管理 API は Basic 認証が必要で、変更リクエストは JSON と同一 Origin に制限する。SQLite Durable Object で交換と無効化を直列化し、IP 単位と全体の失敗回数を制限する。短命 JWT は交換まで一時的に Durable Object に保存し、有効期限後に削除する。秘密鍵と復元パスフレーズは送らない。

ローカル試験は `.dev.vars` にテスト専用 Basic 認証値を設定してから `npm test`、`npm run dev`、別のターミナルで `npm run test:integration`。開発用設定は本番カスタムドメインを持たず、loopback のみ HTTP を許す。本番は `npx wrangler deploy --dry-run`、`npx wrangler deploy`。`mrfps-admin.mizuame.app` はカスタムドメインとして `wrangler.jsonc` に指定。

GitHub Actions は secrets `MRFPS_ADMIN_USERNAME`、`MRFPS_ADMIN_PASSWORD`、`CLOUDFLARE_ACCOUNT_ID`、`CLOUDFLARE_API_TOKEN` と repository variable `MRFPS_ADMIN_CI_DEPLOY=true` が必要。現在は前者3つのみ設定し、API token 未設定のまま有効にしない。Cloudflare の [GitHub Actions の手順](https://developers.cloudflare.com/workers/ci-cd/external-cicd/github-actions/) に従って対象アカウントとゾーンに限定した `Edit Cloudflare Workers` トークンを作成し、`gh secret set CLOUDFLARE_API_TOKEN` に対話入力後、`gh variable set MRFPS_ADMIN_CI_DEPLOY --body true` で有効化する。現在の Wrangler OAuth はトークン管理 API で 403 のため、自動作成できない。Worker secrets はワークフローが安全な一時ファイルから設定する。
