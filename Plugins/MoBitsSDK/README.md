# MoBits SDK 0.5.0

C++ / BlueprintからMoBitsアプリのトラッキングとEMS触覚を利用する、UE 5.4以降向けのソースプラグインです。

この`MoBitsSDK`フォルダをプロジェクトの`Plugins`へコピーし、ビルドしてPlugins画面で有効にします。

- [導入とAPIの使用例](Documentation/getting-started.md)
- [通信とライフサイクル](Documentation/protocol.md)

`UMoBitsConnectionComponent`が接続と触覚、`UMoBitsTrackedDeviceComponent`がデバイス追従、`UMoBitsSkeletonComponent`がPoseable Meshの姿勢適用を担当します。

```cpp
FString Error;
Connection->Connect(Error); // 同じPCのアプリを自動検出
// 接続完了・明示Armを確認してから:
Connection->PlayEffect(EMoBitsBodyTarget::LeftUpperArm, EMoBitsHapticEffect::Tap, 0.15f, 200);
// 整数でも同じ要求: Connection->Play(EMoBitsBodyTarget::LeftUpperArm, 1, 0.15f, 200);
Connection->PlayGlobal(7, 0.15f, 200);
Connection->StopAll();
```

接続情報は実行時にアプリから取得し、接続完了後に明示的にArmします。effectは整数1〜30です。通常セッションの1〜3は`EMoBitsHapticEffect::Tap` / `Nudge` / `Impact`（タップ / ナッジ / インパクト）です。既存enumの`Effect04`〜`Effect30`は保存互換性のため名前を維持します。学園祭登録セッションの4〜13は別enum `EMoBitsMrFpsPreset` で指定します。`PlayGlobal`の番号は部位指定と独立した名称のない整数です。現在、グローバルeffectは全枠未設定のため`effect_unconfigured`が返ります。

学園祭用の登録済みプリセットは [MR FPSガイド](Documentation/mr-fps.md) を参照してください（SDK 0.5.0）。

連射は `Play Automatic Shot` を実際の発射ごとに呼びます。通常150 ms間隔で、初回先読みの後は振動を毎発、EMSを2発に1回提示する列へ追加します。
