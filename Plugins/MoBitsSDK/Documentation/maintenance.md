# SDKの保守と引き継ぎ

SDK0.3.0。通信v2、C++/Blueprintの既存API、効果の整数IDを維持する。部位effect1〜3は`EMoBitsHapticEffect::Tap` / `Nudge` / `Impact`。

## 責務

- `Source/MoBitsSDK/Private/MoBitsClient.cpp`: 要求送信・受信、モーション消費、Tickの入口。
- `MoBitsClientConnection.cpp`: セッション、探索、再試行、Pause/Resume、切断・終了。
- `MoBitsClientEvents.cpp`: 通知上限、古い姿勢通知の除外、待機要求を切り離してから終了通知。
- `MoBitsProtocol.cpp`/`MoBitsCore.h`: codecと通信ルール。`MoBitsDiscovery.cpp`: 同じPCのアプリの探索。
- `MoBitsConnectionComponent.cpp`: UEのライフサイクル。アプリの`src-tauri/src/sdk_bridge/effects.rs`: 実波形の定義。

クライアントはGame Threadが所有する。自動復旧でARM/PLAYを再送しない。STOP未確認・世代・古い姿勢へのガードを維持する。イベントqueueが詰まるとセッションを閉じるため、PendingのMap参照を終了通知のEmit中に保持しない。

## 引き継ぎ先の必須作業（未実施）

1. 部位effectの名称: 1〜3は0.3.0で`EMoBitsHapticEffect::Tap` / `Nudge` / `Impact`と「Play Effect」ノードを追加済み。4〜30は具体的な触覚の仕様決定待ちで、決まり次第SDK・Blueprint・デモ・日本語説明を揃える。enumの値はwire IDと同じで並べ替えない。enumeratorの名前を変える場合は`CoreRedirects`の`EnumRedirect`を追加し、保存済みBlueprintの値を保つ。整数の`Play`/`PlayGlobal`は残す。グローバルの名称（独立した対応表）も仕様決定待ち・未実施。
2. 部位指定と`PLAY_GLOBAL`は別の番号空間。wire ID1〜30を維持する。部位1〜3のみ波形定義済み、4〜30とglobal全枠は未設定。名称変更だけで出力を有効化せず、複数デバイスのパターン定義はアプリ側で行う。
3. WindowsでEditor Development・Game Shipping・標準UAT配布をビルドし、実描画と通信、Blueprint、復旧/停止、全身デモを検証する。macOS Editorの合格やShippingのコンパイルで代用しない。

[通信契約](protocol.md)を維持する。全体の手順と未検証事項はリポジトリ`docs/sdk-handoff.md`、今回の結果は`docs/sdk-refactor-validation.md`を参照。macOS標準UAT Stage/Archive、Shipping実描画/通信、他UE版、正式署名、長時間/性能、実機BLE/USB/FW/EMSは別途確認が必要。配布ZIPにはアプリ・全身デモ・試験スクリプトを含まないため、全リポジトリを受け取る。
