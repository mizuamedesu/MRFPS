# 公開APIとプロトコル v2

MoBitsアプリがBLE・トラッキング原点・電極ペア・EMS波形・感覚校正・出力の上限・物理ACK・停止確認を所有します。UE SDKはゲームの要求と姿勢データを受け渡します。Unity SDK 0.3.0と同じ通信仕様です。

## 接続

互換用の手動JSONは`control_url`、`motion_url`、`token`のみです。URLは同じポートの`ws://127.0.0.1:<port>/mobits/v2/control`と`/mobits/v2/motion`。クエリ、フラグメント、localhost名、LAN、異なるポートは拒否します。

両接続に`Authorization: Bearer <64桁hex token>`、motionにはcontrolのHELLOから得た`X-MoBits-Session: <UUID>`を付けます。`Origin`は付けません。UE 5.4の標準libwebsocketsクライアントはOriginを自動追加するため、SDKはSocketsモジュール上に小さなRFC 6455クライアントを実装しています。TCPは非同期・ノンブロッキングで、1 Tickの受信量、JSONサイズ、キューを制限しています。サーバーフレームのマスク・不正なUTF-8・未交渉の圧縮・バイナリメッセージは拒否します。

HELLOはversion 2、heartbeat 100ms、timeout 500ms。両チャンネルのsessionが一致することを確認します。アプリのserver_time_msは起動後の単調時計です。SDKも単調時計を使い、遅れたPONGで過去の姿勢が新しく見えることを防ぎます。

アプリは各チャンネル1接続のみです。UnityとUEから同じセッションへ同時接続しないでください。

## ローカルアプリの自動検出

MoBitsアプリ0.1.12以降は起動時にモーション専用セッションを用意します。Unity SDK 0.4.0の `MoBitsConnection.ConnectAsync()` / Coreの `MoBitsClient.ConnectAsync()`、UE SDK 0.2.0のBlueprint `Connect` / `Connection->Connect(Error)` は、同じOSユーザーのアプリを自動検出します。固定ポートや接続情報のコピーは不要です。アプリを終了・SDK接続を閉じた場合は発見できません。

| OS | 接続情報の場所 |
| --- | --- |
| Windows | `%LOCALAPPDATA%/MoBits/SDK/connection-v2.json` |
| macOS | `$HOME/Library/Application Support/MoBits/SDK/connection-v2.json` |
| Linux（パス契約のみ、実行環境未検証） | `${XDG_CONFIG_HOME:-$HOME/.config}/MoBits/SDK/connection-v2.json` |

JSONは `discovery_version: 1`、`protocol_version: 2`、`app_version`、`session_id`、`control_url`、`motion_url`、`token` の7フィールドです。最大4096 byte、UTF-8。アプリが一時ファイルから置換し、Unixではディレクトリ0700・ファイル0600、WindowsではユーザーのLocalAppData内に作成します。SDKは同じポートの127.0.0.1だけへ接続し、control HELLOが発見したsession_idと一致することも確認します。認証ヘッダーや既存v2通信は維持します。シーン・Config・ログ・Gitへこのファイルやトークンを保存しないでください。

切断後、アプリはSTOPを確認して新しいポート・token・session_idを公開します。再度Connectで発見し直せます。停止未確認の場合は更新せず、アプリ側で停止を再試行してください。再接続だけでArm・古い触覚要求の再実行は行いません。許可対象の接続や校正が変わった場合、新しいセッションはモーション専用へ戻ります。各セッションのcontrol/motionはそれぞれ1クライアントです。UnityとUEから同時に使うことはできません。複数のMoBitsアプリを起動した場合、最後に公開したアプリが接続先になります。

旧SDK向けの3フィールド接続JSONと手動APIは互換用として残します。通常のConnectはクリップボードを読みません。

## コマンド

SDKが`type`と正の増加IDを付け、ゲームは次の引数を指定します。

```json
{"type":"PLAY","id":3,"target":"left_upper_arm","effect":1,"level":0.15,"ttl_ms":200}
{"type":"PLAY_GLOBAL","id":4,"effect":7,"level":0.15,"ttl_ms":200}
```

| API | Wire |
| --- | --- |
| GetDevices | GET_DEVICES |
| ArmHaptics | ARM_HAPTICS |
| Play(Target, Effect, NormalizedLevel, TtlMs=200) | PLAY |
| PlayEffect(Target, EMoBitsHapticEffect, NormalizedLevel, TtlMs=200) | PLAY（enumの値をそのままeffectへ） |
| PlayGlobal(Effect, NormalizedLevel, TtlMs=200) | PLAY_GLOBAL |
| StopAll | STOP_ALL |
| Tickのheartbeat | PING + last_frame_sequence |

effectは整数1〜30、levelは有限値`(0,1]`、ttl_msは整数1〜500です。グローバルは部位指定と独立した30枠で、現在は全枠未設定です。部位指定1〜3は既存の定義（Tap / Nudge / Impact）、4〜30は未設定です。通信には名称を載せず、整数だけを送ります。

