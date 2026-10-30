#if WITH_DEV_AUTOMATION_TESTS && !UE_SERVER
#include "MRFPSGame.h"
#include "MRFPSLoginMenu.h"
#include "Engine/DemoNetConnection.h"
#include "Engine/DemoNetDriver.h"
#include "Engine/Engine.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRFPSLoginControlTickTest,"MRFPS.Login.BackgroundControl",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)

bool FMRFPSLoginControlTickTest::RunTest(const FString& Parameters)
{
    if (!TestNotNull(TEXT("An engine exists"),GEngine)) return false;
    const auto Init=UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(false)
        .CreateNavigation(false).CreateAISystem(false).ShouldSimulatePhysics(false).CreateFXSystem(false);
    UWorld* World=UWorld::CreateWorld(EWorldType::Game,false,NAME_None,nullptr,true,ERHIFeatureLevel::Num,&Init);
    if (!TestNotNull(TEXT("A separate test world exists"),World)) return false;
    FWorldContext& Context=GEngine->CreateNewWorldContext(EWorldType::Game); Context.SetCurrentWorld(World);
    World->InitializeActorsForPlay(FURL());
    auto* PC=World->SpawnActor<AMRFPSPlayerController>();
    auto* Local=NewObject<ULocalPlayer>(GEngine);
    if (!TestNotNull(TEXT("Local test controller"),PC) || !TestNotNull(TEXT("Local player"),Local)) {
        World->DestroyWorld(false); GEngine->DestroyWorldContext(World); return false;
    }
    PC->Player=Local; Local->PlayerController=PC;
    auto* Menu=NewObject<UMRFPSLoginMenu>(PC);
    Menu->Controller=PC; Menu->bOpen=true; PC->DeviceLogin=Menu;
    const FString Folder=FPaths::Combine(FPaths::ProjectIntermediateDir(),TEXT("MRFPSLoginControlTests"),FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString File=FPaths::Combine(Folder,TEXT("control.json"));
    IFileManager::Get().MakeDirectory(*Folder,true);
    Menu->TestControlPath=File;
    const auto Write=[&]() { return FFileHelper::SaveStringToFile(TEXT("{}"),*File); };
    const auto Exists=[&]() { return IFileManager::Get().FileExists(*File); };

    TestTrue(TEXT("The test menu is never added to a Slate viewport"),!Menu->IsInViewport());
    TestTrue(TEXT("Fixture controller is local"),PC->IsLocalController());
    TestTrue(TEXT("Synthetic instruction created"),Write());
    Menu->TickControlFile();
    TestFalse(TEXT("A game-tick poll consumes the file without a widget paint"),Exists());
    TestTrue(TEXT("A poll establishes its next deadline"),Menu->NextControlCheck>0);

    TestTrue(TEXT("Next instruction created"),Write());
    Menu->NextControlCheck=FPlatformTime::Seconds()+60;
    Menu->TickControlFile();
    TestTrue(TEXT("Repeated polls inside half a second do not consume the next instruction"),Exists());
    Menu->NextControlCheck=0;
    Menu->bExchanging=true; Menu->TickControlFile();
    TestTrue(TEXT("In-flight exchange defers the next instruction"),Exists());
    Menu->bExchanging=false; Menu->bOpen=false; Menu->TickControlFile();
    TestTrue(TEXT("A closed menu does not consume instructions"),Exists());
    Menu->bOpen=true; Menu->bClosing=true; Menu->TickControlFile();
    TestTrue(TEXT("A closing menu does not consume instructions"),Exists());
    Menu->bClosing=false; World->bIsTearingDown=true; Menu->TickControlFile();
    TestTrue(TEXT("A tearing-down world does not consume instructions"),Exists());
    World->bIsTearingDown=false; Context.TravelURL=TEXT("/FixturePendingTravel"); Menu->TickControlFile();
    TestTrue(TEXT("A pending world travel does not consume instructions"),Exists());
    Context.TravelURL.Empty(); PC->DeviceLogin=nullptr; Menu->TickControlFile();
    TestTrue(TEXT("A detached old menu does not consume instructions"),Exists());
    PC->DeviceLogin=Menu; Menu->TickControlFile();
    TestFalse(TEXT("The active menu consumes the deferred instruction"),Exists());

    // An Android Quest overlay can be used while the existing client session
    // remains connected. Desktop clients retain the original restriction.
    auto* Driver=NewObject<UDemoNetDriver>(); Driver->ServerConnection=NewObject<UDemoNetConnection>();
    World->SetNetDriver(Driver); Menu->bVR=true; Menu->NextControlCheck=0;
    TestTrue(TEXT("The fixture represents a client session"),PC->GetNetMode()==NM_Client);
    TestTrue(TEXT("Connected instruction created"),Write());
    Menu->TickControlFile();
    TestTrue(TEXT("A non-Android client still defers control files"),Exists());
    Menu->bTestAndroidControl=true;
    Context.TravelURL=TEXT("/FixturePendingTravel"); Menu->TickControlFile();
    TestTrue(TEXT("Connected Quest also defers control during travel"),Exists());
    Context.TravelURL.Empty(); Menu->TickControlFile();
    TestFalse(TEXT("The stable connected Quest panel consumes the instruction"),Exists());
    World->SetNetDriver(nullptr);

    PC->DeviceLogin=nullptr; Menu->bOpen=false;
    IFileManager::Get().Delete(*File,false,true);
    IFileManager::Get().DeleteDirectory(*Folder,false,false);
    World->DestroyWorld(false); GEngine->DestroyWorldContext(World);
    return true;
}
#endif
