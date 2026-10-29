# 完了条件と証拠

## 現在の確認範囲（2026-10-29）

| 項目 | 状態 |
| --- | --- |
| Manny・全身IK・ロコモーション | protocol5で実装。実測するのは頭・両手、腰・足は推定。UEテストで立位最大0.942cm、しゃがみ1.089cm、骨の長さの偏差0.000cm。実機での見た目と遅延は確認中 |
| 建築・日本語UI | 壁のみ。50cmグリッド、90度回転、マスごとの開口・復元、高さ変更、削除。サーバー権限・開口と弾の衝突・高さの自動試験が合格。実機での編集と同期は別途確認 |
| UE Automation | protocol5、33件合格。`MannyBuild-20261028-1745/Automation-20261028-175401/report.json` |
| Linux Docker・認証 | protocol6コンテナがhealthy。公開鍵だけを渡した実コンテナのruntime negative 8件と公開鍵・ELF監査が合格。既存protocol5コンテナは別に保持 |
| Windows配布版 | protocol6の日本語管理画面とnative DTLS接続を確認。Quest実映像をアプリ内で目視 |
| 複数クライアント | protocol4の2対戦者＋3観戦者、27項目と300秒継続試験が合格。別QAコンテナのscore1・自動開始で検証。物理MRや管理者進行UIの完全な通し試験とは別 |
| Questの実映像 | protocol6の実Quest→Linux→Windows配布版で実写パススルーを目視。送信バッファ1MiBを既定に組み込んだ再ビルド版で、Windows側の94.58秒間のデコード平均28.24fps。共有許可待ちが30秒を超えても同一接続を維持し、許可後に映像を再開。動作中の品質や長時間の安定性は未確認 |
| 固定床・管理 | 既定X4.5m/Y7m。明示校正時だけ二人のXY中点・方向を反映し、床高さ・サイズ・追跡縮尺を維持。protocol4の実UIで壁の配置・他クライアント反映、幅変更と復元、Esc解放を確認 |
| 実測ジオメトリ | 動的な表面送受信、鮮度・校正世代管理、サーバーの三角形弾遮蔽を実装。クライアントの足接地問い合わせも追加。8自動試験が合格。実障害物を使う確認は未完了 |
| Mac | OpenXR依存の分離、MacのEnhancedInput依存、共通映像・認証経路、ビルド・起動スクリプトとメモリ監視を追加。Bash構文と軽量テスト10件合格。Mac実機でのビルド・表示は未確認 |
| ContactGlove | USBドングルとDivingStationのOSC通信を確認。両手disconnected、実ジェスチャーは未確認 |
| MoBits | Android向け公式アプリをQuest 3にビルド・導入し、起動とBluetooth権限を確認。機器の接続、Quest上のMRFPSとのアプリ間連携、実出力は未確認。PC上では実GUI SDK通信とRust SDK・UEの障害復旧結合試験が合格 |

最新の作業runは`Saved/Validation/MannyBuild-20261028-1745/`。protocol4の配布・ネットワーク・画面証拠は`Saved/Validation/AdminVideoRecovery-20261028-1518/`にあります。秘密鍵、JWT、部屋の画像はGitに含めません。実測した範囲と未検証の実機条件を分けて記録します。

以下は以前の検証経過です。最新状態は上表と[Handoff.md](Handoff.md)を参照してください。

## 以前の実UE Automation

最新destination/pretravel版Editor buildは `Saved/Validation/Editor-guarded-destination-20261028-034804/report.json` でExit0です。直前のautomation checkpoint `Saved/Validation/Automation-dtls-clock-20261028-033628/MRFPS/run-report.json` は **2026-10-28T03:36:54.9083610Z**終了、Exit0、結果10件で、下記すべてが `Success` でした。これは最新destination変更前の実行です。

- Auth 4件: `AdmissionAndReplay`、`AtomicReservation`、`ClientVerifiedCredentials`、`EnvironmentFailsClosed`。
- Core 3件: `AuthorityCombat`、`PoseValidation`、`RiseGesture`。
- Gloves 1件: `OSCValidation`。
- Transport 2件: `NativeDTLSHandshake`、`NoPlaintextDowngrade`。

