#if WITH_DEV_AUTOMATION_TESTS && !UE_SERVER
#include "MRFPSGame.h"
#include "MRFPSLoginMenu.h"
#include "MRFPSHapticsComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/DemoNetConnection.h"
#include "Engine/DemoNetDriver.h"
#include "Engine/Engine.h"
#include "Engine/LocalPlayer.h"
#include "Engine/PendingNetGame.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "TimerManager.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRFPSDisconnectAutomationTest,"MRFPS.Login.ExplicitDisconnect",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)

bool FMRFPSDisconnectAutomationTest::RunTest(const FString& Parameters)
{
    if (!TestNotNull(TEXT("Engine"),GEngine)) return false;
    const auto Init=UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(false)
        .CreateNavigation(false).CreateAISystem(false).ShouldSimulatePhysics(false).CreateFXSystem(false);
    UWorld* World=UWorld::CreateWorld(EWorldType::Game,false,NAME_None,nullptr,true,ERHIFeatureLevel::Num,&Init);
    if (!TestNotNull(TEXT("Isolated world"),World)) return false;
    FWorldContext& Context=GEngine->CreateNewWorldContext(EWorldType::Game); Context.SetCurrentWorld(World);
    auto* Instance=NewObject<UMRFPSGameInstance>(GEngine); Context.OwningGameInstance=Instance; World->SetGameInstance(Instance);
    World->InitializeActorsForPlay(FURL());
    auto* PC=World->SpawnActor<AMRFPSPlayerController>(); auto* Fighter=World->SpawnActor<AMRFPSPawn>();
    auto* Local=NewObject<ULocalPlayer>(GEngine);
    if (!TestTrue(TEXT("Local test player exists"),PC && Fighter && Local)) {
        World->DestroyWorld(false); GEngine->DestroyWorldContext(World); return false;
    }
    if (Fighter->Haptics) Fighter->Haptics->bAutoConnectTracking=false;
    PC->Player=Local; Local->PlayerController=PC; PC->Possess(Fighter); PC->bTestQuestVR=true;
    auto* Driver=NewObject<UDemoNetDriver>(); Driver->ServerConnection=NewObject<UDemoNetConnection>(); World->SetNetDriver(Driver);
    const FString Base=FPaths::Combine(FPaths::ProjectIntermediateDir(),TEXT("DisconnectAutomation"),FGuid::NewGuid().ToString(EGuidFormats::Digits));
    IFileManager::Get().MakeDirectory(*Base,true); PC->TestAutomationDirectory=Base;
    TArray<FString> CreatedFolders;
    const auto Submit=[&]() {
        FGuid Guid=FGuid::NewGuid();
        Guid.B=(Guid.B&0xffff0fffU)|0x00004000U; Guid.C=(Guid.C&0x3fffffffU)|0x80000000U;
        const FString Id=Guid.ToString(EGuidFormats::DigitsWithHyphens).ToLower();
        const FString Folder=FPaths::Combine(Base,Id); CreatedFolders.Add(Folder);
        IFileManager::Get().MakeDirectory(*Folder,true);
        const FString Request=FString::Printf(TEXT("{\"version\":1,\"id\":\"%s\",\"operation\":\"connection.set\",\"parameters\":{\"connected\":false}}"),*Id);
        TestTrue(TEXT("The CLI request was staged"),FFileHelper::SaveStringToFile(Request,*FPaths::Combine(Folder,TEXT("request.json"))));
        PC->NextAutomationAt=0; PC->TickAutomation();
        FString Body; TestTrue(TEXT("The CLI result was written"),FFileHelper::LoadFileToString(Body,*FPaths::Combine(Folder,TEXT("result.json"))));
        TSharedPtr<FJsonObject> Result; FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Body),Result); return Result;
    };
    const auto Check=[&](const TSharedPtr<FJsonObject>& Result,const TCHAR* Status,const TCHAR* Code) {
        FString ActualStatus,ActualCode;
        TestTrue(TEXT("The result contains the expected status and code"),Result.IsValid() &&
            Result->TryGetStringField(TEXT("status"),ActualStatus) && Result->TryGetStringField(TEXT("code"),ActualCode) &&
            ActualStatus==Status && ActualCode==Code);
    };
    int32 TravelCalls=0;
    const auto TravelHandle=Instance->OnNotifyPreClientTravel().AddLambda([&](const FString&,ETravelType,bool) { ++TravelCalls; });
    PC->SetQuestDeviceLoginVisible(true);
    auto* Menu=PC->DeviceLogin.Get();
    TestTrue(TEXT("Opening the Quest panel leaves the existing connection intact"),Menu && Menu->IsOpen() && Context.TravelURL.IsEmpty() && World->GetNetDriver()==Driver);
    if (Menu) {
        Menu->bExchanging=true; Menu->ExchangeGeneration=12;
        PC->PendingDeviceOwner=Menu; PC->PendingDeviceGeneration=12; Instance->MarkNextConnectionAsDeviceLogin();
        World->GetTimerManager().SetTimer(PC->PendingConnectionTimer,FTimerDelegate::CreateLambda([]() {}),60.f,false);
    }
    Check(Submit(),TEXT("submitted"),TEXT("DISCONNECT_REQUESTED"));
    TestTrue(TEXT("An open panel cannot suppress an explicit disconnect"),Context.TravelURL==TEXT("?closed") && TravelCalls==1);
    TestTrue(TEXT("The old panel and delayed device attempt are cancelled"),Menu && !Menu->IsOpen() && !World->GetTimerManager().IsTimerActive(PC->PendingConnectionTimer));
    TestTrue(TEXT("The new local world will show the login panel"),Instance->IsLoginScreenRequested());
    Check(Submit(),TEXT("submitted"),TEXT("DISCONNECT_PENDING"));
    TestTrue(TEXT("A distinct repeat does not emit another travel"),Context.TravelURL==TEXT("?closed") && TravelCalls==1);

    // Reset only this isolated fixture, representing a new connected session
    // with the panel already closed.
    Context.TravelURL.Empty(); Instance->ClearLoginScreenRequest();
    Check(Submit(),TEXT("submitted"),TEXT("DISCONNECT_REQUESTED"));
    TestTrue(TEXT("A closed panel also permits one explicit disconnect"),Context.TravelURL==TEXT("?closed") && TravelCalls==2);

    Context.TravelURL.Empty(); World->SetNetDriver(nullptr); Instance->ClearLoginScreenRequest();
    Check(Submit(),TEXT("already_applied"),TEXT("DISCONNECTED"));
    TestEqual(TEXT("An already disconnected stable world does not travel"),TravelCalls,2);

    PC->SetQuestDeviceLoginVisible(true);
    auto* OfflineMenu=PC->DeviceLogin.Get();
    if (OfflineMenu) {
        OfflineMenu->bExchanging=true; OfflineMenu->ExchangeGeneration=19;
        PC->PendingDeviceOwner=OfflineMenu; PC->PendingDeviceGeneration=19;
        World->GetTimerManager().SetTimer(PC->PendingConnectionTimer,FTimerDelegate::CreateLambda([]() {}),60.f,false);
    }
    Check(Submit(),TEXT("already_applied"),TEXT("DISCONNECTED"));
    TestTrue(TEXT("Offline disconnect cancels a pending exchange and certificate timer"),OfflineMenu && !OfflineMenu->IsOpen() &&
        !World->GetTimerManager().IsTimerActive(PC->PendingConnectionTimer) && Instance->IsLoginScreenRequested());
    TestEqual(TEXT("Cancelling an unstarted attempt needs no travel"),TravelCalls,2);

    World->GetTimerManager().SetTimer(PC->PendingConnectionTimer,FTimerDelegate::CreateLambda([]() {}),60.f,false);
    World->GetTimerManager().PauseTimer(PC->PendingConnectionTimer);
    Check(Submit(),TEXT("already_applied"),TEXT("DISCONNECTED"));
    TestFalse(TEXT("A paused legacy connect timer is also cancelled"),World->GetTimerManager().TimerExists(PC->PendingConnectionTimer));

    Context.TravelURL=TEXT("example.invalid:7777");
    Check(Submit(),TEXT("submitted"),TEXT("DISCONNECT_REQUESTED"));
    TestTrue(TEXT("A scheduled outgoing connection is replaced with closed travel"),Context.TravelURL==TEXT("?closed") && TravelCalls==3);
    Context.TravelURL.Empty(); Context.PendingNetGame=NewObject<UPendingNetGame>();
    Check(Submit(),TEXT("submitted"),TEXT("DISCONNECT_REQUESTED"));
    TestTrue(TEXT("A pending net game schedules the engine's cancellation path"),Context.TravelURL==TEXT("?closed") && TravelCalls==4);

    Instance->OnNotifyPreClientTravel().Remove(TravelHandle);
    Context.PendingNetGame=nullptr; Context.TravelURL.Empty();
    for (const auto& Folder:CreatedFolders) {
        IFileManager::Get().Delete(*FPaths::Combine(Folder,TEXT("request.json")),false,true);
        IFileManager::Get().Delete(*FPaths::Combine(Folder,TEXT("started")),false,true);
        IFileManager::Get().Delete(*FPaths::Combine(Folder,TEXT("result.json")),false,true);
        IFileManager::Get().DeleteDirectory(*Folder,false,false);
    }
    IFileManager::Get().DeleteDirectory(*Base,false,false);
    World->DestroyWorld(false); GEngine->DestroyWorldContext(World);
    return true;
}
#endif
