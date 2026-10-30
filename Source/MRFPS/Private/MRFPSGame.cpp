#include "MRFPSGame.h"
#include "MRFPSInspectorHudLayout.h"
#include "MRFPSHealthResponder.h"
#include "MRFPSRosterPublisher.h"
#include "MRFPSAvatar.h"
#include "MRFPSHandPose.h"
#include "MRFPSGloveInput.h"
#include "Misc/Paths.h"
#include "HAL/PlatformTime.h"
#include "Camera/CameraComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SphereComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/TextRenderComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Engine/Canvas.h"
#include "CanvasItem.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/Texture2D.h"
#include "Engine/StaticMesh.h"
#include "Engine/NetConnection.h"
#include "Engine/NetDriver.h"
#include "IpNetDriver.h"
#include "IPAddress.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "Engine/PendingNetGame.h"
#include "EngineUtils.h"
#include "GameFramework/FloatingPawnMovement.h"
#include "GameFramework/PlayerInput.h"
#include "InputKeyEventArgs.h"
#include "GameFramework/GameSession.h"
#include "HeadMountedDisplayFunctionLibrary.h"
#include "HeadMountedDisplayTypes.h"
#include "MotionControllerComponent.h"
#include "VRNotificationsComponent.h"
#include "MRFPSGloveSubsystem.h"
#include "MRFPSHapticsComponent.h"
#include "MRFPSMenu.h"
#include "MRFPSAdminPanel.h"
#include "MRFPSVideoReceiver.h"
#include "MRFPSGeometryWorld.h"
#include "MRFPSAuthClientSubsystem.h"
#include "MRFPSLoginMenu.h"
#include "MRFPSBot.h"
#include "MRFPSTransportModule.h"
#include "MRFPSIpConnection.h"
#include "HAL/IConsoleManager.h"
#include "Misc/ConfigCacheIni.h"
#include "Kismet/GameplayStatics.h"
#include "Net/UnrealNetwork.h"
#include "UObject/ConstructorHelpers.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/PackageName.h"
#include "Misc/NetworkVersion.h"
#include "Misc/App.h"
#include "Materials/MaterialInterface.h"
#include "TimerManager.h"
#if !UE_SERVER
#include "Components/DirectionalLightComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/WidgetComponent.h"
#include "Engine/TextureCube.h"
#include "MRFPSXRModule.h"
#include "MRFPSViewCaptureSubsystem.h"
#include "GeometryCaptureSubsystem.h"
#include "Widgets/SWindow.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogMRFPS, Log, All);
static constexpr ECollisionChannel WeaponChannel = ECC_GameTraceChannel1;

namespace MRFPS
{
    static MRFPSConnectionStatus::ERole ConnectionRole(const FString& Role)
    {
        return Role==TEXT("player")?MRFPSConnectionStatus::ERole::Player:
            Role==TEXT("inspector")?MRFPSConnectionStatus::ERole::Inspector:
            Role==TEXT("admin")?MRFPSConnectionStatus::ERole::Admin:MRFPSConnectionStatus::ERole::Unknown;
    }
    static MRFPSConnectionStatus::EFailure FailureKind(const FString& SafeCode)
    {
        if (SafeCode==TEXT("ARENA_FULL") || SafeCode==TEXT("ADMIN_FULL")) return MRFPSConnectionStatus::EFailure::Capacity;
        if (SafeCode.StartsWith(TEXT("AUTH_")) || SafeCode.StartsWith(TEXT("DTLS_"))) return MRFPSConnectionStatus::EFailure::Authentication;
        return MRFPSConnectionStatus::EFailure::Connection;
    }
    static bool StableLoginWorld(UWorld* World)
    {
        if (!World || World->bIsTearingDown || World->IsInSeamlessTravel()) return false;
        const FWorldContext* Context=GEngine?GEngine->GetWorldContextFromWorld(World):nullptr;
        return Context && Context->TravelURL.IsEmpty() && !Context->PendingNetGame;
    }
    static void QueryOnly(UPrimitiveComponent* C)
    {
        C->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
        C->SetCollisionResponseToAllChannels(ECR_Ignore);
        C->SetCollisionResponseToChannel(WeaponChannel, ECR_Block);
        C->SetGenerateOverlapEvents(false);
    }
    static void Material(UStaticMeshComponent* C, const TCHAR* Name)
    {
        if (C->GetStaticMesh() && C->GetStaticMesh()->GetName().StartsWith(TEXT("SM_MR"))) return;
        const FString Path = FString::Printf(TEXT("/Game/Materials/M_%s.M_%s"), Name, Name);
        if (UMaterialInterface* M = LoadObject<UMaterialInterface>(nullptr, *Path)) C->SetMaterial(0, M);
    }
    static UStaticMeshComponent* Mesh(AActor* A, USceneComponent* Parent, const TCHAR* Name, const TCHAR* Asset, FVector Scale, const TCHAR* GeneratedAsset=nullptr)
    {
        UStaticMeshComponent* M = A->CreateDefaultSubobject<UStaticMeshComponent>(Name);
        M->SetupAttachment(Parent);
        ConstructorHelpers::FObjectFinder<UStaticMesh> Found(Asset);
        M->SetStaticMesh(Found.Object);
        if (GeneratedAsset && FPackageName::DoesPackageExist(FString(TEXT("/Game/Meshes/"))+GeneratedAsset)) {
            const FString Path=FString::Printf(TEXT("/Game/Meshes/%s.%s"),GeneratedAsset,GeneratedAsset);
            if (auto* Custom=LoadObject<UStaticMesh>(nullptr,*Path)) { M->SetStaticMesh(Custom); Scale=FVector::OneVector; }
        }
        M->SetRelativeScale3D(Scale);
        M->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        M->SetCastShadow(false);
        return M;
    }
    static bool IsInspectorOption(const FString& Options)
    {
        return UGameplayStatics::GetIntOption(Options, TEXT("Admin"), 0) == 1 ||
            UGameplayStatics::GetIntOption(Options, TEXT("Inspector"), 0) == 1 ||
            UGameplayStatics::GetIntOption(Options, TEXT("SpectatorOnly"), 0) == 1;
    }
    static FString RequestedRole(const FString& Options)
    { return UGameplayStatics::GetIntOption(Options,TEXT("Admin"),0)==1 ? TEXT("admin") : IsInspectorOption(Options)?TEXT("inspector"):TEXT("player"); }
    static const FMRFPSArenaLayout& Layout(const UWorld* World)
    {
        static const FMRFPSArenaLayout Nominal;
        const auto* GS=World?World->GetGameState<AMRFPSGameState>():nullptr;
        return GS?GS->Layout:Nominal;
    }
    static bool HasDesktopFlag() { return FParse::Param(FCommandLine::Get(), TEXT("MRFPSDesktop")); }
    static bool ViewerShowsAvatars(const UWorld* World)
    {
        const auto* Viewer=World?Cast<AMRFPSPlayerController>(World->GetFirstPlayerController()):nullptr;
        return Viewer && Viewer->IsInspector();
    }
    static FString LoginToken(const FString& Options)
    {
        if (Options.Len()>8192) return FString();
        TArray<FString> Parts; Options.ParseIntoArray(Parts,TEXT("?"),true);
        int32 Count=0; FString Token;
        for (const FString& Part:Parts) if (Part.StartsWith(TEXT("AuthToken="),ESearchCase::IgnoreCase)) { ++Count; Token=Part.Mid(10); }
        return Count==1 ? Token : FString();
    }
}

bool FMRFPSPose::IsSane() const
{
    if (Head.ContainsNaN() || Left.ContainsNaN() || Right.ContainsNaN() ||
        HeadRotation.ContainsNaN() || LeftRotation.ContainsNaN() || RightRotation.ContainsNaN() || RightHandRotation.ContainsNaN() ||
        !FMath::IsFinite(LeftOpenness) || LeftOpenness < 0 || LeftOpenness > 1) return false;
    if (FMath::Abs(Head.X)>MRFPSArenaLimits::MaxCoordinateCm || FMath::Abs(Head.Y)>MRFPSArenaLimits::MaxCoordinateCm || Head.Z < -1000 || Head.Z > 4000) return false;
    return (!bLeftTracked || FVector::Dist(Head, Left) <= 180) &&
        (!bRightTracked || FVector::Dist(Head, Right) <= 180);
}
bool FMRFPSPose::IsInsideArena(const FMRFPSArenaLayout& Layout) const
{
    return bHeadTracked && Layout.ContainsHead(Head);
}
void FMRFPSRiseGesture::Reset() { StartTime = LastTime = -1; bLatched = false; }
bool FMRFPSRiseGesture::Update(float Z, double Time, bool bTracked)
{
    if (!bTracked || !FMath::IsFinite(Z) || !FMath::IsFinite(Time)) { Reset(); return false; }
    if (LastTime < 0 || Time <= LastTime || Time - LastTime > .2) {
        StartTime = LastTime = Time; StartZ = LastZ = Z; bLatched = false; return false;
    }
    const double Dt = Time - LastTime;
    const float Speed = (Z - LastZ) / Dt;
    LastTime = Time; LastZ = Z;
    if (bLatched) {
        if (Z < StartZ + 15) { bLatched = false; StartTime = Time; StartZ = Z; }
        return false;
    }
    if (Z < StartZ || Time - StartTime > 1.5) { StartZ = Z; StartTime = Time; }
    if (Z - StartZ >= 30 && Time - StartTime >= .08 && Speed >= 10) { bLatched = true; return true; }
    return false;
}

AMRFPSPlayerState::AMRFPSPlayerState() { bAlwaysRelevant = true; }
void AMRFPSPlayerState::UpdatePing(float InPing)
{
    if (!FMath::IsFinite(InPing) || InPing<0.f || InPing>60.f) return;
    Super::UpdatePing(InPing);
    // The engine invokes this on a network ACK. Do not infer a measurement
    // from ExactPing/CompressedPing, whose initial zero is ambiguous.
    if (HasAuthority() && !IsABot()) {
        LastRosterPingMs=FMath::RoundToInt(InPing*1000.f);
        LastRosterPingAt=FPlatformTime::Seconds();
        LastRosterPingServerTime=GetWorld()?GetWorld()->GetTimeSeconds():-1.f;
    }
}
void AMRFPSPlayerState::RefreshRosterPing(double Now)
{
    if (!HasAuthority()) return;
    const bool bFresh=!IsABot() && LastRosterPingAt>=0 && LastRosterPingServerTime>=0 && Now>=LastRosterPingAt && Now-LastRosterPingAt<=5.0;
    const int32 Value=bFresh ? LastRosterPingMs : -1;
    const float Timestamp=bFresh ? LastRosterPingServerTime : -1.f;
    if (RosterPing.Milliseconds!=Value || RosterPing.SampleServerTime!=Timestamp) {
        RosterPing.Milliseconds=Value; RosterPing.SampleServerTime=Timestamp; ForceNetUpdate();
    }
}
void AMRFPSPlayerState::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);
    DOREPLIFETIME(AMRFPSPlayerState, Slot); DOREPLIFETIME(AMRFPSPlayerState, bInspector);
    DOREPLIFETIME(AMRFPSPlayerState,bAdmin);
    DOREPLIFETIME(AMRFPSPlayerState, Kills); DOREPLIFETIME(AMRFPSPlayerState, Deaths);
    DOREPLIFETIME(AMRFPSPlayerState,bAuthenticated); DOREPLIFETIME(AMRFPSPlayerState,AuthSubject); DOREPLIFETIME(AMRFPSPlayerState,AuthExpiresAt);
    DOREPLIFETIME(AMRFPSPlayerState,VideoStreamId);
    DOREPLIFETIME(AMRFPSPlayerState,GeometryStreamId);
    DOREPLIFETIME(AMRFPSPlayerState,RosterPing);
}
void AMRFPSGameState::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);
    DOREPLIFETIME(AMRFPSGameState, Phase); DOREPLIFETIME(AMRFPSGameState, WinnerSlot);
    DOREPLIFETIME(AMRFPSGameState,bSoloBotSession); DOREPLIFETIME(AMRFPSGameState,bSoloGeometryReady); DOREPLIFETIME(AMRFPSGameState,SoloHumanSlot); DOREPLIFETIME(AMRFPSGameState,SoloHumanCalibration);
    DOREPLIFETIME(AMRFPSGameState, RestartAt);
    DOREPLIFETIME(AMRFPSGameState,RoundNumber); DOREPLIFETIME(AMRFPSGameState,CompletedRounds);
    DOREPLIFETIME(AMRFPSGameState,RoundWinsSlot0); DOREPLIFETIME(AMRFPSGameState,RoundWinsSlot1);
    DOREPLIFETIME(AMRFPSGameState,LastRoundWinnerSlot); DOREPLIFETIME(AMRFPSGameState,RoundEndsAt); DOREPLIFETIME(AMRFPSGameState,RoundPausedAt);
    DOREPLIFETIME(AMRFPSGameState,Layout); DOREPLIFETIME(AMRFPSGameState,bSharedFloorConfirmed);
    DOREPLIFETIME(AMRFPSGameState,bCombatPaused); DOREPLIFETIME(AMRFPSGameState,bNominalDesktopSession);
    DOREPLIFETIME(AMRFPSGameState,ControlRevision); DOREPLIFETIME(AMRFPSGameState,SharedFloorRevision);
    DOREPLIFETIME(AMRFPSGameState,MeasuredAnchorDistanceCm);
}

AMRFPSBarrier::AMRFPSBarrier()
{
    bReplicates = true; bAlwaysRelevant = true; SetReplicateMovement(true);
    Collision = CreateDefaultSubobject<UBoxComponent>(TEXT("WallCollision")); SetRootComponent(Collision);
    Collision->InitBoxExtent(FVector(8,80,100)); MRFPS::QueryOnly(Collision);
    Visual = MRFPS::Mesh(this, Collision, TEXT("Wall"), TEXT("/Engine/BasicShapes/Cube.Cube"), FVector(.16,1.6,2),TEXT("SM_MRWall"));
    WallCells=CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("EditableWallCells"));
    WallCells->SetupAttachment(Collision);
    static ConstructorHelpers::FObjectFinder<UStaticMesh> Cube(TEXT("/Engine/BasicShapes/Cube.Cube"));
    WallCells->SetStaticMesh(Cube.Object);WallCells->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    WallCells->SetMobility(EComponentMobility::Movable);WallCells->SetVisibility(false);WallCells->SetCastShadow(false);
}
void AMRFPSBarrier::BeginPlay()
{
    Super::BeginPlay(); MRFPS::Material(Visual, TEXT("Wall"));
    ApplyBuildKind();
    if (HasAuthority() && !bAdminPlaced) SetLifeSpan(6);
}
void AMRFPSBarrier::ApplyShot(float Damage)
{
    if (!HasAuthority()) return;
    Health -= Damage;
    if (Health <= 0) Destroy();
}
void AMRFPSBarrier::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps); DOREPLIFETIME(AMRFPSBarrier, Health); DOREPLIFETIME(AMRFPSBarrier, OwnerSlot);
    DOREPLIFETIME(AMRFPSBarrier,bAdminPlaced);
    DOREPLIFETIME(AMRFPSBarrier,BuildKind);
    DOREPLIFETIME(AMRFPSBarrier,WallShape);
}
AMRFPSArena::AMRFPSArena()
{
    bReplicates = true; bAlwaysRelevant = true;
    SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("Root")));
    Floor = MRFPS::Mesh(this, RootComponent, TEXT("ArenaFloor"), TEXT("/Engine/BasicShapes/Cube.Cube"), FVector(4.5,7,.03));
#if !UE_SERVER
    auto* Sun=CreateDefaultSubobject<UDirectionalLightComponent>(TEXT("AvatarKeyLight"));Sun->SetupAttachment(RootComponent);
    Sun->SetRelativeRotation(FRotator(-55,-35,0));Sun->SetIntensity(3.5f);Sun->SetCastShadows(false);
    auto* Ambient=CreateDefaultSubobject<USkyLightComponent>(TEXT("AvatarAmbient"));Ambient->SetupAttachment(RootComponent);
    ConstructorHelpers::FObjectFinder<UTextureCube> Sky(TEXT("/Engine/EngineResources/DefaultTextureCube.DefaultTextureCube"));
    Ambient->SourceType=SLS_SpecifiedCubemap;Ambient->Cubemap=Sky.Object;Ambient->SetIntensity(.8f);Ambient->SetCastShadows(false);
#endif
    Floor->SetRelativeLocation(FVector(0,0,-2));
    for (int32 I=0; I<42; ++I) {
        const FName Name(*FString::Printf(TEXT("Grid%d"), I));
        UStaticMeshComponent* M = CreateDefaultSubobject<UStaticMeshComponent>(Name);
        M->SetupAttachment(RootComponent);
        ConstructorHelpers::FObjectFinder<UStaticMesh> Cube(TEXT("/Engine/BasicShapes/Cube.Cube"));
        M->SetStaticMesh(Cube.Object); M->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        M->SetCastShadow(false); GridLines.Add(M);
    }
    for (int32 Slot=0;Slot<2;++Slot) {
        const FName Name(*FString::Printf(TEXT("NominalStart%d"),Slot));
        NominalStartMarkers.Add(MRFPS::Mesh(this,RootComponent,*Name.ToString(),TEXT("/Engine/BasicShapes/Cylinder.Cylinder"),FVector(.4,.4,.01)));
    }
    ApplyLayout(FMRFPSArenaLayout());
}
void AMRFPSArena::BeginPlay()
{
    Super::BeginPlay(); MRFPS::Material(Floor, TEXT("Floor"));
    TInlineComponentArray<UStaticMeshComponent*> Meshes(this);
    for (auto* M : Meshes) if (M != Floor) MRFPS::Material(M, TEXT("Blue"));
    if (NominalStartMarkers.IsValidIndex(1)) MRFPS::Material(NominalStartMarkers[1],TEXT("Orange"));
    const bool XR = UHeadMountedDisplayFunctionLibrary::IsHeadMountedDisplayEnabled() && !MRFPS::HasDesktopFlag();
    Floor->SetVisibility(!XR);
    ApplyLayout(MRFPS::Layout(GetWorld()));
}