同じvalidationの `MoBitsSDK/run-report.json` は03:37:07ZにExit0、3 entrypointともSuccessです。ただし `RecoveryInterop` と `RustInterop` の2件は `InteropNotExercised` で、外部fixtureを使う部分は未実行です。13個のSuccess stateを13件の完全なinterop実行とは扱いません。Editor全体リンクとSetupの先行記録もあります。これらは実プロセスの対戦、Questの見え方、装着したMoBits・ContactGloveの確認を代替しません。

## 実プロセスの認証試験

最新 `Saved/Validation/AuthAdmission-destination-20261028-034846/report.json` は **2026-10-28T03:50:48.0991993Z** 終了、`Passed=true` です。Windows上の実UE server/clientで有効なplayer・inspectorが接続し、14件のnegative caseがすべて合格しました。ログのcredential leak・fatalなしです。guardは最低空き14.06 GiB／最大commit46.37%でした。先行baselineは `AuthAdmission-20261028-014040/report.json` にあります。

現在の方式はnative client証明書生成後6秒のpretravel待機→token再検証→ClientTravelで、EncryptionAckは同期です。上記destination版が最終確認runです。途中の `AuthAdmission-dtls-clock-20261028-033754/report.json` はvalid player admission timeoutで `Passed=false` のまま保持し、成功runには数えません。

- 欠損・偽造・期限切れ・audience／role違いは、各roleの正規クライアントが入場前に拒否しました。これら10件は、不正JWTを実serverまで送った試験ではありません。
- replay 2件は、実serverの `AUTH_REPLAY` と追加joinがないことを確認しました。
- 署名が正しいwrong-pin token 2件は、native DTLSの証明書fingerprint検証失敗と追加joinがないことを確認しました。serverがそのJWTを解析したという証明ではありません。

この合格は記録されたログ上のcredential漏出検査を含みます。続く `Saved/Validation/AuthExpiry-20261028-015443/report.json` は **2026-10-27T16:55:24.2308945Z**、`Passed=true`、8チェックすべてtrueでした。30秒tokenで入場したplayer・inspectorについて、serverの `MRFPS_AUTH_SESSION_EXPIRED` 2件、両clientの実切断、server継続稼働、ログ上のcredential漏出・fatalなしを確認し、試験の3プロセスは終了しました。

**protocol2の実Linux Docker認証試験も22件合格**しました。`Saved/Validation/linux-security-protocol2-summary.json` は、最大1台ずつの実Windows NullRHI clientで、両roleの正常接続・Linux側replay拒否・接続後の失効と切断、偽署名／wrong audienceのLinux側拒否、10件のclient-local拒否、2件のnative wrong-pin拒否を区別して記録しています。DTLS暗号化application recordの構造を3795件観測し、全8bit offsetとASCII／UTF16でtoken全文・40文字断片・JWT pattern検出0、packet raw保存／復号なしです。18 credential fileを用いた現server全ログ検査も一致0／private PEM0でした。両guard Exit0、containerは同一ID・起動時刻・fingerprintでhealthyを維持し、試験clientはcleanup済みです。未送信のmissing／wrong-role／期限切れJWTをserverが解析したという主張はしません。最初の未built描画pluginによるbootstrap失敗は別に保持しています。protocol3のgeometry、最終Windows配布exe、Linux2対戦者の試合、Quest物理操作はこの合格の範囲外です。

## 実Multiplayer

`Saved/Validation/Multiplayer-20261028-014339/report.json` は **2026-10-27T16:44:29.0625017Z**、`Passed=true`、27チェックすべてtrueでした。Windows上の実UEプロセスで2対戦者と3観戦者を接続し、下記を確認しました。実行プロセスは試験後に終了しています。

