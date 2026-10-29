#include "MRFPSGame.h"
#include "Camera/CameraComponent.h"
#include "Engine/World.h"
#include "GameFramework/PlayerState.h"
#if !UE_SERVER
#include "Components/WidgetComponent.h"
#include "Misc/Paths.h"
#include "Rendering/DrawElements.h"
#include "Styling/CoreStyle.h"
#include "Widgets/SLeafWidget.h"
#include "Widgets/SOverlay.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

// Draw the same compact status language in the headset and desktop: a dark
// translucent panel, angular cyan outline and an actual health meter.
class SMRFPSPlayerHudFrame final : public SLeafWidget
{
public:
    SLATE_BEGIN_ARGS(SMRFPSPlayerHudFrame) {}
        SLATE_ARGUMENT(TWeakObjectPtr<AMRFPSPawn>, Pawn)
    SLATE_END_ARGS()

    void Construct(const FArguments& Args) { Pawn=Args._Pawn; }
    virtual FVector2D ComputeDesiredSize(float) const override { return FVector2D(600,120); }
    virtual int32 OnPaint(const FPaintArgs&,const FGeometry& Geometry,const FSlateRect&,
        FSlateWindowElementList& Elements,int32 Layer,const FWidgetStyle&,bool) const override
    {
        const FVector2D Size=Geometry.GetLocalSize();
        const float W=float(Size.X),H=float(Size.Y);
        const FLinearColor Edge(.19f,.82f,1.f,.88f);
        FSlateDrawElement::MakeBox(Elements,Layer,Geometry.ToPaintGeometry(),
            FCoreStyle::Get().GetBrush("WhiteBrush"),ESlateDrawEffect::None,FLinearColor(.008f,.025f,.045f,.78f));
        const auto Line=[&](TArray<FVector2f> Points,FLinearColor Color,float Thickness) {
            FSlateDrawElement::MakeLines(Elements,Layer+1,Geometry.ToPaintGeometry(),MoveTemp(Points),
                ESlateDrawEffect::None,Color,true,Thickness);
        };
        Line({{3,17},{17,3},{W-17,3},{W-3,17},{W-3,H-17},{W-17,H-3},{17,H-3},{3,H-17},{3,17}},Edge,1.5f);
        Line({{18,13},{66,13}},FLinearColor(.65f,.95f,1.f,1.f),3.f);
        Line({{W-66,13},{W-18,13}},FLinearColor(.65f,.95f,1.f,1.f),3.f);
        Line({{20,H-20},{W*.46f,H-20}},FLinearColor(.12f,.27f,.34f,.8f),5.f);
        const float Health=Pawn.IsValid()?FMath::Clamp(Pawn->Health/100.f,0.f,1.f):0.f;
        if (Health>0) Line({{20,H-20},{20+(W*.46f-20)*Health,H-20}},
            Health<=.25f?FLinearColor(1.f,.3f,.2f,1.f):FLinearColor(.25f,.95f,.84f,1.f),5.f);
        return Layer+1;
    }
private:
    TWeakObjectPtr<AMRFPSPawn> Pawn;
};
#endif

DEFINE_LOG_CATEGORY_STATIC(LogMRFPSWeapon,Log,All);

void FMRFPSReloadGesture::Reset() { StartTime=-1; bLatched=false; }

bool FMRFPSReloadGesture::Update(const FVector& Hand,double Time,bool bEligible)
{
    if (!bEligible || Hand.ContainsNaN() || !FMath::IsFinite(Time) ||
        FMath::Abs(Hand.X)>15 || FMath::Abs(Hand.Y)>17 || Hand.Z < -38 || Hand.Z > 8) {
        Reset(); return false;
    }
    if (bLatched) return false;
    if (StartTime<0 || Time<StartTime || Time-StartTime>1.2) { StartTime=Time;StartZ=Hand.Z;return false; }
    if (Time-StartTime>=.08 && FMath::Abs(Hand.Z-StartZ)>=12) { bLatched=true;return true; }
    return false;
}

void AMRFPSPawn::SpawnWeaponAtPose()
{
    if (!HasAuthority() || !Pose.bHeadTracked || !bCalibrated) return;
    const auto* GS=GetWorld()->GetGameState<AMRFPSGameState>();
    const FMRFPSArenaLayout Layout=GS ? GS->Layout : FMRFPSArenaLayout();
    const FRotator Forward(0,Pose.HeadRotation.Yaw,0);
    FVector Spawn=FVector(Pose.Head)+Forward.Vector()*55+FVector(0,0,-38);
    FVector Local=Layout.ToLocal(Spawn);
    Local.X=FMath::Clamp(Local.X,-double(Layout.WidthCm)*.5+10,double(Layout.WidthCm)*.5-10);
    Local.Y=FMath::Clamp(Local.Y,-double(Layout.LengthCm)*.5+10,double(Layout.LengthCm)*.5-10);
    Spawn=Layout.ToWorld(Local);
    Spawn.Z=FMath::Clamp(Spawn.Z,Layout.Center.Z+50,Layout.Center.Z+220);
    GunRestLocation=Spawn;GunRestRotation=Forward;bGunSpawned=true;
    // The desktop test/player has no hand-grip sensor; it starts equipped.
    bGunHeld=Pose.bDesktop;
    LastGunPosition=Pose.bRightTracked ? FVector(Pose.Right) : Spawn;
    LastGunRotation=Pose.bRightTracked ? Pose.RightRotation : Forward;
    ServerReload.Reset();ForceNetUpdate();
    UE_LOG(LogMRFPSWeapon,Display,TEXT("MRFPS_GUN_SPAWN slot=%d"),GetSlot());
}

