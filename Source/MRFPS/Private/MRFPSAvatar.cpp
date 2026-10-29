#include "MRFPSAvatar.h"
#include "MRFPSGame.h"
#include "MRFPSGeometryWorld.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#if !UE_SERVER
#include "Animation/AnimSequence.h"
#include "Animation/BlendSpace.h"
#include "Animation/Skeleton.h"
#include "Core/PBIKSolver.h"
#include "Core/PBIKBody.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogMRFPSAvatar,Log,All);

struct FMRFPSAvatarRig
{
#if !UE_SERVER
    FPBIKSolver Solver;
    TArray<FTransform> Reference;
    TArray<FTransform> Global;
    TArray<int32> AnimationBones;
    int32 Bone[5]={INDEX_NONE,INDEX_NONE,INDEX_NONE,INDEX_NONE,INDEX_NONE};
    int32 Effector[5]={INDEX_NONE,INDEX_NONE,INDEX_NONE,INDEX_NONE,INDEX_NONE};
    int32 FingerBone[2][5][3];
    FVector FingerFlexAxis[2][5][3];
    FVector FingerSplayAxis[2][5];
    FQuat TrackingToBone[3];
    FVector Feet[2]={FVector::ZeroVector,FVector::ZeroVector};
    FVector FootGoals[2]={FVector::ZeroVector,FVector::ZeroVector};
    float FootYaw[2]={0,0};
    bool Planted[2]={false,false};
    bool FootGoalValid[2]={false,false};
    FVector PreviousHead=FVector::ZeroVector;
    FVector SmoothHead=FVector::ZeroVector;
    FVector Velocity=FVector::ZeroVector;
    FQuat SmoothRotations[3];
    FVector SmoothHands[2];
    FMRFPSPose LastVisualPose;
    FMRFPSFingerPose LastFingers[2];
    FVector LastHandPosition[2];
    FVector HeadAtLastHand[2];
    FRotator LastHandRotation[2];
    FRotator LastWeaponRotation;
    bool HasHand[2]={false,false};
    bool HasVisualPose=false;
    bool HoldingPose=false;
    uint32 LastSequence=0;
    uint32 Calibration=MAX_uint32;
    uint32 LayoutRevision=0;
    double LastReceipt=-100;
    double Phase=0;
    double IdleTime=0;
    float BodyYaw=0;
    float RefEyeHeight=170;
    int32 Triangle=INDEX_NONE;
    bool Initialized=false;
    bool HeightCalibrated=false;
    bool SeenSequence=false;
    bool WasLocal=false;
#endif
};

UMRFPSAvatarComponent::UMRFPSAvatarComponent()
{
    PrimaryComponentTick.bCanEverTick=false;
    SetCollisionEnabled(ECollisionEnabled::NoCollision);
    SetGenerateOverlapEvents(false);
    SetCastShadow(true);
    // Visibility is selected per local viewer by the pawn. Inspectors render
    // this mesh; player views see the physical participants through passthrough.
    SetOwnerNoSee(false);
    BoundsScale=2;
}
UMRFPSAvatarComponent::~UMRFPSAvatarComponent()=default;
void UMRFPSAvatarComponent::BeginPlay() { Super::BeginPlay(); }
bool UMRFPSAvatarComponent::IsAvatarReady() const { return Rig.IsValid(); }
bool UMRFPSAvatarComponent::GetVisualPose(FMRFPSPose& OutPose) const
{
#if !UE_SERVER
    if (Rig && Rig->HasVisualPose) { OutPose=Rig->LastVisualPose; return true; }
#endif
    return false;
}

