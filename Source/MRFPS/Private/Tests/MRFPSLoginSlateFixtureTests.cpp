#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR && !UE_SERVER
#include "MRFPSLoginInteraction.h"
#include "MRFPSWeaponPose.h"

#include "Components/SceneComponent.h"
#include "Components/WidgetComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Application/SlateUser.h"
#include "GameFramework/Actor.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/PlatformTime.h"
#include "InputCoreTypes.h"
#include "Misc/AutomationTest.h"
#include "RHI.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

namespace MRFPSLoginSlateFixture
{
// This is a real world widget and a real Slate virtual user. It does not load
// the login menu, write game settings, use physical input, or contact a server.
class FPointerScenario final : public IAutomationLatentCommand
{
public:
    explicit FPointerScenario(FAutomationTestBase* InTest) : Test(InTest) {}

    virtual bool Update() override
    {
        if (LastFrame==GFrameCounter) return false;
        LastFrame=GFrameCounter;
        if (!World) {
            if (!Create()) return true;
            Deadline=FPlatformTime::Seconds()+15.;
            return false;
        }

        World->Tick(LEVELTICK_All,1.f/60.f);
        if (!Panel->GetRenderTarget() || !Panel->GetSlateWindow().IsValid()) {
            if (FPlatformTime::Seconds()<Deadline) return false;
            Test->AddError(TEXT("World widget never initialized its render target and Slate window"));
            return true;
        }
        First->RefreshPointer();
        Second->RefreshPointer();
        const bool Ready=First->GetLastHitResult().GetComponent()==Panel &&
            Second->GetLastHitResult().GetComponent()==Panel &&
            First->IsOverHitTestVisibleWidget() && Second->IsOverHitTestVisibleWidget() &&
            Button[0]->IsHovered() && Button[1]->IsHovered();
        if (!Ready) {
            if (FPlatformTime::Seconds()<Deadline) return false;
            Test->AddError(TEXT("Real world traces did not hover both rendered fixture buttons"));
            return true;
        }
        return RunAssertions();
    }

    ~FPointerScenario()
    {
        // All of these actors, components, windows, and virtual users are
        // fixture-owned. No global mouse capture or existing world is touched.
        if (First) First->CancelPointer();
        if (Second) Second->CancelPointer();
        if (Alias) Alias->CancelPointer();
        if (World && GEngine) {
            World->EndPlay(EEndPlayReason::EndPlayInEditor);
            World->DestroyWorld(false);
            GEngine->DestroyWorldContext(World);
        }
    }

private:
    bool Create()
    {
        if (!Test->TestTrue(TEXT("Slate and a non-NullRHI renderer are available"),
                            GEngine && FSlateApplication::IsInitialized() && !GUsingNullRHI)) return false;
        const auto Init=UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true)
            .CreateNavigation(false).CreateAISystem(false).ShouldSimulatePhysics(false)
            .EnableTraceCollision(true).CreateFXSystem(false).SetTransactional(false);
        World=UWorld::CreateWorld(EWorldType::Game,false,NAME_None,nullptr,true,ERHIFeatureLevel::Num,&Init);
        if (!Test->TestNotNull(TEXT("A separate fixture world was created"),World)) return false;
        GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
        World->InitializeActorsForPlay(FURL()); World->BeginPlay(); World->GetWorldSettings()->NotifyBeginPlay();
        AActor* Host=World->SpawnActor<AActor>();
        if (!Test->TestNotNull(TEXT("The fixture actor was created"),Host)) return false;
        auto* Root=NewObject<USceneComponent>(Host);
        Host->SetRootComponent(Root); Root->RegisterComponent();

        Button[0]=SNew(SButton).ClickMethod(EButtonClickMethod::DownAndUp)
            .OnClicked_Lambda([this]() { ++Clicks[0]; return FReply::Handled(); })
            [ SNew(STextBlock).Text(FText::FromString(TEXT("fixture 0"))) ];
        Button[1]=SNew(SButton).ClickMethod(EButtonClickMethod::DownAndUp)
            .OnClicked_Lambda([this]() { ++Clicks[1]; return FReply::Handled(); })
            [ SNew(STextBlock).Text(FText::FromString(TEXT("fixture 1"))) ];

