# 学園祭MR FPSプリセット（SDK 0.5.0）

アプリの「MR FPS（学園祭・試作）」で調整した波形を登録し、UEのゲームイベントから呼び出します。アプリ側の `codex/ue-mr-fps-presets` も必要です。従来のmain（c95f097）では未設定の結果になります。

## アプリでの準備

1. EVT5を接続し、身体追跡の部位を **左前腕／右前腕** に割り当てます。Main 1034以上、波形ストリーム能力15・16とLRAが必要です。
2. 「EVT5 2電極」→「押し込み」→「MR FPS（学園祭・試作）」で、その装着者の強さ・校正・反動の組を調整します。被弾の「当たる組」は固定にします。
3. **この設定を学園祭SDKへ登録** を押します。もう一方の前腕も使う場合はそのデバイスで繰り返します。登録はアプリ起動中だけ有効で、再起動後、人・電極・設定の変更後には登録し直します。登録だけでは刺激しません。
4. 他の再生・準備を停止し、身体追跡の **Unity / UE5 SDK** を一度閉じます。登録内容を確認して **学園祭プリセットを準備してSDK接続** を押し、全3組の接触確認を待ちます。
5. UEで `Connect` →接続と新しいモーションフレームを確認→`Arm Haptics`。`OnReceipt` の同じRequest IDに `armed` が返った後、`Play MR FPS Preset` を呼びます。

登録中の標準SDK効果1〜3は、この学園祭専用セッションでは未設定です。通常のSDKセッションを開けば従来どおり使えます。`PlayGlobal` は別の番号空間で、学園祭プリセットには使いません。

## ゲームイベントとの対応

| `EMoBitsMrFpsPreset` | body ID | 用途 |
| --- | ---: | --- |
| `PistolRecoil` | 4 | 拳銃の1発（EMS＋振動） |
| `HeavyRecoil` | 5 | マグナム・ショットガンの1発（振動の2打目を含む） |
| `AutomaticKick` | 6 | 連射の蹴り1回（EMS＋振動） |
| `Graze` | 7 | 被弾・かすり |
| `Hit` | 8 | 命中・打撃＋脈2回 |
| `HeavyHit` | 9 | 重い一撃・打撃＋脈4回 |
| `LethalHit` | 10 | 致命傷・力が抜ける尾と振動のフェード |
| `Revive` | 11 | 復活のゾワゾワ |
| `AutomaticVibration` | 12 | 連射の振動1回（EMSなし） |
| `AutomaticShot` | 13 | 実際の射撃1発を連射列へ追加 |

復活を生成できないP0条件では、その枠を登録せず `effect_unconfigured` を返します。既存の `EMoBitsHapticEffect` の名前・保存値と整数 `Play` は変更していません。上記のIDは専用enumで扱います。

```cpp
// ARMの成功後。1.0はアプリに登録した強さ。小さい値は電流と振動を下げる。
int64 Shot = Connection->PlayMrFpsPreset(
    EMoBitsBodyTarget::RightForearm, EMoBitsMrFpsPreset::PistolRecoil, 0.25f);
int64 Damage = Connection->PlayMrFpsPreset(
    EMoBitsBodyTarget::LeftForearm, EMoBitsMrFpsPreset::Hit, 0.25f);
// 同じ部位の後のイベントはEMSと未送信の振動を置き換える。
// ゲーム終了・中断時：
Connection->StopAll();
```

`Forearm` は左／右前腕だけを受け付けます。未接続、未ARM、不正な部位・番号・レベルはローカルで0を返します。送信した要求にはRequest IDが付き、`OnReceipt` で結果を受け取ります。`device_acknowledged` はSTARTなどの通信書き込みの確認で、再生完了や体感の証明ではありません。再生中の故障は停止・切断で通知されます。

`AutomaticShot` は同じ部位の連射列へ追加します。その他の新しいイベントはEMSストリームと未送信の振動を取り消します。送信済みの振動は有限の長さで終了します。全出力を止めるには `StopAll` を使います。

STOP・切断・アプリ再起動後は、アプリで再準備してから新しい接続と明示ARMを行ってください。自動復旧はトラッキングだけを復旧し、プリセットの再登録・接触確認・ARM・PLAYを自動実行しません。`StopAll` で `stopped_confirmed` が返らない間は出力を再開しません。

## 今回のタイミングと検証範囲

既存の波形ストリームを使い、プリロード後に **400 msの先行時間** を置きます。全体の遅延には通信・プリロード時間も加わります。既定TTL 500 msはSTART要求（連射追加はバッファ確認）の期限で、波形の長さではありません。左右は独立したPLAYで、物理的な同時開始は未検証です。両腕の反動を使う場合の支える腕×0.7などは、各呼び出しのレベルを下げて指定できます。

**150 ms間隔の連射は `Play Automatic Shot`（ID 13）を使います。** VJの時計・波形の先行補充・SYNCを共用し、ストリームを作り直さず1発ずつ継ぎ足します。初回の先読み時間の後は、発射間隔を維持します。振動は毎発、EMSは1・3・5発目（300 msごと）です。`AutomaticKick` / `AutomaticVibration` を交互に呼ぶ方式ではありません。

```cpp
// 接続・新しいモーション・明示ARMの確認後。
// ゲームの実際の ShotFired イベントで1回だけ呼ぶ（通常150 ms間隔）。
Connection->PlayAutomaticShot(EMoBitsBodyTarget::RightForearm, 0.25f);
// PlayMrFpsPreset(RightForearm, EMoBitsMrFpsPreset::AutomaticShot, 0.25f) も同じ。
```

トリガを離したら呼び出しを止めます。受信済みの射撃が先読み分遅れて完了し、新しい射撃は自動生成しません。被弾など別のプリセットは連射を置き換え、次の連射は新しい列の1発目になります。全出力をすぐ止める操作は `StopAll` です（その後はアプリで再準備）。

小さなフレーム揺れを吸収しますが、140 ms未満の入力、増え続ける遅れ、連続800発超過、期限切れや通信・デバイス故障は拒否して停止します。長い連射は800発以内で区切ります。13の `device_acknowledged` は稼働中のEMSストリームのバッファ確認と、アプリ時計への射撃予約を意味します。LRAの実提示や各射撃の再生完了の証明ではありません。遅れた射撃を後からまとめて再生しません。

Addnessで目標とされている50 ms以下の反動には、反射FWの登録・発火・実機検証が別途必要です。

アプリの実際の書き出しを使ったNode/Rust試験、FWのバッファ制限を再現する模擬デバイス試験、SDKのC++ portable試験を実施しています。この変更のUE Editor/UHT/Windowsビルドと実機EMS/LRA・体感は未検証です。旧版のWindows合格を今回の合格とは扱いません。

`Samples/MoBitsExample` の `MoBitsExampleActor` には `Forearm`、`MrFpsPreset`、`TriggerMrFpsPreset`、1発ずつ追加する `TriggerAutomaticShot` を用意しています。既存の接続・ARM・STOPボタンから使え、起動時には自動出力しません。