AMRFPSPawn::AMRFPSPawn()
{
    PrimaryActorTick.bCanEverTick = true; bReplicates = true; bAlwaysRelevant = true;
    SetNetUpdateFrequency(30); SetMinNetUpdateFrequency(15);
    SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("Root")));
    XROrigin = CreateDefaultSubobject<USceneComponent>(TEXT("XROrigin")); XROrigin->SetupAttachment(RootComponent);
    Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("HeadCamera")); Camera->SetupAttachment(XROrigin);
    Camera->SetRelativeLocation(FVector(0,0,170)); Camera->bLockToHmd = true;
    LeftController = CreateDefaultSubobject<UMotionControllerComponent>(TEXT("LeftController"));
    LeftController->SetupAttachment(XROrigin); LeftController->SetTrackingMotionSource(TEXT("Left"));
    RightController = CreateDefaultSubobject<UMotionControllerComponent>(TEXT("RightController"));
    RightController->SetupAttachment(XROrigin); RightController->SetTrackingMotionSource(TEXT("RightAim"));
    HeadHitbox = CreateDefaultSubobject<USphereComponent>(TEXT("HeadHitbox")); HeadHitbox->SetupAttachment(RootComponent);
    HeadHitbox->InitSphereRadius(13); MRFPS::QueryOnly(HeadHitbox);
    BodyHitbox = CreateDefaultSubobject<UCapsuleComponent>(TEXT("BodyHitbox")); BodyHitbox->SetupAttachment(RootComponent);
    BodyHitbox->InitCapsuleSize(22,50); MRFPS::QueryOnly(BodyHitbox);
    MiniShield = CreateDefaultSubobject<UBoxComponent>(TEXT("MiniShield")); MiniShield->SetupAttachment(RootComponent);
    MiniShield->InitBoxExtent(FVector(2,22,22)); MRFPS::QueryOnly(MiniShield);
    HeadVisual = MRFPS::Mesh(this, RootComponent, TEXT("Head"), TEXT("/Engine/BasicShapes/Sphere.Sphere"), FVector(.26),TEXT("SM_MRHead"));
    BodyVisual = MRFPS::Mesh(this, RootComponent, TEXT("Body"), TEXT("/Engine/BasicShapes/Cylinder.Cylinder"), FVector(.4,.4,.8),TEXT("SM_MRBody"));
    LeftVisual = MRFPS::Mesh(this, RootComponent, TEXT("LeftHand"), TEXT("/Engine/BasicShapes/Sphere.Sphere"), FVector(.12),TEXT("SM_MRHand"));
    GunVisual = MRFPS::Mesh(this, RootComponent, TEXT("Gun"), TEXT("/Engine/BasicShapes/Cube.Cube"), FVector(.32,.065,.09),TEXT("SM_MRPistol"));
    ShieldVisual = MRFPS::Mesh(this, MiniShield, TEXT("Shield"), TEXT("/Engine/BasicShapes/Cube.Cube"), FVector(.04,.44,.44),TEXT("SM_MRMiniShield"));
    HeadVisual->SetOwnerNoSee(true); BodyVisual->SetOwnerNoSee(true);
    Avatar=CreateDefaultSubobject<UMRFPSAvatarComponent>(TEXT("Manny"));Avatar->SetupAttachment(RootComponent);
    Label = CreateDefaultSubobject<UTextRenderComponent>(TEXT("PlayerInformation")); Label->SetupAttachment(RootComponent);
    Label->SetHorizontalAlignment(EHTA_Center); Label->SetWorldSize(12); Label->SetOwnerNoSee(true);
    HeadsetStatus = CreateDefaultSubobject<UTextRenderComponent>(TEXT("HeadsetStatus")); HeadsetStatus->SetupAttachment(Camera);
    HeadsetStatus->SetRelativeLocation(FVector(100,0,-27)); HeadsetStatus->SetRelativeRotation(FRotator(0,180,0));
    HeadsetStatus->SetHorizontalAlignment(EHTA_Center); HeadsetStatus->SetWorldSize(2.1);
    HeadsetStatus->SetOnlyOwnerSee(true);
    // The normal HUD already carries status. Do not put another text plane
    // across the owning player's first-person view; remote labels remain.
    HeadsetStatus->SetVisibility(false);
#if !UE_SERVER
    PlayerHud=CreateDefaultSubobject<UWidgetComponent>(TEXT("PlayerHud"));
    PlayerHud->SetupAttachment(Camera);PlayerHud->SetWidgetSpace(EWidgetSpace::World);
    // The reticle stays clear: the status panel is horizontally centred and
    // follows the headset below the user's forward gaze.
    PlayerHud->SetDrawSize(FVector2D(600,120));PlayerHud->SetPivot(FVector2D(.5,.5));
    PlayerHud->SetRelativeScale3D(FVector(.085));
    PlayerHud->SetRelativeLocation(FVector(85,0,-42));PlayerHud->SetRelativeRotation(FRotator(0,180,0));
    PlayerHud->SetCollisionEnabled(ECollisionEnabled::NoCollision);PlayerHud->SetOnlyOwnerSee(true);
    PlayerHud->SetVisibility(false);
    RoundHud=CreateDefaultSubobject<UWidgetComponent>(TEXT("RoundHud"));
    RoundHud->SetupAttachment(Camera); RoundHud->SetWidgetSpace(EWidgetSpace::World);
    RoundHud->SetDrawSize(FVector2D(600,100)); RoundHud->SetPivot(FVector2D(.5,.5));
    RoundHud->SetRelativeScale3D(FVector(.075));
    RoundHud->SetRelativeLocation(FVector(85,0,30)); RoundHud->SetRelativeRotation(FRotator(0,180,0));
    RoundHud->SetCollisionEnabled(ECollisionEnabled::NoCollision); RoundHud->SetOnlyOwnerSee(true);
    RoundHud->SetVisibility(false);
#endif
    Haptics = CreateDefaultSubobject<UMRFPSHapticsComponent>(TEXT("MoBits"));
    XRNotifications=CreateDefaultSubobject<UVRNotificationsComponent>(TEXT("XRNotifications"));
}
void AMRFPSPawn::BeginPlay()
{
    // Smoke/desktop clients must not consume the one official MoBits session.
    if (MRFPS::HasDesktopFlag() || FParse::Param(FCommandLine::Get(), TEXT("MRFPSSmoke"))) Haptics->bAutoConnectTracking = false;
    Super::BeginPlay(); MRFPS::Material(ShieldVisual, TEXT("Shield"));
    XRNotifications->HMDRecenteredDelegate.AddDynamic(this,&AMRFPSPawn::OnXRSpaceChanged);
    XRNotifications->XRTrackingOriginChangedDelegate.AddDynamic(this,&AMRFPSPawn::OnXRSpaceChanged);
    XRNotifications->XRPlayAreaChangedDelegate.AddDynamic(this,&AMRFPSPawn::OnXRSpaceChanged);
    XRNotifications->HMDRemovedFromHeadDelegate.AddDynamic(this,&AMRFPSPawn::InvalidateCalibration);
    XRNotifications->HMDLostDelegate.AddDynamic(this,&AMRFPSPawn::InvalidateCalibration);
}
void AMRFPSPawn::EndPlay(const EEndPlayReason::Type Reason)
{
    if (GetWorld()) if (auto* Geometry=GetWorld()->GetSubsystem<UMRFPSGeometryWorld>()) Geometry->RemoveOwner(this);
    if (IsLocallyControlled() && Haptics) Haptics->StopHaptics();
    Super::EndPlay(Reason);
}
bool AMRFPSPawn::IsXR() const
{
    return IsLocallyControlled() && !MRFPS::HasDesktopFlag() && UHeadMountedDisplayFunctionLibrary::IsHeadMountedDisplayEnabled();
}
bool AMRFPSPawn::GetLocalWeaponPose(FMRFPSWeaponPose& Out)
{
    Out=FMRFPSWeaponPose();
    if (!IsXR() || !XROrigin) return false;
    FXRMotionControllerState Aim,Grip; FXRHandTrackingState Hand;
    UHeadMountedDisplayFunctionLibrary::GetMotionControllerState(this,EXRSpaceType::XRTrackingSpace,EControllerHand::Right,EXRControllerPoseType::Aim,Aim);
    UHeadMountedDisplayFunctionLibrary::GetMotionControllerState(this,EXRSpaceType::XRTrackingSpace,EControllerHand::Right,EXRControllerPoseType::Grip,Grip);
    UHeadMountedDisplayFunctionLibrary::GetHandTrackingState(this,EXRSpaceType::UnrealWorldSpace,EControllerHand::Right,Hand);
    FMRFPSGloveHandFrame Glove;
    if (auto* Instance=GetGameInstance()) if (auto* Gloves=Instance->GetSubsystem<UMRFPSGloveSubsystem>())
        Gloves->GetLatestHand(EMRFPSGloveHand::Right,Glove);
    return MRFPSHandPose::ResolveRightWeapon(Aim,Grip,Hand,
        UHeadMountedDisplayFunctionLibrary::GetTrackingToWorldTransform(this).Inverse(),
        XROrigin->GetComponentTransform(),Glove.bPoseValid,Glove.TrackingTransform,Out);
}
void AMRFPSPawn::SetLoginWeaponPose(const FMRFPSWeaponPose* Weapon)
{
    if (!GunVisual) return;
    if (!Weapon || Weapon->Source==MRFPSLoginPointer::ESource::None) { GunVisual->SetVisibility(false); return; }
    const bool Custom=GunVisual->GetStaticMesh() && GunVisual->GetStaticMesh()->GetName()==TEXT("SM_MRPistol");
    const FTransform Visual=MRFPSWeaponPose::Visual(Weapon->GripWorld,Custom);
    GunVisual->SetWorldLocationAndRotation(Visual.GetLocation(),Visual.GetRotation()); GunVisual->SetVisibility(true);
}
int32 AMRFPSPawn::GetSlot() const
{
    const auto* PS = GetPlayerState<AMRFPSPlayerState>(); return PS ? PS->Slot : -1;
}
bool AMRFPSPawn::IsPoseFresh() const { return GetWorld()->GetTimeSeconds() - LastPoseReceived <= .3; }
bool AMRFPSPawn::IsInsideArena() const { return Pose.IsInsideArena(MRFPS::Layout(GetWorld())); }
bool AMRFPSPawn::GetGeometryCalibration(FTransform& TrackingToArena,double& CalibratedSince) const
{
    if (!bLocalReady || !bCalibrated || !IsXR() || !XROrigin ||
        !MRFPSGeometryWire::CalibrationAcknowledged(GeometryCalibrationFence,GeometryCalibrationGeneration) || !GeometryCalibratedEpoch) return false;
    TrackingToArena=XROrigin->GetComponentTransform(); CalibratedSince=GeometryCalibratedSince;
    return !TrackingToArena.ContainsNaN();
}
void AMRFPSPawn::InvalidateGeometryCalibration() { InvalidateCalibration(); }
bool AMRFPSPawn::CanAct() const
{
    const auto* GS = GetWorld()->GetGameState<AMRFPSGameState>();
    const auto* PS = GetPlayerState<AMRFPSPlayerState>();
    const bool ServerBot=MRFPSBot::IsServerBot(this);
    if (!PS || (!ServerBot && (!PS->bAuthenticated || PS->AuthExpiresAt<=FDateTime::UtcNow().ToUnixTimestamp())) ||
        PS->bInspector || PS->bAdmin || PS->Slot<0 || PS->Slot>=2 || !GS || GS->bCombatPaused ||
        GS->Phase==EMRFPSMatchPhase::Finished || Health<=0 || !Pose.bHeadTracked || !IsPoseFresh()) return false;
    if (GS->Phase==EMRFPSMatchPhase::Playing && GS->RoundEndsAt>0 && GS->GetServerWorldTimeSeconds()>=GS->RoundEndsAt) return false;
    // Practice actions are available immediately; player damage still requires
    // the authoritative match and registered shared floor.
    if (GS->Phase==EMRFPSMatchPhase::Waiting) return !GS->Layout.bCalibrated || IsInsideArena();
    const bool Solo=GS->bSoloBotSession && GS->SoloHumanSlot>=0 && GS->SoloHumanSlot<2 &&
        (ServerBot || (PS->Slot==GS->SoloHumanSlot &&
            (Pose.bDesktop || (bStageFloorReferenced && GeometryCalibrationGeneration==GS->SoloHumanCalibration && GS->SoloHumanCalibration!=0))));
    return GS->Phase==EMRFPSMatchPhase::Playing &&
        ((GS->Layout.bCalibrated && GS->bSharedFloorConfirmed && bStageFloorReferenced && GS->SharedFloorRevision &&
          GS->Layout.RegistrationRevision==GS->SharedFloorRevision) || (GS->bNominalDesktopSession && Pose.bDesktop) || Solo) &&
        bCalibrated && IsInsideArena();
}
void AMRFPSPawn::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);
    DOREPLIFETIME_CONDITION(AMRFPSPawn, Pose, COND_SkipOwner);
    DOREPLIFETIME(AMRFPSPawn, Health); DOREPLIFETIME(AMRFPSPawn, ShieldEnergy); DOREPLIFETIME(AMRFPSPawn, bShieldActive);
    DOREPLIFETIME(AMRFPSPawn,MagazineRounds);
    DOREPLIFETIME(AMRFPSPawn, bCalibrated); DOREPLIFETIME(AMRFPSPawn, RespawnAt); DOREPLIFETIME(AMRFPSPawn, WallReadyAt);
    DOREPLIFETIME(AMRFPSPawn,bStageFloorReferenced);
    DOREPLIFETIME(AMRFPSPawn, GeometryCalibrationGeneration);
}
void AMRFPSPawn::Calibrate()
{
    if (!IsLocallyControlled()) return;
    if (!IsXR() && !MRFPS::HasDesktopFlag()) return;
    if (IsXR()) {
#if !UE_SERVER
        if (!FMRFPSXRModule::IsPassthroughRunning()) return;
#endif
        IgnoreOriginChangesUntil=GetWorld()->GetTimeSeconds()+.5;
        UHeadMountedDisplayFunctionLibrary::SetTrackingOrigin(EHMDTrackingOrigin::Stage);
        FRotator R; FVector P; UHeadMountedDisplayFunctionLibrary::GetOrientationAndPosition(R,P);
        if (!UHeadMountedDisplayFunctionLibrary::HasValidTrackingPosition()) return;
        // Stand at the physical arena centre, facing its agreed +X direction.
        const FQuat Q(FRotator(0,-R.Yaw,0));
        XROrigin->SetRelativeTransform(FTransform(Q, Q.RotateVector(FVector(-P.X,-P.Y,0))));
    }
    if (!MRFPSGeometryWire::AdvanceCalibrationFence(GeometryCalibrationFence,GeometryCalibrationGeneration)) {
        bLocalReady=false; GeometryCalibratedEpoch=0; ServerCalibrate(false); return;
    }
    GeometryCalibratedSince=FPlatformTime::Seconds();
    GeometryCalibratedEpoch=0;
#if !UE_SERVER
    if (IsXR()) if (const auto* Geometry=GetGameInstance()->GetSubsystem<UMRFPSGeometryCaptureSubsystem>())
        GeometryCalibratedEpoch=Geometry->GetTrackingSpaceEpoch();
#endif
    bLocalReady = true;
    ServerCalibrate(true,IsXR() && UHeadMountedDisplayFunctionLibrary::GetTrackingOrigin()==EHMDTrackingOrigin::Stage);
    UE_LOG(LogMRFPS, Display, TEXT("MRFPS_CALIBRATED slot=%d xr=%d"), GetSlot(), IsXR());
}
void AMRFPSPawn::InvalidateCalibration()
{
    if (!IsLocallyControlled() || MRFPS::HasDesktopFlag()) return;
    (void)MRFPSGeometryWire::AdvanceCalibrationFence(GeometryCalibrationFence,GeometryCalibrationGeneration);
    bLocalReady=false; GeometryCalibratedEpoch=0; bFiring=false; bFingerTriggerDown=false; GloveFireInput.Reset(); ServerCalibrate(false);
    if (Haptics) Haptics->StopHaptics();
}
void AMRFPSPawn::OnXRSpaceChanged()
{
    if (GetWorld() && GetWorld()->GetTimeSeconds()>=IgnoreOriginChangesUntil) InvalidateCalibration();
}
void AMRFPSPawn::ServerCalibrate_Implementation(bool bReady,bool FloorReferenced)
{
    if (auto* Geometry=GetWorld()->GetSubsystem<UMRFPSGeometryWorld>()) Geometry->RemoveOwner(this);
    ServerReload.Reset();
    // Every reliable request advances the acknowledged epoch, including loss
    // and requests during a finished match. Otherwise an old ack could unlock
    // a newer local transform or leave stale physical geometry after recenter.
    if (GeometryCalibrationGeneration>=MAX_uint32-1) bReady=false;
    else ++GeometryCalibrationGeneration;
    bCalibrated = bReady; bStageFloorReferenced=bReady && FloorReferenced;
    LastPoseReceived=-100; Pose.bHeadTracked=false; ServerRise.Reset(); ServerLower.Reset(); ForceNetUpdate();
    if (auto* GM=GetWorld()->GetAuthGameMode<AMRFPSGameMode>()) GM->InvalidateSharedFloor();
    UE_LOG(LogMRFPS,Display,TEXT("MRFPS_CALIBRATED server slot=%d ready=%d"),GetSlot(),bReady);
}
void AMRFPSPawn::DesktopMove(const FVector& Input, float Dt)
{
    if (IsXR() || !IsLocallyControlled()) return;
    DesktopHead += FRotator(0,DesktopRotation.Yaw,0).RotateVector(FVector(Input.X,Input.Y,0).GetClampedToMaxSize(1))*180*Dt;
    if (!Input.IsNearlyZero()) DesktopHead=MRFPS::Layout(GetWorld()).ClampPlanar(DesktopHead);
}
void AMRFPSPawn::DesktopLook(float Yaw, float Pitch)
{
    if (IsXR()) return;
    DesktopRotation.Yaw += Yaw; DesktopRotation.Pitch = FMath::Clamp(DesktopRotation.Pitch + Pitch,-80.,80.);
}
void AMRFPSPawn::SetFiring(bool bPressed)
{
    bPhysicalFireDown=bPressed;
    const auto* GI=Cast<UMRFPSGameInstance>(GetGameInstance());
    bFiring=bPressed && !bRequireFireRelease && !(IsLocallyControlled() && GI && !GI->FireTransition.CanPhysical());
}
void AMRFPSPawn::DesktopWall() { if (!IsXR()) ServerDesktopWall(); }
void AMRFPSPawn::ServerDesktopWall_Implementation()
{
    if (Pose.bDesktop) DeployWall();
}
void AMRFPSPawn::OnRepPose() { UpdateVisuals(); }

