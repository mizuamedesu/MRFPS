# Mannyと身体の動き

UE5.8標準テンプレートの`SKM_Manny_Simple`を表示します。`Content/Characters/Mannequins`はエンジンの`Templates/TemplateResources/High/Characters/Content/Mannequins`からコピーした標準素材です。既存の銃・シールドとサーバーの当たり判定は別に維持します。

頭の平面移動から速度・方向を算出し、標準の`BS_Idle_Walk_Run`で待機・歩行・走行をブレンドします。同素材の軸はDirection（−180〜180度）とSpeed（0〜600cm/s）です。アニメーションの周期は実際の移動距離に追従し、静止時には接地した足を保持します。

その姿勢へUnrealのPBIKによる全身IKを適用します。頭・両手が計測対象、骨盤・足が推定対象です。足は登録済みの実測ジオメトリ、なければ共有床へ接地させます。仮想階段を実際の足場とはみなしません。骨を伸ばさず、頭の高さが下がると骨盤と脚を曲げます。体格は校正時に決め、その後のしゃがみで全身を拡縮しません。

視覚用IKはQuestの追跡原点、世界の縮尺、床寸法、サーバーのヒット判定を変更しません。観戦側では短い補間をかけ、追跡が途切れた姿勢は非表示にします。

Quest単体の頭・手の3点追跡では、腰・膝・足先の実際の動作を一意に復元できません。踏み出す足や体重移動は推定です。全身の実測一致には追加の身体トラッカーとその校正が必要です。

実装: `Source/MRFPS/Private/MRFPSAvatar.cpp`。検証: `MRFPS.Avatar.MannyTrackedIKAndLocomotion`。実機の観戦画面で最終的な見た目・遅延も確認します。

参考: [Epic Full Body IK](https://dev.epicgames.com/documentation/en-us/unreal-engine/control-rig-full-body-ik-in-unreal-engine)、[Blend Spaces](https://dev.epicgames.com/documentation/en-us/unreal-engine/blend-spaces-in-unreal-engine)。
