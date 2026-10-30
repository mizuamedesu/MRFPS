#include "MRFPSGame.h"
#include "MRFPSBot.h"
#include "Camera/CameraComponent.h"
#include "Engine/World.h"
#include "GameFramework/PlayerState.h"
#if !UE_SERVER
#include "MRFPSViewCaptureSubsystem.h"
#include "Engine/GameInstance.h"
#include "Components/WidgetComponent.h"
#include "Misc/Paths.h"
#include "Misc/App.h"
#include "Rendering/DrawElements.h"
#include "Styling/CoreStyle.h"
#include "Widgets/SLeafWidget.h"
#include "Widgets/SOverlay.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SBorder.h"
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

void AMRFPSPawn::UpdateWeaponFromPose(double Now)
{
    if (!HasAuthority()) return;
    if (!CanAct()) {
        ServerReload.Reset();return;
    }
    if (Pose.bDesktop) { ServerReload.Reset();return; }
    const auto* GS=GetWorld()->GetGameState<AMRFPSGameState>();
    const bool Eligible=Pose.bRightTracked && Pose.bLeftTracked && Pose.LeftFingers.bValid &&
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
    if (!HasAuthority() || (!Pose.bDesktop && !MRFPSBot::IsServerBot(this)) || !CanAct() || Now-LastReloadAt<.5) return;
    MagazineRounds=MagazineCapacity;LastReloadAt=Now;ForceNetUpdate();
}

void AMRFPSPawn::UpdatePlayerHud()
{
#if !UE_SERVER
    if (!PlayerHud) return;
    const bool Show=IsLocallyControlled() && IsXR();
    PlayerHud->SetVisibility(Show);
    if (RoundHud) RoundHud->SetVisibility(Show);
    if (!Show) return;
    const TWeakObjectPtr<AMRFPSPawn> Weak(this);
    const auto Font=[](int32 Size) { return FSlateFontInfo(FPaths::EngineContentDir()/TEXT("Slate/Fonts/DroidSansFallback.ttf"),Size); };
    if (RoundHud && !RoundHudSlate.IsValid()) {
        RoundHudSlate=SNew(SBorder).BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
            .BorderBackgroundColor(FLinearColor(.008f,.025f,.045f,.78f)).Padding(FMargin(12,5))
            [ SNew(SVerticalBox)
              +SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
              [ SNew(STextBlock).Font(Font(25)).ColorAndOpacity(FSlateColor(FLinearColor(.75f,1.f,.96f)))
                .Text_Lambda([Weak]() {
                    const auto* State=Weak.IsValid() && Weak->GetWorld()?Weak->GetWorld()->GetGameState<AMRFPSGameState>():nullptr;
                    return State?FText::FromString(State->RoundHeaderText()):FText::GetEmpty();
                }) ]
              +SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
              [ SNew(STextBlock).Font(Font(20)).ColorAndOpacity(FSlateColor(FLinearColor(.55f,.8f,.9f)))
                .Text_Lambda([Weak]() {
                    const auto* State=Weak.IsValid() && Weak->GetWorld()?Weak->GetWorld()->GetGameState<AMRFPSGameState>():nullptr;
                    return State?FText::FromString(State->RoundScoreText(Weak->GetSlot())):FText::GetEmpty();
                }) ]
              +SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
              [ SNew(STextBlock).Font(Font(15)).ColorAndOpacity(FSlateColor(FLinearColor(1.f,.68f,.2f)))
                .Text_Lambda([Weak]() {
                    const auto* State=Weak.IsValid() && Weak->GetWorld()?Weak->GetWorld()->GetGameState<AMRFPSGameState>():nullptr;
                    return State && State->bSoloBotSession && !State->bSoloGeometryReady
                        ? FText::FromString(TEXT("障害物スキャン未受信")):FText::GetEmpty();
                }) ]
              +SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
              [ SNew(STextBlock).Font(Font(15)).ColorAndOpacity(FSlateColor(FLinearColor(1.f,.55f,.3f)))
                .Text(FText::FromString(TEXT("映像共有停止　Xで再試行 / 直らなければ再起動")))
                .Visibility_Lambda([Weak]() {
                    auto* World=Weak.IsValid()?Weak->GetWorld():nullptr;
                    auto* Instance=World?World->GetGameInstance():nullptr;
                    const auto* Capture=Instance?Instance->GetSubsystem<UMRFPSViewCaptureSubsystem>():nullptr;
                    return Capture && Capture->HasCaptureError()?EVisibility::Visible:EVisibility::Collapsed;
                }) ]
            ];
        RoundHud->SetSlateWidget(RoundHudSlate);
    }
    // Count only foreground time after the actual 3D widget has a render target.
    if (auto* Instance=GetGameInstance<UMRFPSGameInstance>())
        Instance->ConnectionStatus.AdvanceVisible(FApp::GetDeltaTime(),FApp::HasFocus(),
            PlayerHud->IsWidgetVisible() && PlayerHud->GetRenderTarget()!=nullptr);
    if (PlayerHudSlate.IsValid()) return;
    PlayerHudSlate=SNew(SOverlay)
        +SOverlay::Slot()[ SNew(SMRFPSPlayerHudFrame).Pawn(Weak) ]
        +SOverlay::Slot().HAlign(HAlign_Center).VAlign(VAlign_Top).Padding(FMargin(0,2,0,0))
        [ SNew(STextBlock).Font(Font(19)).ColorAndOpacity(FSlateColor(FLinearColor(1.f,.68f,.2f)))
          .Text_Lambda([Weak]() {
              if (!Weak.IsValid()) return FText::GetEmpty();
              const auto* Instance=Weak->GetGameInstance<UMRFPSGameInstance>();
              if (Instance && Instance->ConnectionStatus.ShowSuccess()) return FText::FromString(TEXT("サーバー接続成功"));
              const auto* PS=Weak->GetPlayerState<AMRFPSPlayerState>();
              return Weak->GetNetMode()==NM_Client && PS && PS->bAuthenticated
                  ? FText::GetEmpty() : FText::FromString(TEXT("接続待ち"));
          }) ]
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