        Panel=NewObject<UWidgetComponent>(Host);
        Panel->SetupAttachment(Root); Panel->SetWidgetSpace(EWidgetSpace::World);
        Panel->SetDrawSize(FVector2D(300,120)); Panel->SetPivot(FVector2D(.5,.5));
        Panel->SetRelativeLocation(FVector(150,0,0)); Panel->SetRelativeRotation(FRotator(0,180,0));
        Panel->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
        Panel->SetCollisionResponseToAllChannels(ECR_Ignore);
        Panel->SetCollisionResponseToChannel(ECC_Visibility,ECR_Block);
        Panel->SetTickWhenOffscreen(true);
        Panel->SetSlateWidget(SNew(SHorizontalBox)
            +SHorizontalBox::Slot().FillWidth(1.f) [ Button[0].ToSharedRef() ]
            +SHorizontalBox::Slot().FillWidth(1.f) [ Button[1].ToSharedRef() ]);
        Panel->RegisterComponent();

        const FTransform Grip(FRotator::ZeroRotator,FVector(-22,-75,0));
        First=MakePointer(Host,Root,0,MRFPSWeaponPose::Muzzle(Grip).GetLocation());
        Test->TestTrue(TEXT("The real widget trace begins at the weapon muzzle"),
            First && First->GetComponentLocation().Equals(MRFPSWeaponPose::Muzzle(Grip).GetLocation(),.01));
        Second=MakePointer(Host,Root,1,FVector(0,75,0));
        // Alias deliberately shares the second pointer index without pressing.
        // Cancelling it must not release another interaction's capture.
        Alias=MakePointer(Host,Root,1,FVector(0,75,0));
        if (!Test->TestTrue(TEXT("All fixture interactions are initialized"),First && Second && Alias)) return false;
        Handle=FSlateApplication::Get().FindOrCreateVirtualUser(VirtualUserIndex);
        User=FSlateApplication::Get().GetUser(Handle->GetUserIndex());
        return Test->TestTrue(TEXT("The fixture owns a Slate virtual user"),User.IsValid() && User->IsVirtualUser());
    }

    UMRFPSLoginInteraction* MakePointer(AActor* Host,USceneComponent* Root,int32 Index,const FVector& Location)
    {
        auto* Pointer=NewObject<UMRFPSLoginInteraction>(Host);
        Pointer->VirtualUserIndex=VirtualUserIndex; Pointer->PointerIndex=Index;
        Pointer->InteractionSource=EWidgetInteractionSource::World; Pointer->TraceChannel=ECC_Visibility;
        Pointer->InteractionDistance=400; Pointer->SetupAttachment(Root);
        Pointer->SetRelativeLocation(Location); Pointer->SetRelativeRotation(FRotator::ZeroRotator);
        Pointer->RegisterComponent(); Pointer->SetComponentTickEnabled(false);
        return Pointer;
    }

