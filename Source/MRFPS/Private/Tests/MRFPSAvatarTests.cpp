#if WITH_DEV_AUTOMATION_TESTS && !UE_SERVER
#include "MRFPSAvatar.h"
#include "MRFPSGame.h"
#include "Engine/Engine.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRFPSMannyTrackingTest,"MRFPS.Avatar.MannyTrackedIKAndLocomotion",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FMRFPSMannyTrackingTest::RunTest(const FString& Parameters)
{
    const auto Init=UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(false).CreateNavigation(false)
        .CreateAISystem(false).ShouldSimulatePhysics(false).CreateFXSystem(false).SetTransactional(false);
    UWorld* World=UWorld::CreateWorld(EWorldType::Game,false,NAME_None,nullptr,true,ERHIFeatureLevel::Num,&Init);
    if(!TestNotNull(TEXT("Isolated visual world"),World)) return false;
    GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
    auto* Owner=World->SpawnActor<AActor>();auto* Avatar=NewObject<UMRFPSAvatarComponent>(Owner);
    Owner->SetRootComponent(Avatar);Owner->AddInstanceComponent(Avatar);Avatar->RegisterComponent();
    if(!TestTrue(TEXT("The actual shipped Manny mesh and directional BlendSpace load"),Avatar->InitializeAvatar())) {
        World->DestroyWorld(false);GEngine->DestroyWorldContext(World);return false;
    }
    TestEqual(TEXT("UE5 default lightweight mannequin is used"),Avatar->GetSkinnedAsset()->GetName(),FString(TEXT("SKM_Manny_Simple")));
    FMRFPSArenaLayout Layout;FMRFPSPose Pose;Pose.bHeadTracked=Pose.bLeftTracked=Pose.bRightTracked=true;
    Pose.Head=FVector(0,0,170);Pose.Left=FVector(30,-30,125);Pose.Right=FVector(40,25,140);
    auto Update=[&](int32 Frames) { for(int32 I=0;I<Frames;++I) { ++Pose.Sequence;Avatar->UpdateTracking(Pose,Layout,1,1.f/60,false); } };
    Update(30);
    AddInfo(FString::Printf(TEXT("Standing tracked end-effector maximum error %.3f cm"),Avatar->GetTrackingErrorCm()));
    TestTrue(TEXT("Reachable measured head/hands converge within 8 cm"),Avatar->GetTrackingErrorCm()<8);
    TestTrue(TEXT("Measured right hand is reached"),FVector::Dist(Avatar->SolvedWorldPosition(TEXT("hand_r")),Pose.Right)<8);
    const float StandingScale=Avatar->GetBodyScale();const FVector StillFoot=Avatar->SolvedWorldPosition(TEXT("foot_l"));
    Pose.HeadRotation=FRotator(0,25,0);Update(20);
    TestTrue(TEXT("Turning the head without a step does not skate a planted foot"),FVector::Dist(StillFoot,Avatar->SolvedWorldPosition(TEXT("foot_l")))<4);
    Pose.Head.Z=120;Pose.Left.Z=90;Pose.Right.Z=105;Update(30);
    AddInfo(FString::Printf(TEXT("Crouched tracked end-effector maximum error %.3f cm"),Avatar->GetTrackingErrorCm()));
    TestEqual(TEXT("Crouching does not resize the person"),Avatar->GetBodyScale(),StandingScale);
    TestTrue(TEXT("Crouched head is solved near the measured position"),FVector::Dist(Avatar->SolvedWorldPosition(TEXT("head")),FVector(Pose.Head)-Pose.HeadRotation.RotateVector(FVector(0,0,8*StandingScale)))<8);
    const auto* Mesh=Cast<USkeletalMesh>(Avatar->GetSkinnedAsset());const auto& Ref=Mesh->GetRefSkeleton();
    double MaxStretch=0;
    for(const FName Name:{FName(TEXT("lowerarm_l")),FName(TEXT("hand_l")),FName(TEXT("calf_l")),FName(TEXT("foot_l"))}) {
        const int32 Index=Ref.FindBoneIndex(Name),Parent=Ref.GetParentIndex(Index);
        const double Original=Ref.GetRefBonePose()[Index].GetLocation().Size()*StandingScale;
        const double Solved=FVector::Dist(Avatar->SolvedWorldPosition(Name),Avatar->SolvedWorldPosition(Ref.GetBoneName(Parent)));
        MaxStretch=FMath::Max(MaxStretch,FMath::Abs(Solved-Original));
    }
    AddInfo(FString::Printf(TEXT("Maximum limb length deviation %.3f cm"),MaxStretch));
    TestTrue(TEXT("Crouching preserves physical limb lengths within solver tolerance"),MaxStretch<2);
    Pose.Head.Z=220;Pose.Left.Z=175;Pose.Right.Z=190;Update(30);
    TestTrue(TEXT("A rising head carries the inferred body without stretching the spine"),FVector::Dist(Avatar->SolvedWorldPosition(TEXT("head")),FVector(Pose.Head)-Pose.HeadRotation.RotateVector(FVector(0,0,8*StandingScale)))<8);
    Pose.Head.Z=170;Pose.Left.Z=125;Pose.Right.Z=140;
    for(int32 Frame=0;Frame<90;++Frame) {
        Pose.Head.X+=2;Pose.Left.X+=2;Pose.Right.X+=2;Update(1);
    }
    TestTrue(TEXT("Measured translation drives the walk/jog blend"),Avatar->GetLocomotionSpeed()>100&&Avatar->GetLocomotionSpeed()<150);
    bool Finite=true;for(const auto& Transform:Avatar->BoneSpaceTransforms) Finite=Finite&&!Transform.ContainsNaN()&&Transform.GetRotation().IsNormalized();
    TestTrue(TEXT("All locomotion and IK bone transforms remain finite and normalized"),Finite);
    World->Tick(LEVELTICK_TimeOnly,.6f);
    Avatar->UpdateTracking(Pose,Layout,1,1.f/60,false);
    TestFalse(TEXT("A stopped pose stream hides its last body"),Avatar->IsVisible());
    Avatar->UpdateTracking(Pose,Layout,1,1.f/60,false);
    TestFalse(TEXT("A stale pose cannot resurrect itself on the next render frame"),Avatar->IsVisible());
    Update(1);TestTrue(TEXT("A newly measured pose restores the body"),Avatar->IsVisible());
    Pose.bHeadTracked=false;Update(1);TestFalse(TEXT("Lost tracking hides the inferred body"),Avatar->IsVisible());
    World->DestroyWorld(false);GEngine->DestroyWorldContext(World);return true;
}
#endif
