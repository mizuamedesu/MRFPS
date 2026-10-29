#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerState.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/HUD.h"
#include "Engine/GameInstance.h"
#include "MRFPSAuthVerifier.h"
#include "MRFPSVideoProtocol.h"
#include "MRFPSGeometryProtocol.h"
#include "MRFPSArenaLayout.h"
#include "MRFPSBuild.h"
#include "MRFPSConsentPolicy.h"
#include "MRFPSGame.generated.h"

class UCameraComponent;
class UMotionControllerComponent;
class UStaticMeshComponent;
class UInstancedStaticMeshComponent;
class UBoxComponent;
class UCapsuleComponent;
class USphereComponent;
class UTextRenderComponent;
class UWidgetComponent;
class UFloatingPawnMovement;
class USceneCaptureComponent2D;
class UTextureRenderTarget2D;
class UMRFPSHapticsComponent;
class UVRNotificationsComponent;
class UMRFPSMenu;
class UTexture2D;
class UMRFPSAdminPanel;
class UMRFPSAvatarComponent;
class SWindow;
class SWidget;

UENUM()
enum class EMRFPSAdminAction : uint8 { Start,Pause,Resume,Reset,ConfirmSharedFloor,CalibrateFloor,SetDimensions,ClearAdminWalls };

UENUM(BlueprintType)
enum class EMRFPSMatchPhase : uint8 { Waiting, Playing, Finished };

// Quantized ContactGlove joint flexions and finger spreads, thumb to little.
// Invalid input is explicit; a disconnected glove must never retain a grip.
USTRUCT()
struct FMRFPSFingerPose
{
    GENERATED_BODY()
    UPROPERTY() bool bValid = false;
    UPROPERTY() uint8 Joint[15] = {};
    UPROPERTY() uint8 Splay[5] = {128,128,128,128,128};
    float Curl(int32 Finger) const
    {
        if (!bValid || Finger<0 || Finger>=5) return 0;
        return (float(Joint[Finger*3])+Joint[Finger*3+1]+Joint[Finger*3+2])/(3.f*255.f);
    }
};

// Measured shared-world centimetres. Configured/nominal starts are separate.
USTRUCT()
struct FMRFPSPose
{
    GENERATED_BODY()
    UPROPERTY() uint32 Sequence = 0;
    UPROPERTY() FVector_NetQuantize10 Head = FVector(0,0,170);
    UPROPERTY() FVector_NetQuantize10 Left = FVector(30,-30,120);
    UPROPERTY() FVector_NetQuantize10 Right = FVector(40,25,130);
    UPROPERTY() FRotator HeadRotation = FRotator::ZeroRotator;
    UPROPERTY() FRotator LeftRotation = FRotator::ZeroRotator;
    // RightRotation is the corrected weapon aim; the separate hand rotation
    // keeps the avatar wrist independent of the weapon grip offset.
    UPROPERTY() FRotator RightRotation = FRotator::ZeroRotator;
    UPROPERTY() FRotator RightHandRotation = FRotator::ZeroRotator;
    UPROPERTY() float LeftOpenness = 0;
    UPROPERTY() FMRFPSFingerPose LeftFingers;
    UPROPERTY() FMRFPSFingerPose RightFingers;
    UPROPERTY() bool bHeadTracked = false;
    UPROPERTY() bool bLeftTracked = false;
    UPROPERTY() bool bRightTracked = false;
    UPROPERTY() bool bDesktop = false;
    bool IsSane() const;
    bool IsInsideArena(const FMRFPSArenaLayout& Layout=FMRFPSArenaLayout()) const;
};

// A single deliberate slide across the magazine, expressed in gun space.
struct FMRFPSReloadGesture
{
    bool Update(const FVector& RelativeHand,double Time,bool bEligible);
    void Reset();
    double StartTime=-1;
    float StartZ=0;
    bool bLatched=false;
};

