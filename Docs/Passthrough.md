# Quest 3 passthrough

`Plugins/MRFPSXR` は UE5.8 同梱 OpenXR の extension plugin API で `XR_FB_passthrough` を使用します。Meta XR plugin は必須ではありません。実 runtime の capability がない場合、エラーを記録して `FMRFPSXRModule::IsPassthroughRunning()` は false を返します。

## 実装と参照

- `PostConfigInit` で `IOpenXRExtensionPlugin` modular feature に登録し、instance 作成時に optional extension を要求します。
- `PostGetSystem` の `XrSystemPassthroughPropertiesFB` で実機の support を確認します。
- `PostCreateSession` で feature と reconstruction layer を paused で作成します。`OnBeginSession` は engine が `xrBeginSession` を呼ぶ前なので、そこで開始しません。session の SYNCHRONIZED / VISIBLE / FOCUSED event で native start と layer resume を成功させた後にだけ running を公開します。
- `UpdateCompositionLayers_RHIThread` で、永続 member の `XrCompositionLayerPassthroughFB` を header 配列の先頭に追加します。`space` は `XR_NULL_HANDLE`。scene projection に source alpha blending を設定します。
- session 停止時に layer と feature を pause します。UE5.8 の `FOpenXRHMD::DestroySession` は rendering を flush してから extension の `OnDestroySession` を呼ぶため、その後に native layer、feature の順で破棄します。module shutdown 自体でも flush します。
- runtime の passthrough error event では running を解除します。recoverable error が restored された場合だけ元の active layer の状態を復元します。reinitialization や nonrecoverable error は session の再作成が必要です。

呼び出し・composition 順は [Meta の native passthrough 実装ガイド](https://developers.meta.com/vr/documentation/native/android/mobile-passthrough/) と、[Khronos の xrCreatePassthroughLayerFB](https://registry.khronos.org/OpenXR/specs/1.1/man/html/xrCreatePassthroughLayerFB.html)、[xrPassthroughStartFB](https://registry.khronos.org/OpenXR/specs/1.1/man/html/xrPassthroughStartFB.html) を確認しています。Meta runtime は passthrough 使用時も environment blend mode OPAQUE を使用し、underlay は projection より前へ送ります。

UE 側の callback と寿命は、実際にインストールされている `C:/UnrealEngine/UE5.8/Engine/Plugins/Runtime/OpenXR/Source/OpenXRHMD/Public/IOpenXRExtensionPlugin.h` と `Private/OpenXRHMD.cpp` の `DestroySession` / `OnFinishRendering_RHIThread` を確認しています。

## Android と画面の alpha

`MRFPSXR_UPL.xml` は head tracking、passthrough feature、optional hand tracking とその permission、Quest 3/3S の supported devices metadata、passthrough loading screen を追加します。VR launch category の重複を避けながら登録します。参考は [Meta Android manifest settings](https://developers.meta.com/vr/documentation/native/android/mobile-native-manifest/) と [passthrough loading screen](https://developers.meta.com/vr/documentation/native/android/mobile-passthrough-loading-screens/) です。

supportedDevicesはEditor側がExtraApplicationSettingsへ再追加する場合があるため、UPLで既存の同name metadataを削除し `quest3|quest3s` の1ノードを追加する実装です。更新packageの生成manifestでcountを確認する工程は別に記録します。

UE5.8 の Android OpenXR loader は `bPackageForMetaQuest=True` により engine OpenXR_APL が生成する `AndroidThunkJava_IsOculusMobileApplication()` の結果も確認します。manifest の VR category だけでは足りません。プロジェクトの AndroidRuntimeSettings でこの値を有効にします。

scene の空部分を compositor が透過できるよう、desktop は `r.PostProcessing.PropagateAlpha=1`、mobile は **`r.Mobile.PropagateAlpha=1`（数値）**を指定します。このUEのAndroid packaging側は整数として読むため、`True`ではruntime Alphaが0になりました。sky、opaque な全画面背景、全面を覆う floor があると underlay は見えません。壁や盾、他プレイヤー等の仮想物体だけを MR 空間に表示し、inspector は別の仮想観戦空間として扱います。

`Saved/Validation/Quest-20261028-120538/logcat.txt` では `AndroidPropagateAlpha=1`、`vr.MobileMultiView=1`、generic `[Android]` profileの `r.Android.DisableVulkanSupport:0 -> 0`、`VulkanRHI will be used!` とVulkan device初期化を確認しました。`Native OpenXR passthrough is running` も記録されています。これはdevice上のnative API起動の証拠で、装着者が見た部屋・仮想物の描画や位置合わせの確認ではありません。

## 実機で必要な確認

起動ログの `XR_FB_passthrough system support: yes` と `Native OpenXR passthrough is running` は API の状態です。見え方の動作確認には Quest 3 実機で部屋が見え、その上に仮想壁・盾・プレイヤーが描画されること、pause/resume 後も戻ること、終了時に handle error がないことを確認する必要があります。これらはビルド成功だけでは検証できません。
