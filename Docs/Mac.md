# Macでの開発・観戦

UE **5.8.1**、同版に対応するXcode、Python3を用意します。エンジンはGitに含めません。既存のWindows固有のEngineAssociationを5.8へ変更し、MacではQuest用OpenXRをロードしない構成にしています。観戦・管理者の署名JWT、公開鍵、DTLS pin、映像受信、共有ジオメトリはWindowsと共通です。Mac実機のビルド・描画・接続はまだ検証していません。

```bash
git clone https://github.com/mizuamedesu/MRFPS.git
cd MRFPS
export UE_ROOT='/Users/Shared/Epic Games/UE_5.8'
bash Scripts/mac/build.sh Editor  # 開発用
bash Scripts/mac/build.sh Client  # 配布アプリ
```

発行者から「現在稼働しているサーバーの証明書pin」に署名された未使用JWTと公開鍵ファイルを受け取り、リポジトリ外に保存してください。秘密鍵はクライアントへ配りません。再接続時は新しいJWTが必要です。

```bash
bash Scripts/mac/run-client.sh '192.168.1.10:7777' ~/.config/mrfps/access.jwt ~/.config/mrfps/issuer-public.pem inspector
# 管理者はadmin署名JWTを指定し、最後の引数をadminに変更
```

`MRFPS_APP`で配布版の実行ファイルを直接指定できます。未指定なら既定の配布版、なければEditorのゲーム実行を使います。画面取得はアプリ自身のUE viewportだけを読みます。Macのデスクトップ全体は取得しません。

ビルドは1 action、.NET heapは2GiB。メモリ監視は総RAMの25%（最小3GiB、最大10GiB）を残し、下限に達したら起動した処理のプロセスグループだけを停止します。Macの`vm_stat`で取得できない場合も起動しません。Mac固有のメモリ監視はWindowsでは実測できないため、初回にレポートを確認してください。

最新の実測範囲と残作業は[ImplementationStatus](ImplementationStatus.md)を参照してください。Questでは別のキャストを終了し、MRFPSの共有を許可します。WindowsのSDKアプリにしかない機器機能をMacで使えたとは扱いません。