void AMRFPSPawn::SampleLocalPose(float Dt)
{
    if (GetSlot()<0) return;
    if (!bDesktopInitialized) {
        bDesktopInitialized = true;
        DesktopHead=MRFPS::Layout(GetWorld()).NominalStart(GetSlot());
        DesktopRotation=MRFPS::Layout(GetWorld()).StartRotation(GetSlot());
        Camera->bLockToHmd = IsXR();
        if (IsXR()) { Camera->SetRelativeLocation(FVector::ZeroVector); UHeadMountedDisplayFunctionLibrary::SetTrackingOrigin(EHMDTrackingOrigin::Stage); }
    }
    FMRFPSPose Next = Pose;
    Next.bDesktop = MRFPS::HasDesktopFlag();
    Camera->bLockToHmd=IsXR();
    if (IsXR()) Camera->SetRelativeLocation(FVector::ZeroVector);
    float IndexCurl = 0, TriggerAnalog = 0; bool bGloveTrigger = false;
    bool bRightPalmPose = false;
    const auto* LocalController=Cast<AMRFPSPlayerController>(Controller);
    const bool Automated=LocalController && LocalController->AutomationInputPawn.Get()==this &&
        LocalController->AutomationInput.Active(FPlatformTime::Seconds()) && LocalController->DesktopAutomationAllowed();
    const bool AutomationFire=Automated && LocalController->AutomationInput.Fire;
    const bool AutomationShield=Automated && LocalController->AutomationInput.Shield;
    FMRFPSGloveHandFrame GloveFrames[2];
    if (IsXR()) if (auto* Gloves=GetGameInstance()->GetSubsystem<UMRFPSGloveSubsystem>()) {
        Gloves->GetLatestHand(EMRFPSGloveHand::Left,GloveFrames[0]);
        Gloves->GetLatestHand(EMRFPSGloveHand::Right,GloveFrames[1]);
    }
    Next.LeftFingers.bValid=Next.RightFingers.bValid=false;
    if (IsXR()) {
        FXRHMDData HMD; UHeadMountedDisplayFunctionLibrary::GetHMDData(this,HMD);
        Next.bHeadTracked = HMD.bValid && HMD.TrackingStatus == ETrackingStatus::Tracked;
        FRotator HRot; FVector HPos; UHeadMountedDisplayFunctionLibrary::GetOrientationAndPosition(HRot,HPos);
        const FTransform T = XROrigin->GetComponentTransform();
        Next.Head = T.TransformPosition(HPos); Next.HeadRotation = T.TransformRotation(HRot.Quaternion()).Rotator();
        auto Hand = [&](EControllerHand Side, FVector_NetQuantize10& P, FRotator& R, bool& Tracked) {
            FXRMotionControllerState M,Grip;
            UHeadMountedDisplayFunctionLibrary::GetMotionControllerState(this,EXRSpaceType::XRTrackingSpace,Side,EXRControllerPoseType::Aim,M);
            UHeadMountedDisplayFunctionLibrary::GetMotionControllerState(this,EXRSpaceType::XRTrackingSpace,Side,EXRControllerPoseType::Grip,Grip);
            const bool AimValid=M.bValid && M.TrackingStatus==ETrackingStatus::Tracked;
            const bool GripValid=Grip.bValid && Grip.TrackingStatus==ETrackingStatus::Tracked;
            Tracked=GripValid || AimValid;
            if (Tracked) {
                P=T.TransformPosition(GripValid?Grip.ControllerLocation:M.ControllerLocation);
                R=T.TransformRotation(AimValid?M.ControllerRotation:Grip.ControllerRotation).Rotator();
            }
            FXRHandTrackingState H;
            UHeadMountedDisplayFunctionLibrary::GetHandTrackingState(this,EXRSpaceType::UnrealWorldSpace,Side,H);
            // ContactGlove supplies finger bends, not position. Its presence
            // must not prevent the independently tracked optical palm from
            // supplying a hand position when the controller is unavailable.
            if (MRFPSHandPose::SelectPositionSource(Tracked,H)==MRFPSHandPose::EPositionSource::OpticalHand) {
                const FTransform EngineToTracking=UHeadMountedDisplayFunctionLibrary::GetTrackingToWorldTransform(this).Inverse();
                if (Side==EControllerHand::Right) {
                    FVector HandPosition; FRotator HandRotation;
                    if (MRFPSHandPose::RightPalmPose(H,EngineToTracking,T,HandPosition,HandRotation)) {
                        Tracked=true; P=HandPosition; R=HandRotation; bRightPalmPose=true;
                    }
                } else {
                    const FQuat Palm=T.TransformRotation(EngineToTracking.TransformRotation(H.HandKeyRotations[0]));
                    Tracked=true; P=T.TransformPosition(EngineToTracking.TransformPosition(H.HandKeyLocations[0]));
                    R=FRotationMatrix::MakeFromXZ(-Palm.GetAxisZ(),Palm.GetAxisX()).Rotator();
                }
            }
            if (Side==EControllerHand::Left) Next.LeftOpenness=0;
        };
        Hand(EControllerHand::Left,Next.Left,Next.LeftRotation,Next.bLeftTracked);
        FMRFPSWeaponPose RightWeapon;
        Next.bRightTracked=GetLocalWeaponPose(RightWeapon);
        if (Next.bRightTracked) {
            Next.Right=RightWeapon.GripWorld.GetLocation();
            Next.RightRotation=RightWeapon.GripWorld.GetRotation().Rotator();
            bRightPalmPose=RightWeapon.Source==MRFPSLoginPointer::ESource::Hand || RightWeapon.Source==MRFPSLoginPointer::ESource::Glove;
        }
        TrackingDescription = TEXT("XR hands/controllers");
    } else if (Next.bDesktop) {
        Next.Head=DesktopHead; Next.HeadRotation=DesktopRotation; Next.bHeadTracked=true;
        const FQuat Q=DesktopRotation.Quaternion();
        Next.Left=DesktopHead+Q.RotateVector(FVector(35,-30,-40));
        Next.Right=DesktopHead+Q.RotateVector(FVector(40,22,-25));
        Next.LeftRotation=Next.RightRotation=DesktopRotation;
        Next.bLeftTracked=Next.bRightTracked=true; Next.LeftOpenness=(bDesktopShield || AutomationShield) ? 1.f : 0.f;
        Camera->SetWorldLocationAndRotation(DesktopHead,DesktopRotation);
        TrackingDescription=TEXT("Desktop simulation");
    } else {
        Next.bHeadTracked=Next.bLeftTracked=Next.bRightTracked=false;
        Next.LeftOpenness=0;
        bFiring=bFingerTriggerDown=false; GloveFireInput.Reset();
        TrackingDescription=TEXT("XR tracking unavailable");
    }
    if (IsXR()) {
        for (int32 I=0; I<2; ++I) {
            const FMRFPSGloveHandFrame& G=GloveFrames[I];
            if (G.bControllerValid && bGloveControlsFresh[I]) {
                if (G.bAButtonPressed && !bPreviousGloveA[I]) {
                    if (I==0) { bPreviousGloveA[I]=true; Calibrate(); return; }
                    if (auto* PC=Cast<AMRFPSPlayerController>(Controller)) PC->ArmHaptics();
                }
                if (I==1 && G.bBButtonPressed && !bPreviousGloveB[I]) if (auto* PC=Cast<AMRFPSPlayerController>(Controller)) PC->StopHaptics();
            }
            bGloveControlsFresh[I]=G.bControllerValid;
            bPreviousGloveA[I]=G.bAButtonPressed; bPreviousGloveB[I]=G.bBButtonPressed;
            FMRFPSFingerPose& Fingers=I==0?Next.LeftFingers:Next.RightFingers;
            MRFPSGloveInput::CopyFingers(G,Fingers);
            if (I==1) { IndexCurl=G.bFlexValid && G.FingerCurls.IsValidIndex(1) ? G.FingerCurls[1] : 0; bGloveTrigger=G.bControllerValid && G.bTriggerPressed; TriggerAnalog=G.bControllerValid ? G.Trigger : 0; }
            // The controller or optical hand takes precedence. Retain an
            // explicitly calibrated external tracker as another fallback.
            if (I==0 && G.bPoseValid && !Next.bLeftTracked) {
                const FTransform T=XROrigin->GetComponentTransform();
                Next.Left=T.TransformPosition(G.TrackingTransform.GetLocation()); const FQuat Palm=T.TransformRotation(G.TrackingTransform.GetRotation()); Next.LeftRotation=FRotationMatrix::MakeFromXZ(-Palm.GetAxisZ(),Palm.GetAxisX()).Rotator(); Next.bLeftTracked=true;
            }
            if (G.bFlexValid) TrackingDescription=TEXT("ContactGlove 2 + XR tracking");
        }
    }
    // A shield requires live hand and head tracking as well as fresh glove
    // data. Reset the hysteresis on loss rather than retaining an open hand.
    if (IsXR()) Next.LeftOpenness=MRFPSGloveInput::LeftOpenness(GloveFrames[0],LocalShieldPalm,Next.bHeadTracked,Next.bLeftTracked);
    else LocalShieldPalm.Reset();
    if (Next.bRightTracked) {
        Next.RightHandRotation=Next.RightRotation;
        if (IsXR()) {
            Next.RightHandRotation=MRFPSHandPose::HandFromWeapon(Next.RightRotation.Quaternion()).Rotator();
        }
    }
    if (IsXR()) {
        if (Pose.LeftFingers.bValid!=Next.LeftFingers.bValid || Pose.bLeftTracked!=Next.bLeftTracked)
            UE_LOG(LogMRFPS,Display,TEXT("MRFPS_GLOVE_LOCAL hand=left fresh=%d tracked_pose=%d"),Next.LeftFingers.bValid,Next.bLeftTracked);
        if (Pose.RightFingers.bValid!=Next.RightFingers.bValid || Pose.bRightTracked!=Next.bRightTracked)
            UE_LOG(LogMRFPS,Display,TEXT("MRFPS_GLOVE_LOCAL hand=right fresh=%d tracked_pose=%d source=%s"),Next.RightFingers.bValid,Next.bRightTracked,bRightPalmPose?TEXT("palm"):TEXT("controller_or_none"));
    }
    if (bLocalReady && Pose.bHeadTracked && !Next.bHeadTracked) InvalidateCalibration();
    // The login pointer shares combat inputs. The game-instance guard keeps
    // each source blocked independently until that source returns to neutral.
    const bool LoginOpen=LocalController && LocalController->DeviceLogin && LocalController->DeviceLogin->IsOpen();
    auto* GI=Cast<UMRFPSGameInstance>(GetGameInstance());
    if (GI && IsXR()) {
        const auto& RightGlove=GloveFrames[1];
        GI->FireTransition.Observe(RightGlove.bFlexValid && RightGlove.FingerCurls.IsValidIndex(1),IndexCurl,RightGlove.FlexReceivedAt,
            RightGlove.bControllerValid,bGloveTrigger,TriggerAnalog,RightGlove.ControllerReceivedAt);
    }
    if (GI) GI->FireTransition.SetAutomation(AutomationFire);
    const bool FlexAllowed=!GI || GI->FireTransition.CanFlex();
    const bool GloveControllerAllowed=!GI || GI->FireTransition.CanController();
    const bool AutomationAllowed=!GI || GI->FireTransition.CanAutomation();
    if (bRequireFireRelease) {
        const auto& RightGlove=GloveFrames[1];
        GloveFireNeutral.Observe(RightGlove.bFlexValid && RightGlove.FingerCurls.IsValidIndex(1),IndexCurl,
            RightGlove.bControllerValid,bGloveTrigger,TriggerAnalog);
    }
    if (LoginOpen) { bFiring=false; bFingerTriggerDown=false; GloveFireInput.Reset(); if (!GI) bRequireFireRelease=true; }
    else {
        if (bRequireFireRelease && GloveFireNeutral.CanRearm(bPhysicalFireDown,AutomationFire)) bRequireFireRelease=false;
        const auto& RightGlove=GloveFrames[1];
        GloveFireInput.Update(!bRequireFireRelease && FlexAllowed && RightGlove.bFlexValid && RightGlove.FingerCurls.IsValidIndex(1),IndexCurl,
            !bRequireFireRelease && GloveControllerAllowed && RightGlove.bControllerValid,bGloveTrigger,TriggerAnalog);
        bFingerTriggerDown=GloveFireInput.Down();
    }
    Next.Sequence = Pose.Sequence + 1;
    // Client pose remains responsive; only the server can change combat state.
    Pose = Next;
    LastLocalPoseSampleAt=FPlatformTime::Seconds();
    PoseAccumulator += Dt;
    if (PoseAccumulator >= 1.f/30.f) { PoseAccumulator=0; ServerPose(Next); }
    const double Now=GetWorld()->GetTimeSeconds();
    const auto* CombatState=GetWorld()->GetGameState<AMRFPSGameState>();
    if (!LoginOpen && !bRequireFireRelease && CombatState && !CombatState->bCombatPaused && (bFiring || (AutomationAllowed && AutomationFire) || bFingerTriggerDown) && Now-LastLocalFire>=.16) { LastLocalFire=Now; ServerFire(); }
}

void AMRFPSPawn::ServerPose_Implementation(const FMRFPSPose& Next)
{
    const double Now=GetWorld()->GetTimeSeconds();
    if (!Next.IsSane() || (LastPoseReceived>=0 && int32(Next.Sequence-Pose.Sequence)<=0 && !IsLocallyControlled())) return;
    if (Next.bDesktop) { const auto* GM=GetWorld()->GetAuthGameMode<AMRFPSGameMode>(); if (!GM || !GM->bAllowDesktop) return; }
    if (LastPoseReceived>=0 && Now-LastPoseReceived<1./120.) return;
    if (LastPoseReceived>=0 && Next.bHeadTracked && Pose.bHeadTracked &&
        FVector::Dist(Next.Head,Pose.Head)>FMath::Max(35.,800.*(Now-LastPoseReceived))) return;
    if (Pose.LeftFingers.bValid!=Next.LeftFingers.bValid)
        UE_LOG(LogMRFPS,Display,TEXT("MRFPS_GLOVE_INPUT slot=%d hand=left fresh=%d tracked_pose=%d"),GetSlot(),Next.LeftFingers.bValid,Next.bLeftTracked);
    if (Pose.RightFingers.bValid!=Next.RightFingers.bValid)
        UE_LOG(LogMRFPS,Display,TEXT("MRFPS_GLOVE_INPUT slot=%d hand=right fresh=%d tracked_pose=%d"),GetSlot(),Next.RightFingers.bValid,Next.bRightTracked);
    Pose=Next; LastPoseReceived=Now;UpdateWeaponFromPose(Now);
    const auto* CombatState=GetWorld()->GetGameState<AMRFPSGameState>();
    if (CombatState && !CombatState->bCombatPaused) {
        const bool Tracked=Pose.bLeftTracked && Pose.bHeadTracked && CanAct();
        const bool Raised=ServerRise.Update(Pose.Left.Z,Now,Tracked);
        const bool Lowered=ServerLower.Update(-Pose.Left.Z,Now,Tracked);
        if (Raised) DeployWall();
        if (Lowered) RemoveDeployedWall();
    } else { ServerRise.Reset(); ServerLower.Reset(); }
    UpdateVisuals();
}
FVector AMRFPSPawn::Muzzle() const { return MRFPSWeaponPose::Muzzle(FTransform(Pose.RightRotation,FVector(Pose.Right))).GetLocation(); }
void AMRFPSPawn::DeployWall()
{
    const double Now=GetWorld()->GetTimeSeconds();
    if (!HasAuthority() || !CanAct() || !Pose.bLeftTracked || Now<WallReadyAt) return;
    const FRotator R(0,Pose.HeadRotation.Yaw,0);
    FVector P=Pose.Left+R.Vector()*85; P.Z=MRFPS::Layout(GetWorld()).Center.Z+100;
    const auto* GS=GetWorld()->GetGameState<AMRFPSGameState>();
    if (!GS || GS->Phase!=EMRFPSMatchPhase::Waiting || GS->Layout.bCalibrated)
        for (double X:{-8.,8.}) for (double Y:{-80.,80.})
            if (!MRFPS::Layout(GetWorld()).ContainsPlanar(P+R.RotateVector(FVector(X,Y,0)))) return;
    FActorSpawnParameters Params; Params.Owner=this; Params.SpawnCollisionHandlingOverride=ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    if (auto* Wall=GetWorld()->SpawnActor<AMRFPSBarrier>(P,R,Params)) {
        Wall->OwnerSlot=GetSlot(); WallReadyAt=Now+4;
        DeployedWalls.RemoveAll([](const TWeakObjectPtr<AMRFPSBarrier>& Item) { return !Item.IsValid(); });
        DeployedWalls.Add(Wall);
        UE_LOG(LogMRFPS,Display,TEXT("MRFPS_WALL slot=%d at=%s"),GetSlot(),*P.ToString());
    }
}
void AMRFPSPawn::RemoveDeployedWall()
{
    if (!HasAuthority() || !CanAct() || !Pose.bLeftTracked) return;
    while (!DeployedWalls.IsEmpty()) {
        const TWeakObjectPtr<AMRFPSBarrier> Last=DeployedWalls.Pop();
        auto* Wall=Last.Get();
        if (!Wall || Wall->bAdminPlaced || Wall->GetOwner()!=this || Wall->OwnerSlot!=GetSlot()) continue;
        Wall->Destroy();
        UE_LOG(LogMRFPS,Display,TEXT("MRFPS_WALL_REMOVED slot=%d"),GetSlot());
        return;
    }
}
void AMRFPSPawn::ServerFire_Implementation()
{
    const double Now=GetWorld()->GetTimeSeconds();
    if (!CanAct() || MagazineRounds<=0 || !Pose.bRightTracked || Now-LastServerFire<.15) return;
    LastServerFire=Now; --MagazineRounds;++AcceptedShots;ForceNetUpdate();
    const FVector Start=Muzzle(); const FVector End=Start+Pose.RightRotation.Vector()*2000;
    FCollisionQueryParams Params(SCENE_QUERY_STAT(MRFPSShot),false,this);
    FHitResult Hit; const bool bHit=GetWorld()->LineTraceSingleByChannel(Hit,Start,End,WeaponChannel,Params);
    FVector GeometryHit; const auto* Geometry=GetWorld()->GetSubsystem<UMRFPSGeometryWorld>();
    const bool bGeometryHit=Geometry && Geometry->Trace(Start,End,GeometryHit) &&
        (!bHit || FVector::DistSquared(Start,GeometryHit)<=FVector::DistSquared(Start,Hit.ImpactPoint));
    if (!bGeometryHit) {
        if (auto* Target=Cast<AMRFPSPawn>(Hit.GetActor())) {
            const auto* GS=GetWorld()->GetGameState<AMRFPSGameState>();
            if (Hit.GetComponent()==Target->MiniShield && Target->bShieldActive && GS && GS->Phase==EMRFPSMatchPhase::Playing && Target->CanAct()) {
                Target->ShieldEnergy=FMath::Max(0.f,Target->ShieldEnergy-22.f); Target->LastShieldUse=Now;
                UE_LOG(LogMRFPS,Display,TEXT("MRFPS_BLOCK shooter=%d target=%d shield=%.1f"),GetSlot(),Target->GetSlot(),Target->ShieldEnergy);
            } else Target->ApplyShot(Hit.GetComponent()==Target->HeadHitbox ? 30.f : 20.f,this,Hit.GetComponent()==Target->HeadHitbox);
        } else if (auto* Wall=Cast<AMRFPSBarrier>(Hit.GetActor())) Wall->ApplyShot(20);
    }
    ClientFeedback(0,0); MulticastShot(Start,bGeometryHit ? GeometryHit : bHit ? Hit.ImpactPoint : End,bHit || bGeometryHit);
    UE_LOG(LogMRFPS,Display,TEXT("MRFPS_SHOT slot=%d count=%d"),GetSlot(),AcceptedShots);
}
void AMRFPSPawn::ApplyShot(float Damage, AMRFPSPawn* Shooter, bool bHeadshot)
{
    const auto* GS=GetWorld()->GetGameState<AMRFPSGameState>();
    if (!HasAuthority() || !GS || GS->Phase!=EMRFPSMatchPhase::Playing || !CanAct() || !Shooter || !Shooter->CanAct() || Shooter==this || !FMath::IsFinite(Damage) || Damage<=0) return;
    Health=FMath::Max(0.f,Health-Damage); ClientFeedback(Health<=0 ? 2 : 1,Damage);
    UE_LOG(LogMRFPS,Display,TEXT("MRFPS_DAMAGE target=%d shooter=%d health=%.1f headshot=%d"),GetSlot(),Shooter->GetSlot(),Health,bHeadshot);
    if (Health<=0) {
        bShieldActive=false; RespawnAt=GetWorld()->GetTimeSeconds()+3;
        if (auto* GM=GetWorld()->GetAuthGameMode<AMRFPSGameMode>()) GM->OnElimination(this,Shooter);
    }
    ForceNetUpdate(); UpdateVisuals();
}

