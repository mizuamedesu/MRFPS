# Quest の MoBits

Quest 版は [MoBits UE5 SDK](https://github.com/MoBits-Project/UE5_SDK) の Quest スタンドアローン版 `55077ab0dcddf6fc8013a9374b1e3c616e425cd2` のMRFPS専用ビルドを使用します。MoBits の別アプリ、PC、ADB 中継を使わずに、Quest 本体の Bluetooth で左右の EVT5 に接続します。

同じ機器を別の MoBits アプリでも使用している場合は、そちらのセッションを安全に停止してからゲームに接続します。機器の登録情報はゲームごとに保存されるため、初回登録はゲーム内で行います。

エラー 7 は Bluetooth 接続に失敗したこと、144 は操作時の状態が条件に合わないこと、188 は Bluetooth 登録の準備に失敗したことを表します。別アプリの接続を終了し、機器の電源と距離を確認して「機器を探す」から選び直します。Quest のペアリング確認が表示された場合は本人が許可してください。原因の特定には `tools/automation/control.py` の `status` が返す `quest_mobits_message` と、Quest の `MoBits-GATT` ログも参照できます。

188 が続く場合、Android に接続されていない機器の古い「ペアリング中」が残っていることがあります。開発用 PC からは、下記の読み取りコマンドで対象機器の状態を確認できます。`state=11 connected=false` はペアリング処理中かつ未接続、`state=10 connected=false` は未登録かつ未接続です。実行場所はリポジトリのルートで、環境に合わせてパスとアドレスを置き換えます。

```powershell
$py = 'C:\UnrealEngine\UE5.8\Engine\Binaries\ThirdParty\Python3\Win64\python.exe'
$adb = '.local/android/platform-tools/adb.exe'
$serial = 'Questのシリアル番号'
$device = '対象機器のBluetoothアドレス'
$sdk = '.local/android'
$jdk = 'C:\Program Files (x86)\Android\openjdk\jdk-17.0.14'
& $py tools/mobits/quest_bond_recovery.py --adb $adb --serial $serial --device $device --android-home $sdk --java-home $jdk
```

古い未接続のペアリングが確認でき、機器単位の解除でも解決しない場合、次の操作は Quest の Bluetooth を一時停止してから再び有効にします。コントローラーなども一時的に切断されます。操作前に Quest のペアリング・画面共有などの確認画面を本人が閉じ、MoBits の刺激セッションを停止してください。コマンドは対象が古い未接続の処理であることや確認画面・刺激セッションがないことを確認できなければ変更しません。既存の登録を削除する操作は行いません。

```powershell
$operationId = [guid]::NewGuid().ToString()
& $py tools/mobits/quest_bluetooth_restart.py --adb $adb --serial $serial --device $device --android-home $sdk --java-home $jdk --operation-id $operationId
```

同じ `$operationId` の再実行は Bluetooth を再起動せず、記録済みの結果を返します。結果が `pending` なら完了の有無が不明なため、新しい ID でやり直さず、Bluetooth と対象機器の状態を先に確認してください。状態が解消しても、その後のペアリングと接続が成功するとは限りません。

接続中にサービス探索の応答がなくなった場合は、その機器の接続を閉じ、該当機器だけの GATT キャッシュを更新して一度だけ再接続します。サービス一覧と必要な MTU が取得できなければ失敗のままにし、ペアリング承認や電気刺激を自動実行しません。

接続の操作は同じローカル操作窓口の `mobits.start`、`mobits.scan`、`mobits.scan.stop`、`mobits.pair`（`{"id":"候補のID"}`）でも要求できます。検索中かどうかは `status` の `quest_mobits_scanning` で確認できます。設定画面を開いた状態では `mobits.identify` で識別振動をやり直せます。各操作は同じ操作 ID を再利用すると重複実行されず、結果が「submitted」のときは `status` で完了を確認します。振動の送信成功は本人が実際に振動を感じたことを保証するものではありません。Bluetooth の権限・ペアリングの承認と、電極の装着・校正・刺激の有効化は本人が行います。

## ゲーム内の準備

サーバーにプレイヤーとして参加してから、右スティックを押し込んで「MoBits 設定」を開きます。右トリガーのポインターで操作し、停止する場合は画面の「停止」を押します。

1. MoBits設定を開くと保存済み機器への再接続を開始します。初回の「付近のデバイス」権限を許可した後は「開始・権限」を押します。登録済み機器は通常起動にします。
2. 初回登録は「機器を探す」を押し、EVT5 をペアリングモードにして、候補を選んで「接続・登録」を押します。登録操作が終わると検索は自動で終了し、保存済み機器の再接続に戻ります。未登録の2台目や再試行には再び「機器を探す」を押します。接続した順に左・右を自動で割り当て、それぞれ短く振動させます。機器の準備待ちでまだ振動を送っていない場合は自動で待ち直します。画面に表示された腕に振動した機器を装着してください。見逃した場合は「振動をやり直す」を押します。接続が一時的に切れても左右は入れ替えません。
3. 両上腕に電極 1・2 を装着し、機器の USB を外してから「装着と左右を確認して開始」を押します。両機器の充電が 10%以上で、対応ファームウェアが必要です。
4. 「校正を開始」し、片腕ずつ弱いパルスから少しずつ調整します。本人が感じ始めた値を記録し、実際に届いた快適な値をその腕の上限として確定します。
5. 左右の確認後、MRFPS専用SDKは4種類の既定波形を準備して有効化します。各波形の個別試用・確定は省き、試用済みとは記録しません。画面を閉じてゲームへ戻ります。

設定・確認・有効化は装着者の操作で行います。ゲームイベントは有効化を行わず、確認前の刺激も再生しません。中断・画面離脱・接続断の後は停止状態を確認し、新しいセッションから再設定します。

## 射撃・被弾

サーバーが確定した発射ごとに右上腕の Energy Blast（エネルギー弾）、被弾時に左上腕の Damage（被弾）を、本人が有効化したローカルセッションに要求します。別のプレイヤーや観戦者には送りません。接続パネルの表示中はゲームイベントの刺激を送りません。

SDK の物理出力間隔・使用量の制限により、短い間隔の連射では一部の刺激要求が見送られます。期限切れの要求を後からまとめて再生しません。Quest のバックグラウンド移行、ヘッドセットの取り外し、ゲーム停止・終了時は停止し、再接続で勝手に有効化しません。

有効化済みでゲームが前面にあり、両機器が接続中の間は、既に開いている窓を事前に更新します。更新は刺激要求を生成しません。応答と機器の準備完了を確認できないときは更新済みと扱わず、停止後の自動再開は行いません。この変更はAndroidライブラリのビルドまでで、実機動作とタイミングは未確認です。

## 開発用 SDK の取得

SDK は非公開リポジトリから `.local/mobits-sdk` に取得し、ゲーム内に組み込みます。Windows の `Scripts/Build-MRFPS.ps1` と Mac の `Scripts/mac/build.sh` は、初回ビルド時に `tools/mobits/ensure_sdk.py` を実行します。Quest ビルドでは識別振動用のヘッダーを取得し、接続復旧の Android ブリッジを固定コミットのソースからローカルで構築します。Gradle 8.14.3 が必要で、コマンドの場所を `MRFPS_MOBITS_GRADLE` に指定できます。両方のリポジトリを読み取れる Git 認証が必要です。元の Android ライブラリの SHA-256 を検証し、ネイティブライブラリを変更せず、非公開の SDK 本体をこのリポジトリへ複製しません。

固定したSDKコミットがまだリモートに公開されていない環境では、同コミットを含むSDKのGit checkoutを用意し、`MRFPS_MOBITS_SDK_SOURCE` にその絶対パスを指定してからビルドしてください。`ensure_sdk.py --source <checkout>` でも同じ指定ができます。チェックアウトの未コミット変更は使われず、固定コミットのソースと専用AARのSHA-256が使われます。通常版AARを代用すると腕のみの設定フローや今回の修正が入りません。

例えばWindowsのPowerShellでは、SDKのGit bundleを受け取った後、次のように指定できます。完全な履歴を含むbundleを使う場合の例です。

```powershell
$sdkBundle = 'C:\work\mobits-sdk-quest-armed-window-full.bundle'
$sdkSource = 'C:\work\mobits-sdk-quest-armed-window'
git clone --no-checkout $sdkBundle $sdkSource
$env:MRFPS_MOBITS_SDK_SOURCE = $sdkSource
.\Scripts\Build-MRFPS.ps1 -Target Quest
```

Git bundle・コミットを共有するまでは、リモートのSDKから固定コミットを取得する通常のビルドは使えません。Questへのインストールや実機での動作確認は、この準備には含まれていません。

SDK 0.7.1 はプレビュー版で、上腕2台と固定4波形を対象とします。検索の自動終了と保存済み機器の再接続を含みます。身体トラッキング、他部位、壁生成、OTA はこのスタンドアローン経路では提供されません。旧 SDK 0.7.0 の Quest 実機では2台の同時接続と再接続を確認し、別の診断プログラムから2台目への EMS コマンドの受理と停止を確認しました。SDK 0.7.1 の実機接続と、ゲームイベントによる出力は未確認です。
