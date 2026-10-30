# アーキテクチャ

## 座標と追跡

長さはUE標準のcm、床Z=0、アリーナはX/Yそれぞれ-500〜500。各QuestはStage追跡空間を使い、校正時のHMDのXYとyawから `XROrigin` を変換します。原点を同じ物理マーカーに、+Xを同じ方向にそろえてください。HMDの高さを0へ潰さず、Stageの床高さを維持します。

UE5.8の手指APIが返すworld変換を一度tracking空間に戻し、独自の校正変換を一度だけ適用します。右手の手指追跡が有効な場合は指先ではなく掌の姿勢を使い、銃口を指方向と手の甲の両方に対して直交する方向へ向けます。手指追跡がない場合はOpenXRのAim poseを銃の照準として使い、その照準から保持する手の姿勢を推定します。銃の姿勢と手首の姿勢は別々に同期し、見た目、銃口位置とサーバーの発射方向には同じ銃の姿勢を使用します。左盾の法線は掌またはコントローラーから決めます。

Questの手の位置と向きはコントローラー、または光学手追跡から取得します。フラッシュグレネードの飛行と閃光の対象判定は専用サーバーで処理します。

## サーバー

`AMRFPSGameMode` は2個の対戦slotと、設定可能なN個の観戦slotを分離します。通常の3人目は `ARENA_FULL` で拒否し、`?Inspector=1` は独立した飛行カメラpawnを取得します。規定値は観戦者32人、GameSessionの全体上限34人です。上限を増やす場合は両設定をそろえます。

対戦poseは30 Hzのunreliable RPCで送信します。有限値、手の到達距離、頭の高さ、移動速度、順序、受信間隔を検査します。追跡情報は300 msで失効します。射撃RPCに命中先・ダメージ・銃口位置を受け取らず、最後に受理した手poseでサーバーがtraceを行います。

発射条件には認証済み・tokenのUTC有効期限内、対戦slot、非観戦者、Playing、校正、HP、アリーナ境界、追跡の鮮度、右手追跡、発射間隔が含まれます。頭・体・盾・壁は専用Weapon trace channelを使います。盾と壁が先に当たれば、その背後の体には当たりません。

壁はサーバー上でジェスチャーを判定してspawnし、healthと所有slotを複製します。盾もサーバー上で掌状態とエネルギーから有効化します。HP、kill/death、試合状態、復活時刻、クールダウンはサーバー管理です。

## 認証と通信

全プレイヤーと全インスペクターの参加には、オフライン発行者のEd25519署名JWTが必要です。サーバーは `MRFPS_AUTH_PUBLIC_KEY_B64` または `MRFPS_AUTH_PUBLIC_KEY_PEM` のちょうど一つから発行者の公開鍵だけを読みます。秘密鍵は発行ホストに保持し、コンテナへ渡しません。未設定・不正鍵・曖昧な設定ではnetwork admissionを開始しません。

JWTには主体 `sub`、`player` / `inspector` のrole、対象 `aud`、UTCの `iat` / `nbf` / `exp`、接続ごとに異なるUUIDv4 `jti`、対象サーバーのDTLS証明書SHA256 `dtls_fp` を含めます。ヘッダーは `alg=EdDSA, typ=JWT` に固定し、署名・型・重複項目・時刻・用途・要求role・証明書pinを検証します。空き枠を確認した後でjtiを原子的に予約し、同じtokenの再使用を拒否します。期限切れsessionは戦闘権限を失い、両roleとも切断対象になります。

アドレス指定とUEのUDP net driverを使い、その上のnative DTLSを必須にします。DTLS用の証明書と秘密鍵はサーバー起動時に独立生成するtransport identityで、JWT発行者の秘密鍵とは別です。クライアントは配備済みの発行者公開鍵でJWTを検証してから、署名された `dtls_fp` をnative handshakeへ渡します。サーバーを再起動すると証明書が変わり、接続tokenも新規発行が必要です。

ClientTravel URLには公開の `EncryptionToken=mrfps-dtls-v1` だけを載せます。JWTは現在のGameInstanceのPendingNetGameに属するServerConnectionの暗号化が有効になった後、NMT_Loginへだけ追加します。平文challengeや古いworldの接続から資格情報を送ることはありません。login optionsを出すUE標準networkログは抑止し、認証ログには安全なcodeと認証済みmetadataだけを記録します。

JWTは認証主体と参加roleを証明します。物理ハードウェアそのものの真正性をリモートサーバーへ証明する仕組みは含めていません。デスクトップ入力の許可は開発用で、MR専用サーバーは `-MRFPSRequireXR` を指定します。鍵の運用・接続例は [Auth](Auth.md)、厳格なclaim契約と検証範囲は [Authentication](Authentication.md) を参照してください。

## 観戦

各観戦者の自由カメラは所有者だけに関連し、他の観戦者にカメラ位置を強制しません。両プレイヤーのHMD poseをカメラ対象にでき、自由カメラへ戻れます。2つのSceneCaptureを512×288、10 Hzで更新し、画面内で同時に確認できます。対戦中の3DラベルはHP、盾、kill/death、準備状態を表示します。

NullRHI通信テストはpose・状態・視点変更要求の伝播を検証します。実際に描画されたカメラとラベルの検証は、描画ありの別工程です。

## ハプティクス

発射・ダメージ・復活はauthorityから所有者へのClient RPCで通知し、所有者だけがMoBitsへ送信します。spectator pawnにMoBits componentはありません。ゲームイベントでARMしません。手動ARM、STOP、アプリ側校正、認証・heartbeatは公式SDKに従います。デスクトップ／smokeでは自動接続を無効にします。

QuestはPC上のMoBitsアプリへ、ADB reverseしたloopback WebSocketで接続します。認証ファイルは端末のアプリ固有領域へ転送し、サーバーやリポジトリには保存しません。
