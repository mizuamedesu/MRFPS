# 開発の引き継ぎ

## 追加変更（protocol5）

- UE5標準`SKM_Manny_Simple`、標準8方向の歩行・走行BlendSpace、頭・両手・両足のPBIK。腰・足は推定、骨を伸ばさず、しゃがみと接地を補正する。
- 壁・床・階段・箱の建築。50cmグリッド、半透明の可否プレビュー、90度回転、長押し配置、削除。Bで開始、1〜4で選択、R回転、Del削除、右クリック/Escで終了。
- 日本語の短いHUD・管理・接続UI。建築のカメラ上下はE/Qに変更し、Rの回転と分離。
- UE Automation **33件合格**。`MannyBuild-20261028-1745/Automation-20261028-175401/report.json`。テスト姿勢で最大IK誤差は立位0.942cm・しゃがみ1.089cm、手足の長さの偏差0.000cm。実機の計測結果ではない。
- AnimationDataとACLPluginを明示有効化。テンプレートのアニメーションが使う標準依存で、無効だとEditorでDataModelをロードできない。必要なManny・BlendSpace・Idleとその依存は`Content/RuntimeAssets/Manny.uasset`のlabelでcookする。
- Macメモリ監視の入力・計測テスト3件合格、Bash構文とIPv4/IPv6/ホスト名の文字種確認が合格。Mac実機は未検証。
- 新しいWindows配布版をビルド中。Linuxコンテナ・Quest APKも同じprotocol5へ更新して検証する。

## 2026-10-29時点の検証済み基盤

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

Questの実写パススルー入り映像は旧実行で目視確認したが、安定性・低FPSは未解決。最新APK更新後はQuestがAsleepで起動要求が完了していない。装着・アプリ起動・画面共有許可後に、Windows配布版で実映像とFPSを測る。外部キャストとの同時使用はMediaProjectionを奪うため、MRFPSの1本からサーバーで配信する。

二台の物理共通原点、実障害物での弾遮蔽、ContactGlove実ジェスチャー、MoBits実デバイス出力は未確認。頭・手の追跡だけでは足腰は推定であり、全身が実測一致したとは扱わない。

## 認証とローカル生成物

`.local/`、`Saved/`、`Artifacts/`、JWT、鍵、署名用keystoreはGitに含めない。`Saved/Validation/AdminVideoRecovery-20261028-1518/`に実測レポートと画像がある。パススルーの部屋の写真もローカルのみ。新PCで接続する時は稼働サーバーのpinに対して発行した新しい1接続トークンを使う。

Windowsの重い処理は`Invoke-MRFPSResourceLimited.ps1`経由（空き10GiB・commit75%）。ビルドと多数クライアントを同時に走らせない。Macは同梱のプロセスグループ監視を使う。ソースUEとSDKは別途導入する。