// State shared by local and authoritative recognizers; uses elapsed receipt time.
struct FMRFPSRiseGesture
{
    bool Update(float Z, double Time, bool bTracked);
    void Reset();
    double StartTime = -1;
    float StartZ = 0;
    double LastTime = -1;
    float LastZ = 0;
    bool bLatched = false;
};

UCLASS()
class MRFPS_API AMRFPSPlayerState : public APlayerState
{
    GENERATED_BODY()
public:
    AMRFPSPlayerState();
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& Out) const override;
    UPROPERTY(Replicated, BlueprintReadOnly) int32 Slot = -1;
    UPROPERTY(Replicated, BlueprintReadOnly) bool bInspector = false;
    UPROPERTY(Replicated, BlueprintReadOnly) bool bAdmin = false;
    UPROPERTY(Replicated, BlueprintReadOnly) int32 Kills = 0;
    UPROPERTY(Replicated, BlueprintReadOnly) int32 Deaths = 0;
    UPROPERTY(Replicated, BlueprintReadOnly) bool bAuthenticated = false;
    UPROPERTY(Replicated, BlueprintReadOnly) FString AuthSubject;
    UPROPERTY(Replicated, BlueprintReadOnly) int64 AuthExpiresAt = 0;
    UPROPERTY(Replicated) uint32 VideoStreamId = 0;
    UPROPERTY(Replicated) uint32 GeometryStreamId = 0;
};

UCLASS()
class MRFPS_API AMRFPSGameState : public AGameStateBase
{
    GENERATED_BODY()
public:
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& Out) const override;
    UPROPERTY(Replicated, BlueprintReadOnly) EMRFPSMatchPhase Phase = EMRFPSMatchPhase::Waiting;
    UPROPERTY(Replicated, BlueprintReadOnly) int32 WinnerSlot = -1;
    UPROPERTY(Replicated, BlueprintReadOnly) int32 ScoreToWin = 10;
    UPROPERTY(Replicated, BlueprintReadOnly) double RestartAt = 0;
    UPROPERTY(ReplicatedUsing=OnRepLayout,BlueprintReadOnly) FMRFPSArenaLayout Layout;
    UPROPERTY(Replicated,BlueprintReadOnly) bool bSharedFloorConfirmed=false;
    UPROPERTY(Replicated,BlueprintReadOnly) bool bNominalDesktopSession=false;
    UPROPERTY(Replicated,BlueprintReadOnly) bool bCombatPaused=false;
    UPROPERTY(Replicated) uint32 ControlRevision=1;
    UPROPERTY(Replicated) uint32 SharedFloorRevision=0;
    UPROPERTY(Replicated,BlueprintReadOnly) float MeasuredAnchorDistanceCm=0;
    UFUNCTION() void OnRepLayout();
};

UCLASS()
class MRFPS_API AMRFPSBarrier : public AActor
{
    GENERATED_BODY()
public:
    AMRFPSBarrier();
    virtual void BeginPlay() override;
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& Out) const override;
    void ApplyShot(float Damage);
    UPROPERTY(VisibleAnywhere) TObjectPtr<UBoxComponent> Collision;
    UPROPERTY(VisibleAnywhere) TObjectPtr<UStaticMeshComponent> Visual;
    UPROPERTY(VisibleAnywhere) TObjectPtr<UInstancedStaticMeshComponent> WallCells;
    UPROPERTY(Replicated) float Health = 120;
    UPROPERTY(Replicated) int32 OwnerSlot = -1;
    UPROPERTY(ReplicatedUsing=ApplyBuildKind) bool bAdminPlaced=false;
    UPROPERTY(ReplicatedUsing=ApplyBuildKind) EMRFPSBuildKind BuildKind=EMRFPSBuildKind::Wall;
    UPROPERTY(ReplicatedUsing=ApplyBuildKind) FMRFPSWallShape WallShape;
    UPROPERTY(Transient) TArray<TObjectPtr<UBoxComponent>> WallCollisionParts;
    bool bPlacementPreview=false;
    // Authority-only floor-local anchor; clients receive the resulting transform.
    bool bMapAnchor=false;
    bool bDefaultStageObject=false;
    FVector MapLocalPosition=FVector::ZeroVector;
    float MapLocalYaw=0;
    UFUNCTION() void ApplyBuildKind();
    void SetPreviewColor(bool Valid);
};

