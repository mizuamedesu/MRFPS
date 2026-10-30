# 公開専用サーバーイメージの発行

利用者が公開を許可したサーバー runtime のみを、既存の private package と別の GHCR package に発行します。`.github/workflows/publish-linux-server.yml` の private 専用ガードは変更しません。

`tools/cloud/publish_public_server.py` は既定で read-only の plan を表示します。`--mode publish` は入力 bundle の SHA-256、サーバー ELF、既知の秘密鍵／JWT パターン、公開 repository と source commit を確認してから、同じ digest の draft release asset を再利用または作成し、GitHub Actions を起動します。同じ操作の実行中・成功済み workflow は再利用し、既存 asset の上書きはしません。

```powershell
$python = '.local\auth-tools\Scripts\python.exe'
& $python tools/cloud/publish_public_server.py `
  --bundle-directory Saved\Validation\CloudP14-Bc0a-20261030-01\bundle `
  --source-commit bc0a8d683a2b7ece5c0ff933be6de1e90afef1fc `
  --server-sha256 52e48e2c598f2dca8d2a3e6686b86e4ac35ddb8f790adf9293c7ff36527a8db9 `
  --mode plan
# 公開が許可された後、上と同じコマンドに --mode publish を指定。
```

公開 workflow は GitHub ホストの runner で構築し、別名 `mrfps-server-public-<bundle SHA 先頭16桁>` を使用します。既存 package の権限や状態が不明な場合は停止します。発行後は公開 metadata、repository への紐付け、署名なしの manifest と layer 取得、ログインなしの Docker pull、pull した実 ELF の hash を検証します。この一連の確認が成功するまでは、Flash が pull できるとは扱いません。

GitHub は workflow の `GITHUB_TOKEN` が新規作成する package について repository の visibility を継承すると[説明しています](https://docs.github.com/en/packages/managing-github-packages/using-github-actions-workflows/publishing-and-installing-a-package-with-github-actions)。公開状態は環境ごとに実測します。SHA-256 の一致は入力の同一性を示しますが、ソースからのビルド再現性や、あらゆる秘密情報の不在を証明するものではありません。Draft release は公開しません。Flash での起動・ネットワーク疎通・クライアント接続は別途実機確認が必要です。