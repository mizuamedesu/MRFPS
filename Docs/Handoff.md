# 開発の引き継ぎ

## 現在の状態（protocol 14、2026-10-30）

- 1ラウンド180秒、最大3ラウンド、先に2勝した側が勝利します。1人の場合は本人の校正と追跡が有効になると Bot が入り、管理者操作を待たずに開始します。操作と停止条件は [Rounds.md](Rounds.md) を参照してください。
- 実 Linux コンテナと Windows 配布版の1人用テストでは、2ラウンドがそれぞれ180.026秒・180.007秒で終了し、Bot の2勝で試合が終了しました。実 Quest の姿勢・校正・障害物スキャンの確認には代わりません。証拠はローカルの `Saved/Validation/RoundBot-p14/packaged-player-qa-report.json` と `qa-server/observations.log` です。
- 観戦メイン画面と独立観戦窓に、ラウンドと勝数を中央上へ表示します。Windows 配布版で画面を確認済みです。最新の UE Automation は58件成功（警告17件、失敗0件）しました。レポートは `Saved/Validation/RoundBot-p14/automation-capture-update/report.json` です。
- 管理 Worker `mrfps-admin.mizuame.app` を更新済みです。更新 Quest APK と Windows 配布版はそれぞれビルド成功し、実機 APK の導入後、両方が発行された資格情報と一致する同じローカル専用サーバーに参加しました。更新した Windows 版は P1・P2・観戦2 の独立窓も開きました。P1 ウィンドウの画像を実際に確認した結果、**映像は Quest の OS 共有許可待ち**でした。新しい protocol 14 の実映像、FPS、装着時の追跡は未確認です。旧 protocol の映像結果を新しい結果として扱いません。
- Quest APK は `Artifacts/Packages-RoundBot-P14-20261030-06/Android_ASTC/MRFPS-arm64.apk`（SHA-256 `2f586329e945b0be5fe551dc7163bb04301c89f802339b89fbb40b7ca4f532e8`）、Windows 版は `Artifacts/Packages-RoundBot-P14-20261030-07/Windows/MRFPS/Binaries/Win64/MRFPS.exe`（SHA-256 `189410cc7ab72cfb961f102afa488dd4802eaf0c22704676f3825b6e4b8307b3`）です。どちらもメモリ監視付きでビルドし、最低空き RAM はそれぞれ約10.50 GiB、12.54 GiB でした。これらローカル成果物は Git に含めません。
- 新しい専用サーバーを private GHCR に公開しました。source commit と digest、公開境界、HeteroCloud の残る要件は [CloudDeployment.md](CloudDeployment.md) を参照してください。HeteroCloud CLI は未ログインで、Flash への実デプロイと `mrfps.mizuame.app:7777` の到達性は未確認です。
- ContactGlove の PC 中継と、Quest からの送信元 IPv4 固定起動を用意しています。現在の DivingStation は左右とも未接続と報告しており、実機の指・射撃・盾・壁操作は未確認です。[DeviceLogin.md](DeviceLogin.md) と [ContactGlove.md](ContactGlove.md) を参照してください。
- Mac の実機ビルド・描画・接続、2台の物理共有原点、実障害物による弾の遮蔽、MoBits の実出力も未確認です。ローカル画像・資格情報は Git に含めません。

## 以前の武器・グローブ・壁編集（protocol 12）

以下は protocol 12 の履歴です。現在のソースは protocol 14 で、protocol 13 以前の配布物とは互換性がありません。全対象の再ビルドと更新が必要です。

認証されたプレイヤーは、試合開始や床合わせの前から射撃・盾・壁を使えます。待機中の練習では他プレイヤーにダメージを与えず、対人戦は共有床の校正と管理者操作に従います。管理者の一時停止は練習中も有効です。Questでは接続時にOSの画面共有許可を求め、切断時はHUDに「接続待ち」を表示します。再接続には、署名された未使用トークンをアプリの資格情報ファイルへ新たに提供する必要があります。

protocol 12のUE Automation 44件が成功しました。観戦姿勢の保持、両手のContactGlove入力、下げる動作での壁削除、初期木箱の衝突・削除と床への追従を含みます。Quest装着者による実際の操作・銃角度と実写映像の確認は別に必要です。

両手のコントローラー位置とContactGloveの指関節を組み合わせます。MRピストルは最初から右手に装備し、右トリガーまたは人差し指で発射、左手を弾倉に沿ってスライドすると30発まで補充します。予備弾薬は無限で、弾数・リロード・命中はサーバーが管理します。銃名・弾数・HPを視線中央より下に表示します。両手の実グローブを着用したQuestでの射撃・リロードとHUDの見え方は実機確認が必要です。

