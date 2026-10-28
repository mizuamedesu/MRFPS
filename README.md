# MRFPS

UE **5.8.1** のC++製、オンライン1対1のMR FPSです。Questをプレイヤー、Windows / Macを観戦・管理クライアントとして使います。Linux Docker専用サーバーが認証、試合、建築、弾・盾・HP・勝敗を管理します。

床は既定 **X4.5m × Y7m** の固定長方形です。管理者が「床の位置調整」を押した瞬間の二人のXY中点を床の中心、P1→P2を+Y方向として計算します。歩行への追従、二人の間隔による伸縮、頭の高さによる床の上下移動は行いません。

プレイヤー表示には**UE5標準Manny**、方向別の歩行・走行BlendSpace、PBIKによる全身IKを使います。頭・両手は計測、腰・足は推定です。しゃがみで体を縮尺変更せず、足の接地を補正します。[身体の仕組みと制限](Docs/Avatar.md)を参照してください。管理者は壁・床・階段・箱を、半透明プレビューとグリッド吸着で配置・回転・削除できます。UIは日本語の短い表示にしています。[操作](Docs/Controls.md)

全クライアントはrole付きEd25519署名JWTと、稼働サーバーのDTLS証明書pinが必要です。発行者の秘密鍵はホストに保持し、コンテナのenvには公開鍵のみを渡します。JWTは1接続で1回使用でき、再接続には新規発行が必要です。鍵・JWT・部屋の映像はGitに含めません。

現在のソースはprotocol5です。**UE Automation33件が合格**し、テスト姿勢で頭・手のIK最大誤差は立位0.94cm、しゃがみ1.09cmでした。新しい配布版のビルド・実画面確認は進行中です。

直前のprotocol4配布版では、実Windowsゲーム画面をWindows観戦アプリ内で約23〜24fps表示し、LAN中継でも確認しました。認証33件、2プレイヤー＋3観戦者の27項目と300秒継続試験も合格しました。これはQuest映像のFPSや物理MRの最終確認とは別です。

Questの実写パススルーと仮想物を含む実映像は旧Editor実行で目視確認しましたが、停止・低FPSの解決はまだ実機で確認できていません。現在のQuestはADB承認済み・スリープ中で、アプリは停止しています。共有にはQuest側の許可が必要です。他のキャストがMediaProjectionを開始すると既存共有が終了するため、MRFPSの1配信をサーバーで観戦者へ分配します。[Metaの仕様](https://developers.meta.com/vr/documentation/native/native-media-projection/)

Macのビルド・起動手順とメモリ監視を同梱していますが、**Mac実機でのビルド・描画・接続は未確認**です。[Macで開発する](Docs/Mac.md)。二台の物理共通原点、実障害物での弾遮蔽、ContactGlove実ジェスチャー、MoBits実デバイス出力も未確認です。最新の実測範囲は[引き継ぎ](Docs/Handoff.md)と[完了条件・証拠](Docs/ImplementationStatus.md)に記録します。

## ビルドと起動

このPCのエンジンは `C:\UnrealEngine\UE5.8`。別PCでは `-EngineRoot` を指定してください。SDKは `.local/android` にローカル導入済みです。

ビルドにはメモリ監視 wrapper を推奨します。起動前と2秒ごとに空き物理RAM 10 GiB以上・system commit 75%以下を確認し、超過を検出すると自身が起動した子プロセスだけを停止します。UBTは既定1 action、Shader workerはこのPCで2、GradleはJava heap 2 GiB・worker 2に制限しています。nativeビルドと複数クライアント試験は順番に実行してください。

```powershell
# Editorとマップ生成 → Linux専用サーバー → Windows観戦アプリ → Quest APK
foreach ($target in @('Editor', 'LinuxServer', 'Windows', 'Quest')) {
  & .\Scripts\Invoke-MRFPSResourceLimited.ps1 `
    -ScriptFile .\Scripts\Build-MRFPS.ps1 -ScriptArguments @('-Target', $target)
  if ($LASTEXITCODE -ne 0) { throw "Build stopped: $target (exit $LASTEXITCODE)" }
}
```

監視JSON・RAM/commitのサンプル・子のstdout/stderrは `Saved/Validation/ResourceGuard-*` に保存します。メモリ上限による拒否・停止はexit 125です。単一targetの指定方法とコンテナの1 GiB RAM・swap無効・2 CPU上限は [Deployment](Docs/Deployment.md) を参照してください。

先に [認証の運用手順](Docs/Auth.md) に沿って発行者の鍵を用意します。鍵生成は暗号化された秘密鍵のpassphraseを要求します。コンテナ起動には公開鍵ファイルを指定します。

```powershell
.\Scripts\Initialize-AuthTools.ps1
$python = Join-Path $PWD '.local\auth-tools\Scripts\python.exe'
& $python tools\auth\mrfps_tokens.py keygen --directory .local\auth-issuer
.\Scripts\Start-LinuxContainer.ps1 -Build `
  -PublicKeyFile .local\auth-issuer\issuer-public.pem -Audience mrfps
```

