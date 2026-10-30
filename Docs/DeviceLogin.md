# デバイスログイン

Quest は起動するとサーバー設定、デバイスログインの順に表示します。右コントローラーで画面を指し、トリガーで操作します。端末内のキーボードでアドレスと6桁の数字を入力できます。Windows も同じ手順で接続します。初期接続先は `mrfps.mizuame.app:7777` で、変更した接続先は端末に保存されます。現在の Flash の直接通信ポートは `mrfps.mizuame.app:30000` です。接続中の Windows では F1 メニューの「サーバー設定・デバイスログイン」で戻れます。

Quest の接続画面では、右コントローラーの先から出る線をボタンに合わせ、右トリガーで選択します。接続後も左 X でパネルを開閉できます。

直接インストールした APK は Quest のホームから「ライブラリ → 提供元不明」で `MRFPS` を選んで起動します。Meta による通常配布の一覧とは区別されます。 [Meta の手順](https://developers.meta.com/vr/documentation/android-apps/enable-developer-mode/)

管理画面に表示された役割と一致する6桁のコードを入力してください。認証先は `https://mrfps-admin.mizuame.app/api/device/exchange` に固定されます。成功画面はコード交換と署名確認の成功を示します。実際のゲーム参加は、その後の専用サーバー側の認証と DTLS 接続が成功して初めて成立します。デバイスコードで接続したセッションが切断されたら、新しいコードでログインし直します。旧来の `-MRFPSAuthTokenFile` 接続では、使い回しを拒否したうえで新しい JWT ファイルの投入を待ち、自動再接続します。

初回は HTTPS で取得した発行者の公開鍵をローカルに固定します（初回接続先を信頼する方式）。後から異なる鍵が届いた場合は拒否し、画面に警告と「保存済みの公開鍵を解除」を表示します。公開鍵が意図的に変更されたと確認してから解除してください。専用サーバーは独立して公開鍵と DTLS の署名済み fingerprint を検証します。

## コマンド操作

`tools/client/device_login.py` は、接続先・役割・入力コードを指定して同じ処理を起動します。コードは**6桁の ASCII テキストファイル**で渡し、コマンドラインや表示・ログには値を出しません。Quest の転送は adb 標準入力とアプリ private storage を使うため、`run-as` に対応した開発 APK が必要です。Windows の制御ファイルは ACL を制限した `%LOCALAPPDATA%\MRFPS\DeviceLogin\controls` に置き、ゲーム側が読み取った直後に削除します。GUI のコード交換処理と同じ HTTPS API とクライアント側の署名検証を使います。

Mac の画面起動とコード投入は [Macでの開発・観戦](Mac.md) の `--device-login` と `tools/client/mac_login.py` を使います。Mac 実機でのビルド・ログイン・接続は未検証です。

Quest の `--show-settings` は起動時の制御ファイルを消して設定画面を開きます。接続先はアプリの保存値で、同時に指定した `--server` は画面に反映されません。結果の `serverArgumentApplied=false` と `pageConfirmed=false` は、引数を適用していないことと、起動後の現在画面を確認していないことを示します。接続前に画面で接続先を確認してください。

```powershell
$mrfpsPython = 'C:\UnrealEngine\UE5.8\Engine\Binaries\ThirdParty\Python3\Win64\python.exe'
# 設定画面をそのまま表示したい場合は --show-settings を追加
# 先に起動してログイン画面を表示（--editor を省くとパッケージ版）
& $mrfpsPython tools/client/device_login.py windows --editor `
  --executable C:\UnrealEngine\UE5.8\Engine\Binaries\Win64\UnrealEditor.exe `
  --name MRFPS-Inspector-1 --role inspector

# 表示後に発行したコードを、起動済みの同じ画面に投入
& $mrfpsPython tools/client/device_login.py windows --editor `
  --executable C:\UnrealEngine\UE5.8\Engine\Binaries\Win64\UnrealEditor.exe `
  --name MRFPS-Inspector-1 --role inspector --code-file .local\device-code.txt

# ガード状態と秘密を除いた認証イベントを確認
& $mrfpsPython tools/client/device_login.py status --name MRFPS-Inspector-1

# ガードに属する対象ウィンドウだけを通常終了（繰り返し実行可）
& $mrfpsPython tools/client/device_login.py stop-windows --name MRFPS-Inspector-1

# Quest（保存済みのサーバー設定画面から起動）
& $mrfpsPython tools/client/device_login.py quest --adb .local\android\platform-tools\adb.exe `
  --serial QUEST_SERIAL --role player --show-settings

# Quest（こちらも最初は --code-file を省いて起動できる）
& $mrfpsPython tools/client/device_login.py quest --adb .local\android\platform-tools\adb.exe `
  --serial QUEST_SERIAL --role player --code-file .local\device-code.txt


# 指定した Quest の MRFPS アプリだけを停止（繰り返し実行可）
& $mrfpsPython tools/client/device_login.py stop-quest --adb .local\android\platform-tools\adb.exe `
  --serial QUEST_SERIAL

# 意図した鍵変更を確認してから実施。何度実行しても同じ状態になります。
& $mrfpsPython tools/client/device_login.py reset-trust quest --adb .local\android\platform-tools\adb.exe --serial QUEST_SERIAL
& $mrfpsPython tools/client/device_login.py reset-trust windows
```

同じコードファイルの同じ更新時刻、接続先、役割、プロセスの組み合わせを再実行すると、既存のプロセスを維持して終了します。同じログイン画面に新しい接続先・役割・コードを渡すと再起動せずに入力できます。`--replace` は、このコマンドが起動した Windows クライアント、または対象 Quest の MRFPS アプリだけを再起動します。別の Windows インスペクターはそれぞれ別の `--name` とコードを使って起動できます。6桁コードの交換自体は意図的に一回限りで、冪等な再発行や同じコードの再利用はできません。`login_queued`／`settings_queued` はファイル投入の完了を示し、画面が開いて処理するまで待ちます。認証や参加の成功を保証しません。信頼解除コマンドは固定された公開鍵と旧アクセス資格情報のみを削除し、稼働中プロセスのメモリ上の鍵には影響しません。稼働中なら対象アプリを再起動してください。


`stop-quest` は指定した serial の `org.mrfps.arena` のみを停止し、終了待ちの起動要求が残る場合にも対象アプリへ `force-stop` を送ります。プロセスが消えたことを確認できない場合はエラーで止まり、同じ停止コマンドは再実行できます。画面共有の OS 許可は操作しません。次の `quest` 起動時には、対象アプリが停止済みで、MRFPS 由来の孤立した共有ダイアログだけだと確認できた場合に限ってその古いタスクを閉じます。確認できない状態では起動を中断します。

Quest のコード投入は、ファイルのパスと更新時刻を送信前に記録します。アプリ停止・再起動やコードなし起動を挟んでも同じ記録のコードは再送しません。転送中に失敗して結果が不明になった場合も再送せず、新しく発行したコードを使用してください。記録にはコード自体を保存せず、壊れた記録は自動消去しません。履歴の上限 16,384 件に達すると送信を停止します。履歴は端末ごとの `Saved/DeviceAuth/quest-<serial>.json` にあります。

Windows も名前ごとの `Saved/DeviceAuth/windows-<name>.json` に投入前の記録を残し、同じコードの再投入を防ぎます。転送中断時は新しいコードを使用してください。ガードの起動途中でプロセスの識別情報を確定できなかった場合は `status` が `launch_unknown` を返し、重複起動や別プロセスの停止を避けるため自動復旧しません。表示される `launch_config` と `guard_report` を使って対象ガードの状態を調べ、稼働中のプロセスを誤って停止しないようにしてください。コードの値は記録されません。

Windows クライアントは毎回、空き RAM 10 GB 以上・コミット率 75% 以下の既存のメモリガードの子プロセスとして起動します。制限を超えた場合、ガードは自身のジョブだけを停止します。公開鍵の Windows 保存先は `%LOCALAPPDATA%\MRFPS\Auth` です。

開発版の UE コンソールでは `MRFPSConfigureServer <host:port>`、`MRFPSLoginFile <path> <player|inspector|admin>`、`MRFPSResetIssuerTrust` を使用できます。ファイルによる起動パラメータは `-MRFPSServer=... -MRFPSRole=... -MRFPSDeviceCodeFile="..."` です。コードそのものを引数にしないでください。
