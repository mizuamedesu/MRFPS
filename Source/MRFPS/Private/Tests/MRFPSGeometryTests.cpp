#if WITH_DEV_AUTOMATION_TESTS && !UE_SERVER
#include "MRFPSGeometryProtocol.h"
#include "MRFPSGeometryWorld.h"
#include "MRFPSGame.h"
#include "MRFPSHapticsComponent.h"
#include "Misc/AutomationTest.h"
#include "Misc/DateTime.h"
#include "Serialization/BufferArchive.h"
#include "Serialization/MemoryReader.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Engine/DemoNetDriver.h"
#include "Engine/DemoNetConnection.h"
#include "HAL/PlatformTime.h"
#include "EngineUtils.h"
#include "GameFramework/WorldSettings.h"
#include <limits>

namespace MRFPSGeometryTests
{
FMRFPSGeometryMesh Wall(double X=0)
{
    FMRFPSGeometryMesh M;
    M.Vertices={FVector(X,-50,90),FVector(X,50,90),FVector(X,50,170),FVector(X,-50,170)};
    M.Indices={0,1,2,0,2,3}; return M;
}
FMRFPSGeometryChunk Header(uint32 Sequence=1)
{
    FMRFPSGeometryChunk H; H.StreamId=H.Calibration=1; H.Sequence=Sequence; H.CaptureMs=10000; return H;
}
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRFPSGeometryCollisionTest,"MRFPS.Geometry.SurfaceCollision",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FMRFPSGeometryCollisionTest::RunTest(const FString& Parameters)
{
    auto Mesh=MRFPSGeometryTests::Wall(); double Fraction=0; FVector Point;
    TestTrue(TEXT("Finite measured surface intercepts a shot"),Mesh.Trace(FVector(-100,0,130),FVector(100,0,130),Fraction,Point));
    TestTrue(TEXT("Hit is exactly the measured plane, halfway along the segment"),Point.Equals(FVector(0,0,130),1.e-6) && FMath::IsNearlyEqual(Fraction,.5,1.e-6));
    TestTrue(TEXT("Depth triangles block from either side"),Mesh.Trace(FVector(100,0,130),FVector(-100,0,130),Fraction,Point));
    TestFalse(TEXT("A nearby ray outside finite triangle bounds is not blocked"),Mesh.Trace(FVector(-100,60,130),FVector(100,60,130),Fraction,Point));
    TestFalse(TEXT("A parallel shot off the plane is not blocked"),Mesh.Trace(FVector(10,-100,130),FVector(10,100,130),Fraction,Point));
    TestFalse(TEXT("Geometry beyond the finite shot endpoint is not hit"),Mesh.Trace(FVector(-100,0,130),FVector(-1,0,130),Fraction,Point));
    TestFalse(TEXT("A zero length shot is rejected"),Mesh.Trace(FVector::ZeroVector,FVector::ZeroVector,Fraction,Point));
    auto Malformed=Mesh; Malformed.Indices[0]=MAX_int32;
    TestFalse(TEXT("Public ray query rejects invalid indices without touching out-of-bounds vertices"),Malformed.Trace(FVector(-100,0,130),FVector(100,0,130),Fraction,Point));
    Malformed=Mesh; Malformed.Vertices[0].Z=std::numeric_limits<double>::quiet_NaN();
    TestFalse(TEXT("Public ray query rejects nonfinite measured coordinates"),Malformed.Trace(FVector(-100,0,130),FVector(100,0,130),Fraction,Point));
    auto Near=MRFPSGeometryTests::Wall(-25);
    const int32 Base=Mesh.Vertices.Num(); Mesh.Vertices.Append(Near.Vertices); for (int32 Index:Near.Indices) Mesh.Indices.Add(Index+Base);
    TestTrue(TEXT("Unsorted surfaces still return the closest measured hit"),Mesh.Trace(FVector(-100,0,130),FVector(100,0,130),Fraction,Point) && FMath::IsNearlyEqual(Point.X,-25.,1.e-6));
    TestTrue(TEXT("Clock mapping accepts 500 ms capture age"),MRFPSGeometryWire::CaptureFresh(9500,10));
    TestFalse(TEXT("Expired captures cannot be made fresh by receipt"),MRFPSGeometryWire::CaptureFresh(9000,10));
    TestTrue(TEXT("Bounded 250 ms server-time estimate lead is allowed"),MRFPSGeometryWire::CaptureFresh(10250,10));
    TestFalse(TEXT("Larger future claims are rejected"),MRFPSGeometryWire::CaptureFresh(10251,10));
    TestFalse(TEXT("Nonfinite time is rejected"),MRFPSGeometryWire::CaptureFresh(0,std::numeric_limits<double>::quiet_NaN()));
    TestTrue(TEXT("Millisecond wrap does not reopen stale capture age"),MRFPSGeometryWire::CaptureFresh(500,(double(MAX_uint32)+1001)/1000));
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRFPSGeometryWireTest,"MRFPS.Geometry.BoundedWireAndLoss",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FMRFPSGeometryWireTest::RunTest(const FString& Parameters)
{
    auto Mesh=MRFPSGeometryTests::Wall(-15); auto H=MRFPSGeometryTests::Header(); TArray<uint8> Bytes;
    TestTrue(TEXT("Real surface encodes"),Mesh.Encode(H,Bytes));
    FMRFPSGeometryMesh Decoded;
    TestTrue(TEXT("Signed arena centimetres and indices round trip"),FMRFPSGeometryMesh::Decode(H,Bytes,Decoded) && Decoded.Vertices==Mesh.Vertices && Decoded.Indices==Mesh.Indices);
    TArray<uint8> Bad=Bytes; Bad[24]=0xff; Bad[25]=0xff;
    TestFalse(TEXT("Out-of-range triangle indices are rejected before commit"),FMRFPSGeometryMesh::Decode(H,Bad,Decoded));
    Bad=Bytes; Bad.SetNum(Bytes.Num()-1);
    TestFalse(TEXT("Truncated complete mesh is rejected"),FMRFPSGeometryMesh::Decode(H,Bad,Decoded));
    auto Invalid=Mesh; Invalid.Vertices[0].X=16001;
    TestFalse(TEXT("Wire coordinate envelope is not silently clamped"),Invalid.Encode(H,Bad));
    Invalid=Mesh; Invalid.Vertices[1]=Invalid.Vertices[0];
    TestFalse(TEXT("Collapsed triangles do not become collider surfaces"),Invalid.Encode(H,Bad));
    Invalid=Mesh; Invalid.Vertices[0].X=std::numeric_limits<double>::infinity();
    TestFalse(TEXT("Infinite source coordinates are rejected"),Invalid.IsSane());
    H=MRFPSGeometryTests::Header(); TestTrue(TEXT("Valid mesh remains encodable"),Mesh.Encode(H,Bytes));
    FMRFPSGeometrySendQueue Queue; Queue.Push(H,Bytes,10);
    TArray<FMRFPSGeometryChunk> Packets; FMRFPSGeometryChunk C;
    while (Queue.Next(C,10.01)) Packets.Add(C);
    TestEqual(TEXT("A small snapshot has one data packet and one padded parity packet"),Packets.Num(),2);
    FMRFPSGeometryAssembler Assembler;
    for (const auto& Packet:Packets) {
        FBufferArchive Wire; bool Success=false; auto Copy=Packet; Copy.NetSerialize(Wire,nullptr,Success);
        TestTrue(TEXT("Every serialized RPC payload fits native DTLS 1024 bytes with ample header room"),Success && Wire.Num()<600);
        FMemoryReader Reader(Wire); FMRFPSGeometryChunk Loaded; Loaded.NetSerialize(Reader,nullptr,Success);
        TestTrue(TEXT("Actual bounded network serializer round trips"),Success && Loaded.Payload==Packet.Payload);
        // Intentionally lose the sole data fragment; parity must reconstruct it.
        if (Packet.Index&MRFPSGeometryWire::ParityFlag) TestTrue(TEXT("Parity can arrive without its data fragment"),Assembler.Add(Loaded,10.02));
    }
    TArray<uint8> Recovered; FMRFPSGeometryChunk RecoveredHeader;
    TestTrue(TEXT("One lost fragment is recovered atomically"),Assembler.Take(RecoveredHeader,Recovered) && Recovered==Bytes);
    TestFalse(TEXT("Completed snapshot cannot replay"),Assembler.Add(Packets[0],10.03));
    Assembler.Reset(); TestTrue(TEXT("First fragment accepted into an incomplete snapshot"),Assembler.Add(Packets[0],10));
    auto Conflict=Packets[0]; Conflict.Payload[0]^=1;
    TestFalse(TEXT("Conflicting duplicate cannot mutate partial bytes"),Assembler.Add(Conflict,10.01));
    Assembler.Reset(); Assembler.Add(Packets[1],10); // Parity itself completes one data chunk.
    Assembler.Reset();
    auto LargeHeader=H; LargeHeader.VertexCount=MRFPSGeometryWire::MaxVertices; LargeHeader.TriangleCount=MRFPSGeometryWire::MaxTriangles;
    LargeHeader.TotalBytes=MRFPSGeometryWire::MaxBytes; LargeHeader.Payload.SetNumZeroed(MRFPSGeometryWire::ChunkBytes);
    TestTrue(TEXT("Maximum bounded geometry begins a partial snapshot"),Assembler.Add(LargeHeader,10));
    auto OtherLayout=LargeHeader; ++OtherLayout.LayoutRevision;
    TestFalse(TEXT("A partial snapshot cannot mix layout coordinate epochs"),Assembler.Add(OtherLayout,10.01));
    TestTrue(TEXT("Assembly capacity, including allocator quantization and bitmaps, is bounded"),Assembler.AllocatedBytes()<=96*1024);
    LargeHeader.Index=1;
    TestFalse(TEXT("Late fragment cannot revive an expired partial snapshot"),Assembler.Add(LargeHeader,10.76));
    // Build a hostile wire header directly, bypassing the saving-side validator.
    FBufferArchive Hostile; uint8 Slot=0,Index=0; uint32 One=1,Time=10000;
    uint16 Vertices=MRFPSGeometryWire::MaxVertices+1,Triangles=1,Total=uint16(6*(Vertices+Triangles)),Length=65535;
    Hostile<<Slot<<One<<One<<One<<One<<Time<<Vertices<<Triangles<<Total<<Index<<Length;
    FMemoryReader HostileReader(Hostile); FMRFPSGeometryChunk Loaded; bool Success=true;
    Loaded.NetSerialize(HostileReader,nullptr,Success);
    TestTrue(TEXT("Malicious wire count/length fails before payload allocation"),!Success && HostileReader.IsError() && Loaded.Payload.Num()==0);
    FBufferArchive BadLength; Vertices=4; Triangles=2; Total=36;
    BadLength<<Slot<<One<<One<<One<<One<<Time<<Vertices<<Triangles<<Total<<Index<<Length;
    FMemoryReader BadLengthReader(BadLength); Loaded=FMRFPSGeometryChunk(); Success=true;
    Loaded.NetSerialize(BadLengthReader,nullptr,Success);
    TestTrue(TEXT("Oversized payload length independently fails before allocation"),!Success && BadLengthReader.IsError() && Loaded.Payload.IsEmpty());
    Queue.Reset(); LargeHeader.Index=0; LargeHeader.Sequence=1; TArray<uint8> LargeBytes; LargeBytes.SetNumZeroed(LargeHeader.TotalBytes);
    Queue.Push(LargeHeader,LargeBytes,20); TestTrue(TEXT("A bounded large snapshot starts"),Queue.Next(C,20.01));
    LargeHeader.Sequence=2; Queue.Push(LargeHeader,LargeBytes,20.02);
    LargeHeader.Sequence=3; Queue.Push(LargeHeader,LargeBytes,20.03);
    bool SawCurrent=false,SawSuperseded=false,SawLatest=false; int32 Count=0;
    while (Queue.Next(C,20.04) && ++Count<200) { SawCurrent|=C.Sequence==1; SawSuperseded|=C.Sequence==2; SawLatest|=C.Sequence==3; }
    TestTrue(TEXT("Current work completes while only the newest pending snapshot survives backpressure"),SawCurrent && !SawSuperseded && SawLatest && Count<200);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRFPSGeometryCalibrationTest,"MRFPS.Geometry.CalibrationOrdering",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FMRFPSGeometryCalibrationTest::RunTest(const FString& Parameters)
{
    uint32 Fence=0;
    TestFalse(TEXT("An unissued calibration cannot be acknowledged"),MRFPSGeometryWire::CalibrationAcknowledged(Fence,0));
    TestTrue(TEXT("First calibration gets its own expected server epoch"),MRFPSGeometryWire::AdvanceCalibrationFence(Fence,0));
    TestTrue(TEXT("A second local transform can be requested before the first ack"),MRFPSGeometryWire::AdvanceCalibrationFence(Fence,0));
    TestFalse(TEXT("The first ack cannot publish the newer transform"),MRFPSGeometryWire::CalibrationAcknowledged(Fence,1));
    TestTrue(TEXT("The second ack unlocks the second transform"),MRFPSGeometryWire::CalibrationAcknowledged(Fence,2));
    MRFPSGeometryWire::AdvanceCalibrationFence(Fence,2); // Tracking loss request.
    MRFPSGeometryWire::AdvanceCalibrationFence(Fence,2); // Recalibrate before loss ack.
    TestFalse(TEXT("A delayed tracking-loss ack cannot unlock the later calibration"),MRFPSGeometryWire::CalibrationAcknowledged(Fence,3));
    TestTrue(TEXT("Only the complete reliable request history reaches the latest fence"),MRFPSGeometryWire::CalibrationAcknowledged(Fence,4));
    Fence=MAX_uint32-1;
    TestFalse(TEXT("Calibration epochs never wrap into an old coordinate system"),MRFPSGeometryWire::AdvanceCalibrationFence(Fence,MAX_uint32-1));
    TestEqual(TEXT("Overflow rejection does not move the fence backwards"),Fence,MAX_uint32-1);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRFPSGeometryAuthorityTest,"MRFPS.Geometry.AuthorityBulletOcclusion",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FMRFPSGeometryAuthorityTest::RunTest(const FString& Parameters)
{
    if (!GEngine) return false;
    const auto Init=UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true).CreateNavigation(false)
        .CreateAISystem(false).ShouldSimulatePhysics(false).EnableTraceCollision(true).CreateFXSystem(false).SetTransactional(false);
    UWorld* World=UWorld::CreateWorld(EWorldType::Game,false,NAME_None,nullptr,true,ERHIFeatureLevel::Num,&Init);
    if (!TestNotNull(TEXT("Authority geometry fixture creates a real world"),World)) return false;
    GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
    auto* GameState=World->SpawnActor<AMRFPSGameState>(); World->SetGameState(GameState);
    // Synthetic test registration exercises authority math, not physical shared-space acquisition.
    GameState->bSharedFloorConfirmed=true; GameState->Layout.bCalibrated=true;
    GameState->SharedFloorRevision=GameState->Layout.RegistrationRevision=1;
    World->InitializeActorsForPlay(FURL()); World->BeginPlay(); World->GetWorldSettings()->NotifyBeginPlay(); GameState->Phase=EMRFPSMatchPhase::Playing;
    const auto Spawn=[World](int32 Slot,double X,float Yaw) {
        auto* P=World->SpawnActor<AMRFPSPawn>(); auto* PS=World->SpawnActor<AMRFPSPlayerState>();
        PS->Slot=Slot; PS->bAuthenticated=true; PS->AuthExpiresAt=FDateTime::UtcNow().ToUnixTimestamp()+3600; P->SetPlayerState(PS);
        P->Haptics->bAutoConnectTracking=false; P->bCalibrated=true; P->bStageFloorReferenced=true; P->GeometryCalibrationGeneration=1;
        P->Pose.Head=FVector(X,0,170); P->Pose.Left=P->Pose.Right=FVector(X+(Slot? -40:40),0,130);
        P->Pose.HeadRotation=P->Pose.LeftRotation=P->Pose.RightRotation=FRotator(0,Yaw,0);
        P->Pose.bHeadTracked=P->Pose.bLeftTracked=P->Pose.bRightTracked=true; P->Pose.bDesktop=false; P->Pose.RightFingers.bValid=true;
        P->LastPoseReceived=World->GetTimeSeconds(); P->OnRepPose(); return P;
    };
    auto* Shooter=Spawn(0,-150,0); auto* Target=Spawn(1,150,180);
    auto* Geometry=World->GetSubsystem<UMRFPSGeometryWorld>();
    if (TestNotNull(TEXT("The actual world subsystem is present"),Geometry)) {
        auto H=MRFPSGeometryTests::Header(); H.CaptureMs=uint32(World->GetTimeSeconds()*1000);
        TArray<uint8> Bytes; auto Mesh=MRFPSGeometryTests::Wall(); Mesh.Encode(H,Bytes);
        auto* PS=Shooter->GetPlayerState<AMRFPSPlayerState>(); PS->GeometryStreamId=H.StreamId;
        const int32 ActorsBefore=[World]() { int32 Count=0; for (TActorIterator<AActor> It(World);It;++It) ++Count; return Count; }();
        TestTrue(TEXT("Authenticated calibrated owner commits a measured surface atomically"),Geometry->Commit(Shooter,H,MoveTemp(Mesh)));
        auto WrongLayout=H; ++WrongLayout.LayoutRevision;
        TestFalse(TEXT("A geometry packet from a different layout epoch cannot commit"),Geometry->Commit(Shooter,WrongLayout,MRFPSGeometryTests::Wall()));
        auto OutOfBounds=MRFPSGeometryTests::Wall(230);
        TestFalse(TEXT("Wire-valid geometry outside the configured rectangular floor cannot commit"),Geometry->Commit(Shooter,H,MoveTemp(OutOfBounds)));
        int32 ActorsAfter=0; for (TActorIterator<AActor> It(World);It;++It) ++ActorsAfter;
        TestEqual(TEXT("Authority geometry creates no collision/render actors"),ActorsAfter,ActorsBefore);
        Shooter->ServerFire(); TestEqual(TEXT("A real server bullet is blocked before target damage"),Target->Health,100.f);
        FVector Hit;
        PS->bInspector=true; TestFalse(TEXT("Inspector role cannot retain a physical obstacle"),Geometry->Trace(FVector(-100,0,130),FVector(100,0,130),Hit)); PS->bInspector=false;
        ++Shooter->GeometryCalibrationGeneration;
        TestFalse(TEXT("Recalibration immediately invalidates the old coordinate epoch"),Geometry->Trace(FVector(-100,0,130),FVector(100,0,130),Hit)); --Shooter->GeometryCalibrationGeneration;
        Geometry->RemoveOwner(Shooter); World->Tick(LEVELTICK_All,.2f);
        Shooter->LastPoseReceived=Target->LastPoseReceived=World->GetTimeSeconds();
        Shooter->ServerFire(); TestEqual(TEXT("Removal restores the existing authoritative body hit"),Target->Health,80.f);
        H.StreamId=2; H.CaptureMs=uint32(World->GetTimeSeconds()*1000); PS->GeometryStreamId=2; Mesh=MRFPSGeometryTests::Wall(200); Mesh.Encode(H,Bytes);
        TestTrue(TEXT("Replacement geometry uses a new server generation"),Geometry->Commit(Shooter,H,MoveTemp(Mesh)));
        World->Tick(LEVELTICK_All,.2f); Shooter->LastPoseReceived=Target->LastPoseReceived=World->GetTimeSeconds();
        Shooter->ServerFire(); TestEqual(TEXT("A surface behind the target cannot override its nearer Chaos hit"),Target->Health,60.f);
        PS->AuthExpiresAt=FDateTime::UtcNow().ToUnixTimestamp()-1;
        TestFalse(TEXT("Expired ownership cannot keep blocking bullets between ticks"),Geometry->Trace(FVector(175,0,130),FVector(225,0,130),Hit));
    }
    World->EndPlay(EEndPlayReason::EndPlayInEditor); World->DestroyWorld(false); GEngine->DestroyWorldContext(World);
    return true;
}

namespace MRFPSGeometryTests
{
// No socket/replay is opened. A concrete engine connection supplies the real
// NM_Client branch while exercising the actual world subsystem and its clock.
struct FLifetimeWorld
{
    UWorld* World=nullptr;
    AMRFPSPawn* Owner=nullptr;
    AMRFPSPlayerState* Player=nullptr;
    UMRFPSGeometryWorld* Geometry=nullptr;
    bool Create(bool Client)
    {
        const auto Init=UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(false).CreateNavigation(false)
            .CreateAISystem(false).ShouldSimulatePhysics(false).CreateFXSystem(false).SetTransactional(false);
        World=UWorld::CreateWorld(EWorldType::Game,false,NAME_None,nullptr,true,ERHIFeatureLevel::Num,&Init);
        if (!World) return false;
        GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
        auto* GS=World->SpawnActor<AMRFPSGameState>(); World->SetGameState(GS);
        GS->bSharedFloorConfirmed=true; GS->Layout.bCalibrated=true;
        GS->SharedFloorRevision=GS->Layout.RegistrationRevision=1;
        World->InitializeActorsForPlay(FURL()); World->BeginPlay(); World->GetWorldSettings()->NotifyBeginPlay();
        Owner=World->SpawnActor<AMRFPSPawn>(); Player=World->SpawnActor<AMRFPSPlayerState>();
        if (!Owner || !Player) return false;
        Player->Slot=0; Player->bAuthenticated=true; Player->AuthExpiresAt=FDateTime::UtcNow().ToUnixTimestamp()+3600;
        Player->GeometryStreamId=1; Owner->SetPlayerState(Player); Owner->Haptics->bAutoConnectTracking=false;
        Owner->bCalibrated=true; Owner->GeometryCalibrationGeneration=1;
        Owner->Pose.Head=FVector(0,0,170); Owner->Pose.Left=Owner->Pose.Right=FVector(40,0,130);
        Owner->Pose.bHeadTracked=true; Owner->Pose.bDesktop=false; Owner->LastPoseReceived=World->GetTimeSeconds();
        Geometry=World->GetSubsystem<UMRFPSGeometryWorld>();
        if (Client) {
            auto* Driver=NewObject<UDemoNetDriver>(); Driver->ServerConnection=NewObject<UDemoNetConnection>();
            World->SetNetDriver(Driver);
        }
        return Geometry!=nullptr && (World->GetNetMode()==NM_Client)==Client;
    }
    bool Commit(uint32 Stream,uint32 Sequence,uint32 Calibration=1)
    {
        auto H=Header(Sequence); H.StreamId=Stream; H.Calibration=Calibration;
        H.CaptureMs=uint32(World->GetTimeSeconds()*1000); TArray<uint8> Bytes; auto Mesh=Wall();
        if (!Mesh.Encode(H,Bytes)) return false;
        return Geometry->Commit(Owner,H,MoveTemp(Mesh));
    }
    ~FLifetimeWorld()
    {
        if (World) {
            World->SetNetDriver(nullptr);
            World->EndPlay(EEndPlayReason::EndPlayInEditor); World->DestroyWorld(false); GEngine->DestroyWorldContext(World);
        }
    }
};
class FLifetimeScenario final : public IAutomationLatentCommand
{
public:
    explicit FLifetimeScenario(FAutomationTestBase* InTest):Test(InTest) {}
    virtual bool Update() override
    {
        if (!bStarted) {
            if (!GEngine || !Receiver.Create(true) || !Authority.Create(false)) { Test->AddError(TEXT("Geometry lifetime worlds could not initialize")); return true; }
            Test->TestTrue(TEXT("Client and authority commit the same validated snapshot"),Receiver.Commit(1,1) && Authority.Commit(1,1));
            StartedAt=FPlatformTime::Seconds(); bStarted=true; return false;
        }
        // Game time stays paused: the production TTL must use monotonic time.
        if (FPlatformTime::Seconds()-StartedAt<MRFPSGeometryWire::SnapshotLifetime+.1) return false;
        Receiver.Geometry->Tick(0); Authority.Geometry->Tick(0);
        Test->TestEqual(TEXT("Stale receiver data is not presented as a live surface"),Receiver.Geometry->GetSafeStatus(),FString(TEXT("No live measured geometry")));
        Test->TestEqual(TEXT("A receiver removes stale display without retiring the server's live stream"),Receiver.Player->GeometryStreamId,uint32(1));
        Test->TestTrue(TEXT("A newer same-owner snapshot recovers after receiver-only expiry"),Receiver.Commit(1,2));
        Test->TestFalse(TEXT("Duplicate complete snapshot remains rejected after recovery"),Receiver.Commit(1,2));
        Test->TestEqual(TEXT("Authority expiry retires the stream even while game time is paused"),Authority.Player->GeometryStreamId,uint32(0));
        Test->TestFalse(TEXT("Expired authority data cannot block a shot"),Authority.Geometry->Trace(FVector(-100,0,130),FVector(100,0,130),Hit));
        Authority.Player->GeometryStreamId=1;
        Test->TestFalse(TEXT("A retired authority generation cannot reopen"),Authority.Commit(1,2));
        Authority.Player->GeometryStreamId=2;
        Test->TestTrue(TEXT("A new authority generation can publish after expiry"),Authority.Commit(2,1));
        Receiver.Owner->GeometryCalibrationGeneration=2; Receiver.Geometry->Tick(0);
        Test->TestFalse(TEXT("A calibration change retires the old stream rather than soft-expiring it"),Receiver.Commit(1,3,2));
        Receiver.Player->GeometryStreamId=2;
        Test->TestTrue(TEXT("New calibrated server generation can recover"),Receiver.Commit(2,1,2));
        Receiver.Player->bAuthenticated=false; Receiver.Geometry->Tick(0); Receiver.Player->bAuthenticated=true;
        Test->TestFalse(TEXT("Authentication revocation permanently closes its old generation"),Receiver.Commit(2,2,2));
        return true;
    }
private:
    FAutomationTestBase* Test;
    FLifetimeWorld Receiver,Authority;
    FVector Hit;
    double StartedAt=0;
    bool bStarted=false;
};
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRFPSGeometryLifetimeTest,"MRFPS.Geometry.ExpiryAndRecovery",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FMRFPSGeometryLifetimeTest::RunTest(const FString& Parameters)
{
    ADD_LATENT_AUTOMATION_COMMAND(MRFPSGeometryTests::FLifetimeScenario(this));
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRFPSAvatarGroundTest,"MRFPS.Geometry.AvatarGroundPreservesAuthority",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FMRFPSAvatarGroundTest::RunTest(const FString& Parameters)
{
    MRFPSGeometryTests::FLifetimeWorld F;
    if(!TestTrue(TEXT("A real client-mode world without sockets"),F.Create(true))) return false;
    auto H=MRFPSGeometryTests::Header(1);H.StreamId=1;H.Calibration=1;
    H.CaptureMs=uint32(F.World->GetTimeSeconds()*1000);
    FMRFPSGeometryMesh Mesh;Mesh.Vertices={FVector(-20,-20,15),FVector(20,-20,15),FVector(20,20,15),FVector(-20,20,15)};
    Mesh.Indices={0,1,2,0,2,3};TArray<uint8> Bytes;
    if(!TestTrue(TEXT("A measured horizontal platform is encoded and committed"),Mesh.Encode(H,Bytes)&&F.Geometry->Commit(F.Owner,H,MoveTemp(Mesh)))) return false;
    float Ground=-100;FVector Hit;
    TestTrue(TEXT("Client avatar can read a validated measured platform"),F.Geometry->SampleAvatarGround(FVector(0,0,15),Ground));
    TestEqual(TEXT("Foot support uses the actual measured surface height"),Ground,15.f);
    TestFalse(TEXT("Visual ground queries do not unlock client combat trace"),F.Geometry->Trace(FVector(0,0,50),FVector(0,0,-20),Hit));
    ++F.Owner->GeometryCalibrationGeneration;
    TestFalse(TEXT("An old calibration cannot hold a foot up between ticks"),F.Geometry->SampleAvatarGround(FVector(0,0,15),Ground));
    --F.Owner->GeometryCalibrationGeneration;
    F.Player->AuthExpiresAt=FDateTime::UtcNow().ToUnixTimestamp()-1;
    TestFalse(TEXT("Expired ownership immediately removes visual support"),F.Geometry->SampleAvatarGround(FVector(0,0,15),Ground));
    return true;
}
#endif