void AMRFPSPawn::DropWeapon()
{
    if (!HasAuthority() || !bGunHeld) return;
    GunRestLocation=LastGunPosition;GunRestRotation=LastGunRotation;bGunHeld=false;
    ServerReload.Reset();ForceNetUpdate();
    UE_LOG(LogMRFPSWeapon,Display,TEXT("MRFPS_GUN_RELEASE slot=%d"),GetSlot());
}

void AMRFPSPawn::UpdateWeaponFromPose(double Now)
{
    if (!HasAuthority()) return;
    const auto* PS=GetPlayerState<AMRFPSPlayerState>();
    const bool Player=PS && PS->bAuthenticated && PS->AuthExpiresAt>FDateTime::UtcNow().ToUnixTimestamp() &&
        !PS->bInspector && !PS->bAdmin && PS->Slot>=0 && PS->Slot<2;
    if (!Player || !bCalibrated || Health<=0 || !Pose.bHeadTracked || !IsInsideArena() || !IsPoseFresh()) {
        DropWeapon();ServerReload.Reset();return;
    }
    if (!bGunSpawned) SpawnWeaponAtPose();
    if (!bGunSpawned) return;
    if (Pose.bDesktop) {
        if (Pose.bRightTracked) { LastGunPosition=Pose.Right;LastGunRotation=Pose.RightRotation; }
        ServerReload.Reset();return;
    }
    const bool RightValid=Pose.bRightTracked && Pose.RightFingers.bValid;
    const float Grip=Pose.RightFingers.GripCurl();
    if (bGunHeld) {
        if (!RightValid || Grip<.35f) DropWeapon();
        else { LastGunPosition=Pose.Right;LastGunRotation=Pose.RightRotation; }
    } else if (RightValid && Grip>.60f && FVector::DistSquared(Pose.Right,GunRestLocation)<=FMath::Square(20.)) {
        bGunHeld=true;LastGunPosition=Pose.Right;LastGunRotation=Pose.RightRotation;ForceNetUpdate();
        UE_LOG(LogMRFPSWeapon,Display,TEXT("MRFPS_GUN_GRAB slot=%d"),GetSlot());
    }
    const auto* GS=GetWorld()->GetGameState<AMRFPSGameState>();
    const bool Eligible=bGunHeld && RightValid && Pose.bLeftTracked && Pose.LeftFingers.bValid &&
        GS && !GS->bCombatPaused && GS->Phase!=EMRFPSMatchPhase::Finished && MagazineRounds<MagazineCapacity;
    const FVector Relative=Pose.RightRotation.UnrotateVector(FVector(Pose.Left)-FVector(Pose.Right));
    if (ServerReload.Update(Relative,Now,Eligible) && Now-LastReloadAt>=.5) {
        MagazineRounds=MagazineCapacity;LastReloadAt=Now;ForceNetUpdate();
        UE_LOG(LogMRFPSWeapon,Display,TEXT("MRFPS_GUN_RELOAD slot=%d rounds=%d"),GetSlot(),MagazineRounds);
    }
}

void AMRFPSPawn::DesktopReload() { if (!IsXR()) ServerDesktopReload(); }
void AMRFPSPawn::ServerDesktopReload_Implementation()
{
    const double Now=GetWorld()->GetTimeSeconds();
    if (!HasAuthority() || !Pose.bDesktop || !CanAct() || !bGunHeld || Now-LastReloadAt<.5) return;
    MagazineRounds=MagazineCapacity;LastReloadAt=Now;ForceNetUpdate();
}

void AMRFPSPawn::UpdatePlayerHud()
{
#if !UE_SERVER
    if (!PlayerHud) return;
    const bool Show=IsLocallyControlled() && IsXR();
    PlayerHud->SetVisibility(Show);
    if (!Show || PlayerHudSlate.IsValid()) return;
    const TWeakObjectPtr<AMRFPSPawn> Weak(this);
    const auto Font=[](int32 Size) { return FSlateFontInfo(FPaths::EngineContentDir()/TEXT("Slate/Fonts/DroidSansFallback.ttf"),Size); };
    PlayerHudSlate=SNew(SOverlay)
        +SOverlay::Slot()[ SNew(SMRFPSPlayerHudFrame).Pawn(Weak) ]
        +SOverlay::Slot().Padding(FMargin(22,22,22,25))
        [ SNew(SHorizontalBox)
          +SHorizontalBox::Slot().FillWidth(.48f).VAlign(VAlign_Center)
          [ SNew(STextBlock).Font(Font(28)).ColorAndOpacity(FSlateColor(FLinearColor(.75f,1.f,.96f)))
            .Text_Lambda([Weak]() { return FText::FromString(Weak.IsValid() ?
                FString::Printf(TEXT("HP  %.0f"),Weak->Health) : TEXT("")); }) ]
          +SHorizontalBox::Slot().FillWidth(.52f).HAlign(HAlign_Right).VAlign(VAlign_Center)
          [ SNew(SVerticalBox)
            +SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Right)
            [ SNew(STextBlock).Font(Font(18)).ColorAndOpacity(FSlateColor(FLinearColor(.55f,.8f,.9f)))
              .Text(FText::FromString(TEXT("MR ピストル"))) ]
            +SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Right)
            [ SNew(STextBlock).Font(Font(28)).ColorAndOpacity(FSlateColor(FLinearColor(.95f,1.f,1.f)))
              .Text_Lambda([Weak]() { return FText::FromString(Weak.IsValid() ?
                  FString::Printf(TEXT("%d  /  ∞"),Weak->MagazineRounds) : TEXT("")); }) ]
          ]
        ];
    PlayerHud->SetSlateWidget(PlayerHudSlate);
#endif
}
