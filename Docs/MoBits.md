# MoBits の発射・被弾フィードバック

公式 [MoBits UE5 SDK](https://github.com/MoBits-Project/UE5_SDK) 0.5.0 / protocol v2 を同梱し、`UMRFPSHapticsComponent` でゲームイベントに接続しています。参照リビジョンは `6d657216b990948581684f61ae18ddbc9ca9116a`。取得元と変更点は [UPSTREAM.md](../Plugins/MoBitsSDK/UPSTREAM.md) に記載しています。

## 動作

| ゲームイベント / API | MoBits の要求 |
|---|---|
| `NotifyShotFired()` | 右前腕 `AutomaticShot` (13)、既定レベル 0.25 |
| `NotifyDamage(Damage, false)` | 左前腕 `Hit` (8)。40以上は `HeavyHit` (9) |
| `NotifyDamage(Damage, true)` | 左前腕 `LethalHit` (10) |
| `NotifyRespawn()` | 左前腕 `Revive` (11)、既定レベル 0.20 |
| 継承 API `ArmHaptics()` | 明示 ARM。成功の応答後だけ出力可能 |
| `StopHaptics()` | ローカル解除と `STOP_ALL`。停止確認は応答で判定 |
| `GetStatus()` | 接続、ARM、再接続、最新の要求結果を表示 |

本体はローカル所有の戦闘プレイヤーの Pawn にだけ接続します。専用サーバーでは無効化し、観戦クライアントと `-MRFPSDesktop` / `-MRFPSSmoke` の試験では接続しません。通常の自動接続はトラッキング専用で、ARMや刺激を自動開始しません。ゲーム側では H / 右AでARM、Escape / 右Bで停止します。

`AutomaticShot` は実際の発射1回に対して1回だけ呼びます。公式 SDK の連射プリセットは 150 ms以上の発射間隔が前提です。初回はプリロードに加えて **400 msの先読み時間** があり、50 ms以下の物理反動遅延を実現したものではありません。左右の前腕を独立に使うことで被弾が右腕の連射列を置き換えることを避けます。公式仕様: [MR FPS プリセット](https://github.com/MoBits-Project/UE5_SDK/blob/6d657216b990948581684f61ae18ddbc9ca9116a/Plugins/MoBitsSDK/Documentation/mr-fps.md)。

## MoBits アプリの準備

公式アプリの `codex/ue-mr-fps-presets` 系の対応版が必要です。EVT5、左右の前腕割当、装着者の校正、MR FPSプリセット登録、接触確認、専用SDKセッションの準備をアプリで行います。通常のSDKセッションのeffect 1〜3や、別の番号空間の `PlayGlobal` には置き換えていません。設定未登録の効果は `effect_unconfigured` として表示されます。

Windows / Quest Link で本ゲームをPC上で実行する場合、同じOSユーザーの `%LOCALAPPDATA%/MoBits/SDK/connection-v2.json` を公式 SDK が自動検出します。

アプリはcontrol/motion各1接続だけを受け付けます。複数プレイヤーの装備は、それぞれ対応するアプリセッションとホストを使います。インスペクターは接続を占有しません。

## Quest 単体実行時の ADB ブリッジ

公式 SDK の通信は認証付き `ws://127.0.0.1:<port>/mobits/v2/control` と `/motion` の2チャネルです。QuestではSDKのIPv4ループバック制約を保ち、認証したADB接続で同じTCPポートをPCへreverseします。LAN上にMoBitsサーバーを公開せず、tokenやsessionを改変しません。

1. PC側アプリで上記のSDKセッションを準備します。
2. QuestでMRFPSをインストール・一度起動し、アプリ固有の保存領域を作成します。
3. 次を実行します。`--adb` はAndroid SDKのadbのパスです。複数デバイス接続時は `--serial <Questのserial>` を指定します。

```powershell
python tools/mobits/quest_bridge.py --adb C:/Android/Sdk/platform-tools/adb.exe --package org.mrfps.arena --watch
```

この配置経路には **development/debuggable APKとrun-as** が必要です。ツールはrun-asのdata rootを検証し、app UIDとしてadb stdinで `files/MoBits/SDK/connection-v2.json.incoming` へ書き、directory0700・file0600からatomic renameします。user0の正規pathは `/data/user/0/org.mrfps.arena/files/MoBits/SDK/connection-v2.json` です。SDK Android discoveryも `Context.getFilesDir()` を使う内部filesへ変更しました。ログやコマンド引数にtokenは含めません。

旧external `/sdcard/Android/data/...` は現Questのapp UIDから親directoryがPermission deniedでした。現在の実装にexternal fallbackはありません。内部配置／discoveryはPython11 testsに加え、下記の実Quest native SDK接続まで確認しました。ゲームserverへのcold-authとは別の接続証拠です。アプリのセッション更新時はファイルとreverseを更新し、ADB再接続後に失われたreverseを復旧する設計です。Ctrl+Cはツールが所有するreverseだけを解除します。

再接続はトラッキングのみです。STOP、切断、アプリ再起動後のプリセット再準備と新しいARMは装着者の操作が必要です。アプリ固有ファイルをソース管理へコピーしないでください。

## 検証範囲

portable coreは2026-10-27、内部bridge更新は2026-10-28にWindowsで確認:

- `python tools/mobits/test_quest_bridge.py -v`: 内部配置変更後は **11テスト合格**（当初7件）。認証情報を表示しない検証、loopback以外の拒否、ポート不一致拒否、run-as data root／stdin／private配置、セッション更新、ADB reverse喪失からの復旧を確認。ADB呼出しはモックであり、Quest実機試験の代用ではありません。
- `python tools/mobits/run_core_tests.py`: 公式SDKの実際のportable core/WebSocket codecに対して MSVC 14.44、C++17、`/W4 /WX` でビルド・実行。**2456チェック、2000の不正入力ケース合格**。明示ARM、STOP後の旧ARM応答の拒否、effect/レベル/TTL、認証付きWebSocket、マスク・フラグメント・UTF-8などを含みます。テスト原本は公式SDK `Tests/CoreTests.cpp` で、includeパスのみ本プロジェクト用に変更しています。

当初は `%LOCALAPPDATA%/MoBits/SDK/connection-v2.json` がなく実appに接続できませんでした。後続の実app SDK検査は下記のとおり合格しています。実機EMS/LRAの提示・体感、AndroidでのMoBits通信、UEゲームイベントからの物理出力は未確認です。`device_acknowledged` は要求受理であり、身体への提示・体感の証明ではありません。

## 2026-10-28の実app buildとSDK診断

`Saved/Validation/MoBitsBuild-20261028-032811/report.json` はfresh target/jobs1による本体buildの **03:31:45Z、child/wrapper Exit0**を記録しています。生成物は `.local/mobits-build/target-j1/debug/mobits-app-tauri.exe`（98,471,936 bytes）で、loader smokeもExit0でした。

`Saved/Validation/MoBitsSDK-20261028-033615/sdk-report.json` は実appに対するhost側SDK診断で `passed=true` です。

- discovery、control/motion両HELLO、GET_DEVICES応答を確認。
- 65 motion frames、22 PONG、STOP receiptを確認。
- device_count/connected_devicesは **0**、ARM要求 **0**、PLAY要求 **0**。`physical_feedback_tested=false`。
- WM_CLOSE待ち後、ツールが所有するapp PIDだけをterminateしてcleanup。app exit1はこの終了処理によるもので、正常終了の証拠にはしません。app終了とdiscoveryファイル消滅を確認しました。

これはUEゲーム内のhaptics出力や装着したMoBitsの動作試験ではありません。UE automationのSDK 3 entrypointは別にSuccessですが、optional `RecoveryInterop` / `RustInterop` の2件はfixture未実行のままです。実app診断をその2件の実行と数えません。

## 実Quest native SDK bridgeの限定接続

`Saved/Validation/MoBitsQuest-20261028-041111/observer.json` は `passed=true` です。新PID5549でnative SDK control/motion両HELLO認証とstate1→2（Connected）を確認しました。private discoveryは302 bytes、mode600でapp UIDから読め、reverseも存在しました。観測windowは **3.062秒**、observerのSDK commandsは0、raw logsは保存していません。Questのmotion frame数は未測定です。

同directoryの `holder.json` はbridge updates2/errors0、ARM0/PLAY0、所有reverse解除・所有app終了・PC discovery削除を記録しました。guardもExit0です。これはAndroidの内部discoveryと認証付きSDK接続の合格で、継続motion、UEのshot/hit連動、EMS/LRA・装着者の体感の合格ではありません。

## 対応アプリの取得と実機準備の現状

2026-10-27 23:55 JST、対応する公式アプリを `.local/mobits-app` へ取得しました。ルートの `.gitignore` によりこのディレクトリ全体はソース管理対象外です。アプリのソースは変更していません。

- リポジトリ: [MoBits-Project/MoBits-app-tauri](https://github.com/MoBits-Project/MoBits-app-tauri)
- ブランチ: `codex/ue-mr-fps-presets`
- commit: `2f025306a55c400acaf4ce327f7daabc88b4091e`
- アプリ版: `0.1.13`
- MR FPSの公式準備手順: [sdk_mr_fps_presets.md](https://github.com/MoBits-Project/MoBits-app-tauri/blob/2f025306a55c400acaf4ce327f7daabc88b4091e/docs/sdk_mr_fps_presets.md)

2026-10-28 01:49 JST、GitHub CLI APIとGitHubコネクターの両方で以下4リポジトリへのアクセス成功を確認しました。その後、公式の [固定リビジョンの .gitmodules](https://github.com/MoBits-Project/MoBits-app-tauri/blob/2f025306a55c400acaf4ce327f7daabc88b4091e/.gitmodules) に従い、すべて親の指定commitへ取得しています。`--remote` や別ブランチへの変更は使っていません。

| 配置先 | 公式URL | 親が固定するcommit |
|---|---|---|
| `external/mobits_connect` | [MoBitsConnect](https://github.com/MoBits-Project/MoBitsConnect) | `b5a143f2272346ac2d74d18f136ea22e67580694` |
| `external/mobits_track_core` | [MobitsTrakerCore](https://github.com/MoBits-Project/MobitsTrakerCore) | `0e8639bf0b76fca5df5171f5329d665f242ffa2f` |
| `external/mobits_vj` | [MoBitsVJ](https://github.com/MoBits-Project/MoBitsVJ) | `6d6f25b86bd43a7edd4608062929294bd4bb15c6` |
| `external/mobitsvr_driver` | [MoBitsVR_Driver](https://github.com/MoBits-Project/MoBitsVR_Driver) | `a3d1ba86b089506de341a14c1b93671f53db8071` |

アプリのCargo manifestはConnectとTracker Coreを必須のローカル依存としており、既存のfeatureは `posenet-coreml` だけです。EMS/SDKだけを残してこれらを省く公式のビルド構成は見つかりませんでした。フロントエンドもMoBitsVJを直接参照します。アプリのGitHub Releasesは空で、公式の [Windows実機テスト手順](https://github.com/MoBits-Project/MoBits-app-tauri/blob/2f025306a55c400acaf4ce327f7daabc88b4091e/docs/windows-hardware-test.md) は、Actions成果物を保存せず配布物をローカル生成する運用を明記しています。

製品アプリが使うネイティブ依存も固定SHAへ取得済みです: libusb `912869b48163078103bd3b121abd68e5cde062f3`、mbedtls `d5161daa48b0b794a2d28de428814a0c0e873d85`、mbedtls-framework `875ec308e7ff34610075507b7216172ce8eb0785`、SimpleBLE `9ca6a3ebb2410249efbfab9dd9031ab799459ee4`、OpenVR v2.12.14 `91825305130f446f82054c1ec3d416321ace0072`。再帰取得に含まれるfirmware専用の `MoBits-Project/Dongle_FW` (`96c8f1bf14aff8577ed2822259f2b3e2c5131a87`) と `MoBits-Project/Tracker_FW` (`4e4676944ce418d633ec5dfbd947a7434bcce018`) は引き続き404でした。これらはConnectの `BUILD_TESTING` 内のfirmware相互運用テストで参照され、公式Cargo build.rsのアプリ構成は `BUILD_TESTING=OFF`、`MOBITS_WITH_BLE=ON`、`MOBITS_WITH_USB=ON` です。アプリの実BLE/USB経路を省いた構成には変更していません。firmware書換えは実施していません。

Node 24.18.0、npm 11.16.0、MSVC、Windows SDK、Visual Studio同梱CMake/Ninja、Git同梱Perl、WebView2 Runtimeを使用しています。追加したRust 1.99.1のcompiler/cargo/stdは、[公式standalone配布](https://forge.rust-lang.org/infra/other-installation-methods.html) のmanifest SHA256とGPG署名を検証しました。LLVM 23.1.4のlibclang/runtime/組込みheadersは、[公式release](https://github.com/llvm/llvm-project/releases/tag/llvmorg-23.1.4) のSHA256とGitHub artifact attestationを検証してarchiveから抽出しました。installerは起動せず、Rust/LLVM/Yarn/Python環境は `.local/mobits-build` に配置しています。PATHや既定toolchainは子process内だけで指定します。当初のcargo/CMakeは2ジョブ、再起動後の本体再生成はfresh target・cargo jobs1とメモリguardを使用しました。検証記録は同ディレクトリの `rust-provenance.json`、`llvm-provenance.json`、`llvm-attestation.json` です。Tauriの要件は [公式前提条件](https://v2.tauri.app/start/prerequisites/) を参照してください。

取得したアプリで実施した確認:

- `npm ci --ignore-scripts --no-audit --no-fund`: 226パッケージのローカル導入に成功。
- `node --test tests/evt5-mr-fps.test.mjs tests/evt5-mr-fps-store.test.mjs`: **32/32合格**。装着者の現在設定を保持したSDK登録、独立したbody ID、反動・被弾・復活のカーネルと保存処理を含みます。これは出力波形の計算・設定契約の試験で、実機刺激を伴いません。
- `npm run build`: 4子module取得後、**Exit0**。TypeScript/Vite frontendを生成しました。CSS minifyの既存警告とchunk size警告があります。取得前の22件のTypeScript欠損エラーは解消しました。
- Tracker CoreとVR driver: 実MSVC Releaseビルド **Exit0**。公式のdriver artifact policy/stamp/verifyも成功しました。`native-prerequisites.log` に記録しています。
- 固定Cargo.lockの `cargo fetch --locked`: **Exit0**。当初のTauri本体debug/no-bundleビルドは2ジョブで進行しましたが、2026-10-28 02:07:56 JSTのPC再起動で中断しました。10:32 JST時点では完成したexeがなく、本体buildは未合格でした。その後、破損した旧rmetaを避けるfresh target/jobs1で再生成し、上記03:31:45Zのrunで成功しました。
- 取得当初の `Get-PnpDevice -PresentOnly` では製品USB ID `VID_303A&PID_4001` 一致0件、Bluetoothクラス0件、MoBitsプロセスとSDK discoveryファイルなしでした。これはその時点の記録です。実機フィードバックは未実施です。

再起動後、アプリ親リポジトリのtracked textと担当ドキュメント／MoBitsツール計1058ファイルを読み、NULを含むファイル・空ファイルはともに0でした。親commitと4子commitは表の値を維持し、Tracker Core `.lib` とVR driver `.dll` も残っています。この検査は部分的なCargo build cacheの正常性を保証するものではありません。

`tools/mobits/check_connection.py` は公式v2のHELLO、GET_DEVICES、PING、STOP_ALLを使う実app診断で、上記SDK runに実行結果があります。ARM/PLAYを送る経路はありません。その後の身体への提示には、公式手順に沿った装着者のEVT5/電極/校正/プリセット登録と明示ARMが必要です。device0のhost診断やモック応答だけで物理確認を完了扱いにしません。

対応ソースを取得できた後の公式Windowsビルド経路は、再帰サブモジュール初期化、mbedtlsのPythonコード生成依存、libclangとRust/MSVC環境、JavaScript依存、`scripts/build-track-core.bat Release`、`scripts/build-vr-driver.bat Release`、`scripts/run-tauri-in-vs-env.bat build --debug --no-bundle` です。最後のラッパーはYarnを使用します。詳細は同じ固定リビジョンの [Windows hardware-test workflow](https://github.com/MoBits-Project/MoBits-app-tauri/blob/2f025306a55c400acaf4ce327f7daabc88b4091e/.github/workflows/windows-hardware-test.yml) にあります。

通常のapp起動はWindowsでSteamVRドライバーの自動配置処理も呼びます。実SDK runではSteamVRディレクトリのmissingを起動前に確認しています。`--windows-loader-smoke-test` はGUI・SDK・device初期化の前に終了するDLL検査で、通信やEMS/LRA試験の代用ではありません。上記のSDK診断はloader smokeとは別に実appを起動した記録です。

### 権限更新の連絡後の再確認

2026-10-28 00:21 JST、権限が追加されたとの連絡を受けて、同じ固定ソースで `git submodule update --init --recursive --depth=1 --filter=blob:none` を再実行しました。必須4リポジトリはいずれも引き続き `Repository not found` でした。GitHubコネクターからのMoBitsConnect取得も404で、現在の認証から見える同組織のリポジトリはUE5_SDK・MoBits-app-tauri・simplebleの3件です。CLIの認証ユーザーは `mizuamedesu` のままで、この組織に絞った未受諾のrepository invitationは0件でした。

この時点ではRust/libclangの追加導入やネイティブビルド、アプリ起動、ARM、実機出力を行っていませんでした。

2026-10-28 01:49 JSTの再確認では、上記4リポジトリすべてについて現在のGitHub CLI APIとGitHubコネクターのread-only取得が成功しました。権限の反映を確認できたため、親commitと4子commitを保持して実ソースの再帰取得・ローカルビルドを再開しています。以前の404は過去の記録であり、現在のアクセス不可を意味しません。アプリのビルド・SDK接続・物理フィードバックの成功は、それぞれ実測後に記録します。ARMや身体への出力は自動実行しません。