- server authorityの盾遮蔽、壁spawn、被弾・死亡、試合開始と2slot。
- 3観戦者それぞれの2プレイヤー／壁／被弾同期、POV0→POV1→freeへの切替。
- 3人目のfighterの `ARENA_FULL` 拒否、対戦joinが2人だけであること。
- 公開鍵認証server、ログ上 `NoCredentialLeak`、`NoFatalError`。

これはWindows上のNullRHI通信試験です。描画済み画面、MR passthrough、Quest実機、実空間校正、MoBits・グローブの物理動作を確認したという意味ではありません。別工程のLinuxServer BuildCookRunは成功し、`Artifacts/Packages/LinuxServer` にパッケージを生成しました。証拠は `Saved/BuildLogs/LinuxServer-Development-20261028-014111.log` の `BUILD SUCCESSFUL` / AutomationTool ExitCode 0です。

最終packageは `Saved/Validation/LinuxServer-guarded-separated-20261028-040525/report.json` でも04:06:48ZにExit0を確認しました。ShaderCompileWorker／UnrealPak／MRFPSServerを個別native buildしてからUAT-skipbuildとし、複合graphのメモリ負荷を避けています。guard最低空き10.60 GiB／最大commit58.51%でした。

## 実Linuxゲームコンテナと単一inspector

`Saved/Validation/LinuxContainer-20261028-022720-limited/report.json` のruntime checksは合格です。実ゲームimageのcontainerはhealthy、実cgroupはRAM **1 GiB**・swap **0**・CPU **2**でした。公開鍵欠落・二重指定・不正base64・非Ed25519 SPKI・audience不正／空の6ケースは実imageがゲーム起動前にexit78で拒否し、公開鍵なしhealth／公開鍵ありゲームprocessなしhealthの2ケースはexit1でした。この8件は設定／health試験で、JWT admissionのnegative試験とは区別します。

同runの `issuer-key-audit.json` はfilesystem・Saved **4219ファイル**とenvを発行者秘密鍵のraw／PKCS8／PEM／common encodingsで照合し、該当なし・公開鍵一致でした。issuer秘密鍵はホストだけで読み、Dockerへ転送していません。観測中は同じStartedAt、RestartCount0、OOMなし、ログのJWT/private PEM該当なしでした。

`Saved/Validation/LinuxInspectorProbe-20261028-023906/report.json` は **12チェック合格**です。Windows NullRHI inspectorを**1本だけ**起動し、専用fresh tokenでLinuxへ接続しました。native証明書検証、client/server両方のDTLS handshake完了、同一subjectのinspector admission／join、`role=inspector`の同期snapshotを確認しました。cleanup後も同じcontainerがhealthyで、clientは終了済みです。guardはExit0、最低空きRAM 12.49 GiB、最大commit44.43%でした。Linux上の2対戦者＋複数観戦者の試合、描画、長時間安定性は未確認です。

最終packageを使う再deployは `Saved/Validation/LinuxContainer-final-20261028-041331/report.json` に記録しました。symbol除外後はExit0、最低空き10.58 GiB／最大commit55.78%、04:18:58Z開始のimage `099c16a1...` がhealthy、caps1GiB/swap0/CPU2、8negative再合格です。先行guardの9.88 GiB停止は失敗として保持しています。ELF hashはhost packageと一致、debug/symはhostに保持してcontext/imageから除外しました。公開鍵だけのissuer監査は4219ファイル＋envの認識可能PEM/PKCS8/text seedで一致0。秘密鍵はread/useせず、未知raw形式までの不存在は主張しません。再起動でfingerprintが変わったため、この最終imageのclient再確認は別runで判断します。

## Questの現在の実証範囲

`Saved/Validation/quest-authorized.json` はADB認証済みQuest 3、Android14、arm64-v8a、Wi-Fi `192.168.1.52`を記録しています。APK native build・cook・packageは成功し、`quest-installed.json` はv2署名検証とinstall成功を記録しました。`Quest-20261028-114343/report.json` はam start後のPID alive、`quest-alpha-ue.log` はruntimeの `AndroidPropagateAlpha = 1` を記録しています。

