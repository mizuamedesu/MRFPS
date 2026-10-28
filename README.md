# MRFPS

UE **5.8.1** のC++製、オンライン1対1のMR FPS。床は既定 **X4.5m × Y7m** の固定長方形です。署名された管理者が校正を押した時だけ、二人のXY中点を床中心、P0→P1を床の+Y方向として再計算します。距離に合わせた伸縮や追従は行いません。Linux専用サーバーが試合、壁、盾、HP、勝敗を管理し、Windows観戦アプリは自由視点とプレイヤー視点を表示します。

Quest 3の実写パススルーと仮想アリーナを含む実プレイヤー視点を、Windows観戦アプリ内で目視確認しました（`Saved/Validation/Complete-20261028-104616/Quest3LiveViewer/visual-review.json`）。確認時は `UnrealEditor.exe -game` です。配信停止と低FPSが残っており、30fpsを達成したとは扱いません。直近のprotocol3実測は受信2〜5fps、画像取得約18fpsでした。配布用Windows EXEはビルド・起動検証中です。

認証admin、進行制御、クリックによる壁配置、固定床校正、リアルタイム環境スキャンとサーバーの弾遮蔽をprotocol4へ統合しています。最新のUE Automationは **30件合格**（`Saved/Validation/AdminVideoRecovery-20261028-1518/Automation-20261028-152514/report.json`）。全ターゲットのビルド・配備、実障害物、二台の物理校正、安定した映像、グローブ操作、身体へのMoBits出力の最終検証は継続中です。[完了条件と残作業](Docs/ImplementationStatus.md) を参照してください。

全プレイヤー、インスペクター、管理者の接続に、roleを含むEd25519署名JWTと、そのJWTに署名された稼働サーバーのDTLS証明書fingerprintが必要です。コンテナへ渡す発行者の鍵は公開鍵だけで、署名秘密鍵は発行ホストに保持します。tokenは1接続に1回だけ使え、再接続には新規発行が必要です。

**開発中です。** 実Rust SDKの通常通信と障害復旧の2結合試験も合格しました（`Complete-20261028-104616/mobits-interop-result.json`）。結合試験の出力はFakeBackendであり、実機出力の確認には数えません。v2アート全6FBXのUE再import・寸法照合も成功しています。

更新したprotocol3のLinuxゲームコンテナはhealthyで、RAM 1 GiB・swap 0・CPU 2、公開鍵設定／healthのnegative 8件、4228ファイルおよびenvに発行者秘密鍵がないことの監査が合格しています。protocol2の実Linux認証試験22件は正常接続、replay、接続後失効、偽署名／audience拒否、client-local拒否、DTLS pin拒否を分けて記録しています。protocol3での配布版Windowsによる2対戦者＋複数観戦者の試合と安定性は次の検証段階です。

Questの旧protocol2 APKでは、install、Alpha1・MobileMultiView1・Vulkan/native passthrough API起動に加え、**cold-process認証接続が合格**しました（03:58:33Z）。private credentialsと6秒pretravel待機を使い、client/server双方のnative DTLS、署名player admission、Linuxのslot0 join、remote map読み込みを確認しました。これは更新したprotocol3 APKの実機確認を代替しません。継続session、MRの見え方・追跡・実空間校正・グローブの物理gesture・MoBitsの物理feedbackは未確認です。

MoBits実GUI appのSDK診断は再確認済みです（GET_DEVICES・96 motion packets・32 PONG・STOP応答、devices0・ARM0・PLAY0）。機器検索はBluetooth／MoBits USBドングル利用不可、ContactGloveはUSBドングルとDivingStationのOSC通信まで確認し両手disconnectedでした。未接続状態のpacketを実際の身体の動き・出力として数えません。QuestはADB承認済みで、protocol3更新APKのinstallと認証接続を確認しました。実測範囲は [ImplementationStatus](Docs/ImplementationStatus.md)、[実機確認](Docs/DeviceValidation.md)、`Saved/Validation` に記録します。

Quest側では画面共有を一つだけ起動し、必要な観戦台数へサーバーから配信します。別アプリのMediaProjection開始が既存共有を終了するのは [Meta公式仕様](https://developers.meta.com/vr/documentation/native/native-media-projection/) です。OSが共有を終了した時は、他のキャストを閉じてQuestでXの共有操作を行い、改めて許可します。許可済みセッションの一時的な画像停止には、上限付きの再接続間隔で同じ共有出力を再接続します。

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