    bool RunAssertions()
    {
        if (!Test->TestTrue(TEXT("The first pointer accepts a real press"),First->BeginPointer())) return true;
        if (!Test->TestTrue(TEXT("A normal down presses and captures the first button"),
                            Button[0]->IsPressed() && User->HasCapture(0))) return true;
        First->EndPointer();
        Test->TestEqual(TEXT("Normal down/up clicks exactly once"),Clicks[0],1);
        Test->TestFalse(TEXT("Normal up releases the first capture"),User->HasCapture(0));

        First->RefreshPointer();
        if (!Test->TestTrue(TEXT("The first pointer accepts the interrupted press"),First->BeginPointer())) return true;
        if (!Test->TestTrue(TEXT("The interrupted down is actually captured"),
                            Button[0]->IsPressed() && User->HasCapture(0))) return true;
        First->CancelPointer();
        Test->TestEqual(TEXT("Cancelling a held pointer fires no click"),Clicks[0],1);
        Test->TestFalse(TEXT("Cancellation resets the Slate button"),Button[0]->IsPressed());
        Test->TestFalse(TEXT("Cancellation releases only its pointer capture"),User->HasCapture(0));
        First->EndPointer();
        Test->TestEqual(TEXT("A late physical release after cancellation cannot click"),Clicks[0],1);

        First->RefreshPointer();
        if (!Test->TestTrue(TEXT("The pointer can press after cancellation"),First->BeginPointer())) return true;
        First->EndPointer();
        Test->TestEqual(TEXT("A fresh down/up works after cancellation"),Clicks[0],2);

        First->SetWorldLocation(FVector(0,-250,0)); First->RefreshPointer();
        const auto BeforeEntry=First->HoveredTarget().Pin();
        First->SetWorldLocation(FVector(0,-75,0)); First->RefreshPointer();
        const auto Entered=First->HoveredTarget().Pin();
        Test->TestFalse(TEXT("Entering a target while neutral does not cancel input"),
            MRFPSLoginPointer::CancelForHitChange(false,BeforeEntry.Get(),Entered.Get()));
        if (!Test->TestTrue(TEXT("An immediate press on the entered button works"),First->BeginPointer())) return true;
        First->EndPointer(); Test->TestEqual(TEXT("Entering and pressing clicks once"),Clicks[0],3);

        First->RefreshPointer(); const auto OldTarget=First->HoveredTarget().Pin();
        if (!Test->TestTrue(TEXT("The original button captures a held press"),First->BeginPointer())) return true;
        First->SetWorldLocation(FVector(0,75,0)); First->RefreshPointer();
        const auto NewTarget=First->HoveredTarget().Pin();
        if (MRFPSLoginPointer::CancelForHitChange(true,OldTarget.Get(),NewTarget.Get())) First->CancelPointer();
        else Test->AddError(TEXT("A held press moving to another button must cancel"));
        First->EndPointer();
        Test->TestEqual(TEXT("Moving a held press cannot click its old target"),Clicks[0],3);
        Test->TestEqual(TEXT("Moving a held press cannot click its new target"),Clicks[1],0);
        First->SetWorldLocation(FVector(0,-75,0)); First->RefreshPointer();

        // Exercise real Slate capture when the physical trigger takes over a
        // held glove press. Cancel clears the WIC hit, so retrace before down.
        MRFPSLoginPointer::FPressState Takeover;
        Takeover.SetSource(MRFPSLoginPointer::ESource::AimGrip); Takeover.SetGloveFlex(true,0); Takeover.Step(.1);
        Takeover.SetGloveFlex(true,.8f);
        if (!Test->TestTrue(TEXT("Glove begins a captured press"),Takeover.Next()==MRFPSLoginPointer::EAction::Press && First->BeginPointer())) return true;
        const auto Expected=First->HoveredTarget().Pin();
        Takeover.SetButton(true);
        if (!Test->TestTrue(TEXT("Physical takeover cancels the old glove capture"),Takeover.Next()==MRFPSLoginPointer::EAction::Cancel)) return true;
        First->CancelPointer(); First->RefreshPointer();
        if (!Test->TestTrue(TEXT("Retrace restores the same login target"),MRFPSLoginPointer::SameHit(Expected.Get(),First->HoveredTarget().Pin().Get()))) return true;
        if (!Test->TestTrue(TEXT("Physical takeover accepts a fresh down"),Takeover.Next()==MRFPSLoginPointer::EAction::Press && First->BeginPointer())) return true;
        Takeover.SetButton(false);
        if (!Test->TestTrue(TEXT("Physical release completes takeover"),Takeover.Next()==MRFPSLoginPointer::EAction::Release)) return true;
        First->EndPointer(); Test->TestEqual(TEXT("Takeover produces only one click"),Clicks[0],4);

        First->RefreshPointer();
        MRFPSLoginPointer::FPressState Stale;
        Stale.SetSource(MRFPSLoginPointer::ESource::AimGrip); Stale.SetGloveFlex(true,0); Stale.Step(.1);
        Stale.SetGloveFlex(true,.8f);
        if (!Test->TestTrue(TEXT("Fresh glove begins a second capture"),Stale.Next()==MRFPSLoginPointer::EAction::Press && First->BeginPointer())) return true;
        const auto StaleTarget=First->HoveredTarget().Pin(); Stale.SetGloveFlex(false,0);
        if (!Test->TestTrue(TEXT("Stale glove cancels capture"),Stale.Next()==MRFPSLoginPointer::EAction::Cancel)) return true;
        First->CancelPointer(); First->RefreshPointer(); Stale.SetButton(true);
        if (!Test->TestTrue(TEXT("A physical trigger works after stale glove cancellation"),
            MRFPSLoginPointer::SameHit(StaleTarget.Get(),First->HoveredTarget().Pin().Get()) &&
            Stale.Next()==MRFPSLoginPointer::EAction::Press && First->BeginPointer())) return true;
        Stale.SetButton(false);
        if (!Test->TestTrue(TEXT("Physical release after stale glove completes"),Stale.Next()==MRFPSLoginPointer::EAction::Release)) return true;
        First->EndPointer(); Test->TestEqual(TEXT("Stale glove then physical trigger clicks once"),Clicks[0],5);
        First->RefreshPointer();

        Second->RefreshPointer();
        if (!Test->TestTrue(TEXT("The second pointer accepts a real press"),Second->BeginPointer())) return true;
        if (!Test->TestTrue(TEXT("The unrelated second pointer captures its button"),
                            Button[1]->IsPressed() && User->HasCapture(1))) return true;
        First->CancelPointer();
        Test->TestTrue(TEXT("Cancelling pointer zero preserves pointer one's capture"),
                       Button[1]->IsPressed() && User->HasCapture(1));
        Alias->CancelPointer();
        if (!Test->TestTrue(TEXT("An unpressed alias cannot cancel another component's capture"),
                            Button[1]->IsPressed() && User->HasCapture(1))) return true;
        Alias->RefreshPointer();
        Test->TestFalse(TEXT("An alias cannot begin while another component has captured"),Alias->BeginPointer());
        Alias->EndPointer();
        if (!Test->TestTrue(TEXT("An alias down/up cannot end another component's capture"),
                            Button[1]->IsPressed() && User->HasCapture(1) && Clicks[1]==0)) return true;
        Alias->RefreshPointer();
        Test->TestFalse(TEXT("An alias remains rejected before cancellation"),Alias->BeginPointer());
        Alias->CancelPointer();
        if (!Test->TestTrue(TEXT("An alias down/cancel cannot steal another component's capture"),
                            Button[1]->IsPressed() && User->HasCapture(1) && Clicks[1]==0)) return true;
        Test->TestEqual(TEXT("Neither unrelated cancel clicks the second button"),Clicks[1],0);
        Second->EndPointer();
        Test->TestEqual(TEXT("The untouched second button still clicks once"),Clicks[1],1);
        return true;
    }

    static constexpr int32 VirtualUserIndex=37;
    FAutomationTestBase* Test=nullptr;
    UWorld* World=nullptr;
    UWidgetComponent* Panel=nullptr;
    UMRFPSLoginInteraction* First=nullptr;
    UMRFPSLoginInteraction* Second=nullptr;
    UMRFPSLoginInteraction* Alias=nullptr;
    TSharedPtr<SButton> Button[2];
    TSharedPtr<FSlateVirtualUserHandle> Handle;
    TSharedPtr<FSlateUser> User;
    int32 Clicks[2]={0,0};
    double Deadline=0;
    uint64 LastFrame=MAX_uint64;
};
}

// This fixture deliberately sits outside the default NullRHI MRFPS suite.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRFPSLoginSlateFixtureTest,"QuestVisualFixture.LoginPointerCancel",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FMRFPSLoginSlateFixtureTest::RunTest(const FString& Parameters)
{
    ADD_LATENT_AUTOMATION_COMMAND(MRFPSLoginSlateFixture::FPointerScenario(this));
    return true;
}
#endif
