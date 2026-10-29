#if WITH_DEV_AUTOMATION_TESTS && !UE_SERVER
#include "MRFPSAvatar.h"
#include "MRFPSGame.h"
#include "Engine/Engine.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include <cfloat>

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
    Update(45);
    double MinRelative[2]={DBL_MAX,DBL_MAX},MaxRelative[2]={-DBL_MAX,-DBL_MAX};
    double MaxFrameTravel[2]={0,0},MinHeight[2]={DBL_MAX,DBL_MAX},MaxHeight[2]={-DBL_MAX,-DBL_MAX};
    FVector PreviousFoot[2]={Avatar->SolvedWorldPosition(TEXT("foot_l")),Avatar->SolvedWorldPosition(TEXT("foot_r"))};
    double MinLead=DBL_MAX,MaxLead=-DBL_MAX;
    for(int32 Frame=0;Frame<150;++Frame) {
        Pose.Head.X+=2;Pose.Left.X+=2;Pose.Right.X+=2;Update(1);
        FVector Foot[2]={Avatar->SolvedWorldPosition(TEXT("foot_l")),Avatar->SolvedWorldPosition(TEXT("foot_r"))};
        for(int32 Side=0;Side<2;++Side) {
            MaxFrameTravel[Side]=FMath::Max(MaxFrameTravel[Side],FVector::Dist(Foot[Side],PreviousFoot[Side]));
            MinHeight[Side]=FMath::Min(MinHeight[Side],Foot[Side].Z);MaxHeight[Side]=FMath::Max(MaxHeight[Side],Foot[Side].Z);
            PreviousFoot[Side]=Foot[Side];
        }
        const double Left=Foot[0].X-Pose.Head.X;
        const double Right=Foot[1].X-Pose.Head.X;
        MinRelative[0]=FMath::Min(MinRelative[0],Left);MaxRelative[0]=FMath::Max(MaxRelative[0],Left);
        MinRelative[1]=FMath::Min(MinRelative[1],Right);MaxRelative[1]=FMath::Max(MaxRelative[1],Right);
        MinLead=FMath::Min(MinLead,Left-Right);MaxLead=FMath::Max(MaxLead,Left-Right);
    }
    TestTrue(TEXT("Measured translation drives the walk/jog blend"),Avatar->GetLocomotionSpeed()>100&&Avatar->GetLocomotionSpeed()<150);
    AddInfo(FString::Printf(TEXT("Forward walk foot travel relative to body: left %.2f cm, right %.2f cm; left lead %.2f..%.2f cm; max frame travel %.2f/%.2f cm; lift %.2f/%.2f cm"),
        MaxRelative[0]-MinRelative[0],MaxRelative[1]-MinRelative[1],MinLead,MaxLead,MaxFrameTravel[0],MaxFrameTravel[1],MaxHeight[0]-MinHeight[0],MaxHeight[1]-MinHeight[1]));
    TestTrue(TEXT("Both feet step relative to the translated body"),MaxRelative[0]-MinRelative[0]>10 && MaxRelative[1]-MinRelative[1]>10);
    TestTrue(TEXT("Left and right feet alternate their forward lead"),MinLead< -5 && MaxLead>5);
    TestTrue(TEXT("Foot steps do not teleport between adjacent frames"),MaxFrameTravel[0]<10 && MaxFrameTravel[1]<10);
    // Observers receive poses at 30 Hz while rendering at 60 Hz. Exercise a
    // sideways movement with repeated samples, rather than 60 Hz ideal input.
    Pose.Head=FVector(0,0,170);Pose.Left=FVector(30,-30,125);Pose.Right=FVector(40,25,140);Pose.HeadRotation=FRotator::ZeroRotator;
    Update(45);
    double SideMin[2]={DBL_MAX,DBL_MAX},SideMax[2]={-DBL_MAX,-DBL_MAX},SideMaxFrame=0;
    FVector SidePrevious[2]={Avatar->SolvedWorldPosition(TEXT("foot_l")),Avatar->SolvedWorldPosition(TEXT("foot_r"))};
    for(int32 Frame=0;Frame<150;++Frame) {
        if(Frame%2==0) { Pose.Head.Y+=4;Pose.Left.Y+=4;Pose.Right.Y+=4;++Pose.Sequence; }
        Avatar->UpdateTracking(Pose,Layout,1,1.f/60,false);
        const FVector Foot[2]={Avatar->SolvedWorldPosition(TEXT("foot_l")),Avatar->SolvedWorldPosition(TEXT("foot_r"))};
        for(int32 Side=0;Side<2;++Side) {
            SideMin[Side]=FMath::Min(SideMin[Side],Foot[Side].Y-Pose.Head.Y);
            SideMax[Side]=FMath::Max(SideMax[Side],Foot[Side].Y-Pose.Head.Y);
            SideMaxFrame=FMath::Max(SideMaxFrame,FVector::Dist(Foot[Side],SidePrevious[Side]));SidePrevious[Side]=Foot[Side];
        }
    }
    AddInfo(FString::Printf(TEXT("30 Hz sideways walk: relative foot travel %.2f/%.2f cm; max frame travel %.2f cm"),
        SideMax[0]-SideMin[0],SideMax[1]-SideMin[1],SideMaxFrame));
    TestTrue(TEXT("Repeated network samples still animate both feet sideways"),SideMax[0]-SideMin[0]>8 && SideMax[1]-SideMin[1]>8 && Avatar->GetLocomotionSpeed()>80);
    TestTrue(TEXT("Replicated sideways foot movement remains continuous"),SideMaxFrame<10);
    Update(60);
    TestTrue(TEXT("Locomotion stops when the tracked body stops"),Avatar->GetLocomotionSpeed()<1);
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
