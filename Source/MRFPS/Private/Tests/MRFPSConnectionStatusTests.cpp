#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR && !UE_SERVER
#include "MRFPSGame.h"
#include "MRFPSLoginMenu.h"
#include "Blueprint/UserWidget.h"
#include "Components/TextBlock.h"
#include "Components/ScrollBox.h"
#include "Engine/Engine.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRFPSConnectionStatusTest,"MRFPS.Auth.ConnectionStatus",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FMRFPSConnectionStatusTest::RunTest(const FString& Parameters)
{
    using namespace MRFPSConnectionStatus;
    FState State;
    State.Begin(ERole::Player);
    TestFalse(TEXT("A code exchange and travel request alone are not a join"),State.ShowSuccess());
    TestFalse(TEXT("Standalone is not a join"),State.ObserveJoin(false,true,true,true,false,false,0));
    TestFalse(TEXT("A closed connection is not a join"),State.ObserveJoin(true,false,true,true,false,false,0));
    TestFalse(TEXT("An unverified player state is not a join"),State.ObserveJoin(true,true,false,true,false,false,0));
    TestFalse(TEXT("A player without a pawn is not ready"),State.ObserveJoin(true,true,true,false,false,false,0));
    TestFalse(TEXT("The wrong role is not the requested join"),State.ObserveJoin(true,true,true,true,false,true,-1));
    TestFalse(TEXT("An unassigned player slot is not a join"),State.ObserveJoin(true,true,true,true,false,false,-1));
    TestTrue(TEXT("A live authenticated player with the requested slot has joined"),State.ObserveJoin(true,true,true,true,false,false,0));
    TestTrue(TEXT("A real join starts the success notice"),State.ShowSuccess());
    State.AdvanceVisible(20,false,true); State.AdvanceVisible(20,true,false);
    TestEqual(TEXT("Background or an unready HUD never consumes the notice"),State.VisibleSeconds,0.);
    State.AdvanceVisible(20,true,true);
    TestTrue(TEXT("A stalled foreground frame cannot consume the entire notice"),State.VisibleSeconds<=.1);
    for (int I=0;I<30;++I) State.AdvanceVisible(.1,true,true);
    TestTrue(TEXT("Notice remains after three visible seconds"),State.ShowSuccess());
    for (int I=0;I<25;++I) State.AdvanceVisible(.1,true,true);
    TestFalse(TEXT("Notice expires only after enough foreground display"),State.ShowSuccess());
    State.Fail(EFailure::Timeout);
    TestFalse(TEXT("A stale join cannot overwrite a failure"),State.ObserveJoin(true,true,true,true,false,false,0));
    TestTrue(TEXT("The failure survives until a new operation"),State.Phase==EPhase::Failure && State.Failure==EFailure::Timeout);
    State.Begin(ERole::Inspector);
    TestFalse(TEXT("An admin cannot satisfy an inspector attempt"),State.ObserveJoin(true,true,true,true,true,true,-1));
    TestTrue(TEXT("An authenticated inspector joins with no player slot"),State.ObserveJoin(true,true,true,true,false,true,-1));
    State.Begin(ERole::Admin);
    TestFalse(TEXT("An inspector cannot satisfy an admin attempt"),State.ObserveJoin(true,true,true,true,false,true,-1));
    TestTrue(TEXT("An authenticated admin joins"),State.ObserveJoin(true,true,true,true,true,true,-1));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRFPSLoginStatusPropagationTest,"MRFPS.Auth.LoginFailureAcrossWorlds",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FMRFPSLoginStatusPropagationTest::RunTest(const FString& Parameters)
{
    if (!TestNotNull(TEXT("Engine exists"),GEngine)) return false;
    auto* Instance=NewObject<UMRFPSGameInstance>(GEngine);
    Instance->SetConnectionFailure(MRFPSConnectionStatus::EFailure::Timeout);
    Instance->SetConnectionFailure(MRFPSConnectionStatus::EFailure::Connection);
    TestTrue(TEXT("A generic follow-up does not erase a specific failure"),
        Instance->ConnectionStatus.Failure==MRFPSConnectionStatus::EFailure::Timeout);
    Instance->RememberConnectionAttempt(TEXT("example.invalid:7777"),TEXT("player"));
    Instance->SetConnectionFailure(MRFPSConnectionStatus::EFailure::Connection);
    Instance->PrepareForNetworkFailure(false);
    auto* JoinOrigin=NewObject<UMRFPSGameInstance>(GEngine);
    const FString Message=Instance->ConnectionFailureMessage();
    TestTrue(TEXT("Failure requests a login screen"),Instance->IsLoginScreenRequested());
    TestTrue(TEXT("Only a short fixed message is retained"),!Message.IsEmpty() && Message.Len()<80 && !Message.Contains(TEXT("example.invalid")));
    Instance->ClearLoginScreenRequest();
    TestTrue(TEXT("A manual open can clear the request without losing the failure"),
        Instance->ConnectionStatus.Phase==MRFPSConnectionStatus::EPhase::Failure);
    for (int Pass=0;Pass<2;++Pass) {
        const auto Init=UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(false)
            .CreateNavigation(false).CreateAISystem(false).ShouldSimulatePhysics(false).CreateFXSystem(false);
        UWorld* World=UWorld::CreateWorld(EWorldType::Game,false,NAME_None,nullptr,true,ERHIFeatureLevel::Num,&Init);
        if (!TestNotNull(TEXT("A separate test world exists"),World)) return false;
        FWorldContext& Context=GEngine->CreateNewWorldContext(EWorldType::Game); Context.OwningGameInstance=Instance;
        Context.SetCurrentWorld(World); World->SetGameInstance(Instance); World->InitializeActorsForPlay(FURL());
        if (Pass==0) {
            JoinOrigin->RememberConnectionAttempt(TEXT("example.invalid:7777"),TEXT("player"),World);
            TestFalse(TEXT("The old connected world cannot satisfy a new attempt"),JoinOrigin->IsNewConnectionWorld(World));
        } else TestTrue(TEXT("A later world can confirm the new attempt"),JoinOrigin->IsNewConnectionWorld(World));
        auto* PC=World->SpawnActor<AMRFPSPlayerController>(); auto* Local=NewObject<ULocalPlayer>(GEngine);
        if (!TestTrue(TEXT("A local test controller exists"),PC && Local)) { World->DestroyWorld(false); GEngine->DestroyWorldContext(World); return false; }
        PC->Player=Local; Local->PlayerController=PC;
        Context.TravelURL=TEXT("/FixturePendingTravel"); PC->OpenDeviceLogin();
        TestNull(TEXT("No duplicate login panel is created during travel"),PC->DeviceLogin.Get());
        Context.TravelURL.Empty();
        auto* Menu=CreateWidget<UMRFPSLoginMenu>(PC,UMRFPSLoginMenu::StaticClass());
        if (!TestNotNull(TEXT("Login widget can be reconstructed"),Menu)) { World->DestroyWorld(false); GEngine->DestroyWorldContext(World); return false; }
        Menu->Controller=PC; Menu->bOpen=true; Menu->bExchanging=true; PC->DeviceLogin=Menu;
        Menu->ShowConnectionFailure();
        TestEqual(TEXT("The reconstructed panel displays the retained failure"),Menu->Status->GetText().ToString(),Message);
        bool bStatusInScroll=false;
        for (auto* Parent=Menu->Status->GetParent();Parent;Parent=Parent->GetParent())
            if (Parent->IsA<UScrollBox>()) bStatusInScroll=true;
        TestFalse(TEXT("The status remains outside the scrollable keyboard"),bStatusInScroll);
        TestFalse(TEXT("A failed connection releases the exchange lock"),Menu->bExchanging);
        PC->OpenDeviceLogin();
        TestTrue(TEXT("Opening an already-open panel is idempotent"),PC->DeviceLogin==Menu);
        PC->DeviceLogin=nullptr; Menu->bOpen=false;
        World->DestroyWorld(false); GEngine->DestroyWorldContext(World);
    }
    Instance->ConnectionStatus.ClearFailure();
    TestTrue(TEXT("An explicit new exchange can clear the prior failure"),Instance->ConnectionStatus.Phase==MRFPSConnectionStatus::EPhase::Idle);
    return true;
}
#endif
