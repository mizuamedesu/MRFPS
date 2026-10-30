#if WITH_DEV_AUTOMATION_TESTS && !UE_SERVER
#include "MRFPSGame.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/GameSession.h"
#include "GameFramework/WorldSettings.h"
#include "Misc/AutomationTest.h"

namespace
{
struct FAdmissionWorld
{
    UWorld* World=nullptr;
    AMRFPSGameMode* Mode=nullptr;
    AGameSession* Session=nullptr;

    bool Create()
    {
        if (!GEngine) return false;
        const auto Init=UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true).CreateNavigation(false)
            .CreateAISystem(false).ShouldSimulatePhysics(false).EnableTraceCollision(true).CreateFXSystem(false).SetTransactional(false);
        World=UWorld::CreateWorld(EWorldType::Game,false,NAME_None,nullptr,true,ERHIFeatureLevel::Num,&Init);
        if (!World) return false;
        FWorldContext& Context=GEngine->CreateNewWorldContext(EWorldType::Game); Context.SetCurrentWorld(World);
        auto* Instance=NewObject<UGameInstance>(GEngine); Context.OwningGameInstance=Instance; World->SetGameInstance(Instance);
        FURL URL; URL.AddOption(TEXT("game=/Script/MRFPS.MRFPSGameMode"));
        if (!World->SetGameMode(URL)) return false;
        Mode=World->GetAuthGameMode<AMRFPSGameMode>();
        World->InitializeActorsForPlay(URL); World->BeginPlay(); World->GetWorldSettings()->NotifyBeginPlay();
        Session=Mode?Mode->GameSession.Get():nullptr;
        return Mode && Session && World->GetGameState<AMRFPSGameState>();
    }

    AMRFPSPlayerState* Add(bool Inspector,bool Admin,int32 Slot)
    {
        auto* PC=World->SpawnActor<AMRFPSPlayerController>();
        auto* PS=PC?PC->GetPlayerState<AMRFPSPlayerState>():nullptr;
        if (!PS) return nullptr;
        // This fixture represents claims already verified by PreLogin. The
        // separate auth tests exercise signature and transport verification.
        PS->bAuthenticated=true; PS->AuthExpiresAt=FDateTime::UtcNow().ToUnixTimestamp()+3600;
        PS->bInspector=Inspector; PS->bAdmin=Admin; PS->Slot=Slot;
        PS->SetIsOnlyASpectator(false); PS->SetIsSpectator(false);
        return PS;
    }

    ~FAdmissionWorld()
    {
        if (World) { World->EndPlay(EEndPlayReason::EndPlayInEditor); World->DestroyWorld(false); GEngine->DestroyWorldContext(World); }
    }
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRFPSSessionAdmissionTest,"MRFPS.Admin.SessionAdmission",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FMRFPSSessionAdmissionTest::RunTest(const FString& Parameters)
{
    FAdmissionWorld F;
    if (!TestTrue(TEXT("A real MRFPS authority, GameState and session are initialized"),F.Create())) return false;
    if (!TestTrue(TEXT("The authority uses the dedicated role-aware session"),F.Session->IsA<AMRFPSGameSession>())) return false;
    // Set the normal engine default explicitly, so this catches a regression
    // even if the host machine's Game.ini happens to specify a larger limit.
    F.Session->MaxPlayers=16;
    F.Session->MaxSplitscreensPerConnection=4;
    for (int32 Index=0;Index<20;++Index) {
        if (!TestTrue(FString::Printf(TEXT("Observer %d passes the actual UE ApproveLogin"),Index+1),
                      F.Session->ApproveLogin(TEXT("?Inspector=1")).IsEmpty())) return false;
        if (!TestNotNull(TEXT("Observer player state"),F.Add(true,false,-1))) return false;
    }
    TestTrue(TEXT("More than the generic engine player limit is actually present"),F.Mode->GetNumPlayers()>16);
    auto* OrdinarySession=F.World->SpawnActor<AGameSession>();
    if (!TestNotNull(TEXT("Ordinary engine session used as the control"),OrdinarySession)) return false;
    OrdinarySession->MaxPlayers=16; OrdinarySession->MaxSplitscreensPerConnection=4;
    TestEqual(TEXT("The ordinary engine session rejects the same populated world"),
              OrdinarySession->ApproveLogin(TEXT("?Inspector=1")),FString(TEXT("Server full.")));
    TestTrue(TEXT("The role check still permits more inspectors"),F.Mode->RoleCapacityError(TEXT("inspector")).IsEmpty());
    TestTrue(TEXT("Splitscreen count at the configured limit is accepted"),
             F.Session->ApproveLogin(TEXT("?Inspector=1?SplitscreenCount=4")).IsEmpty());
    TestEqual(TEXT("Splitscreen count above the limit is rejected by UE"),
              F.Session->ApproveLogin(TEXT("?Inspector=1?SplitscreenCount=5")),FString(TEXT("Maximum splitscreen players")));

    TestTrue(TEXT("Observers do not consume either player slot"),F.Mode->RoleCapacityError(TEXT("player")).IsEmpty());
    auto* Player0=F.Add(false,false,0);
    if (!TestNotNull(TEXT("First player"),Player0)) return false;
    TestTrue(TEXT("A second player is allowed"),F.Mode->RoleCapacityError(TEXT("player")).IsEmpty());
    auto* Player1=F.Add(false,false,1);
    if (!TestNotNull(TEXT("Second player"),Player1)) return false;
    TestEqual(TEXT("A third player is rejected"),F.Mode->RoleCapacityError(TEXT("player")),FString(TEXT("ARENA_FULL")));
    TestTrue(TEXT("A candidate player does not count against itself"),F.Mode->RoleCapacityError(TEXT("player"),Player1).IsEmpty());

    TestTrue(TEXT("The first administrator is allowed"),F.Mode->RoleCapacityError(TEXT("admin")).IsEmpty());
    auto* Admin=F.Add(true,true,-1);
    if (!TestNotNull(TEXT("First administrator"),Admin)) return false;
    TestEqual(TEXT("A second administrator is rejected"),F.Mode->RoleCapacityError(TEXT("admin")),FString(TEXT("ADMIN_FULL")));
    TestTrue(TEXT("A candidate administrator does not count against itself"),F.Mode->RoleCapacityError(TEXT("admin"),Admin).IsEmpty());
    TestTrue(TEXT("More inspectors remain allowed when privileged slots are full"),F.Mode->RoleCapacityError(TEXT("inspector")).IsEmpty());
    return true;
}
#endif