generic `[Android]` profileの `r.Android.DisableVulkanSupport=1` を修正したshader recook/repackageは `Quest-guarded-vulkan-profile/report.json` で03:02:27Z、Exit0です。そのAPKの `Quest-20261028-120538/logcat.txt` ではAlpha1、MobileMultiView1、DisableVulkanSupport0、Vulkan device初期化と `Native OpenXR passthrough is running` を確認しました。native GPU/passthrough API起動は確認済みですが、装着者が見たMR・追跡・位置合わせの合格ではありません。

`Quest-20261028-122912` はprivate credential読み取りとclient certificate `Verified`まで到達後、約0.5秒の時計差でserverがclient証明書notBeforeを拒否した先行失敗runです。その後の `Quest-guarded-private-connect-20261028-035218` は最大1 actionで更新APKをpackageし、03:53:46ZにExit0、最低空き10.09 GiB／最大commit54.71%で終了しました。新APKは131,318,586 bytes、v2署名検証とinstallも成功しました。supportedDevices metadataをUPLで単一の `quest3|quest3s` にそろえる変更を含み、生成manifestの実countは別途確認待ちです。

**Quest cold-authは合格**です。`Saved/Validation/Quest-20261028-125823/admission-report.json` は04:00:31Zに `Passed=true`、18チェックすべてtrueを記録しました。03:58:27Zのcold processで6秒待機し、03:58:33Zにclient/serverのnative証明書検証・DTLS handshake、serverの署名subject `quest-cold-d6d1fefeb9` のplayer admission／slot0 join、clientのremote map読込を確認しました。経路は `192.168.1.10:7777 -> 172.23.166.33:17779`、同じLinux containerはhealthy、restarts0/OOMなし、credential leakなしです。credentialはASCII/no-BOM検査後、run-as app UIDでstdin転送しprivate directory0700/files0600へatomic配置しています。現deploy経路にはdebuggable APKが必要です。

このrunはsleepを含み、継続session、装着者が見たMR alignment、実追跡・gesture・physical hapticsは未検証です。単一Quest playerのcold admissionから2人の実空間試合は推定しません。

## MoBits実appとSDK通信

旧rmeta破損を避けるfresh target・jobs1の本体再生成は `Saved/Validation/MoBitsBuild-20261028-032811/report.json` で **03:31:45Z、Exit0**です。`Saved/Validation/MoBitsSDK-20261028-033615/sdk-report.json` は実appに対するSDK診断の合格を記録しています。control/motion両HELLO、GET_DEVICES、65 motion frames、22 PONG、STOP応答を確認しました。

devices/connected_devicesは0、ARM0、PLAY0、physical_feedback_tested=falseです。所有appはWM_CLOSE待ち後にそのPIDだけterminateしてcleanupし、discoveryファイルも残っていません。app exit1はこのcleanupによるもので、正常終了の証拠にはしません。この実app SDK診断をUEのoptional interop2件や物理出力確認とは数えません。

**Quest native SDK bridgeの限定接続も合格**です。`Saved/Validation/MoBitsQuest-20261028-041111/observer.json` は新PID5549でnative SDK control/motion両HELLO認証、Connection state1→2、private discovery302 bytes/mode600/readableとreverseの存在を記録しました。観測windowは3.062秒、observerからのSDK commands0、raw logsは保存していません。motion frame数は未測定です。`holder.json` はbridge updates2/errors0、ARM0/PLAY0、所有reverse解除・app終了・PC discovery削除を記録しました。接続合格から継続motion・UEイベント・身体への出力は推定しません。

最終video/protocol2・project0.2のimage `82253535...` は `Saved/Validation/LinuxContainer-video-final-20261028-053516/report.json` に記録しました。guarded build/deployはExit0、最低空き15.76 GiB／最大commit45.30%、実上限1GiB/swap0/CPU2、8設定・health negative再合格です。177,739,320-byte ELFのSHA256 `ebc4ed7ce7583f58532739b5d28ba8e31328af54309e18d40b8cf26e7a85ca6f` はhostと一致し、debug/symはhostに保持してimageから除外しました。公開鍵だけのissuer監査は4221ファイル＋envで認識可能形式の一致0、未知raw形式は保証範囲外です。