UCLASS()
class MRFPS_API AMRFPSArena : public AActor
{
    GENERATED_BODY()
public:
    AMRFPSArena();
    virtual void BeginPlay() override;
    UPROPERTY() TObjectPtr<UStaticMeshComponent> Floor;
    UPROPERTY() TArray<TObjectPtr<UStaticMeshComponent>> GridLines;
    UPROPERTY() TArray<TObjectPtr<UStaticMeshComponent>> NominalStartMarkers;
    void ApplyLayout(const FMRFPSArenaLayout& Layout);
};

UCLASS()
class MRFPS_API AMRFPSPawn : public APawn
{
    GENERATED_BODY()
public:
    AMRFPSPawn();
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;
    virtual void Tick(float DeltaSeconds) override;
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& Out) const override;
    void Calibrate();
    void SetFiring(bool bPressed);
    void DesktopReload();
    void DesktopWall();
    void SetDesktopShield(bool bEnabled) { bDesktopShield = bEnabled; }
    void DesktopMove(const FVector& Input, float DeltaSeconds);
    void DesktopLook(float Yaw, float Pitch);
    void Respawn();
    bool IsXR() const;
    bool CanAct() const;
    bool IsPoseFresh() const;
    bool IsInsideArena() const;
    void OffsetCombatTimers(double Seconds);
    bool GetGeometryCalibration(FTransform& TrackingToArena,double& CalibratedSince) const;
    uint64 GetGeometryCalibrationEpoch() const { return GeometryCalibratedEpoch; }
    void InvalidateGeometryCalibration();
    int32 GetSlot() const;
    void ApplyShot(float Damage, AMRFPSPawn* Shooter, bool bHeadshot);
    void DeployWall();
    FVector Muzzle() const;
    UFUNCTION(Server, Unreliable) void ServerPose(const FMRFPSPose& NewPose);
    UFUNCTION(Server, Reliable) void ServerCalibrate(bool bReady,bool bHasStageFloor=false);
    UFUNCTION(Client,Reliable) void ClientCombatStopped();
    UFUNCTION(Client,Reliable) void ClientNominalDesktopStart(FVector_NetQuantize10 Head,FRotator Rotation);
    UFUNCTION(Server, Reliable) void ServerFire();
    UFUNCTION(Server, Reliable) void ServerDesktopReload();
    UFUNCTION(Server, Reliable) void ServerDesktopWall();
    UFUNCTION(Client, Reliable) void ClientFeedback(uint8 Kind, float Damage);
    UFUNCTION(NetMulticast, Unreliable) void MulticastShot(FVector_NetQuantize Start, FVector_NetQuantize End, bool bHit);
    UFUNCTION() void OnRepPose();
    UPROPERTY(ReplicatedUsing=OnRepPose) FMRFPSPose Pose;
    UPROPERTY(Replicated, BlueprintReadOnly) float Health = 100;
    static constexpr int32 MagazineCapacity=30;
    UPROPERTY(Replicated, BlueprintReadOnly) int32 MagazineRounds = MagazineCapacity;
    UPROPERTY(Replicated, BlueprintReadOnly) float ShieldEnergy = 100;
    UPROPERTY(Replicated, BlueprintReadOnly) bool bShieldActive = false;
    UPROPERTY(Replicated, BlueprintReadOnly) bool bCalibrated = false;
    UPROPERTY(Replicated,BlueprintReadOnly) bool bStageFloorReferenced=false;
    UPROPERTY(Replicated) uint32 GeometryCalibrationGeneration = 0;
    UPROPERTY(Replicated, BlueprintReadOnly) double RespawnAt = 0;
    UPROPERTY(Replicated, BlueprintReadOnly) double WallReadyAt = 0;
    UPROPERTY(VisibleAnywhere) TObjectPtr<USceneComponent> XROrigin;
    UPROPERTY(VisibleAnywhere) TObjectPtr<UCameraComponent> Camera;
    UPROPERTY(VisibleAnywhere) TObjectPtr<UMotionControllerComponent> LeftController;
    UPROPERTY(VisibleAnywhere) TObjectPtr<UMotionControllerComponent> RightController;
    UPROPERTY(VisibleAnywhere) TObjectPtr<USphereComponent> HeadHitbox;
    UPROPERTY(VisibleAnywhere) TObjectPtr<UCapsuleComponent> BodyHitbox;
    UPROPERTY(VisibleAnywhere) TObjectPtr<UBoxComponent> MiniShield;
    UPROPERTY(VisibleAnywhere) TObjectPtr<UStaticMeshComponent> HeadVisual;
    UPROPERTY(VisibleAnywhere) TObjectPtr<UStaticMeshComponent> BodyVisual;
    UPROPERTY(VisibleAnywhere) TObjectPtr<UMRFPSAvatarComponent> Avatar;
    UPROPERTY(VisibleAnywhere) TObjectPtr<UStaticMeshComponent> LeftVisual;
    UPROPERTY(VisibleAnywhere) TObjectPtr<UStaticMeshComponent> GunVisual;
    UPROPERTY(VisibleAnywhere) TObjectPtr<UStaticMeshComponent> ShieldVisual;
    UPROPERTY(VisibleAnywhere) TObjectPtr<UTextRenderComponent> Label;
    UPROPERTY(VisibleAnywhere) TObjectPtr<UTextRenderComponent> HeadsetStatus;
    UPROPERTY(VisibleAnywhere) TObjectPtr<UWidgetComponent> PlayerHud;
    UPROPERTY(VisibleAnywhere) TObjectPtr<UMRFPSHapticsComponent> Haptics;
    UPROPERTY(VisibleAnywhere) TObjectPtr<UVRNotificationsComponent> XRNotifications;
    FString TrackingDescription;
    double LastPoseReceived = -100;
    int32 AcceptedShots = 0;
