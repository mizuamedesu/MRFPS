# Native authentication contract

運用の鍵生成・発行・Docker・Quest手順は [Auth.md](Auth.md) を参照してください。本書はUE側の検証、資格情報の扱い、検証結果を記録します。

## 信頼の配置

発行者のEd25519秘密鍵はオフライン発行ホストだけが保持します。専用サーバーは次の環境変数のうち、非空の値を**ちょうど一つ**受け取ります。

| 変数 | 内容 |
|---|---|
| `MRFPS_AUTH_PUBLIC_KEY_B64` | SPKI形式のPUBLIC KEY PEM全体を標準base64で符号化 |
| `MRFPS_AUTH_PUBLIC_KEY_PEM` | SPKI形式のPUBLIC KEY PEMそのもの |
| `MRFPS_AUTH_AUDIENCE` | 対象サービス。既定値 `mrfps` |

未指定、二重指定、不正base64、Ed25519以外、秘密鍵PEM、複数鍵、末尾の余分なデータは拒否します。前に読み込んだ鍵も無効化し、開発用鍵の自動生成や匿名接続へ切り替えません。

デバイスコードのクライアントは HTTPS の認証サービスから公開鍵と署名済み JWT を受け取り、初回に公開鍵を端末へ固定します。公開鍵が変更された場合は拒否し、明示的な信頼解除を必要とします。`UMRFPSAuthClientSubsystem` は公開鍵による署名検証を完了してから JWT を保持し、署名された DTLS 証明書 fingerprint を採用します。秘密鍵はクライアントにも渡しません。旧来どおり公開鍵と JWT を別ファイルから用意する方法も利用できます。詳しくは [デバイスログイン](DeviceLogin.md) を参照してください。

| クライアント指定 | Windows既定値 | Android既定値 |
|---|---|---|
| `-MRFPSAuthPublicKeyFile=` | `%LOCALAPPDATA%/MRFPS/Auth/issuer-public.pem`（Windows） | アプリ内部の `files/MRFPS/issuer-public.pem` |
| `-MRFPSAuthTokenFile=` | `%LOCALAPPDATA%/MRFPS/Auth/access.jwt`（Windows） | アプリ内部の `files/MRFPS/access.jwt` |
| `-MRFPSAuthAudience=` | `mrfps` | `mrfps` |

JWTは反射・UPROPERTY・replication・GameUserSettings・コマンドライン・ClientTravel URLへ保存しません。公開鍵ファイルは1024byte、tokenファイルは4096byteまでを開いたファイルハンドルから上限付きで読みます。保持するJWT自体は2048byteまでです。`ClearAccessToken` とSubsystemの終了時に保持領域を消去します。これはプロセス内の他の一時コピーまで完全消去したという保証ではありません。

## 署名されたプロファイル

ヘッダーは `{"alg":"EdDSA","typ":"JWT"}` の二つの項目だけです。payloadは次の九つだけを受け付けます。

| 項目 | 制約 |
|---|---|
| `iss` | `mrfps-issuer` と一致 |
| `aud` | 設定された対象と完全一致。先頭alnum、以降 `[A-Za-z0-9._:/-]`、1〜128文字 |
| `sub` | 先頭alnum、以降 `[A-Za-z0-9._-]`、1〜64文字 |
| `role` | `player`、`inspector`、`admin` のいずれか。要求された参加ロールと一致 |
| `iat`, `nbf`, `exp` | 非負の整数UTC秒。`iat <= nbf < exp`、有効期間1〜3600秒 |
| `jti` | 正規形の小文字UUIDv4。接続ごとに新規発行 |
| `dtls_fp` | 対象サーバーの稼働中DTLS証明書のSHA256、64桁小文字hex |

base64urlはpaddingなしの正規形に限定し、余ったbitも検証します。三つ以外のsegment、空segment、非ASCII、重複JSON項目、未知の項目、escaped string、配列・object・null・boolによる型置換、浮動小数・指数表記・leading zero・整数overflowを拒否します。JSONの許容範囲は、安全なASCII文字列と整数だけを持つ平坦なobjectです。ヘッダーの復号上限は256byte、payloadは1024byte、署名は64byteです。