このdeployでは初回QuestのDTLS・player認証・slot0 join後、05:39:00Zにserverがexit143となり、Docker RestartCount0のまま複数の実起動が発生しました。認証拒否とは区別して履歴に残しています。guard外で直接保持したWSL keepalive後は、server PID667／05:41:48Z開始、container05:41:49Z開始が05:53:04Zまで不変でhealthy、独立probe間隔は130.09秒でした。fingerprintは `964b42e0b79587e846c5d4e550fdbce3f85ca99098bd35c0c8cd66de00709b2c`、末尾3000行のJWT／private PEM検出0です。これは有限時間の安定確認で、このimageでの継続session・実映像streamの合格ではありません。

## 実写passthroughを含む観戦映像

現行のplayer-view cameraは、同期した頭poseから仮想3Dを再描画する実装です。追加要望の「Questの実際の表示を、実写passthroughごとWindows inspectorへstream」はこの再描画では満たせません。capture／transport／UE表示を追加作業中で、実映像streamは未検証です。開発者overlayが重なるQuest screencapを、MRFPS画面の見え方・位置合わせの合格にはしません。

## 要求と残る確認

| 要求 | 実装 | 完了を証明する検証 | 現在の未達部分 |
|---|---|---|---|
| UE5.8 | 5.8.1登録、Game/Editor/Server targets | UBT全体リンク成功、map/material生成成功 | Editorリンク・Setup・automation・LinuxServer package・Quest APK生成合格。Windows配布物と各実機runtimeは別に確認 |
| オンライン1対1 | 2slot、authority射撃・HP・勝敗・復活 | 2実clientの通信と満員拒否、authority automation | Windows実Multiplayer27チェック合格。Linuxの単一inspectorも合格。Linux2対戦者の試合・Quest・実画像は未確認 |
| 全接続の認証と通信保護（追加指示） | Ed25519 JWT、公開鍵env、role/audience/時刻、単回jti、署名DTLS pin、暗号化後だけtoken送信 | Auth 4件とnative DTLS 2件、両roleの実接続・拒否・replay・失効・ログ／packet照合 | 最新Windows14negative、先行Windows失効、Linux単一inspectorと実Quest cold playerのDTLS／認証合格。packet capture・Linux両roleのnegative JWT／失効・継続sessionは未確認 |
| Linuxコンテナ専用server | Linux target、非root Docker/Compose、公開鍵だけのenv、UDP healthcheck | 実ゲームbinaryのpackage、コンテナhealthy、認証済みclient接続 | 実container healthy、cgroup 1GiB/swap0/CPU2、設定health8negative、4219ファイル＋envのissuer秘密鍵不在、単一inspector12チェック合格。試合・長時間安定性は未確認 |
| Quest 3動作確認 | Android/Vulkan/OpenXR設定、app-private署名token／公開鍵配備 | 実APK install、認証接続、起動継続、MR描画・追跡確認 | APK・native Vulkan/passthrough API・実cold player認証／DTLS／slot0／remote map合格（18checks）。継続session、MRの見え方／alignment／追跡／physical操作は未確認 |
| N観戦者 | 認証済み独立free camera、player view、multiview | 3観戦clientの状態受信、独立操作、描画確認 | Windows3観戦の状態・各POV・free切替とLinux単一観戦の認証同期が合格。Linux複数観戦・描画カメラ／multiviewは未確認 |
| Quest実表示の実写付きstream（追加要望） | capture・transport・Windows UE表示を追加作業中 | 実Quest表示と同時の実写／仮想物／HUDをinspectorで照合し、遅延・切替・認証を確認 | 未検証。既存3D head-view再描画や開発者overlay付きscreencapでは代替しない |
| 3D情報投影 | 実poseの頭・体・手・銃・盾・情報ラベル | 描画と実プレイヤーposeの照合 | 描画確認待ち |
| 約10m四方 | cm単位の床座標と1000cm境界、手動原点/yaw校正 | 実空間の床・向き・境界を測定して照合 | 実機校正確認待ち |
| 左手上昇で壁 | サーバーrecognizer、4s制限、6s寿命、HP | authority実trace/寿命テスト、実グローブ上昇 | RiseGesture・実ワールドの壁被弾／制限／寿命は合格。実グローブ上昇の照合待ち |
| 左パーで小盾 | 掌状態・位置・エネルギー・trace遮蔽 | authority遮蔽テスト、実グローブ開閉 | 実ワールドの盾遮蔽automationは合格。実グローブの掌・位置・開閉の照合待ち |
| 発射/被弾MoBits | 公式SDK、アプリpreset、所有者event | SDK接続・receiptと、装着者による実際の発射/被弾フィードバック | 本体／host SDK診断とQuest native SDK両HELLO/Connected合格。devices0/ARM0/PLAY0、Quest motion frame数未測定、物理出力・UEイベント連動は未確認 |
| ContactGlove 2 | 公式OSC形式、接続mask、freshness、PC relay、tracker校正 | 実DivingStationの両手接続・指変化・位置・ボタンの照合 | ドライバー導入・COM3認識・Station起動済み。実Device statusは左右とも未接続 |
| Claude Code / Blender（追加指示） | Claude Opus 5.5 / xhighのgenerator、Blender5.2、6mesh・blend・FBX/GLB・board | 実manifest、render、UE再importとゲーム表示 | v2実生成5256tris・0errors/0warnings、head球内・board漏れなし。旧5meshの計測値は一致するがhash不一致で完全同一性は未証明。全6FBXのUE再import・寸法照合・Setup合格、ゲーム表示待ち |