AMRFPSInspectorPawn::AMRFPSInspectorPawn()
{
    bReplicates=true; bOnlyRelevantToOwner=true;
    auto* Sphere=CreateDefaultSubobject<USphereComponent>(TEXT("Root")); SetRootComponent(Sphere);
    Sphere->SetCollisionEnabled(ECollisionEnabled::NoCollision); Sphere->InitSphereRadius(10);
    Camera=CreateDefaultSubobject<UCameraComponent>(TEXT("FreeCamera")); Camera->SetupAttachment(RootComponent);
    Camera->bUsePawnControlRotation=true; Camera->bLockToHmd=false;
    Movement=CreateDefaultSubobject<UFloatingPawnMovement>(TEXT("Flight")); Movement->MaxSpeed=650; Movement->Acceleration=1800;
}

void AMRFPSPlayerController::BeginPlay()
{
    Super::BeginPlay();
    if (IsLocalController()) {
        bShowMouseCursor=false; SetInputMode(FInputModeGameOnly());
        if (auto* Receiver=GetGameInstance()->GetSubsystem<UMRFPSVideoReceiver>()) Receiver->ResetAll();
#if !UE_SERVER
        FString WindowName;
        if (FParse::Value(FCommandLine::Get(),TEXT("MRFPSWindowName="),WindowName) && !WindowName.IsEmpty())
            RequestedWindowTitle=WindowName.Left(64);
#endif
    }
    FString StartupServer,DeviceCodeFile;
    const auto* StartupInstance=GetGameInstance<UMRFPSGameInstance>();
    if (IsLocalController() && GetNetMode()==NM_Standalone && !FParse::Param(FCommandLine::Get(),TEXT("MRFPSSmoke")) &&
        !(StartupInstance && StartupInstance->IsAutoReconnectPending()) &&
        !FParse::Value(FCommandLine::Get(),TEXT("MRFPSServer="),StartupServer) &&
        !FParse::Value(FCommandLine::Get(),TEXT("MRFPSDeviceCodeFile="),DeviceCodeFile)) {
        GetWorldTimerManager().SetTimer(StartupLoginTimer,FTimerDelegate::CreateWeakLambda(this,[this]() { OpenDeviceLogin(); }),1.f,false);
    }
}
void AMRFPSPlayerController::EndPlay(const EEndPlayReason::Type Reason)
{
    if (GetWorld()) GetWorldTimerManager().ClearTimer(StartupLoginTimer);
    if (IsLocalController()) { if (DeviceLogin) DeviceLogin->CloseLogin(); ClosePlayerVideoWindows(); CloseInspectorViewWindow(); StopGeometryPublisher(); }
    Super::EndPlay(Reason);
}
void AMRFPSPlayerController::SetupInputComponent()
{
    Super::SetupInputComponent();
    InputComponent->BindAction(TEXT("Fire"),IE_Pressed,this,&AMRFPSPlayerController::FirePressed);
    InputComponent->BindAction(TEXT("Fire"),IE_Released,this,&AMRFPSPlayerController::FireReleased);
    InputComponent->BindKey(EKeys::R,IE_Pressed,this,&AMRFPSPlayerController::ReloadPressed);
    InputComponent->BindAction(TEXT("Calibrate"),IE_Pressed,this,&AMRFPSPlayerController::Calibrate);
    InputComponent->BindAction(TEXT("Wall"),IE_Pressed,this,&AMRFPSPlayerController::WallPressed);
    InputComponent->BindAction(TEXT("Shield"),IE_Pressed,this,&AMRFPSPlayerController::ShieldPressed);
    InputComponent->BindAction(TEXT("Shield"),IE_Released,this,&AMRFPSPlayerController::ShieldReleased);
    InputComponent->BindAction(TEXT("FreeView"),IE_Pressed,this,&AMRFPSPlayerController::FreeView);
    InputComponent->BindAction(TEXT("NextView"),IE_Pressed,this,&AMRFPSPlayerController::NextView);
    InputComponent->BindAction(TEXT("MultiView"),IE_Pressed,this,&AMRFPSPlayerController::ToggleMultiView);
    InputComponent->BindAction(TEXT("ArmHaptics"),IE_Pressed,this,&AMRFPSPlayerController::ArmHaptics);
    InputComponent->BindAction(TEXT("StopHaptics"),IE_Pressed,this,&AMRFPSPlayerController::StopHaptics);
    InputComponent->BindAxis(TEXT("Forward"),this,&AMRFPSPlayerController::MoveForward);
    InputComponent->BindAxis(TEXT("Right"),this,&AMRFPSPlayerController::MoveRight);
    InputComponent->BindAxis(TEXT("Up"),this,&AMRFPSPlayerController::MoveUp);
    InputComponent->BindAxis(TEXT("LookX"),this,&AMRFPSPlayerController::LookX);
    InputComponent->BindAxis(TEXT("LookY"),this,&AMRFPSPlayerController::LookY);
    InputComponent->BindAxis(TEXT("MenuPointerTrigger"),this,&AMRFPSPlayerController::MenuPointerTrigger);
    InputComponent->BindKey(EKeys::One,IE_Pressed,this,&AMRFPSPlayerController::ViewPlayerOne);
    InputComponent->BindKey(EKeys::Two,IE_Pressed,this,&AMRFPSPlayerController::ViewPlayerTwo);
    InputComponent->BindKey(EKeys::F1,IE_Pressed,this,&AMRFPSPlayerController::ToggleMenu);
    InputComponent->BindKey(EKeys::F2,IE_Pressed,this,&AMRFPSPlayerController::ToggleAdminPanel);
    InputComponent->BindKey(EKeys::V,IE_Pressed,this,&AMRFPSPlayerController::MRFPSReconstructedViews);
    InputComponent->BindAction(TEXT("ShareView"),IE_Pressed,this,&AMRFPSPlayerController::MRFPSShareView);
}
bool AMRFPSPlayerController::InputKey(const FInputKeyEventArgs& Params)
{
    // A is also the existing ArmHaptics action. On Quest consume it before
    // PlayerInput sees it, so opening the panel does not arm haptics as well.
    if (HandleQuestLoginButton(Params,PLATFORM_ANDROID)) return true;
    if (HandleQuestHapticsButton(Params,PLATFORM_ANDROID)) return true;
    if (IsLocalController() && !Params.IsSimulatedInput() && Params.Key.GetFName()==FName(TEXT("OculusTouch_Right_Trigger_Click")) &&
        (Params.Event==IE_Pressed || Params.Event==IE_Released)) {
        if (auto* GI=Cast<UMRFPSGameInstance>(GetGameInstance())) GI->FireTransition.SetPhysical(Params.Event==IE_Pressed);
    }
    if (DeviceLogin && DeviceLogin->IsOpen() && Params.Key.GetFName()==FName(TEXT("OculusTouch_Right_Trigger_Click"))) {
        if (Params.Event==IE_Pressed || Params.Event==IE_Released) {
            const bool Down=Params.Event==IE_Pressed; DeviceLogin->SetPointerPressed(Down);
            if (auto* LocalPawn=GetPawn<AMRFPSPawn>()) LocalPawn->SetFiring(Down);
        }
        return true;
    }
    // UIOnly menus own their events. Only a click delivered by the viewport
    // may reacquire inspection controls; a menu click must not do so.
    if (IsLocalController() && IsInspector() && !(SessionMenu && SessionMenu->IsInViewport()) && !(AdminPanel && AdminPanel->IsInViewport())) {
        if(IsAdmin()&&Params.Key==EKeys::B&&Params.Event==IE_Pressed) { SetAdminWallPlacement(!bAdminWallPlacement);return true; }
        if(bAdminWallPlacement&&IsAdmin()) {
            if(Params.Key==EKeys::RightMouseButton) { if(Params.Event==IE_Pressed) SetAdminWallPlacement(false);return true; }
            if(Params.Event==IE_Pressed) {
                if(Params.Key==EKeys::R) { AdminWallYaw=FMath::Fmod(AdminWallYaw+90,360.f);return true; }
                if(Params.Key==EKeys::Delete) { RemoveSelectedBuild();return true; }
                if(Params.Key==EKeys::MouseScrollUp||Params.Key==EKeys::MouseScrollDown||Params.Key==EKeys::Up||Params.Key==EKeys::Down) {
                    AdjustSelectedWallHeight((Params.Key==EKeys::MouseScrollUp||Params.Key==EKeys::Up)?1:-1);return true;
                }
            }
        }
        if (Params.Key==EKeys::Escape) {
            if (Params.Event==IE_Pressed) { StopHaptics(); SetAdminWallPlacement(false); SetInspectorMouseReleased(true); }
            return true;
        }
        if (Params.Key==EKeys::LeftMouseButton) {
            if (bConsumePlayerVideoClick) {
                if (Params.Event==IE_Released) { bConsumePlayerVideoClick=false; return true; }
                // Opening a new native window can take focus before mouse-up.
                // Do not swallow the next click when focus returns here.
                if (Params.Event==IE_Pressed || Params.Event==IE_DoubleClick) bConsumePlayerVideoClick=false;
                else return true;
            }
            if (bInspectorMouseReleased && !bAdminWallPlacement &&
                (Params.Event==IE_Pressed || Params.Event==IE_DoubleClick) && TryOpenInspectorWindowAtCursor()) {
                bConsumePlayerVideoClick=true;
                return true;
            }
            if (bAdminWallPlacement && IsAdmin()) {
                if (Params.Event==IE_Pressed || Params.Event==IE_DoubleClick) {
                    if (SelectedWallHeightCm<0) { bConsumeAdminPlacementClick=false; return true; }
                    bConsumeAdminPlacementClick=!ToggleSelectedWallCell();
                    if(bConsumeAdminPlacementClick) PlaceSelectedBuild();
                }
                if (Params.Event==IE_Released) bConsumeAdminPlacementClick=false;
                return true;
            }
            if (bConsumeAdminPlacementClick) {
                if (Params.Event==IE_Released) bConsumeAdminPlacementClick=false;
                return true;
            }
            if (bInspectorMouseReleased && (Params.Event==IE_Pressed || Params.Event==IE_DoubleClick)) {
                bConsumeInspectorCaptureClick=true;
                SetInspectorMouseReleased(false);
                return true;
            }
            if (bConsumeInspectorCaptureClick) {
                if (Params.Event==IE_Released) {
                    bConsumeInspectorCaptureClick=false;
                    FlushPressedKeys(); RotationInput=FRotator::ZeroRotator;
                }
                return true;
            }
        }
        // Drop pointer motion before PlayerInput can queue it, including the
        // cursor warp caused by capture. Gamepad look is gated in LookX/LookY.
        if ((bInspectorMouseReleased || bConsumeInspectorCaptureClick) &&
            (Params.Key==EKeys::MouseX || Params.Key==EKeys::MouseY)) return true;
    }
    // Players retain the existing Escape -> StopHaptics action binding.
    return Super::InputKey(Params);
}
bool AMRFPSPlayerController::HandleQuestLoginButton(const FInputKeyEventArgs& Params,bool bAndroid)
{
    if (!bAndroid || !IsLocalController() || Params.Key!=EKeys::OculusTouch_Right_A_Click) return false;
    if (Params.Event==IE_Pressed) {
        SetQuestDeviceLoginVisible(!(DeviceLogin && DeviceLogin->IsOpen()));
        UE_LOG(LogMRFPS,Display,TEXT("MRFPS_QUEST_LOGIN_TOGGLE open=%d connected=%d"),
            DeviceLogin && DeviceLogin->IsOpen(),GetNetMode()==NM_Client);
    }
    return true;
}
bool AMRFPSPlayerController::HandleQuestHapticsButton(const FInputKeyEventArgs& Params,bool bAndroid)
{
    if (!bAndroid || !IsLocalController() || Params.Key!=EKeys::OculusTouch_Right_Thumbstick_Click) return false;
    // Handle the physical edge immediately. A click made while the menu is
    // open cannot be queued by PlayerInput and applied after the menu closes.
    if (Params.Event==IE_Pressed && !Params.IsSimulatedInput()) QuestArmHaptics();
    return true;
}
void AMRFPSPlayerController::ApplyInspectorMouseMode()
{
    if (!IsLocalController() || !IsInspector() || (SessionMenu && SessionMenu->IsInViewport()) || (AdminPanel && AdminPanel->IsInViewport())) return;
    bShowMouseCursor=bInspectorMouseReleased;
    if (bInspectorMouseReleased) {
        FInputModeGameAndUI Mode;
        Mode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
        Mode.SetHideCursorDuringCapture(false);
        SetInputMode(Mode);
    } else {
        FInputModeGameOnly Mode; Mode.SetConsumeCaptureMouseDown(true);
        SetInputMode(Mode);
    }
}
void AMRFPSPlayerController::SetInspectorMouseReleased(bool Released)
{
    if (!IsLocalController() || !IsInspector() || (SessionMenu && SessionMenu->IsInViewport()) || (AdminPanel && AdminPanel->IsInViewport())) return;
    bInspectorMouseReleased=Released;
    if (Released) bConsumeInspectorCaptureClick=false;
    MoveAxes=FVector::ZeroVector; RotationInput=FRotator::ZeroRotator;
    FlushPressedKeys();
    if (auto* InspectorPawn=GetPawn<AMRFPSInspectorPawn>(); InspectorPawn && InspectorPawn->Movement) {
        InspectorPawn->Movement->StopMovementImmediately();
        (void)InspectorPawn->Movement->ConsumeInputVector();
    }
    ApplyInspectorMouseMode();
    UE_LOG(LogMRFPS,Display,TEXT("MRFPS_INSPECTOR_MOUSE state=%s"),Released?TEXT("released"):TEXT("captured"));
}
bool AMRFPSPlayerController::IsInspector() const
{
    const auto* PS=GetPlayerState<AMRFPSPlayerState>(); return PS && PS->bInspector;
}
TArray<AMRFPSPawn*> AMRFPSPlayerController::GetPlayers() const
{
    TArray<AMRFPSPawn*> Players;
    for (TActorIterator<AMRFPSPawn> It(GetWorld()); It; ++It) if (It->GetSlot()>=0) Players.Add(*It);
    Players.Sort([](const AMRFPSPawn& A,const AMRFPSPawn& B) { return A.GetSlot()<B.GetSlot(); });
    Players.Sort([](const AMRFPSPawn& A,const AMRFPSPawn& B) { return A.GetSlot()<B.GetSlot(); });
    return Players;
}
AMRFPSPawn* AMRFPSPlayerController::FindPlayer(int32 Slot) const
{
    for (auto* P:GetPlayers()) if (P->GetSlot()==Slot) return P;
    return nullptr;
}
void AMRFPSPlayerController::FreeView()
{
    if (!IsInspector()) return;
    ViewSlot=-1; SetViewTargetWithBlend(GetPawn(),.15);
    UE_LOG(LogMRFPS,Display,TEXT("MRFPS_VIEW inspector=free"));
}
void AMRFPSPlayerController::MRFPSView(int32 Slot)
{
    if (!IsInspector()) return;
    if (Slot<0 || Slot>1) return;
    SetAdminWallPlacement(false);
    ViewSlot=Slot;
    if (auto* P=FindPlayer(Slot)) SetViewTargetWithBlend(P,.15);
    UE_LOG(LogMRFPS,Display,TEXT("MRFPS_VIEW inspector=player slot=%d"),Slot);
}
void AMRFPSPlayerController::NextView()
{
    if (!IsInspector()) return;
    MRFPSView((ViewSlot+1)%2);
}
void AMRFPSPlayerController::ViewPlayerOne() { MRFPSView(0); }
void AMRFPSPlayerController::ViewPlayerTwo() { MRFPSView(1); }
void AMRFPSPlayerController::ToggleMultiView()
{
    if (!IsInspector()) return;
    if (bAdminWallPlacement) { SetAdminWallPlacement(false); return; }
    bMultiView=!bMultiView;
}
void AMRFPSPlayerController::MenuPointerTrigger(float Value)
{
    if (DeviceLogin && DeviceLogin->IsOpen()) DeviceLogin->SetPointerTriggerAxis(Value);
}
void AMRFPSPlayerController::FirePressed()
{
    if (DeviceLogin && DeviceLogin->IsOpen()) { DeviceLogin->SetPointerPressed(true); if (auto* P=GetPawn<AMRFPSPawn>()) P->SetFiring(true); return; }
    if (auto* P=GetPawn<AMRFPSPawn>()) P->SetFiring(true);
}
void AMRFPSPlayerController::FireReleased()
{
    if (DeviceLogin && DeviceLogin->IsOpen()) { DeviceLogin->SetPointerPressed(false); if (auto* P=GetPawn<AMRFPSPawn>()) P->SetFiring(false); return; }
    if (auto* P=GetPawn<AMRFPSPawn>()) P->SetFiring(false);
}
void AMRFPSPlayerController::ReloadPressed() { if (auto* P=GetPawn<AMRFPSPawn>()) P->DesktopReload(); }
void AMRFPSPlayerController::WallPressed() { if (auto* P=GetPawn<AMRFPSPawn>()) P->DesktopWall(); }
void AMRFPSPlayerController::ShieldPressed() { if (auto* P=GetPawn<AMRFPSPawn>()) P->SetDesktopShield(true); }
void AMRFPSPlayerController::ShieldReleased() { if (auto* P=GetPawn<AMRFPSPawn>()) P->SetDesktopShield(false); }
void AMRFPSPlayerController::Calibrate() { if (auto* P=GetPawn<AMRFPSPawn>()) P->Calibrate(); }
void AMRFPSPlayerController::MRFPSReady() { Calibrate(); }
void AMRFPSPlayerController::ArmHaptics()
{
    if (auto* P=GetPawn<AMRFPSPawn>(); P && P->Haptics) {
        FString Error; if (P->Haptics->GetConnectionState()!=EMoBitsConnectionState::Connected) P->Haptics->Connect(Error);
        P->Haptics->ArmHaptics();
    }
}
void AMRFPSPlayerController::QuestArmHaptics()
{
    // Closing the overlay never arms automatically. Require a fresh, explicit
    // right-stick click, and keep haptics stopped while login is displayed.
    if (!IsLocalController() || (DeviceLogin && DeviceLogin->IsOpen())) return;
    const auto* Fighter=GetPawn<AMRFPSPawn>();
    if (!Fighter || !Fighter->Haptics) return;
#if WITH_DEV_AUTOMATION_TESTS
    if (TestQuestArmRequest) { TestQuestArmRequest(); return; }
#endif
    const bool Connected=Fighter->Haptics->GetConnectionState()==EMoBitsConnectionState::Connected;
    ArmHaptics();
    // This records an attempt only. The SDK's asynchronous receipt confirms
    // an actual arm; an unavailable connection may require a second click.
    UE_LOG(LogMRFPS,Display,TEXT("MRFPS_QUEST_HAPTICS_ARM attempted=1 connected=%d"),Connected);
}
void AMRFPSPlayerController::StopHaptics() { if (auto* P=GetPawn<AMRFPSPawn>(); P && P->Haptics) P->Haptics->StopHaptics(); }
void AMRFPSPlayerController::ToggleMenu()
{
    MoveAxes=FVector::ZeroVector; bConsumeInspectorCaptureClick=false;
    SetAdminWallPlacement(false);
    if (AdminPanel && AdminPanel->IsInViewport()) AdminPanel->Close();
    if (SessionMenu && SessionMenu->IsInViewport()) SessionMenu->Close();
    else SessionMenu=UMRFPSMenu::CreateFor(this);
}
void AMRFPSPlayerController::LookX(float V)
{
    if (IsLookInputIgnored() || (IsInspector() && (bInspectorMouseReleased || bConsumeInspectorCaptureClick))) return;
    if (IsInspector() && ViewSlot<0) AddYawInput(V);
    else if (auto* P=GetPawn<AMRFPSPawn>()) P->DesktopLook(V,0);
}
void AMRFPSPlayerController::LookY(float V)
{
    if (IsLookInputIgnored() || (IsInspector() && (bInspectorMouseReleased || bConsumeInspectorCaptureClick))) return;
    if (IsInspector() && ViewSlot<0) AddPitchInput(V);
    // MouseY's -1 axis mapping is already compensated by the controller's
    // legacy pitch scale. DesktopLook changes FRotator.Pitch directly instead.
    else if (auto* P=GetPawn<AMRFPSPawn>()) P->DesktopLook(0,-V);
}
void AMRFPSPlayerController::RefreshCaptures()
{
    if (!IsLocalController() || !IsInspector() || !FApp::CanEverRender()) return;
    if (!bReconstructedViews) {
        for (const auto& C:Captures) if (C) C->DestroyComponent(); Captures.Reset(); ViewTextures.Reset(); return;
    }
    const auto Players=GetPlayers();
    while (Captures.Num()>Players.Num()) { Captures.Last()->DestroyComponent(); Captures.Pop(); ViewTextures.Pop(); }
    while (Captures.Num()<Players.Num()) {
        auto* Texture=NewObject<UTextureRenderTarget2D>(this); Texture->InitAutoFormat(512,288); Texture->ClearColor=FLinearColor::Black;
        ViewTextures.Add(Texture);
        auto* C=NewObject<USceneCaptureComponent2D>(this); C->TextureTarget=Texture;
        C->CaptureSource=ESceneCaptureSource::SCS_FinalColorLDR; C->FOVAngle=90;
        C->bCaptureEveryFrame=false; C->bCaptureOnMovement=false; C->RegisterComponentWithWorld(GetWorld());
        C->ShowFlags.SetMotionBlur(false); Captures.Add(C);
    }
    for (int32 I=0; I<Captures.Num(); ++I) {
        Captures[I]->HiddenActors.Reset();
        Captures[I]->HiddenComponents.Reset();
        if (!Players.IsValidIndex(I) || !bMultiView) continue;
        auto* P=Players[I]; Captures[I]->HiddenComponents.Add(P->HeadVisual); Captures[I]->HiddenComponents.Add(P->BodyVisual);
        Captures[I]->HiddenComponents.Add(P->Avatar);
        Captures[I]->SetWorldLocationAndRotation(P->Pose.Head,P->Pose.HeadRotation); Captures[I]->CaptureScene();
    }
}
void AMRFPSPlayerController::MRFPSConnect(const FString& Address, bool Inspector)
{ ConnectWithRole(Address,Inspector?TEXT("inspector"):TEXT("player")); }
void AMRFPSPlayerController::MRFPSConnectAdmin(const FString& Address)
{ ConnectWithRole(Address,TEXT("admin")); }
void AMRFPSPlayerController::MRFPSReconnect()
{
    if (IsLocalController()) if (auto* Instance=GetGameInstance<UMRFPSGameInstance>()) Instance->ReconnectWithFreshToken();
}
void AMRFPSPlayerController::OpenDeviceLogin()
{
    if (!IsLocalController() || (DeviceLogin && DeviceLogin->IsOpen())) return;
    if (SessionMenu && SessionMenu->IsInViewport()) SessionMenu->Close();
    if (GetNetMode()!=NM_Standalone) {
        if (auto* Instance=GetGameInstance<UMRFPSGameInstance>()) Instance->RequestLoginScreen();
        ClientTravel(TEXT("/Game/Maps/MRFPSArena"),TRAVEL_Absolute); return;
    }
    // A failure can be reported before UE finishes returning to the local map.
    // Do not create a panel in a world that is about to be replaced.
    if (!MRFPS::StableLoginWorld(GetWorld())) return;
    DeviceLogin=UMRFPSLoginMenu::CreateFor(this);
}
void AMRFPSPlayerController::SetQuestDeviceLoginVisible(bool bVisible)
{
    if (!IsLocalController()) return;
    UWorld* World=GetWorld();
    if (!World || World->bIsTearingDown) return;
    if (!bVisible) {
        // An explicit close also cancels the startup timer, including when a
        // CLI close arrives before the automatic first open.
        GetWorldTimerManager().ClearTimer(StartupLoginTimer);
        if (DeviceLogin && DeviceLogin->IsOpen()) DeviceLogin->CloseLogin();
        if (auto* Instance=GetGameInstance<UMRFPSGameInstance>()) Instance->ClearLoginScreenRequest();
        return;
    }
    if (DeviceLogin && DeviceLogin->IsOpen()) {
        GetWorldTimerManager().ClearTimer(StartupLoginTimer);
        return;
    }
    // Do not create UI in a world already being replaced. Unlike
    // OpenDeviceLogin(), opening this overlay never requests ClientTravel.
    const FWorldContext* Context=GEngine?GEngine->GetWorldContextFromWorld(World):nullptr;
    if (!Context || !Context->TravelURL.IsEmpty() || Context->PendingNetGame || World->IsInSeamlessTravel()) return;
    if (SessionMenu && SessionMenu->IsInViewport()) SessionMenu->Close();
    DeviceLogin=UMRFPSLoginMenu::CreateFor(this);
    if (DeviceLogin && DeviceLogin->IsOpen()) {
        GetWorldTimerManager().ClearTimer(StartupLoginTimer);
        if (auto* Instance=GetGameInstance<UMRFPSGameInstance>()) Instance->ClearLoginScreenRequest();
    }
}
void AMRFPSPlayerController::ReportConnectionAttemptFailure()
{
    if (auto* Instance=GetGameInstance<UMRFPSGameInstance>()) {
        Instance->CancelNextDeviceConnection();
        Instance->SetConnectionFailure(MRFPS::FailureKind(ConnectionError));
        Instance->RequestLoginScreen();
        if (DeviceLogin && DeviceLogin->IsOpen() && MRFPS::StableLoginWorld(GetWorld())) {
            DeviceLogin->ShowConnectionFailure(); Instance->ClearLoginScreenRequest();
        }
    }
}
void AMRFPSPlayerController::BeginDeviceConnection(const FString& Address,const FString& Role,UMRFPSLoginMenu* Owner,uint64 Generation)
{
    if (!Owner || !Owner->IsCurrentExchange(Generation)) return;
    ConnectWithRole(Address,Role,Owner,Generation);
}
void AMRFPSPlayerController::CancelPendingDeviceConnection(UMRFPSLoginMenu* Owner,uint64 Generation)
{
    if (PendingDeviceOwner.Get()!=Owner || PendingDeviceGeneration!=Generation) return;
    if (GetWorld()) GetWorldTimerManager().ClearTimer(PendingConnectionTimer);
    PendingDeviceOwner.Reset(); PendingDeviceGeneration=0;
    if (auto* Instance=GetGameInstance<UMRFPSGameInstance>()) Instance->CancelNextDeviceConnection();
}
void AMRFPSPlayerController::MRFPSConfigureServer(const FString& Address)
{
    OpenDeviceLogin(); if (DeviceLogin && DeviceLogin->IsOpen()) DeviceLogin->SetEndpoint(Address);
}
void AMRFPSPlayerController::MRFPSLoginFile(const FString& File,const FString& RequestedRole)
{
    OpenDeviceLogin(); if (DeviceLogin && DeviceLogin->IsOpen()) DeviceLogin->LoginCodeFromFile(File,RequestedRole);
}
void AMRFPSPlayerController::MRFPSResetIssuerTrust()
{
    if (!IsLocalController() || GetNetMode()!=NM_Standalone) return;
    FString Error; if (auto* Credentials=GetGameInstance()->GetSubsystem<UMRFPSAuthClientSubsystem>()) Credentials->ClearIssuerTrust(Error);
}
void AMRFPSPlayerController::ConnectWithRole(const FString& Address,const FString& RequestedRole,UMRFPSLoginMenu* DeviceOwner,uint64 Generation)
{
    if (DeviceOwner && !DeviceOwner->IsCurrentExchange(Generation)) return;
    // Address may belong to the executing timer's lambda. Clearing that timer
    // releases its captures, so own the destination before cancelling it.
    const FString Destination=Address;
    const FString ExpectedRole=RequestedRole;
    GetWorldTimerManager().ClearTimer(PendingConnectionTimer);
    PendingDeviceOwner.Reset(); PendingDeviceGeneration=0;
    if (!DeviceOwner) if (auto* Instance=GetGameInstance<UMRFPSGameInstance>()) Instance->CancelNextDeviceConnection();
    if (Destination.IsEmpty() || Destination.Len()>255) { ConnectionError=TEXT("INVALID_ADDRESS"); ReportConnectionAttemptFailure(); return; }
    for (const TCHAR C:Destination) if (!FChar::IsAlnum(C) && C!='.' && C!=':' && C!='-' && C!='[' && C!=']') {
        ConnectionError=TEXT("INVALID_ADDRESS"); ReportConnectionAttemptFailure(); return;
    }
    auto* Credentials=GetGameInstance()->GetSubsystem<UMRFPSAuthClientSubsystem>();
    bool Ready=Credentials && Credentials->ValidateForNewConnection(ExpectedRole,ConnectionError);
    // Explicit connect also picks up a newly provisioned file. A reload of the
    // old bearer cannot clear its attempted-jti history.
    if (!Ready && Credentials) Ready=Credentials->ReloadTokenFile(ConnectionError) && Credentials->ValidateForNewConnection(ExpectedRole,ConnectionError);
    if (!Ready) {
        if (ConnectionError.IsEmpty()) ConnectionError=TEXT("AUTH_MISSING");
        UE_LOG(LogMRFPS,Warning,TEXT("MRFPS_AUTH_CLIENT_REJECT code=%s role=%s"),*ConnectionError,*ExpectedRole); ReportConnectionAttemptFailure(); return;
    }
    if (!FMRFPSTransportModule::SetClientServerFingerprint(Credentials->GetServerFingerprint(),ConnectionError)) { ReportConnectionAttemptFailure(); return; }
    const double ActivationDelay = FMRFPSTransportModule::GetClientCertificateActivationDelay();
    if (ActivationDelay > 0.0) {
        ConnectionError=TEXT("Connecting...");
        UE_LOG(LogMRFPS,Display,TEXT("MRFPS_DTLS_CLIENT_WAIT seconds=%.2f"),ActivationDelay);
        // A new certificate's notBefore must be in the past on both devices.
        // Wait before opening the connection, then revalidate token/time/role.
        PendingDeviceOwner=DeviceOwner; PendingDeviceGeneration=Generation;
        const TWeakObjectPtr<UMRFPSLoginMenu> WeakDeviceOwner(DeviceOwner);
        const bool DeviceAttempt=DeviceOwner!=nullptr;
        GetWorldTimerManager().SetTimer(PendingConnectionTimer,FTimerDelegate::CreateWeakLambda(this,[this,Destination,ExpectedRole,WeakDeviceOwner,Generation,DeviceAttempt]() {
            // A dispatched old delegate may run after a newer timer is installed.
            // Do not clear the newer attempt's ownership when rejecting it.
            if (DeviceAttempt && (!WeakDeviceOwner.IsValid() || !WeakDeviceOwner->IsCurrentExchange(Generation))) return;
            ConnectWithRole(Destination,ExpectedRole,WeakDeviceOwner.Get(),Generation);
        }),static_cast<float>(ActivationDelay)+0.02f,false);
        return;
    }
    ConnectionError.Empty();
    if (!Credentials->BeginConnectionAttempt(ExpectedRole,ConnectionError)) {
        UE_LOG(LogMRFPS,Warning,TEXT("MRFPS_AUTH_CLIENT_REJECT code=%s role=%s"),*ConnectionError,*ExpectedRole); ReportConnectionAttemptFailure(); return;
    }
    if (auto* Instance=GetGameInstance<UMRFPSGameInstance>()) Instance->RememberConnectionAttempt(Destination,ExpectedRole,GetWorld());
    HandleSessionDisconnected();
    const FString RoleOptions=ExpectedRole==TEXT("admin")?TEXT("?Admin=1?Inspector=1"):ExpectedRole==TEXT("inspector")?TEXT("?Inspector=1"):TEXT("");
    ClientTravel(Destination+RoleOptions+TEXT("?EncryptionToken=")+FMRFPSTransportModule::GetHelloMarker(),TRAVEL_Absolute);
}
void AMRFPSPlayerController::PlayerTick(float Dt)
{
    Super::PlayerTick(Dt); if (!IsLocalController()) return;
#if !UE_SERVER
    // The game window's native handle may be created after BeginPlay. Reapply
    // to the actual window as it appears, including after world travel.
    if (!RequestedWindowTitle.IsEmpty() && FPlatformTime::Seconds()>=NextWindowTitleUpdate) {
        NextWindowTitleUpdate=FPlatformTime::Seconds()+1.;
        if (auto* Instance=GetGameInstance())
            if (auto* Viewport=Instance->GetGameViewportClient())
                if (auto Window=Viewport->GetWindow()) Window->SetTitle(FText::FromString(RequestedWindowTitle));
    }
#endif
    TickAutomation();
    TickAutomationInput(Dt);
    if (auto* Instance=GetGameInstance<UMRFPSGameInstance>()) {
        if (Instance->IsLoginScreenRequested() && GetNetMode()==NM_Standalone && GetPawn() && MRFPS::StableLoginWorld(GetWorld())) {
            OpenDeviceLogin();
            if (DeviceLogin && DeviceLogin->IsOpen()) { DeviceLogin->ShowConnectionFailure(); Instance->ClearLoginScreenRequest(); }
        }
        if (!(DeviceLogin && DeviceLogin->IsOpen())) Instance->TickAutoReconnect();
        const auto* Driver=GetWorld()?GetWorld()->GetNetDriver():nullptr;
        const auto* Connection=Driver?Driver->ServerConnection.Get():nullptr;
        const auto* PS=GetPlayerState<AMRFPSPlayerState>();
        if (Instance->ConnectionStatus.ObserveJoin(Instance->IsNewConnectionWorld(GetWorld()) && GetNetMode()==NM_Client,
            Connection && Connection->GetConnectionState()==USOCK_Open && Connection->PlayerController==this,
            PS && PS->bAuthenticated && PS->AuthExpiresAt>FDateTime::UtcNow().ToUnixTimestamp(),
            GetPawn()!=nullptr,PS && PS->bAdmin,PS && PS->bInspector,PS?PS->Slot:-1))
            UE_LOG(LogMRFPS,Display,TEXT("MRFPS_CLIENT_JOIN_CONFIRMED"));
    }
#if !UE_SERVER
    // A minimized or hidden window is not painted, so UUserWidget::NativeTick
    // is not a reliable place to process local CLI control instructions.
    if (DeviceLogin) DeviceLogin->TickControlFile();
#endif
    if (bAdminWallPlacement && !IsAdmin()) SetAdminWallPlacement(false);
    TickAdminPreview();
    TickQuestVideo();
    TickMeasuredGeometry();
    const double Now=GetWorld()->GetTimeSeconds();
    if (IsMoveInputIgnored() || (IsInspector() && bInspectorMouseReleased && !bAdminWallPlacement)) MoveAxes=FVector::ZeroVector;
    if (IsInspector()) {
        if ((SessionMenu && SessionMenu->IsInViewport()) || (AdminPanel && AdminPanel->IsInViewport())) bConsumeInspectorCaptureClick=false;
        // Menus temporarily own UIOnly input. Their close handler restores
        // GameOnly; restore the inspector's previous release state afterward
        // without changing the menu's ignore-move/look counter ownership.
        if (bInspectorMouseReleased) {
            if (!(SessionMenu && SessionMenu->IsInViewport()) && !(AdminPanel && AdminPanel->IsInViewport()) && !bShowMouseCursor) ApplyInspectorMouseMode();
            if (auto* InspectorPawn=GetPawn<AMRFPSInspectorPawn>(); !bAdminWallPlacement && InspectorPawn && InspectorPawn->Movement) {
                InspectorPawn->Movement->StopMovementImmediately();
                (void)InspectorPawn->Movement->ConsumeInputVector();
            }
        }
        if ((!bInspectorMouseReleased || bAdminWallPlacement) && ViewSlot<0 && GetPawn()) {
            GetPawn()->AddMovementInput(GetControlRotation().Vector(),MoveAxes.X);
            GetPawn()->AddMovementInput(FRotationMatrix(GetControlRotation()).GetUnitAxis(EAxis::Y),MoveAxes.Y);
            GetPawn()->AddMovementInput(FVector::UpVector,MoveAxes.Z);
        }
        if (bReconstructedViews && ViewSlot>=0 && !FindPlayer(ViewSlot)) FreeView();
        if (Now-LastCaptureRefresh>=1./30.) { LastCaptureRefresh=Now; RefreshCaptures(); }
    } else if (auto* P=GetPawn<AMRFPSPawn>()) {
        const bool StillAllowed=AutomationInputPawn.Get()==P && AutomationInput.Active(FPlatformTime::Seconds()) && DesktopAutomationAllowed();
        const FVector Extra=StillAllowed
            ? FVector(AutomationInput.Move.X,AutomationInput.Move.Y,0) : FVector::ZeroVector;
        const float AutoDt=StillAllowed?FMath::Clamp(AutomationMoveSeconds,0.f,FMath::Max(0.f,Dt)):0.f;
        if (!StillAllowed && AutomationInputPawn.IsValid()) ClearAutomationInput();
        if (AutoDt>0) P->DesktopMove((MoveAxes+Extra).GetClampedToMaxSize(1),AutoDt);
        P->DesktopMove(MoveAxes,FMath::Max(0.f,Dt-AutoDt));
    }
    if (FParse::Param(FCommandLine::Get(),TEXT("MRFPSAutoReady"))) {
        if (auto* P=GetPawn<AMRFPSPawn>(); P && P->GetSlot()>=0 && !P->bCalibrated && Now>.5) P->Calibrate();
    }
    if (FParse::Param(FCommandLine::Get(),TEXT("MRFPSSmoke"))) {
        const auto* SmokeGS=GetWorld()->GetGameState<AMRFPSGameState>();
        if (SmokeGS && SmokeGS->Phase==EMRFPSMatchPhase::Playing && SmokeCombatStartedAt<0) SmokeCombatStartedAt=Now;
        const double CombatTime=SmokeCombatStartedAt<0 ? -100 : Now-SmokeCombatStartedAt;
        if (auto* P=GetPawn<AMRFPSPawn>()) {
            if (P->GetSlot()==1) {
                P->SetDesktopShield(CombatTime>=2 && CombatTime<4);
                if (CombatTime>=4.5 && SmokeStage==0) { SmokeStage=1; P->DesktopWall(); }
            } else if (P->GetSlot()==0) {
                if (auto* Other=FindPlayer(1)) {
                    const FVector Aim=Other->bShieldActive ? Other->MiniShield->GetComponentLocation() : FVector(Other->Pose.Head)+FVector(0,0,-52);
                    const FRotator Want=(Aim-P->Muzzle()).Rotation();
                    const FRotator Delta=(Want-P->Pose.RightRotation).GetNormalized(); P->DesktopLook(Delta.Yaw,Delta.Pitch);
                    P->SetFiring((CombatTime>=2.5 && CombatTime<3.6) || (CombatTime>=5 && CombatTime<9));
                }
            }
        }
        if (Now>=NextSmokeAction) {
            NextSmokeAction=Now+1;
            const auto Players=GetPlayers(); int32 Walls=0; for (TActorIterator<AMRFPSBarrier> It(GetWorld());It;++It) if(!It->bPlacementPreview) ++Walls;
            const auto* GS=GetWorld()->GetGameState<AMRFPSGameState>();
            UE_LOG(LogMRFPS,Display,TEXT("MRFPS_SNAPSHOT players=%d role=%s phase=%d walls=%d p0hp=%.1f p1hp=%.1f kills0=%d"),
                Players.Num(),IsInspector()?TEXT("inspector"):TEXT("player"),GS?int32(GS->Phase):-1,Walls,
                FindPlayer(0)?FindPlayer(0)->Health:-1,FindPlayer(1)?FindPlayer(1)->Health:-1,
                FindPlayer(0)&&FindPlayer(0)->GetPlayerState<AMRFPSPlayerState>()?FindPlayer(0)->GetPlayerState<AMRFPSPlayerState>()->Kills:-1);
            if (IsInspector() && Players.Num()==2) { if (SmokeStage==0) { MRFPSView(0); SmokeStage=1; } else if (SmokeStage==1) { MRFPSView(1); SmokeStage=2; } else if (SmokeStage==2) { FreeView(); SmokeStage=3; } }
        }
    }
}