| effect | EMoBitsHapticEffect | 日本語名 | 刺激 |
| --- | --- | --- | --- |
| 1 | Tap | タップ | 短く鋭い刺激（80 ms・120 Hz） |
| 2 | Nudge | ナッジ | 軽く押すような、立ち上がりの柔らかい刺激（150 ms・80 Hz・立ち上がり20 ms） |
| 3 | Impact | インパクト | 長めで低い衝撃（200 ms・40 Hz） |
| 4〜30 | Effect04〜Effect30 | 効果04〜効果30 | 未設定。名称は仕様決定待ち |

グローバル1〜30はこのenumを使わず、名称のない整数のままです。未設定は`effect_unconfigured`を返し、EMSを発火させません。ゲームから電流・波形・デバイス配列・時系列は送れません。

| Body Target | Bone ID | Wire |
| --- | ---: | --- |
| Chest | 4 | chest |
| Waist | 5 | waist |
| Hip | 6 | hip |
| LeftUpperArm / RightUpperArm | 14 / 18 | left_upper_arm / right_upper_arm |
| LeftForearm / RightForearm | 13 / 17 | left_forearm / right_forearm |
| LeftThigh / RightThigh | 7 / 10 | left_thigh / right_thigh |
| LeftShin / RightShin | 8 / 11 | left_shin / right_shin |

BodyTargetのNone=0はBlueprintの未選択値です。送信対象として拒否します。

## 結果と停止

RESULTはId、Status、Reason、DeviceId、AppliedLevel、Succeededへ変換されます。

| Status | Succeeded | Arm状態 |
| --- | --- | --- |
| armed | true | 同じ出力世代でのみtrueへ更新 |
| device_acknowledged | true | 維持 |
| effect_unconfigured | false | 維持 |
| stopped_confirmed | true | 解除 |
| stop_unconfirmed | false | 解除、再停止可能 |
| rejected / expired | false | 現在世代のPLAYでは解除 |

AppliedLevelはアプリ側で制限した知覚レベルです。実測電流・感じた強さの測定値ではありません。device_acknowledgedはデバイスがコマンドを受理した結果です。

StopAllはローカルArmを即時解除し、出力世代を進めます。遅れて到着した古いARMは再Armできず、古いPLAYの失敗も新しい世代を変更しません。停止を通常の最大16件の保留要求とは別枠（最大8件）で送れます。送信済みのTCPバイトを取り消せるわけではなく、アプリの優先停止・キャンセル処理で制御します。

接続終了・通信停止・返信の5秒タイムアウトでは未完了のPLAYを失敗扱いにし、未確認のSTOPを`stop_unconfirmed`とします。STOP未確認時のアプリ側出力所有権は保持され、OFF確認まで再試行されます。通信を閉じただけで`stopped_confirmed`を返すことはありません。

## モーション

Wireの座標は`unity_left_handed_y_up_meters`です。SDKの公開データはUEのX前、Y右、Z上、cmです。

```text
positionUE = 100 * (z, x, y)
quaternionUE.xyzw = normalize(z, x, y, w)
```

Wireですでに反射変換済みなので、Z反転を追加しません。この循環的な軸の置換は回転行列の基底変換と同じです。クォータニオンをEuler角へ分解せず変換し、UEのFQuatで姿勢を表します。

フレームのsession / version / sequence / origin_revision / timestamp / space、有限値、単位クォータニオン、重複デバイス・ボーン、親子順、最大128デバイス・20ボーンを検証します。最新1フレームだけを保持し、Game Tickが取り込んだsequenceだけを100ms周期PINGに含めます。

GetLatestFrameは500msを超える古いフレームではfalseです。TryGetDevicePoseはsourceのsample_age_msに輸送・経過時間を加算し、500msを超える古い姿勢・tracking_active=falseではfalseにします。FrameのDevicePose.SampleAgeMsはsourceの年齢、TryGetDevicePoseのSampleAgeMsは取得時点の総年齢です。Boneデータはアプリがtracking_activeのときだけ提供し、フレーム自体の鮮度で使用可否を判断します。

position_sourceはskeleton_estimateまたはunavailableです。IMUに絶対位置の観測値はありません。推定位置の適用は明示的に有効にします。

| Bone ID | 名称 | Parent ID |
| ---: | --- | ---: |
| 6 | Hips | 0 |
| 5 / 4 / 3 | Spine / Chest / UpperChest | 6 / 5 / 4 |
| 2 / 1 | Neck / Head | 3 / 2 |
| 16 / 20 | LeftShoulder / RightShoulder | 3 |
| 14 / 18 | LeftUpperArm / RightUpperArm | 16 / 20 |
| 13 / 17 | LeftLowerArm / RightLowerArm | 14 / 18 |
| 15 / 19 | LeftHand / RightHand | 13 / 17 |
| 7 / 10 | LeftUpperLeg / RightUpperLeg | 6 |
| 8 / 11 | LeftLowerLeg / RightLowerLeg | 7 / 10 |
| 9 / 12 | LeftFoot / RightFoot | 8 / 11 |

origin_revisionの変更はT-pose、Yaw、装着位置、トラッキング開始・終了などの基準変更です。補間・基準姿勢を切り替えます。UEのWorld Origin Rebaseとは別の改訂番号です。
