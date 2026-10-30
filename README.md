# MRFPS

UE **5.8.1** のC++製、オンライン1対1のMR FPSです。Questをプレイヤー、Windowsを観戦・管理クライアントとして使います。Mac向け手順も同梱していますが、Mac実機は未検証です。Linux Docker専用サーバーが認証、試合、建築、弾・盾・HP・勝敗を管理します。

床は既定 **X4.5m × Y7m** の固定長方形です。管理者が「床の位置調整」を押した瞬間の二人のXY中点を床の中心、P1→P2を+Y方向として計算します。歩行への追従、二人の間隔による伸縮、頭の高さによる床の上下移動は行いません。

観戦画面のプレイヤー表示には**UE5標準Manny**、方向別の歩行・走行BlendSpace、PBIKによる全身IKを使います。プレイヤー画面では自分と相手の仮想アバターを表示しません。頭・両手は計測、腰・足は推定です。しゃがみで体を縮尺変更せず、足の接地を補正します。[身体の仕組みと制限](Docs/Avatar.md)を参照してください。管理者は壁を配置・回転し、壁面のマスを開口・復元して高さを調整できます。UIは日本語の短い表示にしています。[操作](Docs/Controls.md)

全クライアントはrole付きEd25519署名JWTと、稼働サーバーのDTLS証明書pinが必要です。発行者の秘密鍵はホストに保持し、コンテナのenvには公開鍵のみを渡します。JWTは1接続で1回使用でき、再接続には新規発行が必要です。鍵・JWT・部屋の映像はGitに含めません。

実機確認済みの配布版は **protocol5** です。Windows配布版、Linux専用サーバー／Docker image、Quest APKをビルドし、Windowsの日本語UIと管理者接続を確認しています。**UE Automation33件が合格**し、テスト姿勢で頭・手のIK最大誤差は立位0.94cm、しゃがみ1.09cmでした。これは実機の身体追跡精度ではありません。接続復帰を追加した最新ソースは **protocol6** で、全クライアントとサーバーの再ビルド・更新・検証が必要です。protocol5との混在接続はできません。証拠はローカルの `Saved/Validation/MannyBuild-20261028-1745/` にあります。

直前のprotocol4配布版では、実Windowsゲーム画面をWindows観戦アプリ内で約23〜24fps表示し、LAN中継でも確認しました。認証33件、2プレイヤー＋3観戦者の27項目と300秒継続試験も合格しました。これはQuest映像のFPSや物理MRの最終確認とは別です。

