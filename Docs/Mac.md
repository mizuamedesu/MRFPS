# Macでの開発・観戦

UE **5.8.1**、同版に対応するXcode、Python3を用意します。エンジンはGitに含めません。既存のWindows固有のEngineAssociationを5.8へ変更し、MacではQuest用OpenXRをロードしない構成にしています。観戦・管理者の署名JWT、公開鍵、DTLS pin、映像受信、共有ジオメトリはWindowsと共通です。Mac実機のビルド・描画・接続はまだ検証していません。

この開発環境のUE5.8.1は、`Engine/Config/Apple/Apple_SDK.json`の標準Xcodeが**26.1.1**、`Engine/Config/Mac/Mac_SDK.json`の対象OSが**macOS 14.0以上**です。使用するエンジン側の同ファイルを確認し、Xcode初回起動とコマンドラインツールの設定を済ませてください。ビルド時にFullBodyIK、ControlRig、IKRigも有効になります。

```bash
git clone https://github.com/mizuamedesu/MRFPS.git
cd MRFPS
export UE_ROOT='/Users/Shared/Epic Games/UE_5.8'
bash Scripts/mac/build.sh Editor  # 開発用
bash Scripts/mac/build.sh Client  # 配布アプリ
```

Mac 上では、UE を起動せず CLI とメモリガードの軽量テストも実行できます。

```bash
python3 -m unittest discover -s tools/dev -p 'test_mac_resource_guard.py' -v
python3 -m unittest discover -s tools/dev -p 'test_mac_launchers.py' -v
python3 -m unittest discover -s tools/client -p 'test_mac_login.py' -v
```

これらは実クライアントやサーバーへの接続試験ではありません。初回は続けて Mac のビルド、ログイン、サーバー参加、映像・別ウィンドウの表示を実機で確認してください。

デバイスコードでログイン画面から起動する場合は、各ウィンドウに別の名前を指定します。繰り返し起動しても同じ名前の管理対象プロセスが動いていれば再利用します。

```bash
bash Scripts/mac/run-client.sh --device-login MRFPS-Inspector-1
```

画面で接続先と6桁のコードを入力できます。CLI から投入する場合は、管理者が用意した6桁の ASCII コードファイルを端末内で `chmod 600` にし、以下を実行します。コードそのものを引数に書かないでください。

```bash
operation_id="$(python3 -c 'import uuid; print(uuid.uuid4())')"
python3 tools/client/mac_login.py queue --name MRFPS-Inspector-1 \
  --server mrfps.mizuame.app:7777 --role inspector \
  --code-file "$HOME/.config/mrfps/device-code.txt" --operation-id "$operation_id"
python3 tools/client/mac_login.py status --name MRFPS-Inspector-1 --operation-id "$operation_id"
```

画面が開いていることを確認してから投入します。`queued` はローカル投入完了であり、認証やサーバー参加の成功を意味しません。`already_queued` は同じ操作の再実行です。別の操作が投入待ちなら上書きせず失敗し、同じ UUID に別の内容を指定すると拒否します。処理途中に停止して結果が特定できないと `unknown` を返し、同じコードを勝手に再送しません。コードの有効期限は最大300秒で、交換後は一回限りです。

未処理の投入を取り消すには `python3 tools/client/mac_login.py cancel --name MRFPS-Inspector-1 --operation-id "$operation_id"` を使います。画面側が既にファイルを受け取っていた場合は `unknown` であり、認証を取り消せたとは扱いません。起動中断でプロセス状態が `unknown` なら、該当するクライアントが停止していることを確認したうえで `python3 tools/client/mac_login.py recover --name MRFPS-Inspector-1` を実行できます。保護された操作ファイルと結果は `~/Library/Application Support/MRFPS/DeviceLogin` に保存します。

名前を指定して状態を確認し、そのコマンドが管理するクライアントだけを停止できます。停止は再実行できます。

```bash
python3 tools/client/mac_login.py status --name MRFPS-Inspector-1
python3 tools/client/mac_login.py stop --name MRFPS-Inspector-1 --wait-seconds 15
```

`stopped` は対象の終了を確認できた状態です。`stopping` は停止要求を受け付けた状態なので、後で `status` または `stop` を再実行してください。`unknown` の場合は対象が終了したと判断できていません。ガードの終了処理が `cleanup_failed` の場合や、終了レポートを書けずにガードが消えた場合も `unknown` となります。対象の残留プロセスがないことを確認してから `recover` してください。`unsupported` は旧版のガードで起動したプロセスを示します。そのウィンドウを閉じてから新しいコマンドで起動してください。停止前に記録したプロセスの名前・起動時刻・識別子を照合し、一致したガードだけに停止要求を渡します。

`status` の `guard_report` に、`requested_stop`、`memory_limit`、`insufficient_memory`、`monitor_unavailable`、`monitor_error`、`launch_failed`、`cleanup_failed`、`completed`、`child_failed` の終了理由と空きメモリの最小値を表示します。レポートは上記ディレクトリの名前付き `state` 内に保存され、入力コードや JWT は含みません。起動直後など、まだレポートが作られていない場合もあります。

従来の署名済み JWT と公開鍵ファイルを受け取る方法も使えます。秘密鍵はクライアントへ配りません。再接続時は新しい JWT が必要です。

```bash
bash Scripts/mac/run-client.sh '192.168.1.10:7777' ~/.config/mrfps/access.jwt ~/.config/mrfps/issuer-public.pem inspector
# 管理者はadmin署名JWTを指定し、最後の引数をadminに変更
```

`MRFPS_APP`で配布版の実行ファイルを直接指定できます。未指定なら既定の配布版、なければEditorのゲーム実行を使います。両方式とも既存のメモリガードの下で起動します。画面取得はアプリ自身のUE viewportだけを読みます。Macのデスクトップ全体は取得しません。

ビルドは1 action、.NET heapは2GiB。メモリ監視は総RAMの25%（最小3GiB、最大10GiB）を残し、下限に達したら起動した処理のプロセスグループだけを停止します。Macの`vm_stat`で取得できない場合も起動しません。Mac固有のメモリ監視はWindowsでは実測できないため、初回にレポートを確認してください。

最新の実測範囲と残作業は[ImplementationStatus](ImplementationStatus.md)を参照してください。Questでは別のキャストを終了し、MRFPSの共有を許可します。WindowsのSDKアプリにしかない機器機能をMacで使えたとは扱いません。