サーバーログの `MRFPS_AUTH_TRANSPORT_READY fingerprint=...` を確認してから、そのfingerprintを含むtokenを各接続へ発行します。サーバーを再起動するとDTLS証明書が変わるため、tokenも発行し直します。

```powershell
$dtlsFingerprint = '<稼働サーバーログの64桁fingerprint>'
foreach ($subject in @('player-one', 'player-two')) {
  & $python tools\auth\mrfps_tokens.py issue `
    --private-key .local\auth-issuer\issuer-private.pem `
    --subject $subject --role player --audience mrfps --ttl 300 `
    --dtls-fingerprint $dtlsFingerprint --output ".local\access\$subject.jwt"
}
foreach ($subject in @('observer-1', 'observer-2', 'observer-3')) {
  & $python tools\auth\mrfps_tokens.py issue `
    --private-key .local\auth-issuer\issuer-private.pem `
    --subject $subject --role inspector --audience mrfps --ttl 300 `
    --dtls-fingerprint $dtlsFingerprint --output ".local\access\$subject.jwt"
}

# QuestのUSBデバッグを許可してから実行
.\Scripts\Deploy-Quest.ps1 -Server 192.168.1.10 `
  -TokenFile .local\access\player-one.jwt `
  -PublicKeyFile .local\auth-issuer\issuer-public.pem -Audience mrfps

# Windows上に独立した観戦画面をN個開く
.\Scripts\Start-Inspector.ps1 -Count 3 -Server 192.168.1.10 `
  -PublicKeyFile .local\auth-issuer\issuer-public.pem -Audience mrfps `
  -TokenFiles .local\access\observer-1.jwt,.local\access\observer-2.jwt,.local\access\observer-3.jwt
```

上記は初回の新規ファイル名の例です。再発行時は新しい出力ファイル名を使います。`Start-Inspector.ps1` はCountと同数の異なる `inspector` tokenを必要とします。Editorで観戦する場合は同じ引数に `-UseEditor` を追加してください。

WindowsからWSL内のサーバーへ接続する際のUDP経路は [Deployment](Docs/Deployment.md) を参照してください。UDP上のUE通信は、署名されたpinに結び付けたnative DTLSを必須にしています。PCのLAN IPとWSLのIPは起動時に確認してください。

Editorで2人目をデスクトップ入力で参加させる例です。対戦用のデスクトップ入力は `-MRFPSDesktop` を明示した場合だけ有効です。接続処理は公開鍵でtokenを検証してからDTLSを開始し、JWTは暗号化済み接続のlogin optionsへだけ追加します。

```powershell
$editor = 'C:\UnrealEngine\UE5.8\Engine\Binaries\Win64\UnrealEditor.exe'
$project = Join-Path $PWD 'MRFPS.uproject'
$publicKey = Join-Path $PWD '.local\auth-issuer\issuer-public.pem'
$playerToken = Join-Path $PWD '.local\access\player-two.jwt'
& $editor $project '/Game/Maps/MRFPSArena' -game -nohmd -MRFPSDesktop `
  -MRFPSServer=192.168.1.10:7777 `
  "-MRFPSAuthTokenFile=$playerToken" `
  "-MRFPSAuthPublicKeyFile=$publicKey" `
  -MRFPSAuthAudience=mrfps
```

## プレイ

両プレイヤーはそれぞれ実空間のアリーナ中心に立ち、合意した同じ向き（ゲームの+X）を向いて校正します。Quest左Yまたはグローブ左Aで確定し、両者の校正と新鮮な追跡情報がそろうと試合を開始します。共有アンカーの自動解決ではなく、物理マーカーを使う手動原点・向き合わせです。

- 右トリガーまたはContactGlove右人差し指の屈曲で発射。サーバーは150 ms以上の射撃間隔を強制します。
- 左手を0.65秒以内に35 cm以上持ち上げると、前方に壁。再設置4秒、寿命6秒、HP120。
- 左手を開くと小型盾。保持と被弾でエネルギーを消費し、閉じて回復します。
- HP100、胴体20／頭30ダメージ、復活3秒。先に10回倒すと勝利し、8秒後に次の試合。
- 10 m四方の外、追跡消失、未校正、死亡中は攻撃できません。ヘッドセットを外す、再センタリング、追跡原点変更で校正を解除します。
- MoBitsはアプリ側で装着者の校正を済ませ、右A／グローブ右A／H／メニューで明示的にARMします。右B／グローブ右B／EscapeでSTOPします。

キーボード、観戦カメラ、接続メニューの操作は [Controls](Docs/Controls.md) に記載しています。

## 検証

```powershell
# 実プロセス: server + 2 players + 3 inspectors + 満員拒否
.\Scripts\Test-Multiplayer.ps1 -Inspectors 3

# 実プロセス: 両roleの有効token・拒否・replay・異なるDTLS pin
.\Scripts\Test-AuthAdmission.ps1

# UE Automation（Editorビルドとマップ生成後、suiteごとに別process）
.\Scripts\Test-Automation.ps1

# 実在ログ・プロセスから状態を集計
.\Scripts\Get-ValidationSummary.ps1
```

AuthAdmissionとMultiplayerのrunnerは試験専用の一時発行鍵をコンテナ／serverの外で作り、実サーバーのfingerprintを取得してから各クライアントへ異なるtokenを発行します。匿名接続の試験用bypassは使用しません。

`MRFPS.Core.AuthorityCombat` は実authorityワールドと物理トレースで、連射制限、被弾、盾、観戦者の攻撃拒否、復活、壁の被弾・クールダウン・寿命を検証します。10件の単体automation合格は、実プロセスの認証・対戦通信、実ゲームコンテナ、MR表示、身体へのフィードバックまで確認したという意味ではありません。

AuthAdmissionでは両roleの実server replay拒否と、署名済みwrong-pin tokenに対するnative DTLS証明書検証失敗も確認しました。欠損・偽造・期限切れ・audience／role違いの10件は正規クライアント側での拒否です。悪意あるクライアントがそれらをserverへ送信したことや、packet captureで資格情報の非露出を確認したことまでは主張しません。詳細と残る範囲は [ImplementationStatus](Docs/ImplementationStatus.md) を参照してください。

Multiplayerではserverの盾・壁・被弾・死亡、全3観戦者の2プレイヤー／壁／被弾同期と各POV→free切替、3人目の対戦参加拒否を確認しました。NullRHIの試験なので、実際に描画された画面・Quest・身体へのフィードバックの証拠にはしません。

## デバイス連携

- [ContactGlove 2](Docs/ContactGlove.md)：実DivingStation OSC、PC→Quest relay、実SteamVR tracker、床座標・装着offset校正。指屈曲と位置追跡の有効性・鮮度を別々に扱います。
- [MoBits](Docs/MoBits.md)：公式UE SDK 0.5.0、認証付きloopback WebSocket、Quest用ADBトンネル、手動ARM／STOP。発射・被弾・復活のサーバー確定イベントを所有者だけに出力します。
- [Passthrough](Docs/Passthrough.md)：`XR_FB_passthrough` のnative underlay。Meta XRプラグインには依存しません。
- [認証の運用](Docs/Auth.md)、[native検証仕様と証拠](Docs/Authentication.md)：全接続の署名JWT、公開鍵env、単回使用、DTLS証明書pin、資格情報の非表示。
- [実機確認](Docs/DeviceValidation.md)、[実装と未達項目](Docs/ImplementationStatus.md)。