void AMRFPSHUD::DrawHUD()
{
    Super::DrawHUD();if(!Canvas) return;
    auto* PC=Cast<AMRFPSPlayerController>(PlayerOwner);if(!PC) return;
    const auto* GS=GetWorld()->GetGameState<AMRFPSGameState>();const bool Inspector=PC->IsInspector();
    const auto Text=[this](const FString& Value,float X,float Y,int32 Size=16,FLinearColor Color=FLinearColor::White) {
        FCanvasTextItem Item(FVector2D(X,Y),FText::FromString(Value),FSlateFontInfo(FPaths::EngineContentDir()/TEXT("Slate/Fonts/DroidSansFallback.ttf"),Size),Color);
        // Canvas checks UFont even when SlateFontInfo supplies the actual face.
        Item.Font=GEngine?GEngine->GetSmallFont():nullptr;
        Item.EnableShadow(FLinearColor(0,0,0,.6));Canvas->DrawItem(Item);
    };
    const auto Video=[this](UTexture2D* Texture,float X,float Y,float W,float H) {
        DrawRect(FLinearColor::Black,X,Y,W,H);if(!Texture||Texture->GetSizeX()<=0||Texture->GetSizeY()<=0) return;
        const float Scale=FMath::Min(W/Texture->GetSizeX(),H/Texture->GetSizeY());const float TW=Texture->GetSizeX()*Scale,TH=Texture->GetSizeY()*Scale;
        DrawTexture(Texture,X+(W-TW)*.5f,Y+(H-TH)*.5f,TW,TH,0,0,1,1,FLinearColor::White,BLEND_Opaque);
    };
    if(Inspector&&PC->ViewSlot>=0&&!PC->bReconstructedViews) { auto* Frame=PC->GetQuestVideoTexture(PC->ViewSlot);Video(Frame,0,0,Canvas->SizeX,Canvas->SizeY);if(!Frame) Text(TEXT("映像を待っています"),32,Canvas->SizeY*.45,18); }
    const FMRFPSInspectorHudLayout InspectorLayout{float(Canvas->SizeX)};
    const float InfoTop=Inspector?InspectorLayout.InfoTop:18.f;
    DrawRect(FLinearColor(.015,.025,.045,.9),18,InfoTop,260,104);
    if(Inspector && FPlatformProperties::SupportsWindowedMode() && !PC->IsAdminWallPlacement()) for(int32 I=0;I<3;++I) {
        const auto Button=InspectorLayout.Button(I);
        DrawRect(FLinearColor(.04,.20,.36,.95),Button.X,Button.Y,Button.Width,Button.Height);
        const FString Label=I<2?FString::Printf(TEXT("P%d 別窓"),I+1):FString(TEXT("観戦 別窓"));
        Text(Label,Button.X+8,Button.Y+5,14,FLinearColor(.8,.9,1));
    }
    const FString RoleLabel=PC->IsAdmin()?TEXT("管理者"):Inspector?TEXT("観戦"):TEXT("プレイ");
    Text(TEXT("MRFPS  /  ")+RoleLabel,30,InfoTop+8,19,FLinearColor(.5,.8,1));
    FString Phase=TEXT("準備中");
    if(GS) Phase=GS->bCombatPaused?TEXT("一時停止"):GS->Phase==EMRFPSMatchPhase::Playing?TEXT("対戦中"):
        GS->Phase==EMRFPSMatchPhase::BetweenRounds?TEXT("ラウンド間"):
        GS->Phase==EMRFPSMatchPhase::Finished?(GS->WinnerSlot<0?TEXT("引き分け"):FString::Printf(TEXT("P%d の勝利"),GS->WinnerSlot+1)):TEXT("待機中");
    Text(Phase,30,InfoTop+36,15);
    for(auto* P:PC->GetPlayers()) {
        const auto* PS=P->GetPlayerState<AMRFPSPlayerState>();const float X=30+(P->GetSlot()*120);
        Text(FString::Printf(TEXT("P%d  %d"),P->GetSlot()+1,PS?PS->Kills:0),X,InfoTop+62,12);
        DrawRect(FLinearColor(.08,.12,.18),X,InfoTop+85,104,5);DrawRect(FLinearColor(.2,.9,.55),X,InfoTop+85,104*FMath::Clamp(P->Health/100,0.f,1.f),5);
        DrawRect(FLinearColor(.16,.55,1),X,InfoTop+94,104*FMath::Clamp(P->ShieldEnergy/100,0.f,1.f),3);
    }
    const auto* HUDPawn=PC->GetPawn<AMRFPSPawn>();
    const bool DesktopPlayer=!Inspector && HUDPawn && !HUDPawn->IsXR();
    if (GS && (Inspector || DesktopPlayer)) {
        const float Center=Canvas->SizeX*.5f;
        const auto Panel=Inspector?InspectorLayout.RoundPanel():FMRFPSInspectorHudRect{Center-150,18,300,66};
        const bool MissingScan=DesktopPlayer && GS->bSoloBotSession && !GS->bSoloGeometryReady;
        DrawRect(FLinearColor(.008f,.025f,.045f,.78f),Panel.X,Panel.Y,Panel.Width,MissingScan?88:Panel.Height);
        const auto Centered=[this,Center](const FString& Value,float Y,int32 Size,FLinearColor Color) {
            FCanvasTextItem Item(FVector2D(Center,Y),FText::FromString(Value),FSlateFontInfo(FPaths::EngineContentDir()/TEXT("Slate/Fonts/DroidSansFallback.ttf"),Size),Color);
            Item.Font=GEngine?GEngine->GetSmallFont():nullptr; Item.bCentreX=true;
            Item.EnableShadow(FLinearColor(0,0,0,.6)); Canvas->DrawItem(Item);
        };
        Centered(GS->RoundHeaderText(),24,20,FLinearColor(.75f,1.f,.96f));
        Centered(GS->RoundScoreText(Inspector?-1:HUDPawn->GetSlot()),52,16,FLinearColor(.55f,.8f,.9f));
        if (MissingScan) Centered(TEXT("障害物スキャン未受信"),75,13,FLinearColor(1.f,.68f,.2f));
    }
    if (DesktopPlayer) {
        const auto* P=HUDPawn;
        const float W=FMath::Min(440.f,FMath::Max(120.f,float(Canvas->SizeX)-32.f));
        const float X=(Canvas->SizeX-W)*.5f,Y=FMath::Max(16.f,FMath::Min(Canvas->SizeY*.82f,float(Canvas->SizeY)-86.f));
        const FLinearColor Edge(.19f,.82f,1.f,.88f);
        DrawRect(FLinearColor(.008f,.025f,.045f,.78f),X,Y,W,76);
        DrawLine(X+3,Y+14,X+14,Y+3,Edge,1.5f);DrawLine(X+14,Y+3,X+W-14,Y+3,Edge,1.5f);
        DrawLine(X+W-14,Y+3,X+W-3,Y+14,Edge,1.5f);DrawLine(X+W-3,Y+14,X+W-3,Y+62,Edge,1.5f);
        DrawLine(X+W-3,Y+62,X+W-14,Y+73,Edge,1.5f);DrawLine(X+W-14,Y+73,X+14,Y+73,Edge,1.5f);
        DrawLine(X+14,Y+73,X+3,Y+62,Edge,1.5f);DrawLine(X+3,Y+62,X+3,Y+14,Edge,1.5f);
        DrawLine(X+17,Y+10,X+52,Y+10,FLinearColor(.65f,.95f,1.f),2.f);
        DrawLine(X+W-52,Y+10,X+W-17,Y+10,FLinearColor(.65f,.95f,1.f),2.f);
        Text(FString::Printf(TEXT("HP  %.0f"),P->Health),X+18,Y+20,21,FLinearColor(.75f,1.f,.96f));
        const float BarW=FMath::Max(0.f,W*.46f-20),Health=FMath::Clamp(P->Health/100.f,0.f,1.f);
        DrawRect(FLinearColor(.12f,.27f,.34f,.8f),X+18,Y+57,BarW,4);
        DrawRect(Health<=.25f?FLinearColor(1.f,.3f,.2f):FLinearColor(.25f,.95f,.84f),X+18,Y+57,BarW*Health,4);
        Text(TEXT("MR ピストル"),X+W*.60f,Y+16,13,FLinearColor(.55f,.8f,.9f));
        Text(FString::Printf(TEXT("%d  /  ∞"),P->MagazineRounds),X+W*.60f,Y+36,22,FLinearColor(.95f,1.f,1.f));
    }
    if(Inspector&&PC->bMultiView&&!PC->IsAdminWallPlacement()) for(int32 I=0;I<2;++I) {
        const float W=FMath::Min(320.f,(Canvas->SizeX-64)/2),H=W*9/16,X=24+I*(W+16),Y=Canvas->SizeY-H-80;
        DrawRect(FLinearColor(.025,.045,.08,.95),X-3,Y-26,W+6,H+29);
        Text(FString::Printf(TEXT("P%d  %s"),I+1,PC->bReconstructedViews?TEXT("立体視点"):TEXT("映像")),X+8,Y-23,13,FLinearColor(.5,.8,1));
        if(PC->bReconstructedViews) { if(PC->ViewTextures.IsValidIndex(I)) DrawTexture(PC->ViewTextures[I],X,Y,W,H,0,0,1,1,FLinearColor::White,BLEND_Opaque); }
        else { auto* Frame=PC->GetQuestVideoTexture(I);Video(Frame,X,Y,W,H);if(!Frame) Text(TEXT("共有待ち"),X+12,Y+H*.45,14); }
    }
    if(PC->IsAdminWallPlacement()) {
        const float Left=Canvas->SizeX*.5f-250,Top=Canvas->SizeY-92;
        DrawRect(FLinearColor(.025,.045,.08,.95),Left,Top,500,70);
        Text(PC->SelectedWallHeightCm<0?TEXT("木箱   Del 削除"):
            PC->SelectedWallHeightCm>0?FString::Printf(TEXT("壁 %.1f m   ホイール / ↑↓ 高さ"),PC->SelectedWallHeightCm/100.f):TEXT("壁   R 回転   ホイール / ↑↓ 高さ"),Left+12,Top+8,15,FLinearColor(.7,.9,1));
        Text(TEXT("クリック 設置 / 穴・復元   Del 削除   右クリック 終了"),Left+12,Top+39,13);
    } else if(Inspector) Text(PC->IsAdmin()?TEXT("Esc マウス  B 建築  F2 管理"):TEXT("Esc マウス  1 / 2 視点  Tab 自由"),24,Canvas->SizeY-35,13,FLinearColor(.65,.78,.9));
    else if(auto* Pawn=PC->GetPawn<AMRFPSPawn>();Pawn&&!Pawn->bCalibrated) Text(TEXT("C / Y で位置調整"),30,133,16);
    const bool CompactInspector=Inspector && InspectorLayout.bCompact;
    if(PC->IsAdmin()) Text(PC->AdminFeedback(),30,CompactInspector?Canvas->SizeY-74:138,15,FLinearColor(.7,.95,.85));
    if(!PC->ConnectionError.IsEmpty()) Text(TEXT("接続を確認してください"),30,CompactInspector?Canvas->SizeY-54:165,15,FLinearColor(1,.4,.4));
    if (auto* Instance=PC->GetGameInstance<UMRFPSGameInstance>()) {
        const auto* LocalPawn=PC->GetPawn<AMRFPSPawn>();
        const bool XRPlayer=LocalPawn && LocalPawn->IsXR();
        if (!XRPlayer && Instance->ConnectionStatus.ShowSuccess()) {
            Text(TEXT("サーバー接続成功"),Canvas->SizeX*.5f-90.f,110.f,18,FLinearColor(.55f,1.f,.8f));
            Instance->ConnectionStatus.AdvanceVisible(FApp::GetDeltaTime(),FApp::HasFocus(),Canvas->SizeX>0 && Canvas->SizeY>0);
        }
    }
}