private:
    void SampleLocalPose(float DeltaSeconds);
    void UpdateVisuals();
    void RemoveDeployedWall();
    void UpdateWeaponFromPose(double Now);
    void UpdatePlayerHud();
    UFUNCTION() void InvalidateCalibration();
    UFUNCTION() void OnXRSpaceChanged();
    bool bDesktopInitialized = false;
    bool bLocalReady = false;
    uint32 GeometryCalibrationFence = 0;
    double GeometryCalibratedSince = 0;
    uint64 GeometryCalibratedEpoch = 0;
    bool bFiring = false;
    bool bDesktopShield = false;
    bool bShieldExhausted = false;
    bool bFingerTriggerDown = false;
    bool bRequireFireRelease=false;
    bool bGloveControlsFresh[2] = {false,false};
    bool bPreviousGloveA[2] = {false,false};
    bool bPreviousGloveB[2] = {false,false};
    int32 VisualTeam = -99;
    FVector DesktopHead = FVector(0,-350,170);
    FRotator DesktopRotation = FRotator::ZeroRotator;
    float PoseAccumulator = 0;
    double LastLocalFire = -100;
    double LastServerFire = -100;
    double LastReloadAt = -100;
    double LastShieldUse = -100;
    double IgnoreOriginChangesUntil = -1;
    FMRFPSRiseGesture ServerRise;
    FMRFPSRiseGesture ServerLower;
    TArray<TWeakObjectPtr<AMRFPSBarrier>> DeployedWalls;
    FMRFPSReloadGesture ServerReload;
    TSharedPtr<SWidget> PlayerHudSlate;
};

