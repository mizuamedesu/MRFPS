#if WITH_DEV_AUTOMATION_TESTS && !UE_SERVER
#include "MRFPSGame.h"
#include "MRFPSLoginMenu.h"
#include "MRFPSHapticsComponent.h"
#include "Components/EditableTextBox.h"
#include "Engine/DemoNetConnection.h"
#include "Engine/DemoNetDriver.h"
#include "Engine/Engine.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "InputKeyEventArgs.h"
#include "Misc/AutomationTest.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "TimerManager.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRFPSQuestLoginToggleTest,"MRFPS.Login.QuestToggle",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)

bool FMRFPSQuestLoginToggleTest::RunTest(const FString& Parameters)
{
    if (!TestNotNull(TEXT("Engine"),GEngine)) return false;
    const auto Init=UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(false)
        .CreateNavigation(false).CreateAISystem(false).ShouldSimulatePhysics(false).CreateFXSystem(false);
    UWorld* World=UWorld::CreateWorld(EWorldType::Game,false,NAME_None,nullptr,true,ERHIFeatureLevel::Num,&Init);
    if (!TestNotNull(TEXT("Isolated world"),World)) return false;
    FWorldContext& Context=GEngine->CreateNewWorldContext(EWorldType::Game); Context.SetCurrentWorld(World);
    auto* GI=NewObject<UMRFPSGameInstance>(GEngine); Context.OwningGameInstance=GI; World->SetGameInstance(GI);
    World->InitializeActorsForPlay(FURL());
    auto* PC=World->SpawnActor<AMRFPSPlayerController>();
    auto* Pawn=World->SpawnActor<AMRFPSPawn>();
    auto* Local=NewObject<ULocalPlayer>(GEngine);
    if (!TestTrue(TEXT("Local test player exists"),PC && Pawn && Local)) {
        World->DestroyWorld(false); GEngine->DestroyWorldContext(World); return false;
    }
    if (Pawn->Haptics) Pawn->Haptics->bAutoConnectTracking=false;
    PC->Player=Local; Local->PlayerController=PC; PC->Possess(Pawn); PC->bTestQuestVR=true;
    auto* Driver=NewObject<UDemoNetDriver>(); Driver->ServerConnection=NewObject<UDemoNetConnection>();
    World->SetNetDriver(Driver);
    TestTrue(TEXT("The overlay test uses client network mode"),PC->GetNetMode()==NM_Client);

    FInputKeyEventArgs A; A.Key=EKeys::OculusTouch_Right_A_Click; A.Event=IE_Pressed;
    TestFalse(TEXT("Non-Android A stays available to its existing action"),PC->HandleQuestLoginButton(A,false));
    TestTrue(TEXT("Android A is consumed"),PC->HandleQuestLoginButton(A,true));
    auto* First=PC->DeviceLogin.Get();
    if (!TestNotNull(TEXT("Android A creates the real login widget"),First)) {
        World->SetNetDriver(nullptr); World->DestroyWorld(false); GEngine->DestroyWorldContext(World); return false;
    }
    TestTrue(TEXT("Android A opens the real login widget"),First->IsOpen());
    TestTrue(TEXT("The widget is attached to the player's camera"),First->WorldWidget!=nullptr);
    TestTrue(TEXT("Open leaves the connected driver and pawn intact"),World->GetNetDriver()==Driver && PC->GetPawn()==Pawn);
    TestTrue(TEXT("Open did not request travel"),Context.TravelURL.IsEmpty());
    TestFalse(TEXT("Fire is blocked while the overlay is open"),GI->FireTransition.CanPhysical());

    // Feed a settings-only command through the real control-file path while
    // connected. Keep its persisted endpoint in a disposable settings file.
    const FString Folder=FPaths::Combine(FPaths::ProjectIntermediateDir(),TEXT("QuestLoginToggle"),FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString Control=FPaths::Combine(Folder,TEXT("control.json"));
    const FString Settings=FPaths::Combine(Folder,TEXT("GameUserSettings.ini"));
    IFileManager::Get().MakeDirectory(*Folder,true);
    const FString OriginalSettings=GGameUserSettingsIni; GGameUserSettingsIni=Settings;
    First->TestControlPath=Control; First->bTestAndroidControl=true;
    TestTrue(TEXT("A settings instruction can be staged"),FFileHelper::SaveStringToFile(TEXT("{\"server\":\"example.test:7777\",\"role\":\"player\"}"),*Control));
    First->TickControlFile();
    TestFalse(TEXT("Connected Quest consumed the settings instruction"),IFileManager::Get().FileExists(*Control));
    TestTrue(TEXT("The real panel moved to device login"),First->bLoginPage);
    TestTrue(TEXT("The requested endpoint reached the panel"),First->AddressBox && First->AddressBox->GetText().ToString()==TEXT("example.test:7777"));
    TestTrue(TEXT("Settings alone did not disconnect or travel"),World->GetNetDriver()==Driver && Context.TravelURL.IsEmpty());
    if (GConfig) GConfig->UnloadFile(Settings);
    GGameUserSettingsIni=OriginalSettings;
    IFileManager::Get().Delete(*Control,false,true); IFileManager::Get().Delete(*Settings,false,true);
    IFileManager::Get().DeleteDirectory(*Folder,false,false);

    A.Event=IE_Repeat; PC->HandleQuestLoginButton(A,true);
    A.Event=IE_Released; PC->HandleQuestLoginButton(A,true);
    TestTrue(TEXT("Repeat and release do not toggle the panel"),PC->DeviceLogin==First && First->IsOpen());
    PC->SetQuestDeviceLoginVisible(true); PC->SetQuestDeviceLoginVisible(true);
    TestTrue(TEXT("Repeated explicit open preserves the same widget"),PC->DeviceLogin==First && First->IsOpen());
    GI->FireTransition.SetPhysical(true);
    A.Event=IE_Pressed; PC->HandleQuestLoginButton(A,true);
    TestFalse(TEXT("The next A closes the panel"),First->IsOpen());
    TestTrue(TEXT("Close leaves the connected driver intact"),World->GetNetDriver()==Driver && Context.TravelURL.IsEmpty());
    TestFalse(TEXT("A held trigger stays locked after closing"),GI->FireTransition.CanPhysical());
    GI->FireTransition.SetPhysical(false);
    TestTrue(TEXT("A fresh trigger release re-arms firing"),GI->FireTransition.CanPhysical());
    PC->SetQuestDeviceLoginVisible(false); PC->SetQuestDeviceLoginVisible(false);
    TestFalse(TEXT("Repeated explicit close stays closed"),PC->DeviceLogin && PC->DeviceLogin->IsOpen());

    World->SetNetDriver(nullptr);
    TestTrue(TEXT("Startup-close test is offline"),PC->GetNetMode()==NM_Standalone);
    int32 StartupCalls=0;
    World->GetTimerManager().SetTimer(PC->StartupLoginTimer,FTimerDelegate::CreateLambda([&StartupCalls]() { ++StartupCalls; }),1.f,false);
    PC->SetQuestDeviceLoginVisible(false);
    TestFalse(TEXT("Closing before startup cancels the delayed automatic panel"),World->GetTimerManager().IsTimerActive(PC->StartupLoginTimer));
    Context.TravelURL=TEXT("/FixturePendingTravel"); PC->SetQuestDeviceLoginVisible(true);
    TestFalse(TEXT("Opening cannot create a panel in a travelling world"),PC->DeviceLogin && PC->DeviceLogin->IsOpen());
    Context.TravelURL.Empty(); PC->SetQuestDeviceLoginVisible(true);
    TestTrue(TEXT("The panel can be reopened once travel is clear"),PC->DeviceLogin && PC->DeviceLogin->IsOpen());
    PC->SetQuestDeviceLoginVisible(false);
    TestEqual(TEXT("No cancelled startup callback ran"),StartupCalls,0);

    World->SetNetDriver(nullptr);
    World->DestroyWorld(false); GEngine->DestroyWorldContext(World);
    return true;
}
#endif