bool AMRFPSGameSession::AtCapacity(bool /*bSpectator*/)
{
    // AGameSession's generic count treats all of our free-camera inspectors as
    // players. Real player/admin limits are checked by AMRFPSGameMode after
    // the signed role is verified; observers have no application-level cap.
    return false;
}

AMRFPSGameMode::AMRFPSGameMode()
{
    PrimaryActorTick.bCanEverTick=true;
    PlayerControllerClass=AMRFPSPlayerController::StaticClass(); PlayerStateClass=AMRFPSPlayerState::StaticClass();
    GameStateClass=AMRFPSGameState::StaticClass(); DefaultPawnClass=AMRFPSPawn::StaticClass(); HUDClass=AMRFPSHUD::StaticClass();
    GameSessionClass=AMRFPSGameSession::StaticClass();
}
AMRFPSGameMode::~AMRFPSGameMode()=default;
void AMRFPSGameMode::BeginPlay()
{
    Super::BeginPlay();
    if (GetNetMode()!=NM_Standalone) {
        AuthVerifier=MakeUnique<FMRFPSAuthVerifier>();
        FString SafeError,Fingerprint,ConfiguredComponent;
        GConfig->GetString(TEXT("PacketHandlerComponents"),TEXT("EncryptionComponent"),ConfiguredComponent,GEngineIni);
        const auto* EncryptionCvar=IConsoleManager::Get().FindConsoleVariable(TEXT("net.AllowEncryption"));
        const auto* PskCvar=IConsoleManager::Get().FindConsoleVariable(TEXT("DTLS.PreSharedKeys"));
        const bool TransportConfig=ConfiguredComponent==TEXT("MRFPSTransport") && EncryptionCvar && EncryptionCvar->GetInt()==2 && PskCvar && PskCvar->GetInt()==0;
        bNetworkAdmissionReady=TransportConfig && AuthVerifier->InitializeFromEnvironment(SafeError) &&
            FMRFPSTransportModule::EnsureServerCertificate(Fingerprint,SafeError) && AuthVerifier->SetExpectedServerFingerprint(Fingerprint,SafeError);
        if (!bNetworkAdmissionReady) {
            UE_LOG(LogMRFPS,Error,TEXT("MRFPS_AUTH_STARTUP_FAILED code=%s"),SafeError.IsEmpty()?TEXT("AUTH_TRANSPORT_CONFIG"):*SafeError);
            FPlatformMisc::RequestExitWithStatus(true,78); return;
        }
        UE_LOG(LogMRFPS,Display,TEXT("MRFPS_AUTH_TRANSPORT_READY fingerprint=%s"),*Fingerprint);
    }
    bAllowDesktop=!FParse::Param(FCommandLine::Get(),TEXT("MRFPSRequireXR"));
    if (auto* GS=GetGameState<AMRFPSGameState>()) {
        GS->Layout.WidthCm=ArenaWidthCm; GS->Layout.LengthCm=ArenaLengthCm;
        if (!GS->Layout.IsSane()) { UE_LOG(LogMRFPS,Error,TEXT("MRFPS_LAYOUT_CONFIG_INVALID")); FPlatformMisc::RequestExitWithStatus(true,78); return; }
    }
    MaxAdminWalls=FMath::Clamp(MaxAdminWalls,1,32);
    GetWorld()->SpawnActor<AMRFPSArena>();
    SpawnDefaultStage();
    UE_LOG(LogMRFPS,Display,TEXT("MRFPS_SERVER_READY engine=5.8 widthcm=%.0f lengthcm=%.0f players=2 admins=1 inspectors=uncapped desktop=%d adminstart=%d"),ArenaWidthCm,ArenaLengthCm,bAllowDesktop,bRequireAdminStart);
#if PLATFORM_LINUX && UE_SERVER
    if (GetNetMode()==NM_DedicatedServer) {
        HealthResponder=MakeUnique<FMRFPSHealthResponder>();
        if (!HealthResponder->Start()) UE_LOG(LogMRFPS,Warning,TEXT("MRFPS_HEALTH_LISTENER_UNAVAILABLE"));
        RosterPublisher=MakeShared<FMRFPSRosterPublisher>();
        if (!RosterPublisher->Start()) { RosterPublisher.Reset(); UE_LOG(LogMRFPS,Warning,TEXT("MRFPS_ROSTER_PUBLISH_DISABLED")); }
    }
#endif
}
void AMRFPSGameMode::EndPlay(const EEndPlayReason::Type Reason)
{
    HealthResponder.Reset();
    RosterPublisher.Reset();
    Super::EndPlay(Reason);
}
int32 AMRFPSGameMode::CountFighters(const AMRFPSPlayerState* Exclude) const
{
    int32 N=0; if (GameState) for (APlayerState* Base:GameState->PlayerArray) if (const auto* PS=Cast<AMRFPSPlayerState>(Base); PS && PS!=Exclude && !PS->bInspector && PS->Slot>=0) ++N; return N;
}
int32 AMRFPSGameMode::CountAdmins(const AMRFPSPlayerState* Exclude) const
{
    int32 N=0; if (GameState) for (APlayerState* Base:GameState->PlayerArray) if (const auto* PS=Cast<AMRFPSPlayerState>(Base); PS && PS!=Exclude && PS->bAdmin) ++N; return N;
}
FString AMRFPSGameMode::RoleCapacityError(const FString& RequestedRole,const AMRFPSPlayerState* Candidate) const
{
    if (RequestedRole==TEXT("admin") && CountAdmins(Candidate)>=1) return TEXT("ADMIN_FULL");
    if (RequestedRole==TEXT("player") && MRFPSBot::CountHumanFighters(this,Candidate)>=2) return TEXT("ARENA_FULL");
    return FString();
}
void AMRFPSGameMode::PreLogin(const FString& Options,const FString& Address,const FUniqueNetIdRepl& ID,FString& Error)
{
    const FString ExpectedRole=MRFPS::RequestedRole(Options);
    FMRFPSAuthClaims Claims;
    if (!bNetworkAdmissionReady || !AuthVerifier || !AuthVerifier->VerifyOnly(MRFPS::LoginToken(Options),ExpectedRole,Claims,Error)) {
        if (Error.IsEmpty()) Error=TEXT("AUTH_CONFIG");
        UE_LOG(LogMRFPS,Warning,TEXT("MRFPS_AUTH_REJECT code=%s role=%s"),*Error,*ExpectedRole); return;
    }
    const FString BaseOptions=Options.Replace(TEXT("?SpectatorOnly=1"),TEXT("?Inspector=1"),ESearchCase::IgnoreCase);
    Super::PreLogin(BaseOptions,Address,ID,Error); if (!Error.IsEmpty()) return;
    Error=RoleCapacityError(ExpectedRole);
    if (!Error.IsEmpty()) { UE_LOG(LogMRFPS,Warning,TEXT("MRFPS_ROLE_CAPACITY role=%s code=%s"),*ExpectedRole,*Error); return; }
    if (PendingAuthentication.Num()>=128) { Error=TEXT("AUTH_CAPACITY"); return; }
    if (!AuthVerifier->VerifyAndReserve(MRFPS::LoginToken(Options),ExpectedRole,Claims,Error)) {
        UE_LOG(LogMRFPS,Warning,TEXT("MRFPS_AUTH_REJECT code=%s role=%s"),*Error,*ExpectedRole); return;
    }
    PendingAuthentication.Add(Claims.TokenId,Claims);
    PendingAuthenticationDeadlines.Add(Claims.TokenId,GetWorld()->GetTimeSeconds()+30);
}
APlayerController* AMRFPSGameMode::Login(UPlayer* NewPlayer,ENetRole InRemoteRole,const FString& Portal,const FString& Options,const FUniqueNetIdRepl& ID,FString& Error)
{
    if (GetNetMode()!=NM_Standalone) {
        const auto* Connection=Cast<UNetConnection>(NewPlayer);
        if (!bNetworkAdmissionReady || !Connection || !Connection->IsEncryptionEnabled()) {
            Error=TEXT("AUTH_TRANSPORT_REQUIRED"); UE_LOG(LogMRFPS,Warning,TEXT("MRFPS_AUTH_REJECT code=AUTH_TRANSPORT_REQUIRED")); return nullptr;
        }
    }
    return Super::Login(NewPlayer,InRemoteRole,Portal,Options,ID,Error);
}
FString AMRFPSGameMode::InitNewPlayer(APlayerController* PC,const FUniqueNetIdRepl& ID,const FString& Options,const FString& Portal)
{
    FMRFPSAuthClaims Claims;
    const bool Online=GetNetMode()!=NM_Standalone;
    if (Online) {
        FString Error;
        if (!bNetworkAdmissionReady || !AuthVerifier || !AuthVerifier->VerifyOnly(MRFPS::LoginToken(Options),MRFPS::RequestedRole(Options),Claims,Error)) return Error.IsEmpty()?TEXT("AUTH_CONFIG"):Error;
        const auto* Pending=PendingAuthentication.Find(Claims.TokenId);
        if (!Pending || Pending->Subject!=Claims.Subject || Pending->Role!=Claims.Role || Pending->ExpiresAt!=Claims.ExpiresAt) return TEXT("AUTH_RESERVATION_MISSING");
        PendingAuthentication.Remove(Claims.TokenId); PendingAuthenticationDeadlines.Remove(Claims.TokenId);
    }
    const FString BaseOptions=Options.Replace(TEXT("?SpectatorOnly=1"),TEXT("?Inspector=1"),ESearchCase::IgnoreCase);
    FString Error=Super::InitNewPlayer(PC,ID,BaseOptions,Portal); if (!Error.IsEmpty()) return Error;
    auto* PS=PC->GetPlayerState<AMRFPSPlayerState>(); if (!PS) return TEXT("PLAYER_STATE_UNAVAILABLE");
    PS->bAuthenticated=Online;
    PS->AuthSubject=Online?Claims.Subject:TEXT("local-preview"); PS->AuthExpiresAt=Online?Claims.ExpiresAt:0;
    if (Online) PS->SetPlayerName(Claims.Subject);
    PS->bAdmin=Online && Claims.Role==TEXT("admin");
    PS->bInspector=Online ? Claims.Role!=TEXT("player") : MRFPS::IsInspectorOption(Options);
    // A second check handles simultaneous pre-login attempts, excluding the
    // candidate that the engine has already inserted into PlayerArray.
    const FString CapacityRole=PS->bAdmin?TEXT("admin"):PS->bInspector?TEXT("inspector"):TEXT("player");
    Error=RoleCapacityError(CapacityRole,PS);
    if (!Error.IsEmpty()) { UE_LOG(LogMRFPS,Warning,TEXT("MRFPS_ROLE_CAPACITY role=%s code=%s"),*CapacityRole,*Error); return Error; }
    if (Online) UE_LOG(LogMRFPS,Display,TEXT("MRFPS_AUTH_ACCEPT role=%s subject=%s expires=%lld"),*Claims.Role,*Claims.Subject,Claims.ExpiresAt);
    // Inspector is our own pawn role; let UE possess its free camera normally.
    PS->SetIsOnlyASpectator(false); PS->SetIsSpectator(false);
    if (PS->bInspector) {
        PS->Slot=-1;
    }
    else {
        // The network client has passed signature, role, DTLS and capacity
        // checks. Only now may it replace the server-owned bot.
        MRFPSBot::BeforeHumanAdmission(this);
        bool Occupied[2]={false,false};
        for (APlayerState* Base:GameState->PlayerArray) if (const auto* Other=Cast<AMRFPSPlayerState>(Base); Other && Other!=PS && !Other->IsABot() && !Other->bInspector && Other->Slot>=0 && Other->Slot<2) Occupied[Other->Slot]=true;
        PS->Slot=!Occupied[0] ? 0 : !Occupied[1] ? 1 : -1;
        if (PS->Slot<0) return TEXT("ARENA_FULL");
    }
    UE_LOG(LogMRFPS,Display,TEXT("MRFPS_JOIN role=%s slot=%d name=%s"),PS->bAdmin?TEXT("admin"):PS->bInspector?TEXT("inspector"):TEXT("player"),PS->Slot,*PS->GetPlayerName());
    return FString();
}
void AMRFPSGameMode::RestartPlayer(AController* NewPlayer)
{
    if (!NewPlayer || NewPlayer->GetPawn()) return;
    auto* PS=NewPlayer->GetPlayerState<AMRFPSPlayerState>(); if (!PS) return;
    const bool Inspector=PS->bInspector;
    const FVector Pos=Inspector ? FVector(-850,-750,650) : FVector::ZeroVector;
    const FRotator Rot=Inspector ? FRotator(-30,42,0) : FRotator::ZeroRotator;
    FActorSpawnParameters P; P.Owner=NewPlayer; P.SpawnCollisionHandlingOverride=ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    APawn* Pawn=GetWorld()->SpawnActor<APawn>(Inspector ? AMRFPSInspectorPawn::StaticClass() : AMRFPSPawn::StaticClass(),Pos,Rot,P);
    if (!Pawn) return;
    NewPlayer->Possess(Pawn);
    if (auto* PC=Cast<APlayerController>(NewPlayer)) { PC->SetControlRotation(Rot); PC->ClientSetRotation(Rot,true); PC->ClientRestart(Pawn); }
}
void AMRFPSGameMode::Logout(AController* Exiting)
{
    const auto* Departing=Exiting?Exiting->GetPlayerState<AMRFPSPlayerState>():nullptr;
    const bool HumanFighter=Departing && !Departing->IsABot() && !Departing->bInspector && !Departing->bAdmin && Departing->Slot>=0;
    if (Exiting && Exiting->GetPawn()) Exiting->GetPawn()->Destroy();
    Super::Logout(Exiting);
    const auto* CurrentState=GetGameState<AMRFPSGameState>();
    if ((!CurrentState || !CurrentState->bSoloBotSession) && !RegisteredAnchorsValid()) InvalidateSharedFloor();
    if (HumanFighter) ResetForFighterChange();
    if (auto* GS=GetGameState<AMRFPSGameState>(); GS && CountFighters()<2) GS->Phase=EMRFPSMatchPhase::Waiting;
}
void AMRFPSGameMode::OnElimination(AMRFPSPawn* Victim,AMRFPSPawn* Killer)
{
    if (!Victim || !Killer) return;
    const auto* GS=GetGameState<AMRFPSGameState>();
    if (!GS || GS->Phase!=EMRFPSMatchPhase::Playing || GS->bCombatPaused ||
        (GS->RoundEndsAt>0 && GetWorld()->GetTimeSeconds()>=GS->RoundEndsAt)) return;
    auto* VPS=Victim->GetPlayerState<AMRFPSPlayerState>(); auto* KPS=Killer->GetPlayerState<AMRFPSPlayerState>();
    if (VPS) ++VPS->Deaths;
    if (KPS) ++KPS->Kills;
    UE_LOG(LogMRFPS,Display,TEXT("MRFPS_ELIMINATION winner=%d victim=%d kills=%d"),Killer->GetSlot(),Victim->GetSlot(),KPS?KPS->Kills:-1);
    if (VPS) VPS->ForceNetUpdate();
    if (KPS) KPS->ForceNetUpdate();
}
void AMRFPSGameMode::ResetMatch()
{
    SetCombatPaused(false);
    auto* GS=GetGameState<AMRFPSGameState>(); if (!GS) return;
    GS->RoundNumber=GS->CompletedRounds=GS->RoundWinsSlot0=GS->RoundWinsSlot1=0;
    GS->LastRoundWinnerSlot=GS->WinnerSlot=-1; GS->RoundEndsAt=GS->RoundPausedAt=GS->RestartAt=0;
    BeginRound();
    UE_LOG(LogMRFPS,Display,TEXT("MRFPS_MATCH playing"));
}
void AMRFPSGameMode::Tick(float Dt)
{
    Super::Tick(Dt);
    if (HealthResponder) {
        UIpNetDriver* Driver=GetWorld()?Cast<UIpNetDriver>(GetWorld()->GetNetDriver()):nullptr;
        FSocket* GameSocket=Driver && Driver->IsNetResourceValid()?Driver->GetSocket():nullptr;
        ISocketSubsystem* Sockets=Driver?Driver->GetSocketSubsystem():nullptr;
        int32 Port=0;
        if (GameSocket && Sockets) {
            auto ActualAddress=Sockets->CreateInternetAddr();
            ActualAddress->SetPort(0);
            GameSocket->GetAddress(*ActualAddress);
            Port=ActualAddress->GetPort();
        }
        HealthResponder->Tick(bNetworkAdmissionReady && AuthVerifier.Get()!=nullptr && GameSocket!=nullptr,
                              Port,static_cast<std::uint64_t>(FPlatformTime::Seconds()*1000.0));
    }
    for (FConstPlayerControllerIterator It=GetWorld()->GetPlayerControllerIterator();It;++It)
        if (auto* PC=Cast<AMRFPSPlayerController>(It->Get())) { PC->ServiceVideoRelay(); PC->ServiceGeometryRelay(); }
    const double Now=GetWorld()->GetTimeSeconds();
    if (Now>=NextAuthenticationCheck) {
        NextAuthenticationCheck=Now+.5;
        for (auto It=PendingAuthenticationDeadlines.CreateIterator();It;++It) if (Now>=It.Value()) { PendingAuthentication.Remove(It.Key()); It.RemoveCurrent(); }
        TArray<APlayerController*> Expired;
        const int64 Utc=FDateTime::UtcNow().ToUnixTimestamp();
        for (FConstPlayerControllerIterator It=GetWorld()->GetPlayerControllerIterator();It;++It) if (auto* PC=It->Get()) {
            if (auto* PS=PC->GetPlayerState<AMRFPSPlayerState>()) {
                PS->RefreshRosterPing(FPlatformTime::Seconds());
                if (PS->bAuthenticated && Utc>=PS->AuthExpiresAt) { PS->bAuthenticated=false; PS->ForceNetUpdate(); Expired.Add(PC); }
            }
        }
        for (auto* PC:Expired) if (GameSession) { UE_LOG(LogMRFPS,Display,TEXT("MRFPS_AUTH_SESSION_EXPIRED")); GameSession->KickPlayer(PC,FText::FromString(TEXT("AUTH_EXPIRED"))); }
    }
    if (RosterPublisher && bNetworkAdmissionReady) {
        UIpNetDriver* Driver=GetWorld()?Cast<UIpNetDriver>(GetWorld()->GetNetDriver()):nullptr;
        if (Driver && Driver->IsNetResourceValid() && Driver->GetSocket()) RosterPublisher->Tick(GetWorld(),FPlatformTime::Seconds());
    }
    MRFPSBot::Maintain(this,Dt);
    auto* GS=GetGameState<AMRFPSGameState>(); if (!GS) return;
    if (GS->bSharedFloorConfirmed && !RegisteredAnchorsValid()) InvalidateSharedFloor();
    TickRounds();
    // Administrator control never pauses network, tracking, video, geometry,
    // or authentication checks above. Only match progression waits here.
    if (bRequireAdminStart || GS->bCombatPaused) return;
    int32 Ready=0; bool Nominal=true;
    for (TActorIterator<AMRFPSPawn> It(GetWorld());It;++It) if (const auto* PS=It->GetPlayerState<AMRFPSPlayerState>(); PS && PS->bAuthenticated && It->GetSlot()>=0 && It->bCalibrated && It->IsInsideArena() && It->IsPoseFresh()) { ++Ready; Nominal=Nominal&&It->Pose.bDesktop; }
    if (!Nominal && !(GS->bSharedFloorConfirmed && GS->Layout.bCalibrated)) return;
    if (Ready==2) GS->bNominalDesktopSession=Nominal;
    if (GS->Phase==EMRFPSMatchPhase::Waiting && Ready==2 && CountFighters()==2) ResetMatch();
}