bool UMRFPSAvatarComponent::InitializeAvatar()
{
#if !UE_SERVER
    if(Rig) return true;
    if(GetWorld() && GetWorld()->GetNetMode()==NM_DedicatedServer) return false;
    auto* Mesh=LoadObject<USkeletalMesh>(nullptr,TEXT("/Game/Characters/Mannequins/Meshes/SKM_Manny_Simple.SKM_Manny_Simple"));
    Locomotion=LoadObject<UBlendSpace>(nullptr,TEXT("/Game/Characters/Mannequins/Anims/Unarmed/BS_Idle_Walk_Run.BS_Idle_Walk_Run"));
    Idle=LoadObject<UAnimSequence>(nullptr,TEXT("/Game/Characters/Mannequins/Anims/Unarmed/MM_Idle.MM_Idle"));
    if(!Mesh || !Locomotion || !Idle) { UE_LOG(LogMRFPSAvatar,Error,TEXT("MRFPS_AVATAR_ASSETS_MISSING"));return false; }
    SetSkinnedAssetAndUpdate(Mesh);
    const auto& Ref=Mesh->GetRefSkeleton();
    auto Candidate=MakeShared<FMRFPSAvatarRig>();
    Candidate->Reference=Ref.GetRefBonePose();Candidate->Global=Candidate->Reference;
    const auto& AnimationRef=Idle->GetSkeleton()->GetReferenceSkeleton();
    for(int32 I=0;I<Ref.GetNum();++I) {
        const int32 Parent=Ref.GetParentIndex(I);
        if(Parent>=0) Candidate->Global[I]=Candidate->Reference[I]*Candidate->Global[Parent];
        Candidate->Solver.AddBone(Ref.GetBoneName(I),Parent,Candidate->Global[I].GetLocation(),Candidate->Global[I].GetRotation(),Ref.GetBoneName(I)==TEXT("pelvis"));
        Candidate->AnimationBones.Add(AnimationRef.FindBoneIndex(Ref.GetBoneName(I)));
    }
    const FName Names[]={TEXT("head"),TEXT("hand_l"),TEXT("hand_r"),TEXT("foot_l"),TEXT("foot_r")};
    for(int32 I=0;I<5;++I) {
        Candidate->Bone[I]=Ref.FindBoneIndex(Names[I]);
        Candidate->Effector[I]=Candidate->Solver.AddEffector(Names[I]);
        if(Candidate->Bone[I]==INDEX_NONE||Candidate->Effector[I]==INDEX_NONE) return false;
    }
    const TCHAR* FingerNames[]={TEXT("thumb"),TEXT("index"),TEXT("middle"),TEXT("ring"),TEXT("pinky")};
    for (int32 Hand=0;Hand<2;++Hand) {
        const TCHAR* Suffix=Hand==0?TEXT("l"):TEXT("r");
        for (int32 Finger=0;Finger<5;++Finger) {
            Candidate->FingerSplayAxis[Hand][Finger]=FVector::ZeroVector;
            for (int32 Joint=0;Joint<3;++Joint) {
                Candidate->FingerBone[Hand][Finger][Joint]=Ref.FindBoneIndex(*FString::Printf(TEXT("%s_%02d_%s"),FingerNames[Finger],Joint+1,Suffix));
                Candidate->FingerFlexAxis[Hand][Finger][Joint]=FVector::ZeroVector;
            }
        }
        const int32 Middle=Candidate->FingerBone[Hand][2][0],Index=Candidate->FingerBone[Hand][1][0],Little=Candidate->FingerBone[Hand][4][0];
        if (Middle==INDEX_NONE || Index==INDEX_NONE || Little==INDEX_NONE) continue;
        const FVector Forward=(Candidate->Global[Middle].GetLocation()-Candidate->Global[Candidate->Bone[Hand+1]].GetLocation()).GetSafeNormal();
        const FVector Across=(Candidate->Global[Index].GetLocation()-Candidate->Global[Little].GetLocation()).GetSafeNormal();
        FVector Palm=FVector::CrossProduct(Forward,Across).GetSafeNormal();
        if (FVector::DotProduct(Palm,FVector::DownVector)<0) Palm=-Palm;
        if (Palm.IsNearlyZero()) Palm=FVector::DownVector;
        for (int32 Finger=0;Finger<5;++Finger) for (int32 Joint=0;Joint<3;++Joint) {
            const int32 Bone=Candidate->FingerBone[Hand][Finger][Joint];
            const int32 Adjacent=Candidate->FingerBone[Hand][Finger][Joint<2?Joint+1:Joint-1];
            if (Bone==INDEX_NONE || Adjacent==INDEX_NONE) continue;
            FVector Direction=Joint<2 ? Candidate->Global[Adjacent].GetLocation()-Candidate->Global[Bone].GetLocation() :
                Candidate->Global[Bone].GetLocation()-Candidate->Global[Adjacent].GetLocation();
            Direction=Direction.GetSafeNormal();
            const FVector Flex=FVector::CrossProduct(Direction,Palm).GetSafeNormal();
            Candidate->FingerFlexAxis[Hand][Finger][Joint]=Candidate->Global[Bone].InverseTransformVectorNoScale(Flex).GetSafeNormal();
            if (Joint==0) Candidate->FingerSplayAxis[Hand][Finger]=Candidate->Global[Bone].InverseTransformVectorNoScale(Palm).GetSafeNormal();
        }
    }
    if(!Candidate->Solver.Initialize()) return false;
    const auto Configure=[&](const TCHAR* Name,float Position,float Rotation) {
        const int32 Index=Ref.FindBoneIndex(Name);
        if(Index>=0) if(auto* Settings=Candidate->Solver.GetBoneSettings(Index)) {
            Settings->PositionStiffness=Position;Settings->RotationStiffness=Rotation;
        }
    };
    Configure(TEXT("pelvis"),.65f,.8f);
    Configure(TEXT("spine_01"),.4f,.65f);Configure(TEXT("spine_02"),.35f,.5f);
    Configure(TEXT("spine_03"),.25f,.4f);Configure(TEXT("spine_04"),.15f,.3f);
    // Manny's knees bend around local Z. No translational stretch is allowed.
    for(const TCHAR* Name:{TEXT("calf_l"),TEXT("calf_r")}) {
        const int32 Index=Ref.FindBoneIndex(Name);
        if(Index>=0) if(auto* S=Candidate->Solver.GetBoneSettings(Index)) { S->bUsePreferredAngles=true;S->PreferredAngles=FRotator(0,45,0); }
    }
    const FQuat MeshForward(FRotator(0,-90,0));
    Candidate->TrackingToBone[0]=MeshForward*Candidate->Global[Candidate->Bone[0]].GetRotation();
    for(int32 Hand=0;Hand<2;++Hand) {
        const int32 Index=Candidate->Bone[Hand+1],Parent=Ref.GetParentIndex(Index);
        const FVector Direction=MeshForward.RotateVector(Candidate->Global[Index].GetLocation()-Candidate->Global[Parent].GetLocation()).GetSafeNormal();
        const FQuat Aim=FRotationMatrix::MakeFromXZ(Direction,FVector::UpVector).ToQuat();
        Candidate->TrackingToBone[Hand+1]=Aim.Inverse()*MeshForward*Candidate->Global[Index].GetRotation();
    }
    Candidate->RefEyeHeight=Candidate->Global[Candidate->Bone[0]].GetLocation().Z+8;
    Rig=MoveTemp(Candidate);
    UE_LOG(LogMRFPSAvatar,Display,TEXT("MRFPS_AVATAR_READY model=MannySimple bones=%d locomotion=UE5BlendSpace solver=PBIK tracked=head,hands inferred=pelvis,feet"),Ref.GetNum());
    return true;
#else
    return false;
#endif
}

