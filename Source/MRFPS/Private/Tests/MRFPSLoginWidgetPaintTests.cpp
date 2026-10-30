#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR && !UE_SERVER
#include "MRFPSGame.h"
#include "MRFPSLoginInteraction.h"
#include "MRFPSLoginMenu.h"
#include "MRFPSLoginWidgetComponent.h"
#include "MRFPSHapticsComponent.h"
#include "Blueprint/WidgetTree.h"
#include "Components/EditableTextBox.h"
#include "Engine/Engine.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "RHI.h"

// Exercise the real runtime UUserWidget. Unlike a test that traces after an
// arbitrary World::Tick, this observes the grid from inside the original
// SObjectWidget NativeTick as well as from the production post-paint path.
class FMRFPSLoginWidgetPaintScenario final : public IAutomationLatentCommand
{
public:
    explicit FMRFPSLoginWidgetPaintScenario(FAutomationTestBase* InTest) : Test(InTest) {}
    ~FMRFPSLoginWidgetPaintScenario()
    {
        if (Menu) { Menu->TestDuringPaint=nullptr; Menu->CloseLogin(); }
        if (World && GEngine) {
            World->EndPlay(EEndPlayReason::EndPlayInEditor);
            World->DestroyWorld(false); GEngine->DestroyWorldContext(World);
        }
    }
    bool Update() override
    {
        if (LastFrame==GFrameCounter) return false;
        LastFrame=GFrameCounter;
        if (!World) {
            if (!Create()) return true;
            Deadline=FPlatformTime::Seconds()+15.; return false;
        }
        World->Tick(LEVELTICK_All,1.f/60.f);
        if (FPlatformTime::Seconds()>Deadline) {
            Test->AddError(FString::Printf(TEXT("Login paint timeout prepared=%d submitted=%d page=%d world=%d registered=%d tick=%d rt=%d window=%d grid=%d armed=%d hit=%d slate=%d path=%d"),
                bPrepared,bSubmitted,bPageChanged,World->HasBegunPlay(),Panel->IsRegistered(),Panel->IsComponentTickEnabled(),
                Panel->GetRenderTarget()!=nullptr,Panel->GetSlateWindow().IsValid(),Menu->bPointerGridReady,Menu->PointerInput.ButtonArmed,
                Interaction->GetLastHitResult().GetComponent()==Panel,Interaction->IsOverHitTestVisibleWidget(),PathContainsKey()));
            return true;
        }
        if (!Key || !Panel->GetRenderTarget() || !Panel->GetSlateWindow().IsValid()) return false;
        if (!bPrepared) {
            const FGeometry Geometry=Key->GetCachedGeometry();
            if (Geometry.GetLocalSize().IsNearlyZero()) return false;
            const FVector2D Center=Geometry.LocalToAbsolute(Geometry.GetLocalSize()*.5);
            const FVector2D Size=Panel->GetCurrentDrawSize(), Pivot=Panel->GetPivot();
            TargetPoint=Panel->GetComponentTransform().TransformPosition(FVector(0,Size.X*Pivot.X-Center.X,Size.Y*Pivot.Y-Center.Y));
            if (!PathContainsKey()) return false;
            const FVector Direction=-Panel->GetComponentTransform().GetUnitAxis(EAxis::X);
            const FVector Muzzle=TargetPoint-Direction*100;
            Menu->TestWeapon.Source=MRFPSLoginPointer::ESource::AimGrip;
            Menu->TestWeapon.GripWorld=FTransform(Direction.Rotation(),Muzzle-Direction*22);
            Menu->bUseTestWeapon=true;
            Menu->TestDuringPaint=[this]() { ++PaintTicks; if (PathContainsKey()) ++PaintTicksWithKey; };
            Before=Menu->AddressBox->GetText().ToString();
            bPrepared=true; return false;
        }
        if (!bSubmitted) {
            if (!Menu->bPointerGridReady || !Menu->PointerInput.ButtonArmed ||
                Interaction->GetLastHitResult().GetComponent()!=Panel || !Interaction->IsOverHitTestVisibleWidget()) return false;
            const auto Expected=Key->GetCachedWidget();
            if (!Expected || Interaction->HoveredTarget().Pin()!=Expected) return false;
            Menu->SetPointerPressed(true); Menu->SetPointerPressed(false);
            bSubmitted=true; return false;
        }
        if (!bPageChanged) {
            if (Menu->AddressBox->GetText().ToString()!=Before+TEXT("1")) return false;
            Test->TestTrue(TEXT("The real user widget ticked during its panel paint"),PaintTicks>0);
            Test->TestEqual(TEXT("The key is absent from the partially painted hit grid"),PaintTicksWithKey,0);
            Test->TestTrue(TEXT("The post-paint physical edge clicked the real login key once"),
                Menu->AddressBox->GetText().ToString()==Before+TEXT("1"));
            Menu->TestDuringPaint=nullptr;
            Menu->SetStage(true);
            Test->TestFalse(TEXT("A page change fences the old hit grid"),Menu->bPointerGridReady);
            Menu->SetPointerPressed(true); Menu->SetPointerPressed(false);
            Test->TestEqual(TEXT("Edges before the new page paint cannot enter the queue"),Menu->PendingPointerEvents.Count,std::size_t(0));
            bPageChanged=true; return false;
        }
        if (!Menu->bPointerGridReady) return false;
        Test->TestTrue(TEXT("The new page becomes eligible after a completed paint"),Menu->bLoginPage);
        Test->TestTrue(TEXT("No stale key was clicked during the page change"),Menu->AddressBox->GetText().ToString()==Before+TEXT("1"));
        return true;
    }
private:
    bool PathContainsKey() const
    {
        if (!Key || !Panel) return false;
        const auto Cached=Key->GetCachedWidget(); if (!Cached) return false;
        for (const auto& Entry:Panel->GetHitWidgetPath(TargetPoint,false)) if (Entry.Widget==Cached) return true;
        return false;
    }
    bool Create()
    {
        if (!Test->TestTrue(TEXT("Slate and a non-NullRHI renderer are available"),GEngine && FSlateApplication::IsInitialized() && !GUsingNullRHI)) return false;
        const auto Init=UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true)
            .CreateNavigation(false).CreateAISystem(false).ShouldSimulatePhysics(false).EnableTraceCollision(true).CreateFXSystem(false);
        World=UWorld::CreateWorld(EWorldType::Game,false,NAME_None,nullptr,true,ERHIFeatureLevel::Num,&Init);
        if (!Test->TestNotNull(TEXT("A separate login fixture world exists"),World)) return false;
        GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
        World->InitializeActorsForPlay(FURL()); World->BeginPlay(); World->GetWorldSettings()->NotifyBeginPlay();
        auto* PC=World->SpawnActor<AMRFPSPlayerController>();
        auto* Pawn=World->SpawnActor<AMRFPSPawn>();
        auto* Local=NewObject<ULocalPlayer>(GEngine);
        if (!Test->TestTrue(TEXT("Local login controller and pawn exist"),PC && Pawn && Local)) return false;
        // This isolated fixture must never connect to a real haptics device.
        if (Pawn->Haptics) { Pawn->Haptics->bAutoConnectTracking=false; Pawn->Haptics->SetComponentTickEnabled(false); }
        PC->Player=Local; Local->PlayerController=PC; PC->Possess(Pawn); PC->SetActorTickEnabled(false);
        Menu=CreateWidget<UMRFPSLoginMenu>(PC,UMRFPSLoginMenu::StaticClass());
        if (!Test->TestNotNull(TEXT("The production login widget was created"),Menu)) return false;
        Menu->Controller=PC; Menu->bVR=true; Menu->bOpen=true; PC->DeviceLogin=Menu;
        Menu->WidgetTree->ForEachWidget([this](UWidget* Widget) {
            if (auto* Candidate=Cast<UMRFPSLoginKey>(Widget); Candidate && Candidate->KeyValue==TEXT("1") && !Key) Key=Candidate;
        });
        if (!Test->TestNotNull(TEXT("A real virtual keyboard key exists"),Key)) return false;
        Panel=NewObject<UMRFPSLoginWidgetComponent>(Pawn);
        Panel->SetupAttachment(Pawn->GetRootComponent()); Panel->SetWidgetSpace(EWidgetSpace::World);
        Panel->SetDrawSize(FVector2D(900,720)); Panel->SetPivot(FVector2D(.5,.5));
        Panel->SetRelativeLocation(FVector(150,0,0)); Panel->SetRelativeRotation(FRotator(0,180,0));
        Panel->SetRelativeScale3D(FVector(.1)); Panel->SetTickWhenOffscreen(true);
        Panel->SetCollisionEnabled(ECollisionEnabled::NoCollision); Panel->RegisterComponent();
        Menu->WorldWidget=Panel; Panel->SetWidget(Menu);
        Interaction=NewObject<UMRFPSLoginInteraction>(Pawn);
        Interaction->SetupAttachment(Pawn->GetRootComponent()); Interaction->InteractionDistance=400;
        Interaction->SetLoginWorldWidget(Panel); Interaction->RegisterComponent(); Interaction->SetComponentTickEnabled(false);
        Menu->Interaction=Interaction;
        return true;
    }
    FAutomationTestBase* Test=nullptr;
    UWorld* World=nullptr;
    UMRFPSLoginMenu* Menu=nullptr;
    UMRFPSLoginWidgetComponent* Panel=nullptr;
    UMRFPSLoginInteraction* Interaction=nullptr;
    UMRFPSLoginKey* Key=nullptr;
    FVector TargetPoint=FVector::ZeroVector;
    FString Before;
    uint64 LastFrame=MAX_uint64;
    double Deadline=0;
    int32 PaintTicks=0,PaintTicksWithKey=0;
    bool bPrepared=false,bSubmitted=false,bPageChanged=false;
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRFPSLoginWidgetPaintTest,"QuestVisualFixture.LoginWidgetPaintOrdering",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FMRFPSLoginWidgetPaintTest::RunTest(const FString& Parameters)
{
    ADD_LATENT_AUTOMATION_COMMAND(FMRFPSLoginWidgetPaintScenario(this));
    return true;
}
#endif