2026-10-29の更新では、プレイヤー画面の仮想アバターを非表示にし、観戦側で指・全身の姿勢を解決します。グローブ受信を描画から分離し、HP・弾数を視線中央下に配置しました。UE自動試験41件とPC中継の21件が成功し、Windowsのプレイヤー画面でHUDを目視しました。新しいQuest APKを実機へインストールし、同じprotocol 9のDockerサーバーにQuestとWindows管理観戦が認証・参加しました。Quest内HUDと実グローブの動きが観戦画面まで届くことは未確認で、実映像も共有待ちです。Windowsの観戦はUE Editorのゲーム起動で確認しており、更新した単体Windows配布版は未完成です。

管理者はBで建築モードを開始し、壁のマスをクリックして開口／復元、ホイールまたは上下キーで高さ変更、Rで回転、Delで壁や初期木箱を個別に削除します。壁は横3マス、高さ50cm単位で0.5〜4mです。初期マップには壁2枚と木箱2個があり、編集状態と弾の衝突はサーバーから同期されます。protocol 12ではサーバーと全クライアントを同じビルドにそろえてください。実機・複数クライアントでの検証は下記の旧版の結果とは別です。

## 過去の追加変更（protocol5）

- UE5標準`SKM_Manny_Simple`、標準8方向の歩行・走行BlendSpace、頭・両手・両足のPBIK。腰・足は推定、骨を伸ばさず、しゃがみと接地を補正する。
- 壁・床・階段・箱の建築。50cmグリッド、半透明の可否プレビュー、90度回転、長押し配置、削除。Bで開始、1〜4で選択、R回転、Del削除、右クリック/Escで終了。
- 日本語の短いHUD・管理・接続UI。建築のカメラ上下はE/Qに変更し、Rの回転と分離。
- UE Automation **33件合格**。`MannyBuild-20261028-1745/Automation-20261028-175401/report.json`。テスト姿勢で最大IK誤差は立位0.942cm・しゃがみ1.089cm、手足の長さの偏差0.000cm。実機の計測結果ではない。
- AnimationDataとACLPluginを明示有効化。テンプレートのアニメーションが使う標準依存で、無効だとEditorでDataModelをロードできない。必要なManny・BlendSpace・Idleとその依存は`Content/RuntimeAssets/Manny.uasset`のlabelでcookする。
- Macメモリ監視の入力・計測テスト3件合格、Bash構文とIPv4/IPv6/ホスト名の文字種確認が合格。Mac実機は未検証。
- Windows配布版・Linux専用サーバー／Docker image・Quest APKをprotocol5でビルド済み。Windows配布版の日本語管理HUD・床・壁は保存済み実画面で確認した。Mannyの`AVATAR_READY`は両platformのログにあるが、実機の全身姿勢・IKの見た目の合格にはしない。

## 最新protocol5の実記録（2026-10-29）

基準フォルダーはローカルの `Saved/Validation/MannyBuild-20261028-1745/`。時刻はUTC。ここで実行した配布版のnetwork protocolは5、project versionは0.3.0です。接続復帰を追加した最新ソースはprotocol6で、protocol5と混在接続できません。全対象の再ビルド・更新・実機検証が必要です。

| 確認 | 証拠と結果 | 証明していない範囲 |
| --- | --- | --- |
| UE Automation | `Automation-20261028-175401/report.json`、2026-10-28 17:54:37Z、33件Success／Exit0 | 物理MR、実デバイス、最新配布版の通し対戦 |
| Windows配布 | `windows-guard1/report.json`、続く日本語UI更新の`windows-ui-guard1/report.json`がともにExit0。後者は2026-10-29 01:15:46Z終了 | 実映像・FPSの合格ではない |
| Windows実画面と接続 | `native-admin-awaiting-quest-share.jpg`で日本語管理HUD・床・壁・「共有待ち」のP1/P2枠を確認。`live-admin2`で証明書検証・native handshake・remote snapshot | この画像自体は映像受信前のもの |
| Linuxビルドと実container | `linux-guard1/report.json`がExit0。`report.json`はprotocol5／RuntimePassed=true。`runtime-audit.json`でhealthy、同一起動、restart0／OOMなし、RAM1GiB・swap0・CPU2、設定／healthの8negative合格 | 最新protocol5の多人数試合・長時間安定性は別試験 |
| 発行鍵・ELF監査 | `public-only-key-elf-audit.json`で公開鍵一致、実ELFはhostと一致、filesystem4202＋Saved29ファイルとenvに認識可能な発行者秘密鍵の一致なし。ホスト秘密鍵はread/useしていない | 未知のraw seed形式までの不存在保証ではない |
| Quest APK | `quest-guard4/report.json`／stdoutのBuildCookRunがExit0、2026-10-29 01:10:47Z。配布物は`Artifacts/Packages/Android_ASTC/MRFPS-arm64.apk` | build完了からMR表示・追跡品質を推定しない |
| Quest一時join | `quest-diagnostics-20261029-012909/MRFPS.log`で01:28:28ZにVerified／Handshake完了／Linuxのremote map読込。native passthrough API起動も確認 | captureはstate1（共有許可待ち）・accepted0／dequeued0。その後Suspend。継続join・映像再生・FPSは未確認 |
| Quest実映像 | 実Quest→Linux→Windowsの管理画面内にパススルーを目視。`interactive-admin/quest-live-0.jpg`はローカル保存。送信バッファ調整前は20.08秒平均5.33fps、1MiBの一時設定後は52.30秒平均22.92fpsで、約27〜30fpsの区間もあった。別区間では取得・受信とも約18fps。比較中のAndroid全体のUDP送信バッファエラーは5278件と0件 | 短時間・未装着状態の計測。部屋の画像はGitに含めず、長時間の安定性や装着時の見え方を推定しない |