UE5.8の実際の `SSL.Build.cs` / `OpenSSL.Build.cs` が指定するOpenSSL **1.1.1t** をリンクし、`EVP_PKEY_ED25519`、`EVP_DigestVerifyInit(..., nullptr, ..., Key)`、one-shot `EVP_DigestVerify` を使用します。署名アルゴリズムの切替・fallbackは実装していません。APIの使い方は [OpenSSL Ed25519](https://docs.openssl.org/1.1.1/man7/Ed25519/)、アルゴリズム固定とclaim検証の根拠は [JWT Best Current Practices](https://www.rfc-editor.org/rfc/rfc8725.html) を参照してください。

時刻検証の許容差は固定5秒です。クライアントの `HasAccessToken` とゲームの戦闘権限は `exp` のUTC秒で失効します。サーバーは失効したプレイヤー・観戦者を切断対象にし、再接続には新しい `jti` のtokenが必要です。

## UE APIと予約

`FMRFPSAuthVerifier` はGameModeごとに一つ保持します。

```cpp
FString Error;
FMRFPSAuthClaims Claims;
Verifier.InitializeFromEnvironment(Error);
Verifier.SetExpectedServerFingerprint(ActualServerFingerprint, Error);

// PreLogin: 署名・用途・時刻の検証、続いて空き枠確認。
Verifier.VerifyOnly(Token, ExpectedRole, Claims, Error);
// 空き枠を確認した後、再検証してjtiを原子的に予約。
Verifier.VerifyAndReserve(Token, ExpectedRole, Claims, Error);
```

各戻り値を確認して拒否・起動失敗へ反映します。`VerifyOnly` はreplay状態を変更・検査しません。`VerifyAndReserve` は署名をもう一度検証し、FCriticalSection内で重複検査と登録を一体に行います。変更可能なclaimsだけを渡して予約できるAPIはありません。

予約は最大65536件です。受理猶予中の再利用を防ぐため `exp + 5` まで保持し、未失効の予約を追い出して空きを作りません。満杯なら拒否します。予約はプロセス内で保持し、起動時の初期化・Resetで消去されます。実サーバーは起動ごとに独立したDTLS証明書を作るため、再起動後は古い `dtls_fp` のtokenも拒否されます。

`FMRFPSAuthClaims` は `Subject`, `Role`, `Issuer`, `Audience`, `TokenId`, `ServerFingerprint`, `IssuedAt`, `NotBefore`, `ExpiresAt` を返します。JWT本文や署名そのものは返しません。

クライアントAPIは `SetAccessToken`, `HasAccessToken`, `ValidateForRole`, `GetServerFingerprint`, `ClearAccessToken`, `ReloadTokenFile` です。token取得はC++のlogin injector専用で、Blueprintに公開しません。

## 平文challengeとログ

`UMRFPSAuthenticatedLocalPlayer::GetGameLoginOptions` は現在のGameInstanceが所有する **PendingNetGame → NetDriver → ServerConnection** だけを参照します。その接続の `IsEncryptionEnabled()` がtrueになるまでは資格情報を返しません。前のworldや既存sessionのNetDriverへfallbackしません。早すぎる平文 `NMT_Challenge` には空のoptionsと `AUTH_ENCRYPTION_REQUIRED` だけを返します。

JWTはこの検査後のNMT_Loginにだけ追加します。ClientTravel URLの `EncryptionToken=mrfps-dtls-v1` は公開のプロトコル識別子で、資格情報ではありません。署名検証だけを暗号化の証明として扱いません。実際のDTLS通信の確認は接続試験とpacket captureで別途行います。

UE標準の `World.cpp` はlogin optionsやinvalid URLをLogNetに出すため、`FMRFPSAuthLogPrivacy` はLogNet・LogNetTraffic・LogHandshakeをlogin前に抑止します。module起動、engine初期化完了、server設定読込、client初期化、login直前に適用します。別の認証ログには安全な結果code、要求ロール、認証済みsubject、失効時刻だけを残します。

主なcodeは `AUTH_MISSING`, `AUTH_INVALID`, `AUTH_EXPIRED`, `AUTH_AUDIENCE`, `AUTH_ROLE`, `AUTH_REPLAY`, `AUTH_CONFIG`, `AUTH_CAPACITY`, `AUTH_TRANSPORT` です。tokenや秘密鍵の一部をerrorへ含めません。

## 検証結果

2026-10-28、Windowsで実施:

| 検証 | 確認した範囲 |
|---|---|
| `Plugins/MRFPSAuth/Tests/run_core_tests.py` | 実際のparserとengine OpenSSLをMSVC `/W4 /WX` でコンパイル・実行。**3061チェック合格**。うち3000件は不正入力。署名偽造、alg/type変更、重複JSON、時刻、用途、role、DTLS binding、鍵種別も含む |
| Python → native相互運用 | `tools/auth` が独立に発行したJWTと公開鍵ファイルをactual C++ verifierへ渡し **`MRFPS_AUTH_INTEROP_OK`**。issuerの秘密鍵は共有していない。pinは試験専用の `a` 64文字で、実DTLS handshakeの試験ではない |
| `Tests/run_ue_compile_checks.py` | 実UBT生成の定義・ヘッダー・shared PCHでauthの7翻訳単位を個別コンパイル。すべてExit0。OpenSSLのUI型はengine本体と同じ `UI_ST` の局所renameでUE namespaceとの衝突を回避 |
| UE automation | `Saved/Validation/Automation/MRFPS/index.json` の実MRFPS suite **10件すべてSuccess**、うち `MRFPS.Auth.*` は4件。対応run-reportは2026-10-27T16:30:04.0896192Z終了、Exit0。client診断保持と追加assertionを含む |
| 実UE AuthAdmission | `Saved/Validation/AuthAdmission-20261028-014040/report.json`、2026-10-27T16:42:18.5451524Z、**Passed=true**。有効player/inspector接続、両role合計14negative case合格、ログ上 `NoCredentialLeak` / `NoFatalError` / `ValidClientsRemainAlive` はtrue |
| 実UE Multiplayer | `Saved/Validation/Multiplayer-20261028-014339/report.json`、2026-10-27T16:44:29.0625017Z、**Passed=true**。署名tokenを使った2対戦者＋3観戦者、27チェックすべてtrue、ログ上credential漏出・fatalなし。NullRHIのため描画・MR・物理動作は範囲外 |

UE automationには `MRFPS.Auth.AdmissionAndReplay`, `AtomicReservation`, `EnvironmentFailsClosed`, `ClientVerifiedCredentials` を用意しています。8つの並行要求で1つだけがjtiを予約する検査、実署名済みcredentialが読み込まれた状態でも暗号化されたpending接続なしではJWTを出さない検査を含みます。ファイル資格情報の試験はAuth subsystemだけを初期化し、ハードウェアSDKを起動しません。

clientの読み込み時に不正署名・期限切れ・audience違いを検出した場合、次のjoin要求でも元の安全な診断codeを保持します。credentialの未指定・明示削除・空または欠落したtokenファイルは `AUTH_MISSING` のままです。`ClientVerifiedCredentials` はこれらの読込→joinの経路も検査します。

実AuthAdmissionの欠損・偽造・期限切れ・audience／role違いは、両roleとも正規クライアント側で拒否されています。不正JWTを悪意あるclientからserverまで送ったことは証明しません。Replayは両roleで実serverの `AUTH_REPLAY`、wrong-pinの署名済みtokenはnative DTLS証明書fingerprint検証失敗と追加joinなしを確認しました。Wrong-pin JWTをserverが解析したという意味ではありません。

`Saved/Validation/AuthExpiry-20261028-015443/report.json` は2026-10-27T16:55:24.2308945Z、`Passed=true`、8チェックすべてtrueでした。30秒の署名済みtokenでplayer・inspectorを実UE serverへ入場させ、server側の `MRFPS_AUTH_SESSION_EXPIRED` 2件、両clientの実切断、serverの継続稼働を確認しました。ログ上のcredential漏出・fatal errorもありません。試験の3プロセスは終了済みです。

本記録時点で未完了なのは、実際のDocker/Linux game serverでの接続、packet captureによる資格情報非露出の検査などです。ログの漏出検査合格をpacketの検査まで拡張しません。現在の全gateは [ImplementationStatus](ImplementationStatus.md) を参照してください。