## 確認できた前提

- C:/UnrealEngine/UE5.8 は5.8.1のソースエンジン。MSVC14.44、Windows SDK10.0.26100、Linux cross toolchain v26が存在します。
- ローカルAndroid SDKにAPI34、build-tools35.0.1、NDK27.2.12479018、CMake3.22.1を導入し、clang/aapt2/Javaを実行確認しました。
- WSL UbuntuにDocker29.1.3／Compose2.40.3を導入し、hello-worldとWindows↔WSL実コンテナUDP往復、独立client socket、healthcheckの正負ケースを確認しました。
- 個別MSVC検査に加え、Editor全体リンク・Setup・上記実UE automation 10件も成功しました。APK・Linux実container・単一inspectorの結果は上記の独立した証拠で記録します。
- 認証verifierはengine OpenSSL 1.1.1tで3061チェック（不正入力3000件を含む）に合格し、独立したPython発行JWTの署名・9claim・試験用pinの相互運用を確認しました。秘密鍵はserverへ渡していません。運用は [Auth](Auth.md)、検証契約と範囲は [Authentication](Authentication.md) を参照してください。
- glove bridgeの13件、MoBits Quest bridgeの7件、公式SDK portable core2456チェックと不正入力2000件、集計器の偽陽性防止10件を確認しました。各runnerはリポジトリ内にあります。
- グローブの追加監査で、未接続でもStationがrawゼロを送ることを実測しました。`/DS/HC/Device` のconnectedとheartbeatを必須条件に修正し、Python testsは20件へ拡充しました。大量のraw受信を手の接続・動きの証明には使いません。
- Windows標準pnputilで公式同梱ドライバーを導入し、PnP Error Code 28から0、サービスsilabser、ContactGlove2 Dongle(COM3)を確認しました。カーネル署名やWindows security設定は変更していません。
- `Art/Generated/manifest.json` のgenerator v2は6meshで5256tris、errors/warningsとも0。SM_MRHeadは1260tris、0.25192 × 0.251 × 0.249m、r=0.13m球に1.67mmの余裕で収まります。v1復元manifestとの実比較とhash差は [Art/README](../Art/README.md) に記録します。

APK生成・container起動・単一inspector接続の合格から、MR・実空間の試合・身体へのフィードバックの合格は推定しません。未検証の項目がある間は、プロジェクト全体を完了とは扱いません。