UCLASS()
class MRFPS_API AMRFPSInspectorPawn : public APawn
{
    GENERATED_BODY()
public:
    AMRFPSInspectorPawn();
    UPROPERTY(VisibleAnywhere) TObjectPtr<UCameraComponent> Camera;
    UPROPERTY(VisibleAnywhere) TObjectPtr<UFloatingPawnMovement> Movement;
};

UCLASS()
class MRFPS_API AMRFPSPlayerController : public APlayerController
{
    GENERATED_BODY()
public:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;
    virtual void SetupInputComponent() override;
    virtual void PlayerTick(float DeltaSeconds) override;
    virtual bool InputKey(const FInputKeyEventArgs& Params) override;
    bool IsInspector() const;
    bool IsAdmin() const;
    bool IsInspectorMouseReleased() const { return bInspectorMouseReleased; }
    AMRFPSPawn* FindPlayer(int32 Slot) const;
    TArray<AMRFPSPawn*> GetPlayers() const;
    void RefreshCaptures();
    void FreeView();
    void NextView();
    void ViewPlayerOne();
    void ViewPlayerTwo();
    void ToggleMultiView();
    void Calibrate();
    void ArmHaptics();
    void StopHaptics();
    void ToggleMenu();
    void ToggleAdminPanel();
    void AdminPanelClosed();
    void SetAdminWallPlacement(bool Enabled);
    bool IsAdminWallPlacement() const { return bAdminWallPlacement; }
    void RequestAdminAction(EMRFPSAdminAction Action,FVector2D Dimensions=FVector2D::ZeroVector);
    FString AdminStatus;
    UPROPERTY() TObjectPtr<UMRFPSAdminPanel> AdminPanel;
    float AdminWallYaw=0;
    bool bAdminPreviewValid=false;
    float SelectedWallHeightCm=0;
    FString AdminFeedback() const;
    void SetAdminFeedback(FName Code);
    UFUNCTION(Exec) void MRFPSConnect(const FString& Address, bool Inspector = false);
    UFUNCTION(Exec) void MRFPSConnectAdmin(const FString& Address);
    UFUNCTION(Exec) void MRFPSReconnect();
    void HandleSessionDisconnected();
    UFUNCTION(Exec) void MRFPSView(int32 Slot);
    UFUNCTION(Exec) void MRFPSReady();
    UFUNCTION(Exec) void MRFPSShareView();
    UFUNCTION(Exec) void MRFPSReconstructedViews();
    UTexture2D* GetQuestVideoTexture(int32 Slot) const;
    FString GetQuestVideoStatus(int32 Slot) const;
    void OpenPlayerVideoWindow(int32 Slot);
    bool TryOpenInspectorWindowAtCursor();
    void ClosePlayerVideoWindows();
    uint8 PlayerVideoWindowMask() const;
    void OpenInspectorViewWindow();
    void CloseInspectorViewWindow();
    USceneCaptureComponent2D* GetInspectorWindowCamera() const { return InspectorWindowCamera; }
    UTextureRenderTarget2D* GetInspectorWindowTarget() const { return InspectorWindowTarget; }
    void ServiceVideoRelay();
    void ServiceGeometryRelay();
    void ResetGeometryForLayout();
    bool bReconstructedViews = false;
    UPROPERTY() TArray<TObjectPtr<USceneCaptureComponent2D>> Captures;
    UPROPERTY() TArray<TObjectPtr<UTextureRenderTarget2D>> ViewTextures;
    UPROPERTY() TObjectPtr<UMRFPSMenu> SessionMenu;
    bool bMultiView = true;
    int32 ViewSlot = -1;
    FString ConnectionError;
private:
    TWeakPtr<SWindow> PlayerVideoWindows[2];
    TWeakPtr<SWindow> InspectorViewWindow;
    UPROPERTY(Transient) TObjectPtr<USceneCaptureComponent2D> InspectorWindowCamera;
    UPROPERTY(Transient) TObjectPtr<UTextureRenderTarget2D> InspectorWindowTarget;
    bool bConsumePlayerVideoClick=false;
    void ConnectWithRole(const FString& Address,const FString& RequestedRole);
    UFUNCTION(Server,Reliable) void ServerAdminAction(EMRFPSAdminAction Action,FVector2D Dimensions,uint32 ExpectedLayoutRevision,uint32 ExpectedControlRevision);
    UFUNCTION(Server,Reliable) void ServerAdminPlaceWall(FVector_NetQuantize10 Origin,FVector_NetQuantizeNormal Direction,float Yaw,uint32 ExpectedLayoutRevision,uint32 ExpectedControlRevision);
    UFUNCTION(Server,Reliable) void ServerAdminPlaceObject(EMRFPSBuildKind Kind,FVector_NetQuantize10 Origin,FVector_NetQuantizeNormal Direction,float Yaw,uint32 ExpectedLayoutRevision,uint32 ExpectedControlRevision);
    UFUNCTION(Server,Reliable) void ServerAdminEditWall(FVector_NetQuantize10 Origin,FVector_NetQuantizeNormal Direction,EMRFPSWallEdit Edit,uint32 ExpectedLayoutRevision,uint32 ExpectedControlRevision);
    UFUNCTION(Server,Reliable) void ServerAdminRemoveObject(FVector_NetQuantize10 Origin,FVector_NetQuantizeNormal Direction,uint32 ExpectedLayoutRevision,uint32 ExpectedControlRevision);
    UFUNCTION(Client,Reliable) void ClientAdminResult(FName SafeCode);
    bool bAdminWallPlacement=false;
    bool bAdminPlacementWasMultiView=true;
    bool bConsumeAdminPlacementClick=false;
    bool bAdminPanelWasReleased=false;
    double LastAdminActionAt=-100;
    double LastAdminWallAt=-100;
    double LastAdminFeedbackAt=-100,NextAdminRepeatAt=0;
    UPROPERTY(Transient) TObjectPtr<AMRFPSBarrier> AdminPreview;
    UPROPERTY(Transient) TObjectPtr<UStaticMeshComponent> AdminCellPreview;
    void TickAdminPreview();
    void PlaceSelectedBuild();
    bool ToggleSelectedWallCell();
    void AdjustSelectedWallHeight(int32 Delta);
    void RemoveSelectedBuild();
    UFUNCTION(Server, Unreliable) void ServerPublishGeometry(const FMRFPSGeometryChunk& Chunk);
    UFUNCTION(Server, Reliable) void ServerStopGeometry(uint32 Calibration,uint32 FinalSequence,uint32 LayoutRevision);
    UFUNCTION(Client, Unreliable) void ClientMeasuredGeometry(const FMRFPSGeometryChunk& Chunk);
    bool IsGeometrySessionValid(bool Publisher) const;
    void TickMeasuredGeometry();
    void StopGeometryPublisher();
    FMRFPSGeometryAssembler GeometryUpload;
    FMRFPSGeometryAssembler GeometryDownloads[2];
    FMRFPSGeometrySendQueue GeometryPublisherQueue;
    FMRFPSGeometrySendQueue GeometryRelayQueues[2];
    uint32 GeometrySequence=0,GeometryClosedThrough=0,GeometryLastAcceptedSequence=0;
    uint32 GeometryLocalCalibration=0,GeometryNativeSequence=0;
    uint32 GeometryLocalLayoutRevision=0;
    uint64 GeometryTrackingEpoch=0;
    bool bGeometryCaptureRequested=false,bHadGeometryFrames=false;
    int32 NextGeometryRelaySlot=0;
    double GeometryRateAt=0,GeometryRateBytes=0,LastGeometryAcceptedAt=-100,LastGeometryLogAt=-100;
    UFUNCTION(Server, Unreliable) void ServerPublishQuestVideo(const FMRFPSVideoChunk& Chunk);
    UFUNCTION(Server, Reliable) void ServerSubscribeQuestVideo(uint8 Mask);
    UFUNCTION(Server, Reliable) void ServerStopQuestVideo(uint32 FinalSequence);
    UFUNCTION(Server, Reliable) void ServerRequestVideoConsentPause(uint32 Request);
    UFUNCTION(Server, Reliable) void ServerFinishVideoConsentPause(uint32 Request);
    UFUNCTION(Client, Reliable) void ClientVideoConsentPause(uint32 Request,float Seconds);
    UFUNCTION(Client, Unreliable) void ClientQuestVideo(const FMRFPSVideoChunk& Chunk);
    UFUNCTION(Client, Reliable) void ClientQuestVideoStopped(uint8 Slot,uint32 StreamId);
    bool IsVideoSessionValid(bool Publisher) const;
    void TickQuestVideo();
    void LogVideoDiagnostics(const TCHAR* Leg);
    double LastVideoDiagnosticsAt=-10;
    uint64 VideoInboundChunks=0, VideoAssemblyRejected=0, VideoRateRejected=0, VideoFrameRejected=0;
    uint64 VideoAcceptedFrames=0, VideoBackpressureTicks=0;
    FMRFPSVideoAssembler VideoUpload;
    FMRFPSVideoAssembler VideoDownloads[2];
    FMRFPSVideoSendQueue VideoPublisherQueue;
    FMRFPSVideoSendQueue VideoRelayQueues[2];
    uint8 VideoSubscription = 0;
    uint8 LastVideoSubscriptionSent = 255;
    int32 NextRelaySlot = 0;
    uint32 VideoPublishSequence = 0;
    uint32 VideoClosedThrough = 0;
    uint32 VideoLastAcceptedSequence = 0;
    double NextVideoSubscriptionAt = 0;
    double VideoRateAt = 0;
    double VideoRateBytes = 0;
    double LastVideoAcceptedAt = -1;
    double LastVideoLogAt = -10;
    bool bHadVideoInspectorSession = false;
    bool bHadVideoPublisherSession = false;
    bool bViewSharingRequested = false;
    bool bHadCaptureFrames = false;
    MRFPSConsent::FClientFlow VideoConsentFlow;
    uint32 VideoConsentServerRequest = 0;
    void FirePressed();
    void FireReleased();
    void ReloadPressed();
    void WallPressed();
    void ShieldPressed();
    void ShieldReleased();
    void ApplyInspectorMouseMode();
    void SetInspectorMouseReleased(bool Released);
    bool bInspectorMouseReleased = false;
    bool bConsumeInspectorCaptureClick = false;
    void MoveForward(float V) { MoveAxes.X = IsInspector() && bInspectorMouseReleased && !bAdminWallPlacement ? 0.f : V; }
    void MoveRight(float V) { MoveAxes.Y = IsInspector() && bInspectorMouseReleased && !bAdminWallPlacement ? 0.f : V; }
    void MoveUp(float V) { MoveAxes.Z = IsInspector() && bInspectorMouseReleased && !bAdminWallPlacement ? 0.f : V; }
    void LookX(float V);
    void LookY(float V);
    FVector MoveAxes = FVector::ZeroVector;
    FTimerHandle PendingConnectionTimer;
    double LastCaptureRefresh = -100;
    double NextSmokeAction = 0;
    int32 SmokeStage = 0;
    double SmokeCombatStartedAt = -1;
};

