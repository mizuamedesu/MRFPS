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
