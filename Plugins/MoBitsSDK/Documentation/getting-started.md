# 導入

1. `MoBitsSDK`フォルダをプロジェクトの`Plugins/MoBitsSDK`へコピーします。
2. C++プロジェクトをビルドし、EditorのPluginsで「MoBits SDK」を有効にします。Blueprintだけのプロジェクトでは、C++クラスを1つ追加してビルド環境を用意してください。
3. C++から使うモジュールの`.Build.cs`に`"MoBitsSDK"`を依存として追加します。
4. Level内のActorに「MoBits Connection」コンポーネントを追加します。
5. MoBitsアプリ0.1.12以降を同じPC・同じOSユーザーで起動します。触覚を使用する場合はアプリでデバイス・部位・校正・出力許可を設定します。
6. PIE / 実行中に`Connect`を呼び出します。アプリを自動検出し、接続情報のコピーは不要です。`On State Changed = Connected`と、新しい`On Motion`の受信を確認します。
7. ユーザー操作から`Arm Haptics`を呼び、`On Receipt`の`Status = armed`を確認してから触覚要求を送ります。

接続情報はローカルのユーザーファイルから実行時に読みます。Blueprintの定数、Data Asset、Config、Git、ログへ保存しません。`Connect From Clipboard` / `Connect Session Json` は旧アプリ向け手動接続として残します。切断後はConnectを再実行し、触覚は改めてArmしてください。

## Blueprintのノード

| ノード | 引数 / 結果 |
| --- | --- |
| Connect | アプリを自動検出。接続開始のboolとError。接続完了はOnStateChangedで確認 |
| Connect From Clipboard | 旧アプリ向け手動接続 |
| Connect Session Json | 実行時JSON、接続開始のbool、Error |
| Get Devices | リクエストID、結果はOnDevices |
| Arm Haptics | リクエストID、成功後IsHapticsArmedがtrue |
| Play | Body Target、Effect整数、Normalized Level、Ttl Ms |
| Play Effect | Body Target、Effect（`EMoBitsHapticEffect`: Tap / Nudge / Impact / Effect04〜Effect30）、Normalized Level、Ttl Ms。Playと同じwire IDを送る |
| Play MR FPS Preset | 左／右前腕、`EMoBitsMrFpsPreset`、Normalized Level、Ttl Ms（既定500）。アプリでの学園祭プリセット登録・準備が必要 |
| Play Automatic Shot | 左／右前腕、Normalized Level、Ttl Ms（既定500）。実際の連射1発ごとに追加。振動は毎発、EMSは2発に1回 |
| Play Global | Effect整数、Normalized Level、Ttl Ms |
| Stop All | 即座にローカルのArmを解除し、アプリの停止結果を待つ |
| Disconnect | STOPを要求し、確認または5秒の期限後に接続を閉じる |
| Try Get Device Pose | Device ID、boolとPose。古いデータではfalse |
| Get Latest Frame | 最新のFrame。古いフレームではfalse |
| Get Haptic Effect Id / Name / Display Name | enumからwire ID、IDから英語名（"Tap" / "Effect04"）・日本語名（"タップ" / "効果04"）。範囲外は0 / 空 |

各コマンドは即座にIDを返します。`0`はローカル検証で送信しなかったことを示し、詳細は`On Error`へ届きます。非同期応答は`On Receipt`の`Id`で対応付けます。`On Devices`にはデバイスの能力・バッテリー・部位割当てが含まれます。最新一覧の取得に用いるため、この版のOnDevicesにはリクエストIDを含めません。

effectは1〜30、Normalized Levelは有限値の`(0, 1]`、Ttl Msは1〜500（既定200）です。Ttlは通信要求の鮮度であり、刺激の持続時間ではありません。

通常セッションの部位指定effectのうち1〜3には触覚名があります。既存enumの4〜30は保存互換性のため`Effect04`〜`Effect30`のままです。学園祭登録セッションの4〜13は専用enumで指定します（[MR FPSガイド](mr-fps.md)）。

| ID | EMoBitsHapticEffect | 日本語名 | 刺激 |
| --- | --- | --- | --- |
| 1 | Tap | タップ | 短く鋭い刺激（80 ms・120 Hz） |
| 2 | Nudge | ナッジ | 軽く押すような、立ち上がりの柔らかい刺激（150 ms・80 Hz・立ち上がり20 ms） |
| 3 | Impact | インパクト | 長めで低い衝撃（200 ms・40 Hz） |

