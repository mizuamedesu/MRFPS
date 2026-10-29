# Ubuntu の MRFPS 専用 keepalive

この Windows 用コマンドは、Ubuntu 内に MRFPS 専用の低負荷プロセスを一つ維持します。既存の WSL セッション、他のディストリビューション、Docker デーモン、コンテナや WSL 全体を直接停止しません。 Ubuntu が停止中なら `ensure` で起動するため、OS の設定済みサービスが起動する場合があります。既存の手動 keepalive はこのコマンドの管理対象にならず、削除もしません。

```powershell
$taskPython = 'C:\UnrealEngine\UE5.8\Engine\Binaries\ThirdParty\Python3\Win64\python.exe'
& $taskPython tools/wsl_keepalive/keepalive.py ensure
& $taskPython tools/wsl_keepalive/keepalive.py status
& $taskPython tools/wsl_keepalive/keepalive.py stop
```

`ensure` の再実行は確認済みの同じプロセスを再利用し、`stop` の再実行は既に停止していれば何もしません。`status` はプロセスの状態を返します。成功は `started`、`already_running`、`stopped`、`already_stopped` です。`unknown` または非ゼロ終了は完了を意味しません。開始・停止の応答が不明になった場合、同じ操作を自動で繰り返さず、`status` で確認できる場合だけ記録を回復します。

状態と各起動の helper・ログは、Windows ユーザーのみに ACL を制限した `%LOCALAPPDATA%\MRFPS\WslKeepalive` に置きます。ロックはプロセス間で排他的で、状態は操作前に保存します。launcher の出力は保護された専用ファイルに向け、stdout/stderr の pipe を子孫が保持しても launcher の終了待ちが無制限にならないようにします。Windows の PID と生成時刻は launcher が専用 receipt に flush してから完了を通知します。開始完了前に中断され、Windows のプロセス識別子を記録できなかった場合は結果不明で停止します。状態ファイルを消して強制的に再起動すると、未確認のプロセスを重複させる可能性があるため行わないでください。

対象は固定の `Ubuntu`、その `root` で実行する専用 Python helper に限ります。Linux の boot ID、PID、起動 tick、実行ファイル、引数、UUID を照合し、停止には同じ PID に固定した `pidfd` を使います。Linux の `pidfd` を使えない場合は安全側で `unknown` を返し、他の方法で信号を送りません。Windows 側も、自分が開始した `wsl.exe` の PID・生成時刻・実行ファイル・引数を照合します。Windows 側のクライアントだけが先に終了しても、Linux 側が同じプロセスとして生きていれば再起動しません。

`stop` は専用 Linux プロセスへ停止を要求し、そのプロセスと対応する Windows `wsl.exe` の両方の終了を確認します。結果不明のときは勝手に再信号を送りません。Windows 側が残るなど、終了を確認できない場合は `unknown` です。短命の WSL 照会は Ubuntu が稼働中の場合に行います。

Windows では `Start-Process -WindowStyle Hidden` を使います。`windows_in_job` は Windows 側プロセスが Job に所属しているかを示します。この PC では独立した既存 keepalive を維持したまま、新しい keeper の起動・状態確認・重複起動の抑止・停止と停止の再実行を実機で確認しました。その新しい Windows プロセスは Job に所属していました。単独でのアイドル時間や Codex 再起動を超えた持続は未検証です。このコマンドは、PC 再起動・Ubuntu の異常終了・明示的な WSL 終了後の自動起動や、止まったコンテナの復旧を保証しません。

軽量テスト：

```powershell
& $taskPython -m unittest discover -s tools/wsl_keepalive -p 'test_*.py' -v
```