UCLASS()
class MRFPS_API AMRFPSHUD : public AHUD
{
    GENERATED_BODY()
public:
    virtual void DrawHUD() override;
};

UCLASS(Config=Game)
class MRFPS_API AMRFPSGameMode : public AGameModeBase
{
    GENERATED_BODY()
public:
    AMRFPSGameMode();
    virtual ~AMRFPSGameMode() override;
    virtual void BeginPlay() override;
    virtual void Tick(float DeltaSeconds) override;
    virtual void PreLogin(const FString& Options, const FString& Address, const FUniqueNetIdRepl& ID, FString& Error) override;
    virtual APlayerController* Login(UPlayer* NewPlayer, ENetRole InRemoteRole, const FString& Portal, const FString& Options, const FUniqueNetIdRepl& ID, FString& Error) override;
    virtual FString InitNewPlayer(APlayerController* PC, const FUniqueNetIdRepl& ID, const FString& Options, const FString& Portal) override;
    virtual void RestartPlayer(AController* NewPlayer) override;
    virtual void Logout(AController* Exiting) override;
    void OnElimination(AMRFPSPawn* Victim, AMRFPSPawn* Killer);
    bool HandleAdminAction(AMRFPSPlayerController* Admin,EMRFPSAdminAction Action,FVector2D Dimensions,FString& SafeError);
    bool PlaceAdminWall(AMRFPSPlayerController* Admin,const FVector& Origin,const FVector& Direction,float Yaw,FString& SafeError);
    bool PlaceAdminObject(AMRFPSPlayerController* Admin,EMRFPSBuildKind Kind,const FVector& Origin,const FVector& Direction,float Yaw,FString& SafeError);
    bool EditAdminWall(AMRFPSPlayerController* Admin,const FVector& Origin,const FVector& Direction,EMRFPSWallEdit Edit,FString& SafeError);
    bool RemoveAdminObject(AMRFPSPlayerController* Admin,const FVector& Origin,const FVector& Direction,FString& SafeError);
    void InvalidateSharedFloor();
    UPROPERTY(Config) int32 MaxInspectors = 32;
    UPROPERTY(Config) int32 MaxAdmins=4;
    UPROPERTY(Config) int32 ScoreToWin = 10;
    UPROPERTY(Config) float ArenaWidthCm=450;
    UPROPERTY(Config) float ArenaLengthCm=700;
    UPROPERTY(Config) bool bRequireAdminStart=true;
    UPROPERTY(Config) int32 MaxAdminWalls=32;
    bool bAllowDesktop = true;
private:
    bool AdminAuthorized(const AMRFPSPlayerController* Admin) const;
    bool RegisteredAnchorsValid() const;
    bool CaptureSharedFloorRegistration(FString& SafeError);
    void ApplyLayoutChange();
    void SetCombatPaused(bool Paused);
    void ResetToWaiting();
    void SpawnDefaultStage();
    uint32 RegisteredCalibration[2]={0,0};
    TWeakObjectPtr<AMRFPSPawn> RegisteredPlayers[2];
    double CombatPausedAt=0;
    TMap<TWeakObjectPtr<AMRFPSBarrier>,float> PausedWallLifetimes;
    int32 CountFighters() const;
    int32 CountInspectors() const;
    int32 CountAdmins() const;
    void ResetMatch();
    TUniquePtr<FMRFPSAuthVerifier> AuthVerifier;
    TMap<FString,FMRFPSAuthClaims> PendingAuthentication;
    TMap<FString,double> PendingAuthenticationDeadlines;
    bool bNetworkAdmissionReady = false;
    double NextAuthenticationCheck = 0;
};

UCLASS()
class MRFPS_API UMRFPSGameInstance : public UGameInstance
{
    GENERATED_BODY()
public:
    virtual void Init() override;
    virtual void OnStart() override;
    virtual void ReceivedNetworkEncryptionToken(const FString& Token, const FOnEncryptionKeyResponse& Delegate) override;
    virtual void ReceivedNetworkEncryptionAck(const FOnEncryptionKeyResponse& Delegate) override;
    virtual EEncryptionFailureAction ReceivedNetworkEncryptionFailure(UNetConnection* Connection) override;
    void NetworkFailure(UWorld* World, class UNetDriver* Driver, ENetworkFailure::Type Type, const FString& Error);
    void RememberConnectionAttempt(const FString& Address,const FString& Role);
    void ReconnectWithFreshToken();
    void TickAutoReconnect();
private:
    FString LastServerAddress;
    FString LastServerRole;
    bool bReconnectPending=false;
    double NextReconnectCheck=0;
};