void UMRFPSGameInstance::Init()
{
    FNetworkVersion::SetGameNetworkProtocolVersion(14);
    Super::Init();
#if !UE_SERVER && (PLATFORM_WINDOWS || PLATFORM_MAC || PLATFORM_LINUX)
    // This CVar is marked Cheat in UE 5.8, so setting it in Engine.ini causes
    // cook to fail. Set it in the desktop app instead, including direct launch.
    if (auto* Idle=IConsoleManager::Get().FindConsoleVariable(TEXT("t.IdleWhenNotForeground"))) Idle->Set(0,ECVF_SetByCode);
#endif
    if (GEngine) {
        GEngine->OnNetworkFailure().AddUObject(this,&UMRFPSGameInstance::NetworkFailure);
        GEngine->OnTravelFailure().AddUObject(this,&UMRFPSGameInstance::TravelFailure);
    }
}
void UMRFPSGameInstance::OnStart()
{
    Super::OnStart(); FString Address,DeviceCodeFile;
    if (FParse::Value(FCommandLine::Get(),TEXT("MRFPSDeviceCodeFile="),DeviceCodeFile)) {
        FString Role=TEXT("player"); FParse::Value(FCommandLine::Get(),TEXT("MRFPSRole="),Role);
        FParse::Value(FCommandLine::Get(),TEXT("MRFPSServer="),Address);
        FTimerHandle Handle; GetWorld()->GetTimerManager().SetTimer(Handle,FTimerDelegate::CreateWeakLambda(this,[this,Address,DeviceCodeFile,Role]() {
            if (auto* PC=Cast<AMRFPSPlayerController>(GetFirstLocalPlayerController())) {
                PC->OpenDeviceLogin(); if (!Address.IsEmpty()) PC->MRFPSConfigureServer(Address); PC->MRFPSLoginFile(DeviceCodeFile,Role);
            }
        }),1.f,false);
    } else if (FParse::Value(FCommandLine::Get(),TEXT("MRFPSServer="),Address)) {
        const bool Inspector=FParse::Param(FCommandLine::Get(),TEXT("MRFPSInspector"));
        const bool Admin=FParse::Param(FCommandLine::Get(),TEXT("MRFPSAdmin"));
        FTimerHandle Handle; GetWorld()->GetTimerManager().SetTimer(Handle,FTimerDelegate::CreateWeakLambda(this,[this,Address,Inspector,Admin]() {
            if (auto* PC=Cast<AMRFPSPlayerController>(GetFirstLocalPlayerController())) {
                if (Admin) PC->MRFPSConnectAdmin(Address); else PC->MRFPSConnect(Address,Inspector);
            }
        }),.2f,false);
    }
}
void UMRFPSGameInstance::NetworkFailure(UWorld* World,UNetDriver* Driver,ENetworkFailure::Type Type,const FString& Error)
{
    if (World && World->GetGameInstance()!=this) return;
    if (!World && Driver && Driver->GetWorld() && Driver->GetWorld()->GetGameInstance()!=this) return;
    if (auto* Receiver=GetSubsystem<UMRFPSVideoReceiver>()) Receiver->ResetAll();
#if !UE_SERVER
    if (auto* Capture=GetSubsystem<UMRFPSViewCaptureSubsystem>()) Capture->StopCapture();
    if (auto* Geometry=GetSubsystem<UMRFPSGeometryCaptureSubsystem>()) Geometry->StopCapture();
#endif
    UE_LOG(LogMRFPS,Error,TEXT("MRFPS_NETWORK_FAILURE %s"),*Error);
    FString ExplicitTokenFile;
    const bool bHasExplicitTokenFile=FParse::Value(FCommandLine::Get(),TEXT("MRFPSAuthTokenFile="),ExplicitTokenFile) && !ExplicitTokenFile.IsEmpty();
    const auto Reason=Type==ENetworkFailure::ConnectionTimeout?MRFPSConnectionStatus::EFailure::Timeout:
        Type==ENetworkFailure::OutdatedClient?MRFPSConnectionStatus::EFailure::Version:MRFPS::FailureKind(Error);
    SetConnectionFailure(Reason);
    PrepareForNetworkFailure(bHasExplicitTokenFile);
    if (auto* PC=Cast<AMRFPSPlayerController>(GetFirstLocalPlayerController())) {
        PC->HandleSessionDisconnected();
        PC->ConnectionError=TEXT("接続が切れました。新しい認証情報で再接続してください");
    }
}
void UMRFPSGameInstance::TravelFailure(UWorld* World,ETravelFailure::Type /*Type*/,const FString& /*Error*/)
{
    if (!World || World->GetGameInstance()!=this) return;
    // UE may report both a network and travel failure for one attempt. Keep
    // the more specific first reason and never expose the raw travel URL.
    if (ConnectionStatus.Phase!=MRFPSConnectionStatus::EPhase::Failure)
        SetConnectionFailure(MRFPSConnectionStatus::EFailure::Connection);
    FString ExplicitTokenFile;
    const bool HasTokenFile=FParse::Value(FCommandLine::Get(),TEXT("MRFPSAuthTokenFile="),ExplicitTokenFile) && !ExplicitTokenFile.IsEmpty();
    PrepareForNetworkFailure(HasTokenFile);
}
void UMRFPSGameInstance::RememberConnectionAttempt(const FString& Address,const FString& Role,UWorld* OriginWorld)
{
    LastServerAddress=Address; LastServerRole=Role;
    ConnectionOriginWorld=OriginWorld; bConnectionOriginKnown=OriginWorld!=nullptr;
    bLastConnectionFromDevice=bNextConnectionFromDevice; bNextConnectionFromDevice=false;
    bReconnectPending=false;
    ConnectionStatus.Begin(MRFPS::ConnectionRole(Role));
}
bool UMRFPSGameInstance::IsNewConnectionWorld(const UWorld* World) const
{
    // Even if the old world has been garbage-collected, its weak identity is
    // distinct from the new one. Never accept the old live connection.
    return bConnectionOriginKnown && World && ConnectionOriginWorld.Get()!=World;
}
void UMRFPSGameInstance::SetConnectionFailure(MRFPSConnectionStatus::EFailure Reason)
{
    if (ConnectionStatus.Phase==MRFPSConnectionStatus::EPhase::Failure &&
        Reason==MRFPSConnectionStatus::EFailure::Connection &&
        ConnectionStatus.Failure!=MRFPSConnectionStatus::EFailure::Connection) return;
    ConnectionStatus.Fail(Reason);
}
FString UMRFPSGameInstance::ConnectionFailureMessage() const
{
    using MRFPSConnectionStatus::EFailure;
    switch (ConnectionStatus.Failure) {
    case EFailure::Timeout: return TEXT("接続がタイムアウトしました。接続先と新しいコードを確認");
    case EFailure::Version: return TEXT("アプリのバージョンが一致しません");
    case EFailure::Capacity: return TEXT("定員に達しています。新しいコードで再試行");
    case EFailure::Authentication: return TEXT("認証できません。新しいコードで再試行");
    case EFailure::Connection: return TEXT("接続できません。接続先と新しいコードを確認");
    default: return FString();
    }
}
void UMRFPSGameInstance::PrepareForNetworkFailure(bool bHasExplicitTokenFile)
{
    // Never replace an explicit user request to open the login screen. A
    // device-code session cannot refresh a single-use jti without a new code.
    const bool FileReconnect=!bShowLoginAfterFailure && !bLastConnectionFromDevice && bHasExplicitTokenFile &&
        !LastServerAddress.IsEmpty() && !LastServerRole.IsEmpty();
    bReconnectPending=FileReconnect; bShowLoginAfterFailure=!FileReconnect; NextReconnectCheck=0;
}
void UMRFPSGameInstance::TickAutoReconnect()
{
    if (!bReconnectPending || FPlatformTime::Seconds()<NextReconnectCheck) return;
    NextReconnectCheck=FPlatformTime::Seconds()+2.0;
    auto* PC=Cast<AMRFPSPlayerController>(GetFirstLocalPlayerController());
    if (!PC || PC->GetNetMode()!=NM_Standalone) return;
    auto* Credentials=GetSubsystem<UMRFPSAuthClientSubsystem>(); FString Error;
    if (!Credentials || !Credentials->ReloadTokenFile(Error) || !Credentials->ValidateForNewConnection(LastServerRole,Error)) return;
    bReconnectPending=false;
    UE_LOG(LogMRFPS,Display,TEXT("MRFPS_RECONNECT_FRESH_TOKEN"));
    if (LastServerRole==TEXT("admin")) PC->MRFPSConnectAdmin(LastServerAddress);
    else PC->MRFPSConnect(LastServerAddress,LastServerRole==TEXT("inspector"));
}
void UMRFPSGameInstance::ReconnectWithFreshToken()
{
    auto* PC=Cast<AMRFPSPlayerController>(GetFirstLocalPlayerController());
    if (!PC) return;
    if (LastServerAddress.IsEmpty() || LastServerRole.IsEmpty()) { PC->ConnectionError=TEXT("接続先を指定してください"); return; }
    auto* Credentials=GetSubsystem<UMRFPSAuthClientSubsystem>(); FString Error;
    if (!Credentials || !Credentials->ReloadTokenFile(Error) || !Credentials->ValidateForNewConnection(LastServerRole,Error)) {
        PC->ConnectionError=Error.IsEmpty()?TEXT("AUTH_FRESH_TOKEN_REQUIRED"):Error;
        UE_LOG(LogMRFPS,Warning,TEXT("MRFPS_RECONNECT_WAIT code=%s"),*PC->ConnectionError); return;
    }
    if (LastServerRole==TEXT("admin")) PC->MRFPSConnectAdmin(LastServerAddress);
    else PC->MRFPSConnect(LastServerAddress,LastServerRole==TEXT("inspector"));
}
void UMRFPSGameInstance::ReceivedNetworkEncryptionToken(const FString& Token,const FOnEncryptionKeyResponse& Delegate)
{
    FMRFPSTransportModule::HandleServerEncryptionToken(Token,Delegate);
}
void UMRFPSGameInstance::ReceivedNetworkEncryptionAck(const FOnEncryptionKeyResponse& Delegate)
{
    FString Error;
    auto* Credentials=GetSubsystem<UMRFPSAuthClientSubsystem>();
    if (Credentials && Credentials->HasAccessToken()) {
        const FString Pin=Credentials->GetServerFingerprint();
        if (FMRFPSTransportModule::SetClientServerFingerprint(Pin,Error)) {
            // Record only the nonsecret pin selected for this exact pending
            // connection. The existing delegate still performs the handshake.
            // UE binds this delegate to the specific UPendingNetGame that
            // received the ACK. Do not infer it from a global world context.
            auto* Pending=Cast<UPendingNetGame>(Delegate.GetUObject());
            auto* Driver=Pending?Pending->NetDriver.Get():nullptr;
            if (auto* Connection=Driver?Cast<UMRFPSIpConnection>(Driver->ServerConnection.Get()):nullptr)
                Connection->RecordDiagnosticServerPin(Pin);
        }
    }
    else FMRFPSTransportModule::ClearClientServerFingerprint();
    FMRFPSTransportModule::HandleClientEncryptionAck(Delegate);
}
EEncryptionFailureAction UMRFPSGameInstance::ReceivedNetworkEncryptionFailure(UNetConnection* Connection)
{
    UE_LOG(LogMRFPS,Warning,TEXT("MRFPS_AUTH_REJECT code=AUTH_ENCRYPTION_REQUIRED"));
    return EEncryptionFailureAction::RejectConnection;
}
void AMRFPSPawn::Respawn()
{
    if (!HasAuthority()) return;
    if (Pose.bDesktop) {
        const auto& ArenaLayout=MRFPS::Layout(GetWorld());
        ClientNominalDesktopStart(ArenaLayout.NominalStart(GetSlot()),ArenaLayout.StartRotation(GetSlot()));
    }
    Health=100; ShieldEnergy=100; bShieldExhausted=false; RespawnAt=0;MagazineRounds=MagazineCapacity;
    ServerReload.Reset();ServerRise.Reset();ServerLower.Reset();LastReloadAt=-100;
    ClientFeedback(3,0); ForceNetUpdate();
}
void AMRFPSPawn::ClientFeedback_Implementation(uint8 Kind, float Damage)
{
    if (!IsLocallyControlled() || !Haptics) return;
    if (Kind==0) Haptics->NotifyShotFired();
    else if (Kind==3) Haptics->NotifyRespawn();
    else Haptics->NotifyDamage(Damage,Kind==2);
}
void AMRFPSPawn::MulticastShot_Implementation(FVector_NetQuantize Start, FVector_NetQuantize End, bool bHit)
{
    if (GetNetMode()==NM_DedicatedServer) return;
    AActor* A=GetWorld()->SpawnActor<AActor>();
    auto* C=NewObject<UStaticMeshComponent>(A); A->SetRootComponent(C); A->AddInstanceComponent(C);
    C->SetStaticMesh(LoadObject<UStaticMesh>(nullptr,TEXT("/Engine/BasicShapes/Cylinder.Cylinder")));
    C->SetCollisionEnabled(ECollisionEnabled::NoCollision); C->RegisterComponent(); MRFPS::Material(C,TEXT("Tracer"));
    const FVector Delta=FVector(End)-FVector(Start);
    A->SetActorLocation((FVector(Start)+FVector(End))*.5);
    A->SetActorRotation(FRotationMatrix::MakeFromZ(Delta).Rotator());
    A->SetActorScale3D(FVector(.012,.012,Delta.Size()/100)); A->SetLifeSpan(.065);
}
void AMRFPSPawn::UpdateVisuals()
{
    if (GetSlot()>=0 && VisualTeam!=GetSlot()) {
        VisualTeam=GetSlot();
        const FString TeamPath=VisualTeam==0 ? TEXT("/Game/Materials/M_Blue.M_Blue") : TEXT("/Game/Materials/M_Orange.M_Orange");
        if (auto* Team=LoadObject<UMaterialInterface>(nullptr,*TeamPath)) for (auto* C:{HeadVisual.Get(),BodyVisual.Get(),LeftVisual.Get(),GunVisual.Get()}) {
            if (C->GetStaticMesh() && C->GetStaticMesh()->GetName().StartsWith(TEXT("SM_MR"))) {
                const auto& Slots=C->GetStaticMesh()->GetStaticMaterials();
                for (int32 I=0;I<Slots.Num();++I) if (Slots[I].MaterialSlotName.ToString().Contains(TEXT("Blue")) || Slots[I].MaterialSlotName.ToString().Contains(TEXT("Orange"))) C->SetMaterial(I,Team);
            } else C->SetMaterial(0,Team);
        }
    }
    HeadHitbox->SetWorldLocation(Pose.Head);
    BodyHitbox->SetWorldLocation(FVector(Pose.Head)+FVector(0,0,-58));
    MiniShield->SetWorldLocationAndRotation(FVector(Pose.Left)+Pose.LeftRotation.Vector()*13,Pose.LeftRotation);
    const bool Alive=Health>0;
    const bool Targetable=Alive && IsInsideArena() && (!HasAuthority() || IsPoseFresh());
    HeadHitbox->SetCollisionEnabled(Targetable ? ECollisionEnabled::QueryOnly : ECollisionEnabled::NoCollision);
    BodyHitbox->SetCollisionEnabled(Targetable ? ECollisionEnabled::QueryOnly : ECollisionEnabled::NoCollision);
    MiniShield->SetCollisionEnabled(Targetable && bShieldActive ? ECollisionEnabled::QueryOnly : ECollisionEnabled::NoCollision);
    const auto* VisualState=GetPlayerState<AMRFPSPlayerState>();
    const bool ShowAvatar=MRFPS::ViewerShowsAvatars(GetWorld()) || (GetNetMode()!=NM_DedicatedServer && VisualState && VisualState->IsABot());
    FMRFPSPose RetainedPose;
    const bool HaveRetainedPose=ShowAvatar && Avatar && Avatar->GetVisualPose(RetainedPose);
    const FMRFPSPose& DisplayPose=HaveRetainedPose?RetainedPose:Pose;
    HeadVisual->SetWorldLocationAndRotation(DisplayPose.Head,DisplayPose.HeadRotation);
    BodyVisual->SetWorldLocation(FVector(DisplayPose.Head)+FVector(0,0,-58));
    const FQuat LeftQ=DisplayPose.LeftRotation.Quaternion();
    if (LeftVisual->GetStaticMesh() && LeftVisual->GetStaticMesh()->GetName()==TEXT("SM_MRHand")) {
        LeftVisual->SetWorldLocationAndRotation(FVector(DisplayPose.Left)-LeftQ.GetAxisZ()*8,FRotationMatrix::MakeFromXZ(LeftQ.GetAxisZ(),-LeftQ.GetAxisX()).Rotator());
    } else LeftVisual->SetWorldLocation(DisplayPose.Left);
    const bool CustomGun=GunVisual->GetStaticMesh() && GunVisual->GetStaticMesh()->GetName()==TEXT("SM_MRPistol");
    const auto* LocalPC=IsLocallyControlled()?Cast<AMRFPSPlayerController>(Controller):nullptr;
    const bool LoginGun=IsXR() && LocalPC && LocalPC->DeviceLogin && LocalPC->DeviceLogin->IsOpen();
    if (!LoginGun) {
        const FTransform Gun=MRFPSWeaponPose::Visual(FTransform(DisplayPose.RightRotation,FVector(DisplayPose.Right)),CustomGun);
        GunVisual->SetWorldLocationAndRotation(Gun.GetLocation(),Gun.GetRotation());
        GunVisual->SetVisibility(Alive && DisplayPose.bRightTracked);
    }
    const bool Manny=Avatar && Avatar->IsAvatarReady();
    HeadVisual->SetVisibility(ShowAvatar && !Manny && Alive && DisplayPose.bHeadTracked); BodyVisual->SetVisibility(ShowAvatar && !Manny && Alive && DisplayPose.bHeadTracked);
    LeftVisual->SetVisibility(ShowAvatar && !Manny && Alive && DisplayPose.bLeftTracked);
    ShieldVisual->SetVisibility(Alive && bShieldActive);
    Label->SetWorldLocation(FVector(DisplayPose.Head)+FVector(0,0,35));
    Label->SetText(FText::FromString(FString::Printf(TEXT("P%d"),GetSlot()+1)));
    Label->SetVisibility(ShowAvatar && Alive && DisplayPose.bHeadTracked && GetSlot()>=0);
}
void AMRFPSPawn::Tick(float Dt)
{
    Super::Tick(Dt);
    if (IsLocallyControlled() && !MRFPSBot::IsServerBot(this)) SampleLocalPose(Dt);
    const auto* CombatState=GetWorld()->GetGameState<AMRFPSGameState>();
    if (HasAuthority() && (!CombatState || !CombatState->bCombatPaused)) {
        const double Now=GetWorld()->GetTimeSeconds();
        if (Pose.LeftOpenness<.5 || !Pose.bLeftTracked) bShieldExhausted=false;
        if (ShieldEnergy<=0) bShieldExhausted=true;
        const bool PreviousShield=bShieldActive;
        bShieldActive=CanAct() && Pose.bLeftTracked && Pose.LeftOpenness>.8 && !bShieldExhausted && ShieldEnergy>0;
        if (bShieldActive!=PreviousShield) UE_LOG(LogMRFPS,Display,TEXT("MRFPS_SHIELD slot=%d active=%d"),GetSlot(),bShieldActive);
        if (bShieldActive) { ShieldEnergy=FMath::Max(0.f,ShieldEnergy-18*Dt); LastShieldUse=Now; }
        else if (Now-LastShieldUse>1.0) ShieldEnergy=FMath::Min(100.f,ShieldEnergy+14*Dt);
        if (Health<=0 && Now>=RespawnAt) if (auto* GS=GetWorld()->GetGameState<AMRFPSGameState>(); GS && GS->Phase==EMRFPSMatchPhase::Playing) Respawn();
    }
    const auto* VisualState=GetPlayerState<AMRFPSPlayerState>();
    const bool ShowAvatar=MRFPS::ViewerShowsAvatars(GetWorld()) || (GetNetMode()!=NM_DedicatedServer && VisualState && VisualState->IsABot());
    if (ShowAvatar && Avatar && !Avatar->IsAvatarReady()) Avatar->InitializeAvatar();
    if(Avatar) {
        if (ShowAvatar && Health>0 && Avatar->IsAvatarReady()) Avatar->UpdateTracking(Pose,MRFPS::Layout(GetWorld()),GeometryCalibrationGeneration,Dt,false);
        else Avatar->SetVisibility(false);
    }
    UpdateVisuals();UpdatePlayerHud();
    if (APlayerController* PC=GetWorld()->GetFirstPlayerController(); PC && PC->PlayerCameraManager) {
        Label->SetWorldRotation((PC->PlayerCameraManager->GetCameraLocation()-Label->GetComponentLocation()).Rotation());
    }
    if (!IsLocallyControlled()) {
        FMRFPSPose CameraPose=Pose;
        if (ShowAvatar && Avatar) Avatar->GetVisualPose(CameraPose);
        Camera->bLockToHmd=false; Camera->SetWorldLocationAndRotation(CameraPose.Head,CameraPose.HeadRotation);
    }
    else {
        FString MR=TEXT("Desktop");
#if !UE_SERVER
        if (IsXR()) MR=FMRFPSXRModule::IsPassthroughRunning() ? TEXT("MR passthrough active") : TEXT("PASSTHROUGH UNAVAILABLE");
#endif
        const auto* GS=GetWorld()->GetGameState<AMRFPSGameState>();
        const FString State=!Pose.bHeadTracked ? TEXT("TRACKING LOST - combat disabled") :
            !bLocalReady ? TEXT("Stand at arena centre, face +X, press left Y / glove A") :
            !IsInsideArena() ? TEXT("OUT OF ARENA - return inside boundary") :
            GS && GS->bCombatPaused ? TEXT("COMBAT PAUSED - tracking/video remain live") :
            GS && GS->Phase==EMRFPSMatchPhase::Waiting ? TEXT("Waiting for ADMIN start") :
            Health<=0 ? TEXT("Eliminated - respawning") : TEXT("Left palm: shield | raise left hand: wall");
        HeadsetStatus->SetText(FText::FromString(FString::Printf(TEXT("%s | P%d | HP %.0f | shield %.0f\n%s\n%s\nA: arm calibrated MoBits | B: STOP"),
            *MR,GetSlot()+1,Health,ShieldEnergy,*State,*TrackingDescription)));
    }
}
