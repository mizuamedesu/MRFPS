# ContactGlove 2 入力

MRFPSGloves は DivingStation が出力する実際の OSC 入力を受ける UE runtime plugin です。UE の入力と Quest の実機接続が揃うまでは、グローブ動作確認済みとはしません。

## Quest 3 への Bluetooth 直結について

CG2 の[専用ドングルの公式仕様](https://store.melt-interface-technologies.com/products/usb-dongle)は「2.4GHz独自通信」です。[公開された DivingStationProtocol v1](https://docs.melt-interface-technologies.com/contact-glove-2-dev/cg2_protocolv1) は DivingStation とアプリ間の OSC であり、グローブと Quest の Bluetooth/GATT 仕様ではありません。Magnetra 2 はグローブに接続するアクセサリーです。公開資料から CG2/Magnetra 2 の Quest 内蔵 Bluetooth 用モードは確認できておらず、このプロジェクトも直接接続には対応していません。対応にはメーカーによる BLE 対応の有無と、その firmware・GATT 仕様、または Android 向け SDK の確認が必要です。

CG2単体は空間内の手の位置を計測できません。[メーカーのQuest用アダプター](https://store.melt-interface-technologies.com/en/products/quest-3-3s-pro-adapter-for-cg2-3)はQuest 3 / 3s / Proのコントローラーをグローブへ取り付ける構成です。コントローラー側の位置追跡とグローブ側の指入力は別々のデータであり、アダプターだけでグローブの指データがQuestへBluetooth直結されるわけではありません。取付位置と手の位置の差は実機で校正する必要があります。

2026-10-29には両手ともQuestアプリで新鮮なグローブ指データと手の位置を受信したことをログで確認しました。その後Questがスリープしてサーバーとの接続が切れており、射撃・盾・壁がサーバーまで届くことは別途実機確認が必要です。

## 確認した公式仕様

- [公式 ContactGlove 2](https://melt-interface-technologies.com/en/products/contactglove2/) は SteamVR 対応のフィンガートラッキング機器です。
- [公式ダウンロード](https://melt-interface-technologies.com/en/download/) の [UE SDK versions.json](https://melt-interface-technologies.com/bin/UnrealEngine%20SDK/versions.json) は v1.2.0、2026-07-03 を掲載しています。
- [SDK v1.2.0 ZIP](https://melt-interface-technologies.com/bin/UnrealEngine%20SDK/v1.2.0/ContactGloveSDK_UE.zip) を実際に取得し、UE_5.7/ContactGlovePlugin 内の OSC インターフェースを確認しました。ZIP の SHA-256 は `292697a5c7873e2468da2fbd8f7f96c3f547853e2693907614c92ce0f51cbbf3`。
- [公式 CG2 UE セットアップ](https://docs.melt-interface-technologies.com/contact-glove-2-dev/cg2_unrealengine-setup) へのリンクは SDK の README にあります。

SDK 同梱 LICENSE は SDK および派生物の第三者への再配布を制限しています。そのため vendor コードや DLL はリポジトリに入れていません。本 plugin は OSC のデータ形式を独立に実装し、DivingStation の実入力を使用します。vendor SDK のプラグインを同時起動すると受信ポートが競合するため、同じポートで動かさないでください。

## 位置と指のデータを区別する

ContactGlove の `/DS/HC/Wrist` には WXYZ の回転のみが含まれ、手の位置は含まれません。手を引き上げる壁ジェスチャーと銃の位置・方向には、OpenXR MotionController pose、Questの光学手追跡、またはSteamVR tracker poseが必要です。Wrist IMU の回転から架空の位置を生成する処理はありません。

`/DS/HC/Hand` の 20 floats は親指→人差し指→中指→薬指→小指の順で、各指は 3 関節の屈曲値と開き値です。`/DS/HC/HandQuat` は 15 関節の回転 WXYZ です。vendor の骨座標のまま格納し、UE の任意の skeleton へそのまま適用できるとは扱いません。DivingStation は信頼度の確率を提供しないため、plugin は受信済み・鮮度・位置トラッキングの有無を別々のフラグで返します。

ゲーム内の両手の指表示とパー判定はContactGloveの `/DS/HC/Hand` を使い、Questの指関節から屈曲を推定しません。グローブ入力が途切れた間、観戦画面は最後に受けた指姿勢を表示しますが、盾や射撃の入力は古い値を使いません。手の位置・向きは別途コントローラーやQuestの掌位置を使用します。

## PC / Quest Link

1. 公式 DivingStation と SteamVR をセットアップし、グローブをペアリング、指を校正します。
2. MRFPSGloves を有効にしてゲームを起動します。Windows では既定で `127.0.0.1:25790` へ OSC `/DS/HC/Connect` と受信ポート `25788` を通知します。Device heartbeat が途切れた場合だけ、最大2秒おきに再登録します。
3. glove 入力は `25788/udp`、送信元 `127.0.0.1` のみ受けます。複数の DivingStation device があるときは `-MRFPSGloveDevice=デバイスID` を指定します。未指定なら最初の有効な device ID に固定します。
4. OpenXR 側で実際にトラッキングされる手の位置をゲームが使用します。

設定は `DefaultGame.ini` の以下の section です。Quest の場合は `AllowedSender` を PC の実際の LAN IPv4 に変更してください。`-MRFPSGloveSender=192.168.1.10` でも指定できます。

```ini
[/Script/MRFPSGloves.MRFPSGloveSettings]
bEnabled=True
ListenPort=25788
BindAddress=0.0.0.0
AllowedSender=127.0.0.1
StationAddress=127.0.0.1
StationPort=25790
StaleAfterSeconds=0.15
```

## Quest standalone への PC relay

Quest配備時は、PCブリッジの実IPv4を明示します。例えば `Scripts/Deploy-Quest.ps1` の通常の認証付き配備引数へ `-GloveSender 192.168.1.10` を追加します。必要なら `-GloveDevice` と `-GlovePort` で対象を固定できます。ゲームサーバーとブリッジPCが同じホストであるとは仮定せず、許可送信元を自動で全開放しません。

ContactGlove 2 の公式配布から Android ネイティブ機器接続の保証は確認できていません。Quest standalone では、DivingStation が動く Windows PC から LAN 上の Quest へ実入力を送ります。

```powershell
uv run --python 3.12 python tools/gloves/contactglove_bridge.py --target 192.168.1.20
```

PC relay は `127.0.0.1:25789/udp` で実際の DivingStation に購読し、Device snapshot と、実接続状態が確認できる手の Hand / HandQuat / Wrist / Controller メッセージだけを Quest の `25788/udp` に転送します。各 route/hand の新規sampleを最大90Hzへまとめます (`--rate-hz` で15〜120)。DivingStation が大量の重複パケットを送る場合は、受信処理を制限し、小さな受信キューから古いパケットの滞留を抑えます。古いcached sampleを再送して鮮度を作る処理はありません。複数 `--target` を指定すると同じ入力を複数の開発端末へ送れます。Quest の手をそれぞれ動かす用途では各プレイヤーの別 device ID と別 relay を使用します。relay を停止した場合、ゲーム側は 150ms で入力を無効化し、古いパーやトリガーを押し続けません。Wi-Fi の端末間通信を許可し、Quest が受信する送信元 IP を設定します。

独立した SteamVR tracker から位置を送る場合は、トラッカーのシリアル番号と Quest stage との校正値を JSON に用意します。

```json
{
  "left_serial": "左手の実際のtracker serial",
  "right_serial": "右手の実際のtracker serial",
  "yaw_degrees": 0.0,
  "translation_cm": [0.0, 0.0, 0.0]
}
```

上記のゼロは記入例です。2 つの独立した tracking space が一致した証拠にはなりません。

```powershell
uv run --python 3.12 --with openvr==2.12.1401 python tools/gloves/contactglove_bridge.py --target 192.168.1.20 --trackers local-trackers.json
```

relay は Valve の `getDeviceToAbsoluteTrackingPose(TrackingUniverseStanding, 0, ...)` を読み、connected・pose valid・Running_OK の条件が揃うときだけ有効な pose を送信します。シリアル番号で手を固定し、再接続は再走査します。座標変換は [Valve OpenVR の正式ヘッダー](https://github.com/ValveSoftware/openvr/blob/master/headers/openvr.h) の右手系 x=右、y=上、-z=前、メートルから、UE の x=前、y=右、z=上、センチメートルです。SteamVR と Quest の間の yaw と平行移動を適用後に送信します。

同じ実点を 2 点以上、両方の tracking space で計測した場合は、次で校正 JSON を作れます。`source_cm` は OpenVR を UE 基底に変換した `[-z*100, x*100, y*100]`、`quest_cm` は Quest stage の対応点です。50cm 以上離れた点を使用します。

```json
{
  "left_serial": "左手tracker serial",
  "right_serial": "右手tracker serial",
  "source_cm": [[0, 0, 0], [100, 0, 0], [0, 100, 0]],
  "quest_cm": [[10, 20, 0], [10, 120, 0], [-90, 20, 0]]
}
```

```powershell
uv run --python 3.12 python tools/gloves/calibrate_alignment.py local-measurements.json local-trackers.json
```

3cm を超える RMS 誤差、短すぎる基線、有限でない入力は拒否します。recenter・tracking origin の変更後は校正し直します。

## グローブに固定した tracker の向き

tracking space 同士の校正に加え、tracker と手の取り付け位置・向きも実測して校正してください。`UMRFPSGloveSettings::LeftTrackerToHand` / `RightTrackerToHand` は、UE 基底に変換済みの tracker に対する手の local transform です。Project Settings の MRFPS ContactGlove に表示され、DefaultGame.ini に保存されます。既定の identity は取り付け方向を計測済みという意味ではありません。

最終的な手の座標は **+X が指先方向、+Z が手の甲から外向き** の anatomical palm frame に揃えます。左盾の前面は掌から外への `-Z` 方向であり、`+X` の指先方向を盾の法線として扱わないでください。右の銃は指先方向 `+X` を基準に hand/gun socket で取り付けるか、別途 controller Aim pose を使用します。DivingStation の IMU の Wrist quaternion とこの frame は同じものではありません。

UE の transform 乗算規約に従い、external packet に対して `HandInTrackingSpace = TrackerToHand * TrackerInTrackingSpace` の順で合成します。local OpenXR から `SubmitTrackedPose` した pose には、この external tracker 用 offset を適用しません。offset の平行移動は tracker local の cm、scale は常に 1 です。不正な回転・非有限値・100cm を超える mount translation・scale 変更は positional tracking を無効化し、指の受信は継続します。新鮮な packet の tracked flag が false の場合、offset を設定しても `bPoseValid` は true になりません。

## ゲームからの API

`MRFPSGloves` を module dependency に加え、`MRFPSGloveSubsystem.h` を include します。

```cpp
FMRFPSGloveHandFrame Frame;
UMRFPSGloveSubsystem* Gloves = GetGameInstance()->GetSubsystem<UMRFPSGloveSubsystem>();
if (Gloves && Gloves->GetLatestHand(EMRFPSGloveHand::Left, Frame))
{
    const bool bOpenPalm = Frame.IsOpenPalm();
    // Frame.bPoseValid を別に確認して壁や盾の位置を取得する。
}
```

- `GetLatestHand()` と `bFlexValid` は Device heartbeat が新鮮で実handが connected、かつ finger flex が新鮮な場合だけ true です。DivingStation は未接続でも既定のゼロHand等を送るため、パケット鮮度だけでは有効にしません。`bDeviceStatusValid` / `bConnected` / `BatteryPercent` で機器状態、`bFlexStreamFresh` でraw packetの到着だけを別々に確認できます。
- `bControllerValid`、`bJointRotationsValid`、`bWristImuValid` も、それぞれの鮮度と新鮮なconnected状態を必要とします。`bPoseValid` は別途実際にトラッキングされた位置の鮮度を表し、gloveの接続状態から架空の位置を作りません。
- `FingerCurls[5]` は各指の平均屈曲、`Flexion[15]` は各関節、`Splay[5]` は開き値です。
- `Trigger` とボタンは Controller パケットが新鮮なときだけ有効です。touch 状態 1 と pressed 状態 2 を区別します。
- `TrackingTransform` は実トラッカーから mount offset を適用した、+X 指先 / +Z 手の甲の UE tracking space hand pose です。ゲーム world への変換は pawn の tracking origin が担当します。
- `SubmitTrackedPose()` で local OpenXR pose を明示的に供給できます。古い最後の pose を tracked として渡さないでください。
- 受信時刻は各ストリーム別の process monotonic clock です。元の DivingStation パケットには送信時刻・sequence がないため、ネットワーク到着前の遅延を検出できるという意味ではありません。
- plugin subsystem は dedicated server、明示的な `-MRFPSDesktop` / `-MRFPSSmoke` / `-MRFPSInspector` / `-Inspector` client では生成せず、実機の OSC 受信ポートや DivingStation 接続を消費しません。

## 検証

```powershell
uv run --python 3.12 python -m unittest discover -s tools/gloves -p "test_*.py" -v
```

20 tests が OSC schema、bundle、不正 packet / NaN、device 固定、座標変換、計測点校正、未受信・鮮度・実測変化の判定、未接続dongleが送る既定ゼロの拒否、実際に起動した relay プロセスの UDP connect/forward を検証します。この fixture は実グローブの代わりであり、ハードウェア動作検証には数えません。

UE Automation の `MRFPS.Gloves.OSCValidation` は本番 C++ parser の byte order、型、不正 packet、時刻鮮度判定を対象にします。UE 環境でビルドして実行が必要です。

実際の DivingStation 入力をゲームとは別に確認するときは、グローブを装着・校正して次を実行し、観測中に両手をパー・グーに動かしてください。

```powershell
uv run --python 3.12 python tools/gloves/check_device.py --duration 10 --require-motion --output Saved/Validation/contactglove-input.json
```

専用受信ポート `25787/udp` で実パケットだけを観測します。両手の Device status が connected で新鮮、かつ flex 入力が最後の150ms以内に届き、両手のいずれかの指の実測変化が 0.08 以上あることを確認します。未受信・古い値・未接続dongleからのゼロ初期値は成功になりません。Wrist/HandQuat/Controller の受信数と各 stream の鮮度、機器側の接続状態・batteryも JSON に記録します。`last_received_curls_thumb_to_little` はraw packetの内容であり、未接続時のゼロを物理計測値と見なしません。合格は指入力だけの検証であり、Quest 上の MR・実際の手の位置・壁盾射撃が合格したという意味ではありません。

## このPCの実機準備で確認した事実

2026-10-27 の読取監査で、`ContactGlove2Dongle` (`USB\\VID_10C4&PID_6B27`) が接続されていましたが、PnP ProblemCode は 28、DriverDesc/Service は空でした。Microsoft の [Code 28 の説明](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/cm-prob-failed-install) はドライバー未導入を示します。既知のインストール先・uninstall registry・process では DivingStation を確認できず、Steam は存在するものの SteamVR exe と OpenVR runtime path は確認できませんでした。実際の 5 秒 OSC 観測は 0 packet でした。これらは当日の観測であり、インストール後は再確認してください。

公式 `DivingStation/versions.json` が返した最新版 v1.11.9 (2026-10-23) を `.local/contactglove` にダウンロードして展開しています。installer は Authenticode Valid / Melt Interface Technologies, Inc.、実行 manifest は requireAdministrator です。配布物同梱 `resources/usb_driver/slabvcp.inf` は実機の PID_6B27 に一致し、DriverVer は 07/19/2024,7.7.7.2230、Provider は Diver-X Inc.。INF/SYS の catalog membership は Authenticode policy で検証成功です。kernel policy の個別検証では custom catalog が失敗し、SYS は primary Silicon Labs 署名と secondary Microsoft Hardware Compatibility Publisher 署名を持ち、全署名検証では 1 署名成功・1 署名失敗でした。

2026-10-28、正規の昇格 `pnputil` でこの未改変INFを導入し Exit 0。その後の実再照会では `ContactGlove2 Dongle (COM3)`、ProblemCode 0、service `silabser`、installed INF `oem4.inf` / version `7.7.7.2230` を確認しています。展開した署名済み DivingStation.exe も起動し、UDP25790のlistenerを確認しました。driver signing / Secure Boot / memory integrity 等の設定は変更していません。

ただし、その後の実 OSC 観測で `/DS/HC/Device` が左右とも connected=0、battery=0 と報告しました。DivingStation はこの状態でも各 Hand 等のゼロ初期値を連続送信します。改修した checker は fresh raw packet と connected sensor を区別し、実5秒観測でpacketが多数到着しても手の動作成功と判定せず `input_check_passed=false`、`DivingStation reports disconnected hands: left, right` と記録しています。左右グローブの電源・pairing、実際の指の動作は未検証です。最新の件数・状態は `Saved/Validation/contactglove-hardware.json`、`contactglove-input.json`、`contactglove-osc-routes.json` を参照してください。

[公式 CG2 FAQ](https://melt-interface-technologies.com/en/products/contactglove2/) は本体に位置追跡がなく、Quest standalone を直接サポートせず、公式構成では Quest Link / SteamVR と対応 tracker・tracking space 校正を使うことを明記しています。本プロジェクトの PC relay は独自の standalone 連携経路なので、実機での追加検証が必要です。Quest の光学手追跡がグローブ越しに正しく見えることは保証されず、CG2 の指入力を Quest の光学入力で検証済みと置き換えません。

未確認の実機 gate: DivingStation で CG2 の左右指が追従すること、Quest 上で受信の鮮度が維持されること、SteamVR/Quest の座標校正、手の上昇による壁、パーによる盾、トリガーによる射撃、機器切断時に入力が解除されること。

受信は描画フレームとは別の軽量スレッドで到着時刻を記録します。フレームが遅くても新しい指情報を取り込み、古い入力を再開時に新しいものとして扱いません。`MRFPS.Gloves.ResumeDiscardsOldInput` は左右の実形式OSCと低フレームレートを検証します。

2026-10-29の再計測では左右ともDivingStationの接続と指データを確認しましたが、その後グローブが未接続状態となり、実際に指を動かしたQuestから観戦画面までの追従は未確認です。PC中継がメモリ保護で停止するとQuestには入力が届かないため、再開と状態の確認が必要です。