void UMRFPSAvatarComponent::UpdateTracking(const FMRFPSPose& InputPose,const FMRFPSArenaLayout& Layout,uint32 Calibration,float Dt,bool LocalView)
{
#if !UE_SERVER
    if(!Rig || !InputPose.IsSane() || !Layout.IsSane() || !FMath::IsFinite(Dt) || Dt<=0) return;
    auto& R=*Rig;
    const double Now=GetWorld()?GetWorld()->GetTimeSeconds():0;
    const bool NewPose=!R.SeenSequence || InputPose.Sequence!=R.LastSequence;
    if(NewPose) { R.SeenSequence=true;R.LastSequence=InputPose.Sequence;R.LastReceipt=Now; }
    const bool Fresh=LocalView || Now-R.LastReceipt<.4;
    if (!InputPose.bHeadTracked || !Fresh) {
        SetVisibility(R.HasVisualPose);
        R.HoldingPose=R.HasVisualPose; R.Velocity=FVector::ZeroVector; LocomotionSpeed=0;
        return;
    }
    SetVisibility(true);
    FMRFPSPose Pose=InputPose;
    if (Calibration!=R.Calibration || Layout.Revision!=R.LayoutRevision) {
        for (int32 Hand=0;Hand<2;++Hand) { R.HasHand[Hand]=false; R.LastFingers[Hand].bValid=false; }
    }
    const auto RetainHand=[&](int32 Hand,bool& Tracked,FVector_NetQuantize10& Position,
        FRotator& HandRotation,FRotator* WeaponRotation,FMRFPSFingerPose& Fingers) {
        if (Tracked) {
            R.HasHand[Hand]=true; R.LastHandPosition[Hand]=Position; R.HeadAtLastHand[Hand]=Pose.Head;
            R.LastHandRotation[Hand]=HandRotation;
            if (WeaponRotation) R.LastWeaponRotation=*WeaponRotation;
        } else if (R.HasHand[Hand]) {
            // Keep the last measured hand offset to the still-tracked head so
            // the displayed arm does not detach as the player moves.
            Position=R.LastHandPosition[Hand]+(FVector(Pose.Head)-R.HeadAtLastHand[Hand]);
            HandRotation=R.LastHandRotation[Hand];
            if (WeaponRotation) *WeaponRotation=R.LastWeaponRotation;
            Tracked=true;
        }
        if (Fingers.bValid) R.LastFingers[Hand]=Fingers;
        else if (R.LastFingers[Hand].bValid && Tracked) Fingers=R.LastFingers[Hand];
    };
    RetainHand(0,Pose.bLeftTracked,Pose.Left,Pose.LeftRotation,nullptr,Pose.LeftFingers);
    RetainHand(1,Pose.bRightTracked,Pose.Right,Pose.RightHandRotation,&Pose.RightRotation,Pose.RightFingers);
    const bool WasHolding=R.HoldingPose; R.HoldingPose=false;
    const FVector RawHead=Pose.Head;
    const FQuat Rotations[]={Pose.HeadRotation.Quaternion(),Pose.LeftRotation.Quaternion(),Pose.RightHandRotation.Quaternion()};
    const FVector Hands[]={FVector(Pose.Left),FVector(Pose.Right)};
    const bool Reset=!R.Initialized || Calibration!=R.Calibration || Layout.Revision!=R.LayoutRevision ||
        FVector::DistSquared(R.SmoothHead,RawHead)>FMath::Square(120.);
    const bool HeightCalibration=!R.HeightCalibrated || Calibration!=R.Calibration;
    Dt=FMath::Clamp(Dt,.001f,.05f);
    if(Reset) {
        R.Initialized=true;R.Calibration=Calibration;R.LayoutRevision=Layout.Revision;R.PreviousHead=R.SmoothHead=RawHead;
        R.Velocity=FVector::ZeroVector;R.BodyYaw=Pose.HeadRotation.Yaw;R.Phase=0;
        for(int32 I=0;I<3;++I) R.SmoothRotations[I]=Rotations[I];
        for(int32 I=0;I<2;++I) { R.SmoothHands[I]=Hands[I];R.Planted[I]=false;R.FootGoalValid[I]=false; }
        // Standing height is fixed for this calibration. Crouching changes the
        // pelvis/leg solution, never the model scale or physical world scale.
        if(HeightCalibration) { BodyScale=FMath::Clamp(float((RawHead.Z-Layout.Center.Z)/R.RefEyeHeight),.75f,1.25f);R.HeightCalibrated=true; }
    }
    const float Blend=LocalView?1.f:1.f-FMath::Exp(-28*Dt);
    R.SmoothHead=FMath::Lerp(R.SmoothHead,RawHead,double(Blend));
    for(int32 I=0;I<3;++I) R.SmoothRotations[I]=FQuat::Slerp(R.SmoothRotations[I],Rotations[I],Blend).GetNormalized();
    for(int32 I=0;I<2;++I) R.SmoothHands[I]=FMath::Lerp(R.SmoothHands[I],Hands[I],double(Blend));
    FVector Velocity=WasHolding?FVector::ZeroVector:(R.SmoothHead-R.PreviousHead)/Dt;Velocity.Z=0;Velocity=Velocity.GetClampedToMaxSize(650);
    R.PreviousHead=R.SmoothHead;R.Velocity=FMath::VInterpTo(R.Velocity,Velocity,Dt,12);
    LocomotionSpeed=R.Velocity.Size();
    const float HeadYaw=R.SmoothRotations[0].Rotator().Yaw;
    const float HeadBody=FMath::FindDeltaAngleDegrees(R.BodyYaw,HeadYaw);
    float DesiredYaw=R.BodyYaw;
    if(LocomotionSpeed>12 || FMath::Abs(HeadBody)>55) DesiredYaw=HeadYaw;
    R.BodyYaw=FMath::FixedTurn(R.BodyYaw,DesiredYaw,Dt*180);
    const float RemainingHeadTurn=FMath::FindDeltaAngleDegrees(R.BodyYaw,HeadYaw);
    if(FMath::Abs(RemainingHeadTurn)>75) R.BodyYaw=HeadYaw-FMath::Clamp(RemainingHeadTurn,-75.f,75.f);
    const FQuat ComponentRotation(FRotator(0,R.BodyYaw-90,0));
    const FVector Forward=FRotator(0,R.BodyYaw,0).Vector();
    float BodyGround=Layout.Center.Z;
    auto* Geometry=GetWorld()?GetWorld()->GetSubsystem<UMRFPSGeometryWorld>():nullptr;
    if(Geometry) {
        float Measured;
        FVector Probe=R.SmoothHead;Probe.Z-=R.RefEyeHeight*BodyScale;
        if(Geometry->SampleAvatarGround(Probe,Measured) && Measured<R.SmoothHead.Z-65) BodyGround=Measured;
    }
    FVector Root=R.SmoothHead-Forward*5;
    Root.Z=FMath::Max(double(BodyGround),R.SmoothHead.Z-R.RefEyeHeight*BodyScale);
    const FTransform BodyTransform(ComponentRotation,Root,FVector(BodyScale));
    SetWorldTransform(BodyTransform);
    if(LocalView!=R.WasLocal || Reset) { if(LocalView) HideBoneByName(TEXT("head"),PBO_None);else UnHideBoneByName(TEXT("head"));R.WasLocal=LocalView; }
    const auto* Mesh=Cast<USkeletalMesh>(GetSkinnedAsset());if(!Mesh) return;
    const auto& Ref=Mesh->GetRefSkeleton();

    const FVector LocalVelocity=FRotator(0,R.BodyYaw,0).UnrotateVector(R.Velocity);
    const double Direction=FMath::RadiansToDegrees(FMath::Atan2(LocalVelocity.Y,LocalVelocity.X));
    FVector Input=FVector::ZeroVector;
    for(int32 Axis=0;Axis<2;++Axis) {
        const auto& Parameter=Locomotion->GetBlendParameter(Axis);
        Input[Axis]=Parameter.Min<0?Direction:FMath::Clamp(double(LocomotionSpeed/BodyScale),double(Parameter.Min),double(Parameter.Max));
    }
    R.IdleTime+=Dt;
    const float Stride=FMath::Lerp(150.f,240.f,FMath::Clamp((LocomotionSpeed-150)/300,0.f,1.f))*BodyScale;
    if(LocomotionSpeed>4) R.Phase=FMath::Frac(R.Phase+LocomotionSpeed*Dt/Stride);
    TArray<FBlendSampleData> Samples;
    Locomotion->GetSamplesFromBlendInput(Input,Samples,R.Triangle,true);
    if(Samples.IsEmpty()) { FBlendSampleData Sample;Sample.Animation=Idle;Sample.TotalWeight=1;Samples.Add(Sample); }
    BoneSpaceTransforms=R.Reference;
    for(int32 I=0;I<Ref.GetNum();++I) {
        if(R.AnimationBones[I]<0) continue;
        FQuat Rotation(0,0,0,0);FVector Position=FVector::ZeroVector,Scale=FVector::ZeroVector;double WeightSum=0;
        FQuat Hemisphere=FQuat::Identity;
        for(const auto& Sample:Samples) {
            auto* Animation=Sample.Animation.Get();const double Weight=FMath::Max(0.f,Sample.GetClampedWeight());
            if(!Animation||Weight<.0001) continue;
            const bool IsIdle=Animation==Idle || LocomotionSpeed<4;
            const double Time=IsIdle?FMath::Fmod(R.IdleTime,double(Animation->GetPlayLength())):R.Phase*Animation->GetPlayLength();
            FTransform T;Animation->GetBoneTransform(T,FSkeletonPoseBoneIndex(R.AnimationBones[I]),FAnimExtractContext(Time,false),false);
            FQuat Q=T.GetRotation();if(WeightSum==0) Hemisphere=Q;else if((Hemisphere|Q)<0) Q=Q*-1;
            Rotation=Rotation+Q*Weight;Position+=T.GetLocation()*Weight;Scale+=T.GetScale3D()*Weight;WeightSum+=Weight;
        }
        if(WeightSum>0 && Rotation.SizeSquared()>.001) BoneSpaceTransforms[I]=FTransform(Rotation.GetNormalized(),Position/WeightSum,Scale/WeightSum);
    }
    // World motion is measured by the headset, not animation root motion.
    BoneSpaceTransforms[0]=R.Reference[0];
    const float Crouch=FMath::Clamp(float((R.RefEyeHeight*BodyScale-(R.SmoothHead.Z-BodyGround))/BodyScale),0.f,120.f);
    const int32 Pelvis=Ref.FindBoneIndex(TEXT("pelvis"));
    if(Pelvis>=0) BoneSpaceTransforms[Pelvis].AddToTranslation(FVector(-Crouch*.12,0,-Crouch*.95));
    for(int32 I=0;I<Ref.GetNum();++I) {
        const int32 Parent=Ref.GetParentIndex(I);
        R.Global[I]=Parent<0?BoneSpaceTransforms[I]:BoneSpaceTransforms[I]*R.Global[Parent];
        R.Solver.SetBoneTransform(I,R.Global[I]);
    }
    FTransform Goals[5];
    Goals[0]=FTransform(BodyTransform.InverseTransformRotation(R.SmoothRotations[0]*R.TrackingToBone[0]),
        BodyTransform.InverseTransformPosition(R.SmoothHead-R.SmoothRotations[0].RotateVector(FVector(0,0,8*BodyScale))));
    for(int32 Hand=0;Hand<2;++Hand) Goals[Hand+1]=FTransform(BodyTransform.InverseTransformRotation(R.SmoothRotations[Hand+1]*R.TrackingToBone[Hand+1]),BodyTransform.InverseTransformPosition(R.SmoothHands[Hand]));
    for(int32 Foot=0;Foot<2;++Foot) {
        const int32 Index=R.Bone[Foot+3];const FTransform Animated=R.Global[Index]*BodyTransform;
        FVector Target=Animated.GetLocation();
        float Ground=Layout.Center.Z;
        if(Geometry) {
            // Only registered measured surfaces can raise a physical foot.
            float Measured;
            if(Geometry->SampleAvatarGround(Target,Measured)) Ground=Measured;
        }
        const double Sole=13.5*BodyScale;
        const bool InStance=Target.Z<Ground+Sole+6*BodyScale;
        const bool Turned=R.Planted[Foot] && (FVector::DistSquared2D(R.Feet[Foot],Target)>FMath::Square(35.) || FMath::Abs(FMath::FindDeltaAngleDegrees(R.FootYaw[Foot],R.BodyYaw))>35);
        if(InStance && !Turned) {
            if(!R.Planted[Foot]) { R.Feet[Foot]=Target;R.Feet[Foot].Z=Ground+Sole;R.FootYaw[Foot]=R.BodyYaw; }
            Target=R.Feet[Foot];R.Planted[Foot]=true;
        } else { R.Planted[Foot]=false;Target.Z=FMath::Max(Target.Z,double(Ground)+Sole); }
        // Releasing a planted foot can move its animation target tens of cm in
        // one frame. Follow that target at a bounded speed instead of snapping;
        // the world-space anchor itself remains fixed while in stance.
        if(!R.FootGoalValid[Foot]) { R.FootGoals[Foot]=Target;R.FootGoalValid[Foot]=true; }
        else R.FootGoals[Foot]+=(Target-R.FootGoals[Foot]).GetClampedToMaxSize(350*BodyScale*Dt);
        Goals[Foot+3]=FTransform(R.Global[Index].GetRotation(),BodyTransform.InverseTransformPosition(R.FootGoals[Foot]));
    }
    for(int32 I=0;I<5;++I) {
        PBIK::FEffectorSettings Settings;Settings.ChainDepth=I==0?6:3;Settings.RotationAlpha=I<3?1.f:.6f;
        const bool Tracked=I==0 || I>=3 || (I==1?Pose.bLeftTracked:Pose.bRightTracked);
        Settings.StrengthAlpha=Tracked?1.f:0;Settings.PositionAlpha=Tracked?1.f:0;Settings.RotationAlpha*=Tracked?1.f:0;
        R.Solver.SetEffectorGoal(R.Effector[I],Goals[I].GetLocation(),Goals[I].GetRotation(),Settings);
    }
    FPBIKSolverSettings Settings;Settings.Iterations=16;Settings.SubIterations=4;Settings.bAllowStretch=false;
    Settings.PrePullRootSettings.PositionAlpha=.2;Settings.PrePullRootSettings.PositionAlphaZ=.5;
    R.Solver.Solve(Settings);
    TrackingErrorCm=0;
    for(int32 I=0;I<Ref.GetNum();++I) R.Solver.GetBoneGlobalTransform(I,R.Global[I]);
    for(int32 I=0;I<3;++I) if(I==0 || (I==1?Pose.bLeftTracked:Pose.bRightTracked))
        TrackingErrorCm=FMath::Max(TrackingErrorCm,float(FVector::Dist(R.Global[R.Bone[I]].GetLocation(),Goals[I].GetLocation())*BodyScale));
    for(int32 I=0;I<Ref.GetNum();++I) {
        const int32 Parent=Ref.GetParentIndex(I);
        const FTransform Local=Parent<0?R.Global[I]:R.Global[I].GetRelativeTransform(R.Global[Parent]);
        if(!Local.ContainsNaN()) BoneSpaceTransforms[I]=Local;
    }
    for (int32 Hand=0;Hand<2;++Hand) {
        const FMRFPSFingerPose& Fingers=Hand==0?Pose.LeftFingers:Pose.RightFingers;
        if (!Fingers.bValid || !(Hand==0?Pose.bLeftTracked:Pose.bRightTracked)) continue;
        for (int32 Finger=0;Finger<5;++Finger) for (int32 Joint=0;Joint<3;++Joint) {
            const int32 Bone=R.FingerBone[Hand][Finger][Joint];
            if (Bone==INDEX_NONE || R.FingerFlexAxis[Hand][Finger][Joint].IsNearlyZero()) continue;
            const float Flex=float(Fingers.Joint[Finger*3+Joint])/255.f;
            const float Degrees=Flex*(Joint==0?70.f:85.f);
            FQuat Delta(R.FingerFlexAxis[Hand][Finger][Joint],FMath::DegreesToRadians(Degrees));
            if (Joint==0 && !R.FingerSplayAxis[Hand][Finger].IsNearlyZero()) {
                const float Spread=(float(Fingers.Splay[Finger])/127.5f-1.f)*15.f;
                Delta=FQuat(R.FingerSplayAxis[Hand][Finger],FMath::DegreesToRadians(Spread))*Delta;
            }
            BoneSpaceTransforms[Bone].SetRotation((R.Reference[Bone].GetRotation()*Delta).GetNormalized());
        }
    }
    for (int32 I=0;I<Ref.GetNum();++I) {
        const int32 Parent=Ref.GetParentIndex(I);
        R.Global[I]=Parent<0?BoneSpaceTransforms[I]:BoneSpaceTransforms[I]*R.Global[Parent];
    }
    R.LastVisualPose=Pose; R.HasVisualPose=true;
    MarkRefreshTransformDirty();RefreshBoneTransforms();
#endif
}

FVector UMRFPSAvatarComponent::SolvedWorldPosition(FName Bone) const
{
#if !UE_SERVER
    if(Rig) if(const auto* Mesh=Cast<USkeletalMesh>(GetSkinnedAsset())) {
        const int32 Index=Mesh->GetRefSkeleton().FindBoneIndex(Bone);
        if(Rig->Global.IsValidIndex(Index)) return GetComponentTransform().TransformPosition(Rig->Global[Index].GetLocation());
    }
#endif
    return FVector::ZeroVector;
}