`quest-deploy-guard1`はinstallの`Success`、app-private資格情報の配置、起動要求後、出力処理のcp932 `UnicodeEncodeError`でExit1でした。deploy runner全体のPASSとは扱わず、その後の独立したQuest実行ログで一時joinを判断します。

`live-admin2/report.json`は接続・snapshot受信の後に`NETWORK_FAILURE`となり、最終`Passed=false`です。`RealVideoVerified`／`QuestPassthroughVerified`もfalseのままです。初回接続と別に撮影した実画面の確認を、映像試験・1時間安定性の合格へ読み替えません。

## 先行protocol4の実績（最新protocol5とは別）

- UE5.8.1、protocol4。Windows配布版、Linux Docker専用サーバー、Quest APKを生成。
- Ed25519 role署名とDTLS pinを必須化。稼働コンテナは発行者公開鍵のみを受け取り、署名秘密鍵を含まない。実Windows配布版からplayer / inspector / adminの認証33件が合格。
- 実Windowsゲーム画面をWindows観戦アプリで表示。直結・Questと同じLAN中継の両方で約23〜24fpsを確認。これはQuest映像のFPS確認ではない。
- 管理者による壁のクリック配置・他クライアントへの反映・床寸法変更と復元・Esc解放を実画面で確認。
- 2プレイヤー＋3観戦者、27項目と勝敗・スコアボードの確認、および300秒の継続試験が合格。別のQAコンテナでscore1と自動開始を明示設定した検証で、管理者進行UIの通し試験や物理MRの証拠とはしない。
- UE Automation30件合格。床は4.5m×7m固定。明示校正時だけ二人の中点・方向を反映し、距離に合わせて伸縮しない。

## 開発の参照

- 建築・日本語UIの操作は[Controls.md](Controls.md)。
- Mannyと推定する身体の仕様は[Avatar.md](Avatar.md)。
- Mac観戦・管理クライアント。手順は[Mac.md](Mac.md)。Windows専用のOpenXR依存をMacから外し、Mac実機でのビルド・表示は未確認。

## 実機で残っている確認

Questの実写パススルー映像はprotocol5のWindows配布版で目視できた。未装着ではスリープ・復帰が発生するため、装着した状態で映像を継続し、動き・遅延・FPS・再接続を検証する。送信バッファの既定値変更および後続の接続復帰変更を含む配布版は再ビルドして確認する。外部キャストとの同時使用はMediaProjectionを奪うため、MRFPSの1本からサーバーで配信する。

Mac実機でのビルド・描画・接続、二台の物理共通原点、実障害物での弾遮蔽、ContactGlove実ジェスチャー、MoBits実デバイス出力は未確認。頭・手の追跡だけでは足腰は推定であり、全身が実測一致したとは扱わない。

## 認証とローカル生成物

`.local/`、`Saved/`、`Artifacts/`、JWT、鍵、署名用keystoreはGitに含めない。最新は`Saved/Validation/MannyBuild-20261028-1745/`、先行protocol4は`Saved/Validation/AdminVideoRecovery-20261028-1518/`。レポート・スクリーンショットはローカルに保持し、パススルーの部屋の写真を文書へ埋め込んだりGitへ追加したりしない。新PC・再接続・サーバー再起動後は、現在の稼働pinを含む新しい1接続トークンを発行する。

Windowsの重い処理は`Invoke-MRFPSResourceLimited.ps1`経由（空き10GiB・commit75%）。ビルドと多数クライアントを同時に走らせない。Macは同梱のプロセスグループ監視を使う。ソースUEとSDKは別途導入する。