enumの値はwire IDと同じです（`None`=0は非表示）。0.2.xの整数`Play`ノードと保存済みBlueprintはそのまま使えます。グローバルeffectの内容・複数デバイスでの移動感はアプリで設定します。この版では全グローバル枠が未設定です。

## C++

```cpp
#include "MoBitsConnectionComponent.h"

FString Error;
if (!Connection->Connect(Error))
{
    // Errorだけを表示。認証情報をログに出さない。
}
// OnStateChanged / OnMotionを確認後、別のユーザー操作で:
const int64 ArmId = Connection->ArmHaptics();
// OnReceipt(ArmId, "armed")を確認した後:
const int64 LocalId = Connection->PlayEffect(EMoBitsBodyTarget::LeftUpperArm, EMoBitsHapticEffect::Tap, 0.15f, 200);
// 整数IDでも同じ: Connection->Play(EMoBitsBodyTarget::LeftUpperArm, 1, 0.15f, 200);
const int64 GlobalId = Connection->PlayGlobal(7, 0.15f, 200);
const int64 StopId = Connection->StopAll();
```

SDKリポジトリ内の`Samples/MoBitsExample`には、接続・Arm・送信・停止を別々のBlueprint操作として公開するActorがあります。

## デバイス追従

Actorに「MoBits Tracked Device」を追加し、表示用Meshを子にします。Connectionに上記接続コンポーネント、Device IdにOnDevicesで受信したIDを設定します。Motion Originは任意の固定Scene Componentで、アプリのトラッキング原点をゲーム内へ配置します。

デフォルトでは回転だけを適用します。`Apply Estimated Position`を有効にすると、IMU骨格からの推定位置も適用します。絶対位置の測定値ではありません。Smoothing Secondsは既定0.03秒で、実時間に基づいて補間します。古いデータ・接続終了では現在位置を保持し、`Is Tracked`をfalseにします。

アプリでT-pose・Yaw・装着位置などの原点が変わった際には、既定で現在の見た目を維持して基準を切り替えます。`Preserve Pose On Origin Change`を無効にすると、改訂された座標へ直接追従します。`Reset Tracking Offset`で保持した差分を解除できます。

## 骨格

ActorへPoseable Mesh Componentと「MoBits Skeleton」を追加し、Target Mesh / Connectionを指定します。通常のSkeletal Mesh / Anim Blueprintに対しては`On Motion`のBonesをゲームのAnimation Blueprint / Control Rigに渡してください。

MoBits Skeletonは初回の受信姿勢と現在のリグ姿勢を基準として、20ボーンの回転差分を適用します。既定のBindingsはUE4 Mannequin系の名称です。UE5 Manny、MetaHuman、独自リグでは各骨の名称を合わせ、重複のないBone IDを指定してください。Bindingsを実行時に変更した場合は`Recalibrate`を呼びます。

リグの骨長・ボーン移動とActorの移動は保持します。`Apply Estimated Root Position`を有効にした場合だけ、Hipsの推定変位をMeshの位置へ一度だけ適用します。HipsボーンとActorの両方に位置を二重適用しません。再校正・原点改訂・追跡再開時は現在のリグ姿勢を基準にし直します。

## 終了・ポーズ

通常は`Disconnect`で停止確認まで待ちます。コンポーネントのDeactivate / EndPlay / 破棄とゲームのポーズでは接続を閉じ、アプリの独立した停止処理へ移ります。この場合、SDKは停止確認を受信できたとは扱いません。

Game Tickが500ms止まると、新しいPINGやフレーム進行の通知が送られません。アプリ側watchdogが出力を取り消します。再びTickした際にもSDKが接続をFaultedにして古い出力要求を捨てます。再開には新たな接続と明示的なArmが必要です。低速デバッグ・ブレークポイントでもこの動作になります。

## 学園祭MR FPS

アプリで調整した反動・被弾・復活は専用の `Play MR FPS Preset` ノードで呼べます。[アプリでの登録、イベント対応表、停止、タイミング](mr-fps.md)を参照してください。