Quest実機→Linuxコンテナ→Windows配布版で、実写パススルーを含む映像を観戦画面に表示して目視確認しました。protocol5でQuestのUDP送信バッファを32KiBから1MiBに変えた比較では、Windowsの受信は調整前の約5fpsから、調整後の52秒間平均約23fpsに改善しました。2026-10-29には既定設定を組み込んだprotocol6のAPKでも確認し、Windows側の94.58秒間の受信・デコードは平均28.24fpsでした。共有許可画面で30秒以上待機しても同じ認証済み接続が維持され、許可後に映像が再開しました。プレイヤーが動いたときや長時間の安定性は未確認です。共有にはQuest側の許可が必要です。他のキャストがMediaProjectionを開始すると既存共有が終了するため、MRFPSの1配信をサーバーで観戦者へ分配します。[Metaの仕様](https://developers.meta.com/vr/documentation/native/native-media-projection/)

Macのビルド・起動手順とメモリ監視を同梱しています。[Macで開発する](Docs/Mac.md)。最新の運用情報は[引き継ぎ](Docs/Handoff.md)に記録します。

## ビルドと起動

このPCのエンジンは `C:\UnrealEngine\UE5.8`。別PCでは `-EngineRoot` を指定してください。SDKは `.local/android` にローカル導入済みです。

ビルドにはメモリ監視 wrapper を推奨します。起動前と2秒ごとに空き物理RAM 10 GiB以上・system commit 75%以下を確認し、超過を検出すると自身が起動した子プロセスだけを停止します。UBTは既定1 action、Shader workerはこのPCで2、GradleはJava heap 2 GiB・worker 2に制限しています。メモリ節約のためLinuxサーバーの既定ビルドはリンク時のデバッグ情報とBreakpad生成を省きます。フルシンボルが必要な場合は、十分なメモリを確保して `-LinuxPortableSymbols` を指定してください。nativeビルドと複数クライアント試験は順番に実行してください。

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
& $python tools\auth\mrfps_tokens.py issue `
  --private-key .local\auth-issuer\issuer-private.pem `
  --subject operator-1 --role admin --audience mrfps --ttl 300 `
  --dtls-fingerprint $dtlsFingerprint --output .local\access\operator-1.jwt

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

既定では管理者の「開始」が必要です。署名された `admin` tokenでWindows配布版を開き、F2で管理パネルを表示します。観戦者tokenでは管理者へ昇格できません。

```powershell
$client = Join-Path $PWD 'Artifacts\Packages\Windows\MRFPS.exe'
$adminToken = Join-Path $PWD '.local\access\operator-1.jwt'
$publicKey = Join-Path $PWD '.local\auth-issuer\issuer-public.pem'
& $client '/Game/Maps/MRFPSArena' -nohmd -MRFPSDesktop -MRFPSAdmin `
  -MRFPSServer=192.168.1.10:7777 `
  "-MRFPSAuthTokenFile=$adminToken" "-MRFPSAuthPublicKeyFile=$publicKey" `
  -MRFPSAuthAudience=mrfps
```

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

両プレイヤーは同じ実床の原点・向き（ゲームの+X）を使い、Quest左Yで個別に校正します。管理者は双方の基準を確認してF2の「原点登録」、現在の二人の位置を使う「床の位置調整」を行い、準備がそろったら「開始」を押します。共有アンカーの自動解決ではなく、物理マーカーを使う手動原点・向き合わせです。床は明示校正の間は静止し、プレイヤーの実測位置を開始点へ移動・縮尺変更しません。desktopの開始点は床のY両端に置くnominal位置として区別します。

- MRピストルは最初から右手に装備し、右トリガーで発射します。射撃していない間に右Aを2秒押し続けるとリロードします。右グリップでフラッシュグレネードに切り替え、離すと手の速度で投げます。
- 左手を上げると前方に壁を出し、下げると自分が最後に出した壁を消します。再設置4秒、寿命6秒、HP120。
- 左手を開くと小型盾。保持と被弾でエネルギーを消費し、閉じて回復します。
- HP100、胴体20／頭30ダメージ、復活3秒。既定は先に10回倒すと勝利。次の試合は管理者が「試合リセット」から進めます。
- 認証接続後の待機中は床合わせ前でも射撃・盾・壁を練習できます。対人ダメージには校正済みの共有床が必要です。追跡消失、死亡中、戦闘の一時停止中は攻撃できません。
- 管理者の一時停止は戦闘だけを止め、追跡・映像・通信・認証期限の処理は続けます。Bで壁編集、壁のマスをクリックして開口・復元、ホイールで高さを変更、Rで回転、Delで削除します。
- 初期マップに壁2枚と木箱2個を配置します。木箱は弾を遮り、建築モードで指してDelを押すと個別に削除できます。
- Quest の MoBits は右スティック押込でゲーム内設定を開き、左右の登録・装着者の校正・波形確認後に有効化します。右Bで停止します。

キーボード、観戦カメラ、接続メニューの操作は [Controls](Docs/Controls.md) に記載しています。

## 運用状態

[UE local control](tools/automation/README.md) から起動中のクライアントとサーバーの状態を確認できます。

## デバイス連携

- [MoBits](Docs/MoBits.md)：Quest内蔵Bluetoothによる単体接続とゲーム内校正。所有者の発射・被弾を反動・被弾波形に結び付けます。
- [Passthrough](Docs/Passthrough.md)：`XR_FB_passthrough` のnative underlay。Meta XRプラグインには依存しません。
- [壁の編集](Docs/WallEditing.md)：管理者による壁の配置、開口、高さ調整。
- [認証の運用](Docs/Auth.md)、[native検証仕様と証拠](Docs/Authentication.md)：全接続の署名JWT、公開鍵env、単回使用、DTLS証明書pin、資格情報の非表示。
- [実機確認](Docs/DeviceValidation.md)、[実装と未達項目](Docs/ImplementationStatus.md)。
